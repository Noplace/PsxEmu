/*****************************************************************************************************************
* Copyright (c) 2012 Khalid Ali Al-Kooheji                                                                       *
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

// What a compiled block is handed, and what it can call back into.
//
// One pointer arrives in the block and everything else hangs off it at a fixed
// offset, so the emitted code is a series of loads from one base register
// rather than four arguments to juggle.
//
// The callbacks are why this file exists. A compiled load has to go through
// the same path the interpreter's does - the region decode, the timing, and
// later the check that a store is not landing on compiled code - and that path
// lives in Cpu. Rather than have the recompiler include and depend on the CPU,
// the host fills these in with thunks; the tests fill them in with a fake
// memory. Nothing in rec/ knows psx/ exists, which is what keeps the
// recompiler switchable rather than woven in.
//
// With one exception, made because it is most of the traffic: a load from main
// RAM, while the host says that is nothing but a read and a fixed stall, reads
// `ram` itself and adds the stall to `extra_cycles` (Recompiler::set_ram).

#include <cstdint>

namespace emulation {
namespace rec {

// One rule these have to obey, which step 6 created and nothing enforces: a
// callback must not modify the guest register file. Since a block may be
// keeping some of those registers in host registers for its duration, a change
// made here would be overwritten when the block writes them back on its way
// out. Reading them is fine. Cpu::Load and Cpu::Store do not write them.
//
// `context` is whatever the host wants back - a Cpu*, in the end. `pc` is the
// guest address of the instruction making the access, which the host needs for
// two reasons: an exception raised by the access has to point at it, and the
// callback has to be able to say "that faulted" by setting BlockState::fault.
typedef uint32_t (*Load32Fn)(void* context, uint32_t address, uint32_t pc);
typedef uint32_t (*Load16Fn)(void* context, uint32_t address, uint32_t pc);
typedef uint32_t (*Load8Fn)(void* context, uint32_t address, uint32_t pc);
typedef void (*Store32Fn)(void* context, uint32_t address, uint32_t value, uint32_t pc);
typedef void (*Store16Fn)(void* context, uint32_t address, uint32_t value, uint32_t pc);
typedef void (*Store8Fn)(void* context, uint32_t address, uint32_t value, uint32_t pc);
// A register copied to another - `move to, from`, as addu/or with r0 or addiu/ori with 0 - for a
// host that keeps something beside each register which has to travel with it (the emulator's
// PGXP). Emitted only while the host asks (Recompiler::set_track_moves), and like the memory
// callbacks it must not touch the guest register file.
typedef void (*MoveFn)(void* context, uint32_t to, uint32_t from);
// The instructions that are more than arithmetic - a trapping add that overflowed, the multiply and
// divide unit and its two result registers. One entry rather than one each, because everything the
// compiled code reaches by a one-byte offset into BlockState has to fit in the first 128 bytes.
// `operation` is the instruction's funct field, or kSpecialOverflow, with the instruction's index in
// its block shifted up by 8 (what the host needs to know how far into the chain this is); `a` and `b`
// are the operands, or for an overflow, `a` is the instruction's pc and `b` is 1 when it is in a branch's
// delay slot. A read returns its value.
typedef uint32_t (*SpecialFn)(void* context, uint32_t operation, uint32_t a, uint32_t b);
const uint32_t kSpecialOverflow = 0x3F;

// The coprocessor 2 operations, in the same `operation` field and past every funct. For the host's
// `gte` (HostInterface): `a` is the value to write, and `b` the GTE register, or for a command the
// instruction word.
const uint32_t kSpecialGteCommand = 0x40;     // a cop2 command: b is the instruction
const uint32_t kSpecialGteMfc2 = 0x41;        // data register b -> the value returned
const uint32_t kSpecialGteCfc2 = 0x42;        // control register b -> the value returned
const uint32_t kSpecialGteMtc2 = 0x43;        // data register b <- a
const uint32_t kSpecialGteCtc2 = 0x44;        // control register b <- a
const uint32_t kSpecialGteLoad = 0x45;        // lwc2: data register b <- a, the word just loaded
const uint32_t kSpecialGteStore = 0x46;       // swc2: data register b -> the value returned

// The GTE compiled in place (HostInterface::gte_layout): what the host says about its coprocessor
// so that compiled code can do without BlockState::special for most of it. A register move becomes
// a load or store at a fixed offset into the host's GTE object, the busy clock is kept by compiled
// code against the host's own clock, and a command is a direct call to the function for that one
// command - picked when the block is compiled, from the instruction word - rather than a trip
// through the special thunk, the host's callback and a dispatch on the opcode at run time.
//
// How compiled code reaches one register. A register whose read or write is more than a load or a
// store - SXYP's push, IRGB's expansion, LZCS's count, FLAG's error bit - says kCall, and that
// instruction goes to `special` as before.
struct GteRegisterAccess {
  enum Kind : uint8_t {
    kCall = 0,       // through BlockState::special
    kWord,           // 32 bits, read or written whole
    kSigned16,       // read: a halfword, sign-extended
    kUnsigned16,     // read: a halfword, zero-extended
    kHalf,           // write: the low halfword
    kIgnored,        // write: nothing happens (a read-only register)
  };
  uint8_t read = kCall;
  uint8_t write = kCall;
  int32_t offset = 0;      // from the GTE object
  // A word the host clears beside a write - where a vertex was loaded from, which a write that
  // carries no address (every compiled one) says is not known. -1 for none.
  int32_t cleared = -1;
};

// One command, run directly: the host's GTE object and the instruction word.
typedef void (*GteCommandFn)(void* gte, uint32_t word);

// Where the GTE keeps its state, for the commands emitted as x64 (GteCompiler, gte_compiler.h):
// each field's offset from the GTE object, in the hardware's own shapes - V0-V2 as three rows of
// three int16, the three matrices as 3x3x3 int16 and the three vectors beside them as 3x3 int32,
// the FIFOs oldest first. `divide_table` is the perspective divide's 257-entry reciprocal seed;
// null, and no command is emitted.
struct GteFields {
  int32_t v = 0, rgbc = 0, otz = 0, ir = 0, sxy = 0, sz = 0, rgb_fifo = 0, mac = 0;
  int32_t matrix = 0, translation = 0, ofx = 0, ofy = 0, h = 0, dqa = 0, dqb = 0;
  int32_t zsf3 = 0, zsf4 = 0, flag = 0;
  int32_t lm = 0, sf = 0;    // a bool and an int32: the command's limit and shift, as Execute leaves them
  int32_t pgxp = 0;          // a bool: set, RTPS, RTPT and NCLIP are run by the host's own function
  int32_t commands = 0, unknown_commands = 0, executed = 0;   // the counts: u64, u64, u32[64]
  const uint8_t* divide_table = nullptr;
};

struct GteLayout {
  GteRegisterAccess registers[64];   // data registers 0-31, then control registers 0-31
  GteCommandFn commands[64];         // by the command's low six bits; null calls out
  uint8_t cycles[64];                // how long each keeps the unit busy (Gte::Execute's return)
  GteFields fields;                  // for the commands as x64, when the host allows it
};

// The layout the emitted code addresses by offset. Field order is load-bearing
// in the sense that the compiler hard-codes the offsets - keep the two in step,
// and BlockCompiler asserts the ones it uses against offsetof.
struct BlockState {
  uint32_t* regs = nullptr;     // the guest's 32 general-purpose registers
  void* context = nullptr;      // handed back to every callback

  Load32Fn load32 = nullptr;
  Load16Fn load16 = nullptr;
  Load8Fn load8 = nullptr;
  Store32Fn store32 = nullptr;
  Store16Fn store16 = nullptr;
  Store8Fn store8 = nullptr;

  // Where execution goes when the block ends. A block that falls out of its
  // last instruction sets this to the address after it; a branch or jump sets
  // it to wherever it goes - computed from the registers *as they were at the
  // branch*, before the delay slot ran, which is what the hardware does.
  uint32_t next_pc = 0;

  // Set by a load or store callback that raised a guest exception - an
  // unaligned address or an unmapped one. Compiled code checks it after every
  // memory access and leaves the block immediately when it is set, without
  // running the rest of the block.
  //
  // This is what makes compiled memory access safe to use at all. Cpu::Load
  // raises an address error on a misaligned access, and a block that carried on
  // through one would execute instructions the exception was supposed to skip -
  // with the CPU already vectored somewhere else. Where execution resumes after
  // a fault is the host's business, not this struct's: the exception has
  // already set the CPU's own pc.
  uint32_t fault = 0;
  // fault = 2 is not a fault: the block left on purpose before an instruction it does not do, `next_pc`
  // says where, and the interpreter is to run that one (lwl, lwr, swl and swr for anything but RAM).
  // The low byte of `fault`; the rest says which register had a load in flight that the interpreter's
  // lwl or lwr is to merge into - its number, shifted up by kBailRegShift, and its value in `bail_value`.
  static const uint32_t kBail = 2;
  static const uint32_t kBailRegShift = 8;

  // How many more guest instructions compiled code may run before handing
  // control back. Every block subtracts its own length on the way out and
  // returns to the dispatcher when this reaches zero.
  //
  // This is what makes block linking safe. Once blocks jump straight to each
  // other, a guest loop entirely inside compiled code would never come back,
  // and the emulator would have no chance to take an interrupt, run a timer or
  // draw a frame. The budget is the leash: the host sets it to however long it
  // can afford not to hear from the CPU - in the emulator, the cycles until the
  // next scheduled event.
  //
  // It doubles as the accounting: what the host set, minus what is left, is
  // how many instructions the chain executed.
  int32_t budget = 0;

  MoveFn move = nullptr;

  // Main RAM, for the loads compiled to read it directly - or nullptr, and every
  // load calls out. Each such load checks it as it runs, so the host can switch
  // between the two before any step without a block being compiled again: the
  // emulator does, whenever something - PGXP, a debugger watchpoint, the write
  // queue, an isolated cache - makes a RAM load more than a read.
  uint8_t* ram = nullptr;

  // Cycles owed beyond one an instruction: what each direct RAM read would have
  // stalled for in the callback (HostInterface::ram_read_cycles), added up as the
  // chain runs and charged with its instructions. The budget is left alone, so
  // it still bounds a chain in instructions.
  uint32_t extra_cycles = 0;

  // The guest address of the block that is running, written at its start when it has a memory
  // access. A callout that reaches past RAM is handed only the instruction's address, and the engine
  // needs to know how far into its block that is - and so how far into the chain - to bring the
  // machine's clock up to the instruction before the hardware is touched (HostInterface::sync).
  uint32_t block_pc = 0;

  // Main RAM for the stores compiled to write it directly, or nullptr and every store
  // calls out - decided per step like `ram`, but separately, since a store has more to
  // answer than a load (a watched address, and the compiled code it may land on).
  uint8_t* ram_store = nullptr;

  // BlockCache's bitmap of the 4 KB pages that blocks were compiled from, which never
  // moves. A direct store tests its page's bit as it runs and calls out when it is set,
  // so the store that throws compiled code away is always the callback's.
  const uint64_t* code_pages = nullptr;

  // The multiply and divide unit, and the trap on a signed overflow: see SpecialFn.
  SpecialFn special = nullptr;

  // Past the 128 bytes a one-byte offset reaches (the compiled code addresses it by four): the value of a
  // load in flight when a block bails, see `fault`.
  uint32_t bail_value = 0;

  // The console's scratchpad for the accesses compiled to reach it directly, beside `ram` and with
  // the same meaning for null: the host wants every access to go through its callbacks. Also past
  // the 128 bytes of one-byte offsets.
  uint8_t* scratchpad = nullptr;

  // The GTE compiled in place (GteLayout), all past the 128 bytes too. `gte` is the host's GTE
  // object, which the layout's offsets are from and which a command is handed. `clock` is the
  // host's cycle count, and `gte_busy_until` the cycle on it the unit is busy until - the host's
  // own variables, so compiled code and the host's callbacks hold one clock between them, whatever
  // a callout ticks in between.
  uint8_t* gte = nullptr;
  const uint64_t* clock = nullptr;
  uint64_t* gte_busy_until = nullptr;
  // Where in time an instruction is, on `clock`: *clock + clock_offset + extra_cycles - budget +
  // the instruction's index in its block. The engine keeps it (the budget it gave the chain, less
  // what it has synced the host for), and it is the same `elapsed` a SpecialFn call is given.
  uint32_t clock_offset = 0;
  // What `extra_cycles` holds that the multiply/divide unit and the GTE asked for - the rest is
  // direct reads' stalls - for the engine's statistics.
  uint32_t special_cycles = 0;
};

}  // namespace rec
}  // namespace emulation
