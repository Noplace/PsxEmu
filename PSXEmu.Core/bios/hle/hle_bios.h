#pragma once

// A high-level emulation of the PlayStation BIOS: the machine boots and runs games with no BIOS
// dump at all. EmuConfig::hle_bios chooses it; Docs/HLE-BIOS.md is the plan and its status.
//
// What "high level" means here, and what it does not. The kernel's *interface* is reproduced as
// it is on a console, because that is what software sees and depends on:
//
//  - The first 64 KB of RAM is laid out as the SCPH-1001 kernel (CEX-3000/1001/1002, 1995-12-04)
//    lays it out: the vectors at 0, 80h, A0h, B0h and C0h, the table of tables at 100h, the A0h
//    table at 200h, the dispatchers at 5C4h/5E0h/600h, the B0h and C0h tables at 874h and 674h,
//    the exception handler at C80h, the exception stack and exit structure at 6CF0h/6CF4h/75D0h,
//    the DCBs at 6EE0h and FCBs at 8648h, and the control blocks allocated from E000h in the
//    same order and sizes. Games walk these tables, read the control blocks, chain handlers into
//    the exception chains and patch the exception handler by offset from C(06h) - psx-spx "BIOS
//    Patches" - so all of that is real data and real MIPS code, built by bios/mips_assembler.h.
//  - Every kernel *function* - what the A0h/B0h/C0h tables point at - is a host function. Each
//    has an address of its own in ROM, in the trap region below, and System::StepImpl hands a
//    step whose pc lands there to Trap() instead of executing the word that is there. The table
//    entries and every pointer to a function (an ExCB's handlers, the default exit structure, a
//    DCB's entry points) are those addresses, so a game that reads one, calls it, or replaces it
//    gets what it would on a console.
//
// Kernel state lives in guest RAM - the control blocks where the kernel keeps them, everything
// else in KernelVars at 1000h - never in this object. A save state therefore holds all of it
// without a format change, and the recompiler needs to know nothing beyond "do not compile the
// trap region".
//
// Calling guest code from a host function (an event's callback, a handler in a chain) cannot
// nest a CPU loop inside a trap. Instead the host function hands the CPU the call with `ra` set
// to a resume trap, after pushing what it still has to do onto a continuation stack in kernel
// RAM; the resume trap pops it and carries on. See CallGuest and Continue.

#include <cstdint>
#include <string>
#include <vector>

namespace emulation {
namespace psx {

class System;
class Iso9660;

namespace hle {

    class HleBios {
    public:
        // ---- The ROM -------------------------------------------------------------------------------
        // The trap region: kernel functions' entry points, one 16-byte slot each, and above them the
        // pad and card driver's 8 KB (see kB5BOffset). Physical addresses, so KSEG0, KSEG1 and KUSEG
        // reach the same slot.
        static const uint32_t kTrapBase = 0x1FC10000;
        static const uint32_t kTrapSize = 0x00012000;
        static bool IsTrap(uint32_t pc) { return ((pc & 0x1FFFFFFF) - kTrapBase) < kTrapSize; }

        // Function numbers: the three tables, then the kernel's own entry points.
        enum : uint32_t { kTableA = 0x000, kTableB = 0x100, kTableC = 0x200, kInternal = 0x300 };
        enum Internal : uint32_t {
            kReset = kInternal,     // BFC00000's work: build the kernel and boot
            kResume,                // where a host function's call into guest code returns to
            kUnknown,               // a table entry with nothing behind it
            kExeReturned,           // what the boot executable returns to
            kDoExecuteReturned,     // what an executable started by DoExecute returns to
            kSyscallVerifier,       // the default handlers in the exception chains: first function...
            kCdIoVerifier,
            kTimerVerifier0, kTimerVerifier1, kTimerVerifier2, kTimerVerifier3,
            kTimerHandler0, kTimerHandler1, kTimerHandler2, kTimerHandler3,   // ...and second
            kPadCardVerifier, kPadCardHandler,
            kDefIntVerifier,
            kSetPadOutput,          // inside B(5Bh)'s driver, where games find them by offset
            kSetPadEnableFlag,
            kClearPadEnableFlag,
            kDelay,                 // a call that has done its work, waiting out the time it takes
            kShell,                 // the shell's frame loop (hle_shell.cpp)
            kInternalEnd
        };

        HleBios() = default;
        void set_system(System* system) { system_ = system; }

        // The 512 KB ROM: the header a kernel has at BFC00100h, the version string at BFC7FF32h,
        // the reset vector and the trap region. The same bytes every time - a save state names its
        // BIOS by hashing them.
        static void BuildRom(uint8_t* rom);

        // Runs the kernel function whose entry point `pc` is. Called by System::StepImpl in place of
        // the instruction there; whatever the function does to the registers, memory and the pc is
        // the whole of the step. Returns the cycles the step is to cost.
        //
        // What a function costs is the time the SCPH-1001 kernel's took, measured: boot_runner
        // --time-calls on ten games, 19 million calls (hle_bios_timing.cpp has the table). Up to
        // kDelayThreshold it is charged to the step; above, the call is held at the kDelay trap until
        // that many cycles have passed - so a file that takes a console 22 ms to load takes 22 ms here,
        // with vblanks and every other interrupt taken along the way, as they are while the kernel's
        // own code runs.
        uint32_t Trap(uint32_t pc);

        // The least a trap costs: an internal entry point, or the slice of a delay.
        static const uint32_t kTrapCycles = 24;
        static const uint32_t kDelayThreshold = 20000;
        static const uint32_t kDelaySlice = 1024;
        // The devices, as the kernel drives them: a CD-ROM sector at double speed (150 a second), the
        // seek and settle a file read starts with (the measured FileRead's fixed part), and a memory
        // card sector, which the kernel's driver moves one of every second frame.
        static const uint32_t kCdSectorCycles = 225792;
        static const uint32_t kCdSeekCycles = 540000;
        static const uint32_t kCardSectorCycles = 1128960;

        // A store to the ROM, which the bus discards: a game patching the kernel's pad and card driver
        // by offset from B(5Bh) (psx-spx "BIOS Patches"). The patches whose purpose is a behaviour this
        // kernel has are turned into that behaviour; the rest are noted in messages().
        void RomWrite(uint32_t physical, uint32_t value);
        // Whether a game has applied patch_no_pad_card_auto_ack - for hle_test.
        bool pad_ack_removed() { return vars().pad_ack_removed != 0; }

        // What did not work, for a harness to print: a function nothing implements yet, a jump into
        // the middle of a slot, a system error. Each message once.
        const std::vector<std::string>& messages() const { return messages_; }

        // The messages since the last call, moved into `out` - for a front end to show as they happen
        // (the BIOS console, a notification) rather than a harness reading them all at the end.
        void TakeNewMessages(std::vector<std::string>* out) {
            out->clear();
            out->swap(new_messages_);
        }

        // Every kernel function entered, with its arguments and caller, one line each - for finding
        // where a game and this kernel part ways. Null turns it off.
        void set_trace(FILE* trace) { trace_ = trace; }

        // ---- Kernel RAM ----------------------------------------------------------------------------
        static const uint32_t kExceptionVector = 0x080;
        static const uint32_t kTableOfTables = 0x100;
        static const uint32_t kBootArgument = 0x180;   // the rest of SYSTEM.CNF's BOOT line
        static const uint32_t kATable = 0x200;         // A(00h-BFh)
        static const uint32_t kADispatch = 0x5C4;
        static const uint32_t kBDispatch = 0x5E0;
        static const uint32_t kCDispatch = 0x600;
        static const uint32_t kCTable = 0x674;         // C(00h-7Fh)
        static const uint32_t kBTable = 0x874;         // B(00h-FFh)
        static const uint32_t kExceptionHandler = 0xC80;
        static const uint32_t kExceptionCauseEpc = 0xEA0;   // its helper: v0 = cause, v1 = epc
        static const uint32_t kChangeThreadStub = 0xEB0;    // syscall(3): ChangeThread's last step
        static const uint32_t kVars = 0x1000;               // KernelVars
        static const uint32_t kExceptionStackVar = 0x6CF0;  // holds the exception stack's top
        static const uint32_t kDefaultExit = 0x6CF4;        // the default exit structure, 30h bytes
        static const uint32_t kDcbs = 0x6EE0;               // 10 x 50h
        static const uint32_t kCurrentExit = 0x75D0;        // points at the exit structure in use
        static const uint32_t kExceptionStackTop = 0x85D8;  // 1000h bytes down from here
        static const uint32_t kFcbs = 0x8648;               // 16 x 2Ch
        static const uint32_t kKernelHeap = 0xE000;         // 2000h bytes: ExCBs, EvCBs, PCB, TCBs
        static const uint32_t kKernelHeapSize = 0x2000;

        static const int kFcbCount = 16;
        static const int kDcbCount = 10;

    private:
        // ---- Control blocks, as they sit in RAM (psx-spx "BIOS Control Blocks") -----------------
        struct Tcb {
            uint32_t status;        // 1000h free, 4000h used
            uint32_t unused;
            uint32_t reg[32];
            uint32_t epc, hi, lo, sr, cause;
            uint32_t pad[9];
        };
        struct EvCb {
            uint32_t cls, status, spec, mode, func, unused[2];
        };
        struct Fcb {
            uint32_t status, port, transfer_address, transfer_length, position, device_flags, error,
                dcb, size, lba, number;
        };
        struct Dcb {
            uint32_t name, flags, sector_size, long_name, init, open, in_out, close, ioctl, read, write,
                erase, undelete, firstfile, nextfile, format, chdir, rename, remove, testdevice;
        };
        // The 16-byte element an exception chain is made of (C(02h) SysEnqIntRP).
        struct ChainElement {
            uint32_t next, second, first, unused;
        };
        // One call into guest code still to come back: what to do then, and what with.
        struct Frame {
            uint32_t kind, w[7];
        };

        // Everything else the kernel remembers. Laid over RAM at kVars, so a save state carries it.
        struct KernelVars {
            uint32_t rand_seed;
            uint32_t heap_start, heap_end;                      // InitHeap
            uint32_t kernel_heap_start, kernel_heap_end;        // SysInitMemory
            uint32_t strtok_next;
            uint32_t conf_tcb, conf_evcb, conf_stacktop;        // in this order: see BuildRom
            uint32_t pad_buf[2], pad_size[2];                   // InitPad
            uint32_t pad_started;                               // InitPad done
            uint32_t pad_enable;                                // InitCard(pad_enable)
            uint32_t pad_autoack;                               // ChangeClearPad
            uint32_t pad_output[2];                             // SetPadOutput: what to send, or 0
            uint32_t outdated_pad_dest;                         // OutdatedPadInitAndStart
            uint32_t card_started;
            uint32_t card_status[2];                            // get_card_status
            uint32_t card_op[2], card_sector[2], card_buffer[2];   // the sector waiting for a vblank
            uint32_t card_last_port;                            // get_bu_callback_port
            uint32_t card_allow_new;                            // allow_new_card
            uint32_t card_find_mode;
            uint32_t card_auto_format;
            uint32_t card_flip;                                 // which slot the next vblank serves
            uint32_t timer_autoack[4];                          // ChangeClearRCnt
            uint32_t irq_autoack[11];                           // SetIrqAutoAck
            uint32_t last_error;                                // GetLastError
            uint32_t search_device, search_index, search_port;  // firstfile/nextfile
            char search_pattern[64];
            uint32_t frames;                                    // continuation stack depth
            uint32_t exec_header;                               // DoExecute's headerbuf
            uint32_t boot_stack;                                // from SYSTEM.CNF
            uint32_t cd_seek_lba;                               // CdAsyncSeekL
            uint32_t cd_events_open;
            uint32_t cd_event[5];
            uint32_t cd_int5[2];                                // CdromGetInt5errCode
            char cd_directory[64];                              // chdir
            char boot_path[64];                                 // the BOOT line's file
            uint32_t outdated_pad_type;
            uint32_t bu_init_stage;                             // _bu_init's reads, in progress
            uint32_t delay_active;                              // a call held at kDelay
            uint32_t delay_until_low, delay_until_high;         // until this cycle
            uint32_t delay_return;                              // and then on here
            uint32_t pad_ack_removed;                           // patch_no_pad_card_auto_ack applied
            uint32_t reserved[10];
        };

        // Where the parts of the kernel that are not the big tables live, inside the first 64 KB.
        static const uint32_t kChains = 0x1300;          // ChainElement x 16
        static const uint32_t kFrames = 0x1400;          // Frame x kFrameCount
        static const int kFrameCount = 16;
        static const uint32_t kStrtokBuffer = 0x1600;    // 100h
        static const uint32_t kOutdatedPadBuffers = 0x1700;   // 2 x 22h
        static const uint32_t kStrings = 0x1780;         // device names
        static const uint32_t kExecHeaderBuffer = 0x1800;     // the boot executable's header, 3Ch
        static const uint32_t kCardDirectoryBuffer = 0x1880;  // where _bu_init reads sector 0, 80h
    public:
        static const uint32_t kShellVars = 0x2000;   // the shell's state, a page of kernel RAM
    private:
        friend class HleShell;
        // The shell (hle_shell.cpp): what the machine runs with nothing to boot.
        void ShellStart();
        void ShellFrame();

        // ---- Helpers over guest memory -----------------------------------------------------------
        CpuContext& cpu();
        uint8_t* RamPointer(uint32_t address);   // RAM or the scratchpad; null for anything else
        uint8_t Read8(uint32_t address);
        uint16_t Read16(uint32_t address);
        uint32_t Read32(uint32_t address);
        void Write8(uint32_t address, uint8_t value);
        void Write16(uint32_t address, uint16_t value);
        void Write32(uint32_t address, uint32_t value);
        void ReadBlock(uint32_t address, void* out, uint32_t bytes);
        void WriteBlock(uint32_t address, const void* data, uint32_t bytes);
        void FillBlock(uint32_t address, uint8_t value, uint32_t bytes);
        std::string ReadString(uint32_t address, uint32_t limit = 256);
        void WriteString(uint32_t address, const std::string& text);
        uint32_t Arg(int index);   // a0-a3, then the caller's stack from sp+10h
        KernelVars& vars();
        template <typename T> T* At(uint32_t address) {
            return reinterpret_cast<T*>(RamPointer(address));
        }

        // The hardware, through the same paths as a store from the CPU would take.
        uint32_t ReadIo32(uint32_t address);
        uint16_t ReadIo16(uint32_t address);
        void WriteIo32(uint32_t address, uint32_t value);
        void WriteIo16(uint32_t address, uint16_t value);
        void WriteIo8(uint32_t address, uint8_t value);

        // ---- Returning ---------------------------------------------------------------------------
        void Return(uint32_t v0);
        void ReturnVoid();
        void Message(const std::string& text);   // into messages(), once
        void Console(const std::string& text);   // onto the BIOS console, as the kernel's printf would

        static uint32_t TrapAddress(uint32_t function);   // KSEG1 entry point for a function number
        void Dispatch(uint32_t function);

        // Krom2Offset: the glyph of a Shift-JIS code in its character set, and the set's base.
        static uint32_t KromOffset(uint32_t code, uint32_t* base);

        // What `function` costs, from its arguments before it runs (hle_bios_timing.cpp).
        uint32_t CostOf(uint32_t function);
        uint32_t MeasuredCost(uint32_t function);   // as measured, dispatcher and all
        uint32_t StringLength(uint32_t address, uint32_t limit = 4096);
        // Extra cycles a function adds while it runs - a CD-ROM read's sectors, a card's - on top of
        // CostOf's, known only once it has worked out what it read.
        void AddCost(uint32_t cycles) { cost_ += cycles; }

        // ---- Boot (hle_bios.cpp) -----------------------------------------------------------------
        void Reset();
        void InitHardware(bool pal);
        void BuildKernel(uint32_t tcbs, uint32_t evcbs, uint32_t stacktop);
        void BuildVectorsAndHandler();
        void AllocateControlBlocks(uint32_t tcbs, uint32_t evcbs);
        void InstallDefaultHandlers();
        void InstallDevices();
        bool BootDisc();
        bool BootExecutable(const std::vector<uint8_t>& image, uint32_t stack);
        bool LoadExecutable(const std::vector<uint8_t>& image, uint32_t header_buffer);
        void StartExecutable(uint32_t header_buffer, uint32_t a0, uint32_t a1, uint32_t ra);

        // ---- Exceptions, events, threads ---------------------------------------------------------
        uint32_t CurrentTcb();
        void ReturnFromException();
        void EnqueueHandler(uint32_t priority, uint32_t element);
        uint32_t DequeueHandler(uint32_t priority, uint32_t element);
        uint32_t ChainElementFor(uint32_t first, uint32_t second);   // a default handler's element
        uint32_t OpenEvent(uint32_t cls, uint32_t spec, uint32_t mode, uint32_t func);
        EvCb* Event(uint32_t handle);
        uint32_t EventCount();

        // What a host function still has to do once a DeliverEvent - which may call guest code - has
        // finished. kThenReturn: pc = a, v0 = b.
        enum ThenKind : uint32_t {
            kThenReturn, kThenReturnFromException, kThenDefInt, kThenSyscallUnresolved,
            kThenCardEvents, kThenFileEvents,
        };
        struct Then {
            uint32_t kind = kThenReturn;
            uint32_t a = 0, b = 0, c = 0;
        };
        enum FrameKind : uint32_t { kFrameDeliver = 1, kFrameQsort, kFrameLsearch, kFrameBsearch };
        // qsort, lsearch and bsearch: a compare through the game's callback at each step, the loop's
        // state carried in the frame. `result` is what the last compare returned; `first` is the call
        // that starts the loop, which has none.
        void SearchStep(Frame frame, bool first, int32_t result);
        void DeliverEvent(uint32_t cls, uint32_t spec, const Then& then) {
            DeliverEventFrom(cls, spec, 0, then);
        }
        void DeliverEventFrom(uint32_t cls, uint32_t spec, uint32_t first, const Then& then);
        void UndeliverEvent(uint32_t cls, uint32_t spec);
        void CallGuest(uint32_t function, const Frame& frame);
        void Resume();
        void Continue(const Then& then);
        void DefIntFrom(uint32_t irq, uint32_t return_to);
        void SystemError(const char* what);

        // ---- The rest of the kernel ---------------------------------------------------------------
        // Each returns through Return(), or sets the pc itself when it goes elsewhere.
        void FunctionA(uint32_t number);    // hle_bios_libc.cpp and hle_bios_io.cpp, by number
        void FunctionB(uint32_t number);
        void FunctionC(uint32_t number);
        bool LibcFunction(uint32_t number);  // A(0Ah-3Fh) less the files: true if it was one
        bool IoFunction(uint32_t number);    // B(n), or A(n) as 100h + n: true if it was one
        void Printf(uint32_t format, int first_argument);
        uint32_t HeapAlloc(uint32_t start, uint32_t end, uint32_t size);
        void HeapInit(uint32_t start, uint32_t size);
        uint32_t KernelAlloc(uint32_t size);
        int32_t StrToL(uint32_t source, uint32_t end_out, int base, bool is_signed, bool atoi);

        // Devices (hle_bios_io.cpp).
        struct Path {
            std::string device;   // "cdrom", "bu", "tty"
            uint32_t port = 0;    // bu00: 0, bu10: 10h
            std::string name;     // everything after the colon
            bool valid = false;
        };
        Path ParsePath(const std::string& text);
        Fcb* FileBlock(uint32_t fd);
        int32_t FreeFileHandle();
        uint32_t FileOpen(uint32_t name, uint32_t mode);
        uint32_t FileRead(uint32_t fd, uint32_t dst, uint32_t length);
        uint32_t FileWrite(uint32_t fd, uint32_t src, uint32_t length);
        uint32_t FileClose(uint32_t fd);
        uint32_t FileSeek(uint32_t fd, int32_t offset, uint32_t whence);
        uint32_t FirstFile(uint32_t name, uint32_t direntry);
        uint32_t NextFile(uint32_t direntry);
        uint32_t FileDelete(uint32_t name, bool undelete);
        uint32_t FileRename(uint32_t from, uint32_t to);
        uint32_t FormatDevice(uint32_t name);
        uint32_t ChangeDirectory(uint32_t name);
        // Returns `result` to the caller - after delivering the events a memory card file operation
        // left in file_event_fd_ and file_event_spec_, when it left any.
        void FinishFile(uint32_t result);
        bool OpenDisc();
        bool CdFind(const std::string& path, uint32_t* lba, uint32_t* size);
        bool CdRead(uint32_t lba, uint32_t offset, uint32_t dst, uint32_t bytes);
        bool LoadFileImage(const std::string& path, std::vector<uint8_t>* image);
        void CdInit();
        void CdRemove();
        void CdIoVerifier();

        // Memory cards, files and sectors.
        class MC* Card(uint32_t port);
        uint8_t* CardImage(uint32_t port);
        int CardFindFile(uint32_t port, const std::string& name, bool deleted);
        bool CardCreate(uint32_t port, const std::string& name, uint32_t blocks, int* first);
        void CardWriteFrame(uint32_t port, int frame);   // after a directory frame changed
        bool CardFileOffset(uint32_t port, int first, uint32_t position, uint32_t* offset);
        void CardSectorOperation(uint32_t function, uint32_t port, uint32_t sector, uint32_t buffer);
        void CardVblank(uint32_t return_to);

        // Pads.
        void PadVblank();
        void ReadPad(int port);
        uint32_t OutdatedPadButtons();

        System* system_ = nullptr;
        std::vector<std::string> messages_;
        std::vector<std::string> new_messages_;   // not yet taken by TakeNewMessages
        FILE* trace_ = nullptr;
        // Only ever set and consumed inside one trap, so not machine state: a memory card file
        // operation's events, for FinishFile.
        uint32_t file_event_fd_ = 0xFFFFFFFF;
        uint32_t file_event_spec_ = 0;
        uint32_t cost_ = 0;   // this trap's cycles, likewise only alive inside one
    };

}  // namespace hle
}  // namespace psx
}  // namespace emulation
