// What each HLE kernel function costs, in CPU cycles: the time the SCPH-1001 kernel's took.
//
// Measured with `boot_runner bios/SCPH1001.BIN --disc <game> --frames 3000 --time-calls <csv>` on
// ten games (Air Combat, Metal Gear Solid, Breath of Fire III, Ridge Racer, Wild Arms, Legend of
// Mana, Final Fantasy VII, Tekken 3, Crash Bandicoot 2, Castlevania: SotN, Vandal Hearts) on
// 2026-10-09 - 19 million calls, from the call to the return to its caller. The figure taken is the
// median, or the least where the median is swollen by interrupts taken during the call (a call
// that runs with them enabled is charged for every handler that ran inside it, and here those
// handlers run and are charged separately). Functions whose time grows with their arguments get a
// line through the fastest call at each size; the string functions, whose lengths the
// measurement did not record, a per-character figure fitted to their medians.
//
// What is not measured - a function no game called in those runs - costs kDefault, about what the
// kernel's small functions take.

#include "psx/psx.h"
#include "psx/hle_bios.h"

namespace emulation {
namespace psx {

namespace {

const uint32_t kDefault = 100;

// A table entry: base cycles, and per-unit cycles of the argument `unit_arg` (-1: none).
struct Cost {
  uint16_t number;
  uint32_t base;
  int unit_arg;
  float per_unit;
};

const Cost kCostsA[] = {
  { 0x0A, 40, -1, 0 },          // todigit
  { 0x0E, 25, -1, 0 },          // abs
  { 0x0F, 25, -1, 0 },          // labs
  { 0x13, 30, -1, 0 },          // SaveState
  { 0x14, 30, -1, 0 },          // RestoreState
  { 0x25, 33, -1, 0 },          // toupper
  { 0x26, 33, -1, 0 },          // tolower
  { 0x27, 17, 2, 10.71f },      // bcopy, as memcpy
  { 0x28, 24, 1, 4.00f },       // bzero
  { 0x29, 40, 2, 12.0f },       // bcmp
  { 0x2A, 17, 2, 10.71f },      // memcpy
  { 0x2B, 40, 2, 4.29f },       // memset
  { 0x2C, 17, 2, 10.71f },      // memmove
  { 0x2D, 40, 2, 12.0f },       // memcmp
  { 0x2E, 30, 2, 8.0f },        // memchr
  { 0x2F, 54, -1, 0 },          // rand
  { 0x30, 25, -1, 0 },          // srand
  { 0x33, 188, -1, 0 },         // malloc
  { 0x34, 24, -1, 0 },          // free
  { 0x37, 200, -1, 0 },         // calloc (its bzero is added below)
  { 0x38, 300, -1, 0 },         // realloc
  { 0x39, 29, -1, 0 },          // InitHeap
  { 0x44, 1460, -1, 0 },        // FlushCache
  { 0x45, 300, -1, 0 },         // init_a0_b0_c0_vectors
  { 0x48, 40, -1, 0 },          // SendGP1Command
  { 0x49, 112, -1, 0 },         // GPU_cw
  { 0x4A, 112, 1, 30.0f },      // GPU_cwp
  { 0x4B, 200, -1, 0 },         // send_gpu_linked_list
  { 0x4D, 30, -1, 0 },          // GetGPUStatus
  { 0x4E, 60, -1, 0 },          // gpu_sync
  { 0x54, 2789794, -1, 0 },     // CdInit
  { 0x56, 770, -1, 0 },         // CdRemove
  { 0x71, 2789794, -1, 0 },     // CdInit
  { 0x72, 770, -1, 0 },         // CdRemove
  { 0x78, 1801, -1, 0 },        // CdAsyncSeekL
  { 0x7C, 2320, -1, 0 },        // CdAsyncGetStatus
  { 0x7E, 2406, -1, 0 },        // CdAsyncReadSector
  { 0x81, 2300, -1, 0 },        // CdAsyncSetMode
  { 0x95, 143112, -1, 0 },      // CdInitSubFunc
  { 0x96, 2573, -1, 0 },        // AddCDROMDevice
  { 0x97, 2585, -1, 0 },        // AddMemCardDevice
  { 0x98, 2561, -1, 0 },        // AddDuartTtyDevice
  { 0x99, 2561, -1, 0 },        // AddDummyTtyDevice
  { 0x9C, 3000, -1, 0 },        // SetConf
  { 0x9D, 60, -1, 0 },          // GetConf
  { 0x9F, 15972, -1, 0 },       // SetMemSize
  { 0xA2, 123, -1, 0 },         // EnqueueCdIntr
  { 0xA3, 133, -1, 0 },         // DequeueCdIntr
  { 0xA6, 2400, -1, 0 },        // CdGetStatus
  { 0xA7, 89, -1, 0 },          // bu_callback_okay
  { 0xA8, 89, -1, 0 },          // bu_callback_err_write
  { 0xA9, 89, -1, 0 },          // bu_callback_err_busy
  { 0xAA, 89, -1, 0 },          // bu_callback_err_eject
  { 0xAB, 104, -1, 0 },         // _card_info
  { 0xAC, 126, -1, 0 },         // _card_async_load_directory
  { 0xAD, 25, -1, 0 },          // set_card_auto_format
  { 0xAF, 120, -1, 0 },         // card_write_test
  { 0xB4, 50, -1, 0 },          // GetSystemInfo
};

const Cost kCostsB[] = {
  { 0x00, 301, -1, 0 },         // alloc_kernel_memory
  { 0x01, 24, -1, 0 },          // free_kernel_memory
  { 0x02, 80, -1, 0 },          // init_timer
  { 0x03, 30, -1, 0 },          // get_timer
  { 0x04, 40, -1, 0 },          // enable_timer_irq
  { 0x05, 40, -1, 0 },          // disable_timer_irq
  { 0x06, 30, -1, 0 },          // restart_timer
  { 0x07, 343, -1, 0 },         // DeliverEvent
  { 0x08, 290, -1, 0 },         // OpenEvent
  { 0x09, 30, -1, 0 },          // CloseEvent
  { 0x0A, 38, -1, 0 },          // WaitEvent: per look, as TestEvent
  { 0x0B, 38, -1, 0 },          // TestEvent
  { 0x0C, 39, -1, 0 },          // EnableEvent
  { 0x0D, 37, -1, 0 },          // DisableEvent
  { 0x0E, 263, -1, 0 },         // OpenThread
  { 0x0F, 31, -1, 0 },          // CloseThread
  { 0x10, 100, -1, 0 },         // ChangeThread: the syscall it makes runs for real
  { 0x12, 15337, -1, 0 },       // InitPad
  { 0x13, 1421, -1, 0 },        // StartPad
  { 0x14, 869, -1, 0 },         // StopPad
  { 0x15, 33732, -1, 0 },       // OutdatedPadInitAndStart
  { 0x16, 85, -1, 0 },          // OutdatedPadGetButtons
  { 0x17, 80, -1, 0 },          // ReturnFromException
  { 0x18, 21, -1, 0 },          // SetDefaultExitFromException
  { 0x19, 19, -1, 0 },          // SetCustomExitFromException
  { 0x20, 298, -1, 0 },         // UnDeliverEvent
  { 0x32, 26740, -1, 0 },       // FileOpen (a CD-ROM directory or a card sector is added)
  { 0x33, 60, -1, 0 },          // FileSeek
  { 0x34, 200, -1, 0 },         // FileRead (the sectors are added as they are read)
  { 0x35, 355, -1, 0 },         // FileWrite
  { 0x36, 88, -1, 0 },          // FileClose
  { 0x3D, 404, -1, 0 },         // std_out_putchar
  { 0x3F, 200, -1, 0 },         // std_out_puts (its characters below)
  { 0x40, 26740, -1, 0 },       // chdir
  { 0x42, 26740, -1, 0 },       // firstfile
  { 0x43, 2000, -1, 0 },        // nextfile
  { 0x47, 2530, -1, 0 },        // AddDevice
  { 0x4A, 1590, -1, 0 },        // InitCard
  { 0x4B, 1450, -1, 0 },        // StartCard
  { 0x4C, 884, -1, 0 },         // StopCard
  { 0x4D, 58, -1, 0 },          // _card_info_subfunc
  { 0x4E, 62, -1, 0 },          // write_card_sector
  { 0x4F, 62, -1, 0 },          // read_card_sector
  { 0x50, 20, -1, 0 },          // allow_new_card
  { 0x54, 25, -1, 0 },          // GetLastError
  { 0x55, 30, -1, 0 },          // GetLastFileError
  { 0x56, 19, -1, 0 },          // GetC0Table
  { 0x57, 19, -1, 0 },          // GetB0Table
  { 0x58, 32, -1, 0 },          // get_bu_callback_port
  { 0x5B, 26, -1, 0 },          // ChangeClearPad
  { 0x5C, 30, -1, 0 },          // get_card_status
  { 0x5D, 30, -1, 0 },          // wait_card_status: per look
};

const Cost kCostsC[] = {
  { 0x00, 260, -1, 0 },         // EnqueueTimerAndVblankIrqs
  { 0x01, 46, -1, 0 },          // EnqueueSyscallHandler
  { 0x02, 33, -1, 0 },          // SysEnqIntRP
  { 0x03, 46, -1, 0 },          // SysDeqIntRP
  { 0x04, 60, -1, 0 },          // get_free_EvCB_slot
  { 0x05, 60, -1, 0 },          // get_free_TCB_slot
  { 0x07, 200, -1, 0 },         // InstallExceptionHandlers
  { 0x08, 30, -1, 0 },          // SysInitMemory
  { 0x0A, 28, -1, 0 },          // ChangeClearRCnt
  { 0x0C, 69, -1, 0 },          // InitDefInt
  { 0x0D, 30, -1, 0 },          // SetIrqAutoAck
  { 0x12, 12603, -1, 0 },       // InstallDevices
  { 0x1C, 154, -1, 0 },         // AdjustA0Table
};

uint32_t Lookup(const Cost* table, size_t count, uint32_t number, const uint32_t* args) {
  for (size_t i = 0; i < count; ++i) {
    if (table[i].number != number)
      continue;
    uint32_t cycles = table[i].base;
    if (table[i].unit_arg >= 0) {
      uint32_t units = args[table[i].unit_arg];
      if (units > 0x200000)
        units = 0;   // a refused length: the function gives up at once
      cycles += static_cast<uint32_t>(table[i].per_unit * units);
    }
    return cycles;
  }
  return kDefault;
}

}  // namespace

uint32_t HleBios::StringLength(uint32_t address, uint32_t limit) {
  if (address == 0)
    return 0;
  uint32_t length = 0;
  while (length < limit && Read8(address + length) != 0)
    ++length;
  return length;
}

// The measured times run from the jump to A0h/B0h/C0h to the return, so they include the vector
// and the dispatcher - about 15 cycles of MIPS that the HLE BIOS runs too, before the trap.
const uint32_t kDispatch = 15;

uint32_t HleBios::CostOf(uint32_t function) {
  const uint32_t cycles = MeasuredCost(function);
  return cycles > kDispatch ? cycles - kDispatch : 1;
}

uint32_t HleBios::MeasuredCost(uint32_t function) {
  const CpuContext& c = cpu();
  const uint32_t args[4] = { c.gp.a0, c.gp.a1, c.gp.a2, c.gp.a3 };
  if (function < kTableB) {
    const uint32_t n = function - kTableA;
    switch (n) {
      // The string functions, by length: strlen's median of 588 is about 19 characters at 30.
      case 0x1B: return 27 + 30 * StringLength(args[0]);                               // strlen
      case 0x17: case 0x18: {                                                           // strcmp
        // Up to the first difference: 37 when the first characters differ, the least measured.
        uint32_t same = 0;
        if (args[0] != 0 && args[1] != 0)
          while (same < 256 && Read8(args[0] + same) == Read8(args[1] + same) &&
                 Read8(args[0] + same) != 0)
            ++same;
        return (n == 0x17 ? 37 : 51) + 5 * same;
      }
      case 0x19: case 0x1A: return 60 + 6 * StringLength(args[1], 256);                // strcpy
      case 0x15: case 0x16:                                                             // strcat
        return 70 + 4 * StringLength(args[0], 256) + 8 * StringLength(args[1], 256);
      case 0x1C: case 0x1D: case 0x1E: case 0x1F: case 0x20: case 0x21: case 0x22:
      case 0x23: case 0x24:
        return 40 + 25 * StringLength(args[0], 256);
      case 0x0C: case 0x0D: case 0x10: case 0x11: case 0x12:                         // strtol...
        return 80 + 40 * StringLength(args[0], 32);
      // printf: about 1,500 to start and a putchar's 404 for every character it writes - the
      // format's length stands in for what it prints.
      case 0x3F: return 1500 + 404 * StringLength(args[0], 256);
      case 0x3C: return 404;                                                            // putchar
      case 0x3E: return 200 + 404 * StringLength(args[0], 256);                       // puts
      default:
        if (n <= 0x09 || (n >= 0x3B && n <= 0x3E))
          return MeasuredCost(kTableB + (n <= 0x09 ? 0x32 + n : 0x3C + (n - 0x3B)));
        return Lookup(kCostsA, sizeof(kCostsA) / sizeof(kCostsA[0]), n, args);
    }
  }
  if (function < kTableC) {
    const uint32_t n = function - kTableB;
    if (n == 0x3F)
      return 200 + 404 * StringLength(args[0], 256);
    return Lookup(kCostsB, sizeof(kCostsB) / sizeof(kCostsB[0]), n, args);
  }
  return Lookup(kCostsC, sizeof(kCostsC) / sizeof(kCostsC[0]), function - kTableC, args);
}

}  // namespace psx
}  // namespace emulation
