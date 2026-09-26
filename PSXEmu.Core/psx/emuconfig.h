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

#include <array>
#include <string>

// The knobs that change how the machine behaves for the person using it, as
// opposed to emulated hardware state.
//
// Nothing outside this file should declare its own copy of a setting. A
// component that needs one reads it from the config rather than caching it -
// `system().config()` from anything deriving from Component.
//
// Measured hardware characteristics do not belong here. The CD-ROM's sector
// timing, the GPU's dot clock and the load stalls in `Cpu::Load` are all
// constants describing a PlayStation, not choices, and they stay next to the
// code that uses them.

namespace emulation {
namespace psx {

struct EmuConfig {
  // --- Audio ----------------------------------------------------------
  // A gain applied after the SPU's own main volume, so it covers the voices,
  // the reverb and CD audio alike.
  //
  // A PlayStation is quiet by modern standards: games set the main volume
  // conservatively and the mix peaks well below full scale - measured at about
  // a fifth of it on the discs tested here. That is faithful, and it is also
  // not what anyone wants out of their speakers, so this exists to make up the
  // difference without pretending the hardware did it.
  //
  // 1.0 is the hardware level. The mix is clamped afterwards, so a high value
  // distorts loud passages rather than wrapping them.
  //
  // It defaulted to 2.0 while the SPU decoded a fixed-level volume register
  // without doubling it, which put every voice and the main volume at half.
  // The two cancelled, so this read as taste rather than as the correction it
  // was. The mix is right now, so the default is the hardware's own level -
  // a setting already in a psxemu.ini still says 2.0 and will now be twice as
  // loud as before, which the Audio menu changes.
  float audio_volume = 1.0f;

  static const float kMinAudioVolume;
  static const float kMaxAudioVolume;

  // Which output the front end sends sound to. "wasapi" is the default
  // because it is what was used before this was a choice - WASAPI was always
  // tried first, with DirectSound only as the fallback - so nobody's sound
  // changes on upgrade. DirectSound is there for the machines where WASAPI's
  // shared-mode latency or a driver quirk makes it the worse of the two.
  std::string audio_backend = "wasapi";
  static const std::array<const char*, 2> kValidAudioBackends;

  // --- Video ------------------------------------------------------------
  // Which presenter draws the framebuffer. "d3d11" is the default so an
  // existing settings file (or none at all) behaves exactly as it did before
  // the D3D12 path existed - nobody's picture changes on upgrade unless they
  // opt in.
  std::string graphics_backend = "d3d11";
  static const std::array<const char*, 4> kValidGraphicsBackends;

  // A pixel-shader filter, by the key it was loaded under - see
  // PSXEmu.Win32/shaders/. Empty means the engine's own built-in
  // pass-through. Only the D3D12 backend honours this; the D3D11 path has no
  // filter support (see D3D11Presenter's class comment).
  std::string video_filter = "";
  static const std::array<const char*, 10> kValidVideoFilters;

  // --- Input --------------------------------------------------------------
  // What is plugged into each SIO0 port - one of the three real PS1
  // controllers, a mouse, a multitap, a GunCon, or nothing at all. See
  // Sio::ControllerType. "dualshock" for both, so an existing game that
  // already negotiates analog input keeps working exactly as it did before
  // this was choosable.
  std::array<std::string, 2> controller_type = { "dualshock", "dualshock" };
  static const std::array<const char*, 7> kValidControllerTypes;

  // Which physical source drives each PSX port - the keyboard, or one of the
  // four XInput slots the Input menu labels "Gamepad 1".."Gamepad 4" (XInput
  // user index 0-3). Both ports default to a gamepad: two people already
  // play this way today, one pad per port with no keyboard fallback to
  // reason about. Meaningless, and ignored, for a port whose controller_type
  // is "mouse" (fixed to the real mouse) or "multitap" (see
  // multitap_player_source instead) or "none" (nothing to source at all).
  std::array<std::string, 2> input_source = { "gamepad1", "gamepad2" };
  static const std::array<const char*, 5> kValidInputSources;

  // Which physical source drives each of a multitap's four players
  // (Player A-D), for whichever port's controller_type is "multitap" -
  // meaningless, and ignored, otherwise. Indexed [port][player]. Only
  // Player A defaults to a gamepad (the same one that port's own
  // input_source already defaulted to, so a game that only ever reads
  // Player A sees no difference from before multitap existed); B-D default
  // to "keyboard" rather than a second/third/fourth gamepad no one may
  // actually have connected, exactly like a real multitap's own extra
  // sockets start out with nothing plugged into them.
  std::array<std::array<std::string, 4>, 2> multitap_player_source = {{
      { "gamepad1", "keyboard", "keyboard", "keyboard" },
      { "gamepad2", "keyboard", "keyboard", "keyboard" },
  }};

  // What each of those players is (bug 98): "digital", "dual_analog",
  // "dualshock" or "none". All four default to a DualShock, which is what
  // every player behind a multitap was before this existed.
  std::array<std::array<std::string, 4>, 2> multitap_player_type = {{
      { "dualshock", "dualshock", "dualshock", "dualshock" },
      { "dualshock", "dualshock", "dualshock", "dualshock" },
  }};
  static const std::array<const char*, 4> kValidMultitapPlayerTypes;

  // --- Timing -------------------------------------------------------------
  // Whether the front end holds the machine to the emulated display's own
  // frame rate - 59.29 Hz in NTSC, 49.76 in PAL - against the wall clock.
  //
  // On is a console, and is the default because it is the only setting that
  // runs at a defined speed. Off does not mean "uncapped" so much as "paced by
  // whatever happens to block first": the monitor's refresh rate through
  // Present with vsync on, or the sound device's buffer filling. On a 165 Hz
  // display with audio not gating, that is 2.8x - which is what made the BIOS
  // intro flash past and is the whole reason this exists (bug 49).
  //
  // Worth turning off to get through a long load or an unskippable intro, or
  // to see how much headroom the host has - the window title's percentage is
  // only interesting when something is allowed to exceed 100.
  bool frame_limiter = true;

  // --- CD-ROM -------------------------------------------------------------
  // Whether the drive is charged for the mechanical work a real one does:
  // spinning up from a standstill, moving the head a distance rather than
  // teleporting it to the target, and waiting for the disc to come round.
  //
  // The costs themselves are hardware characteristics and stay in cdrom.cpp
  // beside the code that uses them, per the note above; what lives here is
  // only the choice of whether to charge them, because it is a genuine
  // trade-off rather than a fact about a PlayStation.
  //
  // Off - the default, and the timing this emulator has always had - makes
  // every seek flat and nearly free. Loading is faster than hardware
  // everywhere, and it shows most on the BIOS's "Licensed by SCEA" logo
  // screen, which is on screen for exactly as long as the drive takes and so
  // lasts about 2.5 seconds instead of the several a console spends there.
  //
  // On is closer to a console at the cost of waiting for it, and of moving
  // every disc-dependent timing baseline in Docs/Test-Suite.md - which is why
  // it is opt-in rather than the default.
  bool cdrom_mechanical_timing = false;

  // --- Boot -----------------------------------------------------------
  // Whether booting a disc arms System::set_auto_boot instead of letting the
  // BIOS run all the way through on its own - see the comment there for what
  // that actually does (it still runs real BIOS init, just hands off to the
  // game at the address the shell's own GUI code would otherwise start
  // running from, so the logo and the disc-check screen never execute).
  //
  // Off - the default, matching how a disc boot has always behaved here -
  // shows both. A bare PS-EXE has no shell path to skip in the first place:
  // System::set_auto_boot_exe uses the same hand-off unconditionally, since
  // a raw side-load without it never clears BEV or Isolate Cache in the
  // first place, so this setting has nothing to add there.
  bool skip_bios_intro = false;

  // --- CPU ----------------------------------------------------------------
  // Run the machine on the recompiler rather than the interpreter.
  //
  // Roughly twice the speed (Docs/Recompiler-Plan.md has the numbers), and
  // off by default because its timing is not yet proven equivalent: the BIOS
  // boot is identical either way, but a game's frame timing differs slightly,
  // so the interpreter stays the thing every baseline is measured against.
  //
  // Safe to change while the machine is running. System::StepInstruction
  // applies it between instructions, on the thread that runs the machine -
  // which matters, because switching it off frees the compiled code, and
  // doing that from the message thread while a block is executing would be
  // freeing the ground out from under it.
  bool recompiler = false;

  // Rasterise on a thread of its own, behind a queue of parsed primitives
  // (phase 7 of Docs/Threading-Plan.md). The GPU's timing stays on the machine
  // thread either way - only the pixels move - and every read of VRAM waits
  // for the rasteriser, so a threaded run is byte-identical to an unthreaded
  // one. Read once, when the GPU is initialised.
  bool gpu_thread = true;

  // Charge the GPU for CPU-to-VRAM and VRAM-to-CPU transfers: one tick per pixel
  // moved, so commands issued after a large upload wait for it the way they
  // would on hardware. Off by default because it can only make a game slower,
  // and nothing measured says exactly how much slower; the figure is derived
  // from the VRAM-to-VRAM copy cost, not measured. See Docs/Bugs-Found.md 93.
  bool gpu_transfer_timing = false;

  // Charge instruction fetches for the instruction cache: a hit is free, a miss
  // refills the rest of its 16-byte line, and code run uncached - the BIOS ROM
  // through KSEG1, most of all - pays the full bus cost of every fetch. The
  // interpreter only; with the recompiler on it has no effect, because compiled
  // blocks do not fetch. Off by default because it moves the timing of every
  // game. A timing model only - instructions still come from memory, never
  // from the cache. See Docs/Bugs-Found.md 94.
  bool icache_timing = false;

  // Four finer timing models, each replacing an approximation Docs/Gaps.md lists, and each
  // off by default so that nothing a game does changes unless it is asked for. Safe to
  // change while a game runs: the machine picks each up between instructions.
  //
  // Devices are brought up to date at the next event rather than in fixed 32-cycle
  // batches: a counter's target, a DMA finishing, a CD response, the edges of hblank and
  // the end of each scanline. So an interrupt is raised on the cycle it happens, not up
  // to 31 later, and counter 1 counts an hblank as the beam enters it rather than at the
  // end of the line. Costs speed in proportion to how often those events come.
  bool exact_event_timing = false;

  // The CPU waits while a DMA holds the bus, as it does on a console, instead of running
  // on through the transfer's time. The busy bit and the interrupt already waited for
  // that time either way (bug 38); what this adds is that the program cannot use it.
  bool dma_stops_cpu = false;

  // The 8- and 16-bit buses (the BIOS ROM, the expansion regions, the CD-ROM and the
  // SPU) timed by a rule fitted to JaCzekanski's access-time table from a real console
  // rather than by psx-spx's formula, which is 1 to 4 cycles out for the regions with a
  // recovery or pre-strobe period. An access right behind another pays the recovery
  // period, and an lwl or lwr reads only the part of the word it needs.
  bool measured_bus_timing = false;

  // Stores go through the CPU's write queue: free until it is full, and a load waits
  // for it to empty. Four entries, each draining in the time the same access would take
  // to read. psx-spx describes the queue but gives no numbers, and nobody here has
  // measured one, so this is a model rather than a measurement. The interpreter only,
  // like icache_timing.
  bool write_queue_timing = false;

  // --- BIOS ---------------------------------------------------------------
  // Which image in the front end's BIOS folder to boot, by filename alone -
  // "SCPH1001.BIN", not a path. The folder is the front end's to know
  // (Documents\My Games\PSXEmu\bios), and keeping only the name here means a
  // settings file still points at the right dump after that folder moves,
  // which is the whole reason it is not stored as a path.
  //
  // Empty - the default, and what a first run has - means "whichever the
  // front end would have found on its own", so nothing changes for someone
  // who never opens the menu. A name that is no longer in the folder falls
  // back the same way rather than refusing to boot.
  std::string bios_file = "";

  // --- Speed --------------------------------------------------------------
  // How fast to run the machine against the wall clock: 1.0 is a console, 2.0
  // is twice as fast. Nothing about the emulated machine changes - it is still
  // 33.8688 MHz and 59.29 Hz, and no baseline in Docs/Test-Suite.md moves -
  // this only scales the rate the frame limiter paces to, and the rate the
  // audio is resampled at on its way to a device that consumes 44,100 samples
  // per *real* second however many the SPU made in an emulated one.
  //
  // Means nothing with frame_limiter off, which is already "as fast as
  // whatever blocks first"; the menu greys the choices out there.
  float emulation_speed = 1.0f;

  static const std::array<float, 6> kValidSpeeds;

  // --- Front end ----------------------------------------------------------
  // Whether the machine pauses while one of the front end's menus - or a
  // dialog opened from one - is open.
  //
  // Off, the default, is what DuckStation, PCSX2 and Dolphin do: the machine
  // runs on its own thread, so a menu blocks nothing but the window, and the
  // game carries on underneath it. On is for anyone who would rather not lose
  // a second of a game to reading a menu. See Docs/Threading-Plan.md.
  bool pause_in_menus = false;

  // Whether the window title carries where each frame's time goes - emulating,
  // presenting, sound, input - on top of the frame rate. Docs/Threading-
  // Plan.md's phase 0: the numbers the threading work is measured by.
  bool show_timings = false;

  // Whether the BIOS console window is open: everything software writes
  // through the BIOS's putchar/puts/printf, as it arrives. Nothing in the
  // machine reads this - the text is recorded either way (Kernel) - so it is
  // purely which windows the front end has up.
  bool show_bios_console = false;

  // Whether a byte the serial port transmits is copied into that same console
  // text (Sio1). Nothing is attached to SIO1, so what a program sends there is
  // otherwise gone; a homebrew program printing over the port is readable with
  // this on. Off by default: it is a debugging aid, and on a retail game the
  // port is silent anyway.
  bool sio1_to_console = false;

  // --- Mouse ------------------------------------------------------------
  // How a host mouse's movement becomes a PlayStation mouse's counts, when a
  // port is set to Sio::kMouse. The device itself has no say in this - it
  // reports raw counts and any acceleration is the game's - so this is a
  // choice about the host, and utilities::MouseMotion (platform/
  // mouse_scaling.h) has what each one means:
  //
  //   "desktop"   the cursor's own movement, exactly as Windows accelerated
  //               it, with the pointer captured to the window
  //   "windows"   raw counts with Windows' curve reapplied (approximated)
  //   "hardware"  raw counts, linear, scaled for a 1994 ball mouse
  //
  // "desktop" by default: it is the one that needs nothing guessed, and the
  // one whose feel a person can check against their own desktop.
  std::string mouse_motion = "desktop";
  static const std::array<const char*, 3> kValidMouseMotions;

  // What the host mouse is set to, in counts per inch. Windows cannot be
  // asked - HID carries no such field - so "hardware" needs it from the
  // person; it is what their mouse's own software says. 800 is the common
  // default on a modern mouse.
  int mouse_dpi = 800;
  static const std::array<int, 4> kValidMouseDpis;
};

// Out of line so there is one definition; these are bounds a UI can offer
// rather than anything the emulation depends on.
inline const float EmuConfig::kMinAudioVolume = 0.0f;
inline const float EmuConfig::kMaxAudioVolume = 8.0f;

inline const std::array<const char*, 4>
    EmuConfig::kValidGraphicsBackends = { "d3d11", "d3d12", "opengl", "vulkan" };

inline const std::array<const char*, 2>
    EmuConfig::kValidAudioBackends = { "wasapi", "dsound" };

// Empty string ("None") first, then the nine loaded filters in the same
// order PSXEmu.Win32's Video > Filter menu offers them.
inline const std::array<const char*, 10> EmuConfig::kValidVideoFilters = {
    "",         "nearest",    "bilinear", "crt",   "eagle",
    "hq2x",     "xbrz_legacy", "scanline", "xbrz",     "superxbr",
};

// In the same order PSXEmu.Win32's Emulation > Speed menu offers them.
inline const std::array<float, 6> EmuConfig::kValidSpeeds = {
    0.5f, 1.0f, 1.5f, 2.0f, 2.5f, 3.0f,
};

// In the same order PSXEmu.Win32's Input > Mouse menus offer them, and the
// same order utilities::MouseMotion declares them.
inline const std::array<const char*, 3> EmuConfig::kValidMouseMotions = {
    "desktop", "windows", "hardware",
};

inline const std::array<int, 4> EmuConfig::kValidMouseDpis = {
    400, 800, 1600, 3200,
};

// Order matches PSXEmu.Win32's Input > Controller Port menus and
// Sio::ControllerType (kDigital, kDualAnalog, kDualShock, kMouse, kNone,
// kMultitap).
inline const std::array<const char*, 7> EmuConfig::kValidControllerTypes = {
    "digital", "dual_analog", "dualshock", "mouse", "none", "multitap", "guncon" };

// Order matches PSXEmu.Win32's Input > Port Source menus. Four gamepad
// slots, not two, because a single multitap wants up to four independently
// assignable ones - bounded there rather than open-ended since XInput
// itself only ever supports four physical controllers.
inline const std::array<const char*, 4> EmuConfig::kValidMultitapPlayerTypes = {
    "digital", "dual_analog", "dualshock", "none" };

inline const std::array<const char*, 5> EmuConfig::kValidInputSources = {
    "keyboard", "gamepad1", "gamepad2", "gamepad3", "gamepad4" };

}
}
