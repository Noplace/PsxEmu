/*****************************************************************************************************************
* Copyright (c) 2014 Khalid Ali Al-Kooheji                                                                       *
*                                                                                                                *
* Permission is hereby granted, free of charge, to any person obtaining a copy of this software and              *
* associated documentation files (the "Software"), to deal in the Software without restriction, including        *
* without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell        *
* copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the       *
* following conditions:                                                                                          *
*                                                                                                                *
* The above copyright notice and this permission notice shall be included in all copies or substantial           *
* portions of the Software.                                                                                      *
*                                                                                                                *
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT          *
* LIMITED TO THE WARRANTIES OF MERCHANTABILITY, * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.          *
* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, * DAMAGES OR OTHER LIABILITY,      *
* WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE            *
* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                                                         *
*****************************************************************************************************************/
#pragma once



namespace emulation {
namespace psx {

// Defined in emu/recompiler_bridge.h, which System only ever holds a pointer
// to: including it here would pull the whole recompiler into everything that
// includes system.h.
class RecompilerBridge;

class System {
 friend DebugAssist;
 public:
  // Sizes of the memories the machine actually has, not settings.
  static const uint32_t kRamSize  = 0x200000;   // 2 MB
  static const uint32_t kBiosSize = 0x80000;    // 512 KB

  System();
  ~System();

  // The BIOS path is a setting, not a constant: the user supplies the dump. With
  // EmuConfig::hle_bios on, or no dump to be had, the machine boots the HLE BIOS instead
  // (psx/hle_bios.h) - so a missing file is a failure only when HLE is off.
  //
  // Which of the two is chosen is settled at the first instruction, not here: a disc's own
  // settings are applied after Initialize (the front end only knows the game once the disc is
  // in), and they may say otherwise.
  int Initialize(const char* bios_path);

  // Brings every device up without a BIOS, for tests that drive a device
  // through its protocol and never execute an instruction.
  int InitializeWithoutBios();
  int Deinitialize();

  // One instruction, plus whatever interrupt it lets through. Deterministic
  // and free of any wall-clock throttling, so a headless harness can drive it
  // directly and get the same answer every run.
  //
  // False when the debugger halted the machine instead (debug/debugger.h): nothing ran, and the
  // caller should stop and not count it.
  bool StepInstruction() { return StepImpl<true>(); }

  // The same instruction with the debugger's check compiled out. Only for a caller that has
  // seen debugger().armed() is false and knows nothing can arm it before it looks again -
  // host::Machine, for a frame: the window's requests run between frames, never inside one.
  // An idle debugger then costs one test a frame rather than one an instruction, which on the
  // BIOS boot measured about 2.5% of the interpreter's time.
  void StepInstructionUnarmed() { StepImpl<false>(); }

  void LoadBiosFromMemory(const void* buffer);
  bool LoadBiosFromFile(const char* filename);

  // The HLE BIOS, and whether the machine is running it rather than a dump.
  hle::HleBios& hle() { return hle_; }
  bool hle_active() const { return hle_active_; }
  // Puts the chosen BIOS in the ROM: the HLE one when EmuConfig::hle_bios says so or there is no
  // dump. Done by Initialize, and again at the first instruction if the setting changed since.
  void SelectBios();
  bool LoadPsExe(const char* filename);
  bool LoadPsExeFromMemory(const void* data, size_t size);

  // What BootDisc found, whether or not it succeeded. A boot that fails says
  // which step failed rather than just "no".
  struct DiscBootInfo {
    std::string volume_id;
    std::string boot_path;      // the BOOT line, verbatim
    std::string executable;     // the identifier as stored on the disc
    uint32_t executable_size;
    const char* error;
    DiscBootInfo() : executable_size(0), error(nullptr) {}
  };

  // Reads SYSTEM.CNF from the mounted disc and starts the executable it names.
  bool BootDisc(DiscBootInfo* info);

  // The filesystem on the mounted disc, for a harness that wants to look.
  Iso9660& iso() { return iso_; }

  static bool ParseSystemCnf(const std::vector<uint8_t>& contents,
                             std::string* boot_path);

  // Watch a VRAM rectangle, for the harnesses.
  void WatchVram(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    gpu_.WatchVram(x, y, w, h);
  }

  // Mounting a disc. Accepts a cue sheet, an image file or a drive letter;
  // an empty path ejects.
  bool LoadDisc(const char* path) { return io_.cdrom.OpenDisc(path); }
  void EjectDisc() { io_.cdrom.CloseDisc(); }
  Cdrom& cdrom() { return io_.cdrom; }
  Sio& sio() { return io_.sio; }
  Cpu& cpu() { return cpu_; };
  Spu& spu() { return spu_; };
  IOInterface& io() { return io_; };
  MC& mc(int slot) { return mc_[slot]; }
  // A port's four memory card slots (bug 99). Slot 0 is the port's own card - the one mc(port)
  // has always meant, so a card and its saves stay where they were - and 1-3 are the extra
  // sockets a multitap adds, which the card bus reaches as 82h-84h. A slot with no file loaded
  // is an empty socket.
  static const int kCardSlotsPerPort = 4;
  MC& mc(int port, int slot) { return slot == 0 ? mc_[port] : multitap_mc_[port][slot - 1]; }
  Kernel& kernel() { return kernel_; };
  // Breakpoints and stepping (debug/debugger.h). After StepInstruction, debugger().halted() says
  // the step was a halt - nothing ran - and whoever is running the machine stops and does not
  // count it.
  Debugger& debugger() { return debugger_; }
  GTE& gte() { return gte_; };
  // PGXP's shadows (enhance/pgxp.h): on while EmuConfig::pgxp_vertices is and the hardware
  // rasteriser draws.
  Pgxp& pgxp() { return pgxp_; }
  // The user-facing settings. Read rather than cached, so a change takes
  // effect without anything needing to be told about it.
  EmuConfig& config() { return config_; }
  const EmuConfig& config() const { return config_; }
  // The recompiler, off unless something turns it on.
  //
  // With it off, StepInstruction is the code it always was. With it on, the
  // machine runs blocks of compiled guest code instead of one interpreted
  // instruction at a time, which changes when interrupts land and charges
  // cycles in bursts - see emu/recompiler_bridge.h. Opt-in for that reason,
  // and checked against the regression baselines rather than assumed
  // equivalent.
  void EnableRecompiler(bool on);
  bool recompiler_enabled() const { return recompiler_ != nullptr; }
  RecompilerBridge* recompiler() { return recompiler_.get(); }

  // Software saying it has replaced code: the cache-control register at
  // 0xFFFE0130. Throws away every compiled block, the way that write throws
  // away the instruction cache. Does nothing when the recompiler is off, and
  // is defined in system.cpp because only that file knows what a
  // RecompilerBridge is.
  void ResetCompiledCode();

  uint8_t* ram() { return io_.ram_buffer.u8; }
  uint8_t* bios() { return io_.bios_buffer.u8; }
  double base_freq_hz() { return base_freq_hz_; }
  void set_base_freq_hz(double base_freq_hz) { base_freq_hz_ = base_freq_hz; }
  // The hardware rasteriser a front end can make, for EmuConfig::gpu_rasteriser =
  // "hardware". Set before Initialize; the GPU asks for one each time it is initialised.
  void set_hardware_raster(RasterFactory factory) { hardware_raster_ = std::move(factory); }
  const RasterFactory& hardware_raster() const { return hardware_raster_; }

  // The core owns the GPU and the framebuffer; a front end only reads them.
  Gpu& gpu() { return gpu_; }
  GpuCore* gpu_core() { return &gpu_; }
  
  bool auto_boot_ = false;
  std::string auto_boot_path_;
  void set_auto_boot(bool v, const std::string& path = "") {
    auto_boot_ = v;
    auto_boot_path_ = path;
  }

  // Same idea, for a bare PS-EXE rather than a disc: let the BIOS run for
  // real - which is what clears BEV and Isolate Cache and sets up the kernel,
  // none of which a raw side-load ever touches - and side-load the executable
  // once the kernel is up: when the BIOS writes POST code 7, as DuckStation
  // does. Not at pc=80030000, which is where this used to fire: by then the
  // BIOS has copied its shell there, and an executable whose uninitialised
  // data overlaps the shell's image reads the shell's bytes where it expects
  // the zeroes it would find booted from a disc (bug 65).
  bool auto_boot_exe_ = false;
  std::string auto_boot_exe_path_;
  void set_auto_boot_exe(bool v, const std::string& path = "") {
    auto_boot_exe_ = v;
    auto_boot_exe_path_ = path;
  }
  #ifdef _DEBUG
  DebugAssist csvlog;
  #endif
  inline void Tick() {
    ++cpu_context_.cycles;
    ++cpu_context_.current_cycles;
    io_.Tick(1);
  }

  // How many hardware interrupts were actually taken. An interrupt that is
  // raised but never delivered looks exactly like one that was never raised.
  uint64_t interrupts_taken() const { return interrupts_taken_; }
  const uint64_t* interrupts_taken_by_source() const {
    return interrupts_taken_by_source_;
  }
  // Instructions that ran with an interrupt pending but blocked by Cop0 SR.
  uint64_t interrupts_blocked() const { return interrupts_blocked_; }
  uint64_t interrupts_blocked_im() const { return interrupts_blocked_im_; }
  // Interrupts that landed on a GTE command and so were delivered behind it
  // rather than in front - see StepInstruction.
  uint64_t interrupts_after_gte_command() const {
    return interrupts_after_gte_command_;
  }
  // Instructions that ran with interrupts globally enabled.
  uint64_t instructions_with_ie() const { return instructions_with_ie_; }

  // Writes/reads every component's Serialise, in a fixed order, behind the
  // header Docs/Save-States-Plan.md specifies. Returns an empty string on
  // success, or the reason on failure. The header checks (magic, version,
  // BIOS hash) run before any live component is touched, so the expected
  // failure modes - wrong BIOS, wrong version, not a state file, a moved
  // disc image - never leave a running machine partially overwritten. A
  // payload hand-truncated after a valid header is the one case that can:
  // there is no way to know a field is the last one before reading past it,
  // short of a second, wasted pass over every component first.
  std::string SaveState(const std::string& path);
  std::string LoadState(const std::string& path);

  // The two halves of a state without its file: the payload - every component's Serialise in
  // the fixed order, either direction - and, once one has been read, what a load does to the
  // machine around it. `drop_compiled_code` false keeps the recompiler's blocks, which is
  // right only when RAM's code is the same bytes as before the load; boot_runner --bench-state
  // uses it to measure what throwing them away costs (Docs/Rewind-and-Run-Ahead-Plan.md).
  void SerialisePayload(StateIO& state);
  void FinishLoad(bool drop_compiled_code);

  // A state kept in memory, for rewind and run-ahead (Docs/Rewind-and-Run-Ahead-Plan.md). It
  // has no header, since it never leaves this process, this BIOS or this disc set. Its buffers
  // are kept: taking another into the same MemoryState allocates nothing.
  struct MemoryState {
    std::vector<uint8_t> payload;
    // Every slot's card, when taken with them: run-ahead has to take back what its speculative
    // frames wrote. Rewind leaves the cards alone, as loading a save state does.
    bool has_cards = false;
    MC::Snapshot cards[2][kCardSlotsPerPort];
  };
  void SaveMemoryState(MemoryState* state, bool with_cards);
  // The same machine as LoadState gives, more cheaply: no copy of the payload, the disc left
  // open when it is the one the state was taken on, and only the compiled code from RAM words
  // that changed thrown away - which is also what keeps a recompiled machine on the course it
  // would take with no load. An empty string, or why not: a disc that can no longer be opened.
  std::string LoadMemoryState(const MemoryState& state);

 private:
  void SerialiseAdditions(StateIO& state);
  // StateIO::RamWatcher for LoadMemoryState: a run of RAM words that changed, whose compiled
  // code goes.
  static void RamWordsChanged(void* context, uint32_t offset, uint32_t bytes);
  // The payload SaveState writes and LoadState reads, kept so neither allocates once both have
  // run.
  std::vector<uint8_t> state_buffer_;
  // StepInstruction and StepInstructionUnarmed: one body, with and without the debugger.
  template <bool kDebugger>
  bool StepImpl();
  void SettleBios();

  std::unique_ptr<RecompilerBridge> recompiler_;
  hle::HleBios hle_;
  bool hle_active_ = false;
  // Until the first instruction runs, the BIOS can still be swapped for the other one.
  bool bios_pending_ = false;
  std::vector<uint8_t> bios_file_;   // the dump Initialize was given, if it could be read
  double base_freq_hz_;
  uint64_t interrupts_taken_;
  // How many interrupts were taken with each I_STAT bit pending, so "the
  // interrupt fires but the handler does nothing" can be told apart from "the
  // interrupt never fires".
  uint64_t interrupts_taken_by_source_[11] = {0};
  uint64_t interrupts_blocked_;
  uint64_t interrupts_blocked_im_;
  uint64_t interrupts_after_gte_command_ = 0;
  uint64_t instructions_with_ie_;
  RasterFactory hardware_raster_;   // declared before gpu_, which asks for it
  Gpu gpu_;
  CpuContext cpu_context_;
  Cpu cpu_;
  Spu spu_;
  IOInterface io_;
  MC mc_[2];
  MC multitap_mc_[2][3];
  Kernel kernel_;
  Debugger debugger_{this};
  GTE gte_;
  Pgxp pgxp_;
  Iso9660 iso_;
  EmuConfig config_;
};

}
}

