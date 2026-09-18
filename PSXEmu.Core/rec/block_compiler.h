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

// Steps 3 and 4 of Docs/Recompiler-Plan.md: the arithmetic, then memory and
// branches.
//
// Step 3 compiled what cannot fault, cannot branch and touches nothing but the
// guest's registers. Step 4 adds the two things that make a block worth
// compiling at all - it reaches memory, and it knows where it goes next - and
// both arrive the careful way rather than the fast way:
//
//   - **Loads and stores call out.** The address decode, the timing and, later,
//     the check that a store is not landing on compiled code all live in the
//     interpreter's Cpu::Load and Cpu::Store. Duplicating them here would mean
//     two copies of the memory map to keep in step, which is how a recompiler
//     starts disagreeing with its own interpreter. The call goes through a
//     function pointer in BlockState (see rec/runtime.h), so this file still
//     does not know psx/ exists.
//   - **Branches do not jump.** Every guest branch is "one of two addresses",
//     which is a compare and a conditional move into `next_pc`. No host branch
//     is emitted for the guest's own control flow.
//
// The emitted function is
//
//     void block(BlockState* state);
//
// Steps 6 and 7 added two things on top of that, both switchable and both
// measured (see Docs/Recompiler-Plan.md and tools/rec_bench.cpp):
//
//   - **Register allocation.** Up to four of the block's busiest guest
//     registers live in host registers for its duration. Only for blocks long
//     enough to amortise filling and spilling them, which is measured rather
//     than assumed.
//   - **Block linking.** A block's tail can jump straight to the next block
//     instead of returning to the dispatcher. That works because the tail
//     restores the frame first: at the jump the stack is exactly as it was on
//     entry, so the next block's prologue sees what it expects and whichever
//     block finally returns goes back to the dispatcher that called the first.
//     The jump's displacement is written by the engine, which is also what
//     takes it apart again when the block it points at is thrown away.
//
// Four rules that are easy to get wrong and are therefore stated here:
//
//   - **r0 is always zero.** A write to it is discarded rather than performed.
//   - **Trapping forms are not compiled.** `add`, `addi` and `sub` raise an
//     overflow exception on the R3000A; `addu`, `addiu` and `subu` do not.
//     Compiling a trapping one as its unsigned twin would silently drop an
//     exception a game may rely on, so they end the compiled run instead.
//   - **A load's value arrives one instruction late.** The R3000A's load delay
//     slot is real and this core models it (Cpu::AdvanceLoadDelay): the
//     instruction after a load still sees the register's old value, and if that
//     instruction writes the same register, the load is cancelled and the
//     instruction wins. Compiled code resolves that statically - see
//     FlushPending - because the compiler can see both instructions at once and
//     the hardware cannot.
//   - **An instruction whose effect lands on the next one is compiled only if
//     the next one is compiled too.** That covers loads, whose value lands
//     late, and branches, whose delay slot has to run. Otherwise the compiled
//     code would stop halfway through an effect the interpreter has no way to
//     be told about.

#include "rec/emitter.h"
#include "rec/block_decoder.h"
#include "rec/runtime.h"
#include "rec/x86_extras.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace emulation {
namespace rec {

struct CompiledBlock {
  void* code = nullptr;
  uint32_t host_bytes = 0;

  // How many of the decoded block's instructions the emitted code actually
  // performs. The interpreter resumes at instruction `compiled` of the block -
  // never before it, never after it - so this number is the contract between
  // the two, and it is what the differential harness checks first.
  uint32_t compiled = 0;
  bool complete = false;   // the whole decoded block was compiled

  // Whether `state->next_pc` was written by a compiled branch or jump. When it
  // was not, next_pc is simply the address after the last compiled instruction,
  // which is where the interpreter picks up.
  bool ends_with_branch = false;

  // How many guest registers this block kept in host registers rather than in
  // memory. Zero when the allocator is off, and zero for a block too short or
  // too scattered to be worth it.
  int registers_allocated = 0;

  // Where this block can jump straight to the next one instead of returning to
  // the dispatcher, once that next one exists.
  //
  // A slot is a `jmp rel32` whose displacement the engine rewrites. Until it
  // does, the displacement points at the block's own `ret`, so an unlinked slot
  // costs one predictable jump and nothing else. `target` is the guest address
  // the slot goes to, which is how the engine knows which block to point it at
  // and which links to break when that block is thrown away.
  struct LinkSlot {
    uint32_t target = 0;         // guest address
    uint32_t site = 0;           // byte offset of the rel32, from the block start
    uint32_t after = 0;          // byte offset just past the jump
    int32_t unlinked = 0;        // the displacement that means "return instead"
  };
  LinkSlot links[2];
  int link_count = 0;
};

class BlockCompiler {
 public:
  // The host registers, and why each one.
  //
  // Three are callee-saved, which costs three pushes in the prologue and buys
  // the only thing that matters once the block can call out: they survive the
  // call. A compiled load that kept the register file in RDX would find it gone
  // the moment it called Cpu::Load, and the corruption would look like a CPU
  // bug three layers away.
  static const uint8_t kStatePtr = 3;      // RBX - the BlockState
  static const uint8_t kRegsPtr = 6;       // RSI - the guest's register file
  static const uint8_t kPending = 7;       // EDI - a load's value, in flight
  static const uint8_t kScratchA = 0;      // EAX - and a call's return value
  static const uint8_t kScratchB = 1;      // ECX - a shift count, and arg 1
  static const uint8_t kScratchC = 2;      // EDX - and arg 2
  static const uint8_t kArg3 = 8;          // R8D
  static const uint8_t kArg4 = 9;          // R9D - the guest pc of an access

  // Windows x64: 32 bytes of shadow space, and RSP 16-byte aligned at the call.
  // Three pushes leave RSP 16-aligned, so the shadow space is all that is
  // needed and it happens to be a multiple of 16 itself. An odd number of
  // allocated registers pushes that out again, which is what kStackBytesOdd is
  // for.
  static const uint8_t kStackBytes = 32;
  static const uint8_t kStackBytesOdd = 40;

  // Step 6: the guest registers a block keeps in host registers for its
  // duration, instead of going to memory around every operation.
  //
  // They have to be callee-saved, because a block calls out for every load and
  // store and a volatile register would not survive the call. R12 to R15 are
  // the ones nothing else here wants; RBX, RSI and RDI are already spoken for.
  // Four is not a lot, and it does not need to be: a basic block on this
  // machine is short, and the top four registers of a short block are most of
  // its traffic.
  static const int kMaxAllocated = 4;

  // How many times a register has to appear in a block before caching it pays
  // for the load that puts it there and the push that makes room for it. One
  // use never pays; the measurements in Docs/Recompiler-Plan.md are what set
  // this.
  static const uint32_t kMinimumUses = 2;

  // And how long a block has to be before allocating anything pays at all.
  //
  // This is the whole shape of step 6: the cost is per block *entry* - the
  // pushes, the loads that fill the cached registers, the write-backs - and the
  // saving is per *instruction*. A short block has nothing to amortise the
  // entry over. rec_bench measures exactly where that crosses over: allocation
  // runs at 0.88x the memory-only speed for a seven-instruction block, breaks
  // even around eleven, and reaches 1.37x at sixty-three. Twelve is the first
  // length past break-even.
  //
  // Which means: for this to pay on real code, blocks have to get longer -
  // which is what block linking is for, and why it comes next rather than a
  // cleverer allocator.
  static const uint32_t kMinimumBlockInstructions = 12;

  explicit BlockCompiler(Emitter* emitter) : emitter_(emitter) {
    for (int i = 0; i < 32; ++i)
      host_of_[i] = -1;   // everything in memory until a block says otherwise
  }

  // Off compiles exactly what step 4 compiled, which is what makes the two
  // comparable - and what lets the whole differential suite run twice, once
  // each way, so the allocator cannot quietly change an answer.
  void set_allocate_registers(bool on) { allocate_registers_ = on; }
  bool allocate_registers() const { return allocate_registers_; }

  // Lowering this is for tests: it makes short blocks allocate too, so the
  // differential suite exercises the allocator on the delay slots and branches
  // that real blocks are too short to reach. It is a correctness lever, not a
  // performance one - kMinimumBlockInstructions is where the measurements put
  // the break-even.
  void set_minimum_block_instructions(uint32_t instructions) {
    minimum_block_instructions_ = instructions;
  }

  // Off emits no link slots at all, so a block always returns to the
  // dispatcher. The A/B for linking, and the same switch discipline as the
  // allocator's.
  void set_link_blocks(bool on) { link_blocks_ = on; }

  CompiledBlock Compile(const DecodedBlock& block, CodeBlock* code) {
    CompiledBlock result;
    if (code == nullptr)
      return result;

    // Emission starts wherever this code block's cursor already is, not at its
    // start: a running recompiler packs many blocks into one arena, because a
    // VirtualAlloc per block would spend a 4 KB page on 60 bytes of code. The
    // cursor lives in the CodeBlock and set_block does not reset it, so the two
    // uses - one block per allocation in the tests, many per arena in the
    // engine - are the same code path.
    emitter_->set_block(code);
    const size_t start = code->cursor;
    result.code = static_cast<uint8_t*>(code->address) + start;
    code_ = code;
    block_start_ = start;
    fault_exits_.clear();

    pending_active_ = false;
    pending_reg_ = 0;
    ends_with_branch_ = false;
    successor_count_ = 0;

    const uint32_t count = CompilablePrefix(block);
    PlanAllocation(block, count);

    EmitPrologue();
    for (uint32_t i = 0; i < count; ++i)
      Emit(block.instructions[i]);

    // A load is never the last instruction of the compiled prefix - it needs a
    // follower, and CompilablePrefix enforces that - so this should never fire.
    // It is here because the alternative to a redundant store is a value that
    // silently never reaches its register.
    if (pending_active_)
      EmitPendingStore();

    // Where the interpreter picks up. A branch already wrote it; otherwise it
    // is the address after the last instruction the compiled code performed,
    // which is also correct when the run stopped early.
    if (!ends_with_branch_) {
      x86::MovMemImm(emitter_, kStatePtr, kOffNextPc,
                     block.start_pc + count * 4);
      // A block that simply ran out has exactly one place to go next.
      successors_[0] = block.start_pc + count * 4;
      successor_count_ = 1;
    }

    EmitTail(code, start, count, &result);
    EmitFaultExit(code);

    result.compiled = count;
    result.host_bytes = static_cast<uint32_t>(code->cursor - start);
    result.complete = (count == block.instructions.size());
    result.ends_with_branch = ends_with_branch_;
    result.registers_allocated = allocated_count_;
    return result;
  }

  // Which registers this block appears to use, and how often. Only ever used to
  // decide what is worth caching, so being approximate costs speed and never
  // correctness - a register counted that is not used wastes a host register,
  // and one missed is simply read from memory as before.
  static void CountUses(uint32_t word, uint32_t counts[32]) {
    const uint32_t opcode = word >> 26;
    const uint32_t rs = (word >> 21) & 0x1F;
    const uint32_t rt = (word >> 16) & 0x1F;
    const uint32_t rd = (word >> 11) & 0x1F;
    struct Use {
      uint32_t* counts;
      void operator()(uint32_t reg) const {
        if (reg != 0)
          ++counts[reg];
      }
    } use{counts};

    if (opcode == 0x00) {
      switch (word & 0x3F) {
        case 0x00: case 0x02: case 0x03:            // shifts by a constant
          use(rt); use(rd); return;
        case 0x08:                                  // jr
          use(rs); return;
        case 0x09:                                  // jalr
          use(rs); use(rd); return;
        default:
          use(rs); use(rt); use(rd); return;
      }
    }
    switch (opcode) {
      case 0x01: case 0x06: case 0x07:              // the one-operand branches
        use(rs); return;
      case 0x02:                                    // j
        return;
      case 0x03:                                    // jal
        use(31); return;
      case 0x0F:                                    // lui
        use(rt); return;
      default:
        use(rs); use(rt); return;
    }
  }

  // Whether this instruction is in the compiled subset at all. The single
  // authority on that question: Compile's look-ahead asks it, and Emit's
  // switch covers exactly what it admits. rec_test sweeps every opcode and
  // funct comparing the two, because a disagreement here is the one that
  // produces an instruction executed twice or not at all.
  static bool Compilable(uint32_t word) {
    const uint32_t opcode = word >> 26;
    if (opcode == 0x00) {
      switch (word & 0x3F) {
        case 0x00: case 0x02: case 0x03:            // sll, srl, sra
        case 0x04: case 0x06: case 0x07:            // sllv, srlv, srav
        case 0x08: case 0x09:                       // jr, jalr
        case 0x21: case 0x23:                       // addu, subu
        case 0x24: case 0x25: case 0x26: case 0x27: // and, or, xor, nor
        case 0x2A: case 0x2B:                       // slt, sltu
          return true;
        default:
          return false;   // add and sub trap; the rest is not here yet
      }
    }
    switch (opcode) {
      case 0x01:   // REGIMM - bltz and bgez only; the "and link" forms both
                   // branch and write r31, and are left for later
        return ((word >> 16) & 0x1F) <= 0x01;
      case 0x02: case 0x03:                         // j, jal
      case 0x04: case 0x05: case 0x06: case 0x07:   // beq, bne, blez, bgtz
      case 0x09: case 0x0A: case 0x0B:              // addiu, slti, sltiu
      case 0x0C: case 0x0D: case 0x0E: case 0x0F:   // andi, ori, xori, lui
      case 0x20: case 0x21: case 0x23:              // lb, lh, lw
      case 0x24: case 0x25:                         // lbu, lhu
      case 0x28: case 0x29: case 0x2B:              // sb, sh, sw
        return true;
      default:
        // lwl, lwr, swl, swr read the register a load is still in flight to
        // (Cpu::ReadRegForwarded), the coprocessor forms are the GTE's, and
        // addi traps. All the interpreter's.
        return false;
    }
  }

  // Which register this instruction writes, or 0 for none. Used to decide
  // whether it cancels a load still in flight - the hardware writes the load
  // back first and the instruction's own result second, so the instruction
  // wins (Cpu::WriteReg), and a second load to the same register discards the
  // first before it ever lands (Cpu::ArmLoad). Both are "the later write wins",
  // which is what returning the destination here expresses.
  static uint32_t Destination(uint32_t word) {
    const uint32_t opcode = word >> 26;
    if (opcode == 0x00) {
      const uint32_t funct = word & 0x3F;
      if (funct == 0x08)                  // jr writes nothing
        return 0;
      return (word >> 11) & 0x1F;         // rd, including jalr's
    }
    switch (opcode) {
      case 0x03:                          // jal
        return 31;
      case 0x09: case 0x0A: case 0x0B:
      case 0x0C: case 0x0D: case 0x0E: case 0x0F:
      case 0x20: case 0x21: case 0x23: case 0x24: case 0x25:
        return (word >> 16) & 0x1F;       // rt
      default:
        return 0;                         // branches, jumps, stores
    }
  }

  // Whether this instruction's effect spills onto the one after it: a load,
  // whose value lands an instruction late, or a branch, whose delay slot runs
  // either way. Compiling one without the other would leave the interpreter to
  // resume in the middle of an effect it was never told about.
  static bool NeedsFollower(uint32_t word) {
    const uint32_t opcode = word >> 26;
    if (opcode == 0x00) {
      const uint32_t funct = word & 0x3F;
      return funct == 0x08 || funct == 0x09;   // jr, jalr
    }
    if (opcode == 0x01 || (opcode >= 0x02 && opcode <= 0x07))
      return true;                             // the branches and jumps
    switch (opcode) {
      case 0x20: case 0x21: case 0x23: case 0x24: case 0x25:
        // A load into r0 delivers nothing, so nothing lands late. The memory
        // read still happens - it can have side effects on this machine.
        return ((word >> 16) & 0x1F) != 0;
      default:
        return false;
    }
  }

 private:
  // The BlockState fields the emitted code reaches, by offset. All within a
  // one-byte displacement, which is what keeps the encoding short.
  static const int8_t kOffRegs = static_cast<int8_t>(offsetof(BlockState, regs));
  static const int8_t kOffContext = static_cast<int8_t>(offsetof(BlockState, context));
  static const int8_t kOffLoad32 = static_cast<int8_t>(offsetof(BlockState, load32));
  static const int8_t kOffLoad16 = static_cast<int8_t>(offsetof(BlockState, load16));
  static const int8_t kOffLoad8 = static_cast<int8_t>(offsetof(BlockState, load8));
  static const int8_t kOffStore32 = static_cast<int8_t>(offsetof(BlockState, store32));
  static const int8_t kOffStore16 = static_cast<int8_t>(offsetof(BlockState, store16));
  static const int8_t kOffStore8 = static_cast<int8_t>(offsetof(BlockState, store8));
  static const int8_t kOffNextPc = static_cast<int8_t>(offsetof(BlockState, next_pc));
  static const int8_t kOffBudget = static_cast<int8_t>(offsetof(BlockState, budget));
  static const int8_t kOffFault = static_cast<int8_t>(offsetof(BlockState, fault));

  // How far into the block the compiled code gets. Computed backwards, because
  // whether an instruction can be compiled depends on whether the one after it
  // can be: a branch needs its delay slot, and a load needs somewhere to land.
  static bool IsMemoryWord(uint32_t word) {
    const uint32_t opcode = word >> 26;
    return (opcode >= 0x20 && opcode <= 0x26) || (opcode >= 0x28 && opcode <= 0x2E);
  }

  static bool IsLoadWord(uint32_t word) {
    const uint32_t opcode = word >> 26;
    return opcode >= 0x20 && opcode <= 0x25;
  }

  static uint32_t CompilablePrefix(const DecodedBlock& block) {
    size_t n = block.instructions.size();

    // A memory access whose fault would have to be taken while a load is still
    // in flight to a register it uses cannot be compiled: the value is written
    // out before the access so that a fault leaves a state the interpreter can
    // resume from, and that is only invisible when the access does not touch
    // the register. Stop the block before the load rather than before the
    // access, since a load needs a follower either way.
    for (size_t i = 1; i < n; ++i) {
      const uint32_t previous = block.instructions[i - 1].word;
      if (!IsLoadWord(previous))
        continue;
      const uint32_t pending = (previous >> 16) & 0x1F;
      const uint32_t word = block.instructions[i].word;
      if (IsMemoryWord(word) && MemoryOpBlockedByPendingLoad(word, pending)) {
        n = i - 1;
        break;
      }
    }

    std::vector<bool> ok(n, false);
    for (size_t i = n; i-- > 0;) {
      const uint32_t word = block.instructions[i].word;
      bool can = Compilable(word);
      if (can && NeedsFollower(word))
        can = (i + 1 < n) && ok[i + 1];
      ok[i] = can;
    }
    uint32_t count = 0;
    while (count < n && ok[count])
      ++count;
    return count;
  }

  // Picks the guest registers worth keeping in host registers for this block,
  // by how often they appear in it. Nothing clever: a basic block here is at
  // most 64 instructions and usually far fewer, and the four busiest registers
  // of a short block are most of what it touches.
  void PlanAllocation(const DecodedBlock& block, uint32_t count) {
    for (int i = 0; i < 32; ++i) {
      host_of_[i] = -1;
      dirty_[i] = false;
    }
    allocated_count_ = 0;
    if (!allocate_registers_ || count < minimum_block_instructions_)
      return;

    uint32_t counts[32] = {};
    for (uint32_t i = 0; i < count; ++i)
      CountUses(block.instructions[i].word, counts);

    static const uint8_t kAllocatable[kMaxAllocated] = { 12, 13, 14, 15 };
    for (int slot = 0; slot < kMaxAllocated; ++slot) {
      uint32_t best = 0;
      uint32_t best_count = kMinimumUses - 1;
      for (uint32_t reg = 1; reg < 32; ++reg) {
        if (host_of_[reg] < 0 && counts[reg] > best_count) {
          best = reg;
          best_count = counts[reg];
        }
      }
      if (best == 0)
        break;   // nothing left worth caching
      host_of_[best] = static_cast<int8_t>(kAllocatable[slot]);
      allocated_[slot] = static_cast<uint8_t>(best);
      ++allocated_count_;
    }
  }

  void EmitPrologue() {
    x86::Push(emitter_, kStatePtr);
    x86::Push(emitter_, kRegsPtr);
    x86::Push(emitter_, kPending);
    for (int i = 0; i < allocated_count_; ++i)
      x86::Push(emitter_, static_cast<uint8_t>(host_of_[allocated_[i]]));

    // Three pushes leave the stack aligned; a fourth, sixth or eighth keeps it
    // that way and an odd one does not, so the shadow space absorbs the
    // difference. Getting this wrong is invisible until a callee spills an
    // aligned SSE register, which is why rec_test checks it at every call.
    x86::SubRspImm8(emitter_, (allocated_count_ & 1) ? kStackBytesOdd
                                                     : kStackBytes);

    x86::Mov64RegReg(emitter_, kStatePtr, 1);              // mov rbx, rcx
    x86::Mov64RegMem(emitter_, kRegsPtr, kStatePtr, kOffRegs);

    // Fill the cached registers from the register file. A register written
    // before it is read pays for a load it did not need; finding those costs
    // another pass over the block and saves one instruction per block.
    for (int i = 0; i < allocated_count_; ++i) {
      const uint32_t guest = allocated_[i];
      x86::MovRegMem(emitter_, static_cast<uint8_t>(host_of_[guest]), kRegsPtr,
                     Offset(guest));
    }
  }

  // Everything the epilogue does except return: the write-backs and the stack.
  // After this the stack is exactly as it was when the block was entered, with
  // the caller's return address on top - which is what lets a block jump
  // straight to another one instead of returning. The next block's prologue
  // pushes again, its epilogue pops again, and whichever block finally executes
  // `ret` returns to the dispatcher that called the first of them.
  void EmitFrameRestore() {
    // Only what was written needs writing back. A block that merely reads a
    // register leaves memory as it found it.
    for (int i = 0; i < allocated_count_; ++i) {
      const uint32_t guest = allocated_[i];
      if (dirty_[guest]) {
        x86::MovMemReg(emitter_, static_cast<uint8_t>(host_of_[guest]),
                       kRegsPtr, Offset(guest));
      }
    }

    x86::AddRspImm8(emitter_, (allocated_count_ & 1) ? kStackBytesOdd
                                                     : kStackBytes);
    for (int i = allocated_count_ - 1; i >= 0; --i)
      x86::Pop(emitter_, static_cast<uint8_t>(host_of_[allocated_[i]]));
    x86::Pop(emitter_, kPending);
    x86::Pop(emitter_, kRegsPtr);
    x86::Pop(emitter_, kStatePtr);
  }

  // The tail: hand the state back to the argument register, unwind, charge the
  // block's instructions to the budget, and then either jump to the next block
  // or return.
  //
  // The state pointer has to move from RBX to RCX *before* the unwind pops RBX,
  // because a block reached by a jump runs its own prologue, and that prologue
  // expects the state where the calling convention puts it. RCX is volatile and
  // free by now: the last call this block made is long past.
  void EmitTail(CodeBlock* code, size_t block_start, uint32_t count,
                CompiledBlock* result) {
    x86::Mov64RegReg(emitter_, kScratchB, kStatePtr);   // mov rcx, rbx
    EmitFrameRestore();

    if (count == 0) {
      // Nothing ran, so nothing is charged and there is nowhere to go.
      x86::Ret(emitter_);
      return;
    }

    x86::SubMemImm8(emitter_, kScratchB, kOffBudget, static_cast<uint8_t>(count));

    const bool linkable = link_blocks_ && successor_count_ > 0;
    if (!linkable) {
      x86::Ret(emitter_);
      return;
    }

    // Out of budget: return to the dispatcher rather than chaining. The
    // displacement is filled in once the length of what follows is known.
    const size_t budget_jump = code->cursor;
    x86::JccRel8(emitter_, x86::Cc::kLessEqual, 0);

    if (successor_count_ == 2) {
      // Two ways out, so which one this is has to be decided at run time. The
      // address the branch chose is in next_pc; compare it against the taken
      // target and take the matching slot.
      x86::MovRegMem(emitter_, kScratchA, kScratchB, kOffNextPc);
      x86::AluRegImm(emitter_, x86::AluImmOp::kCmp, kScratchA, successors_[0]);
      const size_t second_jump = code->cursor;
      x86::JccRel8(emitter_, x86::Cc::kNotEqual, 0);
      AddLinkSlot(code, block_start, successors_[0], result);
      PatchRel8(code, second_jump, code->cursor);
    }

    AddLinkSlot(code, block_start, successors_[successor_count_ - 1], result);

    PatchRel8(code, budget_jump, code->cursor);
    x86::Ret(emitter_);

    // Every slot's "not linked yet" displacement points at that `ret`.
    const size_t exit = code->cursor - 1;
    for (int i = 0; i < result->link_count; ++i) {
      CompiledBlock::LinkSlot& slot = result->links[i];
      slot.unlinked = static_cast<int32_t>(exit - (block_start + slot.after));
      PatchRel32(code, block_start + slot.site, slot.unlinked);
    }
  }

  // The way out when a memory access raised a guest exception: unwind and
  // return, with none of the block's remaining instructions run and nothing
  // charged to the budget. The CPU's own pc has already been moved to the
  // exception vector by whoever raised it, so there is nothing to say about
  // where execution goes next.
  //
  // It sits after the normal tail's `ret`, so nothing reaches it by falling
  // through - only the jumps that each memory access left behind.
  void EmitFaultExit(CodeBlock* code) {
    if (fault_exits_.empty())
      return;
    const size_t stub = code->cursor;
    x86::Mov64RegReg(emitter_, kScratchB, kStatePtr);
    EmitFrameRestore();
    x86::Ret(emitter_);

    for (size_t site : fault_exits_)
      PatchRel32(code, site + 1, static_cast<int32_t>(stub - (site + 5)));
    fault_exits_.clear();
  }

  void AddLinkSlot(CodeBlock* code, size_t block_start, uint32_t target,
                   CompiledBlock* result) {
    CompiledBlock::LinkSlot& slot = result->links[result->link_count++];
    slot.target = target;
    x86::JmpRel32(emitter_, 0);
    slot.after = static_cast<uint32_t>(code->cursor - block_start);
    slot.site = slot.after - 4;
  }

  // The displacement of a jump is measured from the end of the instruction, and
  // a rel8 jump is two bytes long.
  static void PatchRel8(CodeBlock* code, size_t jump_at, size_t target) {
    code->ptr8bit[jump_at + 1] =
        static_cast<uint8_t>(static_cast<int8_t>(target - (jump_at + 2)));
  }

  static void PatchRel32(CodeBlock* code, size_t site, int32_t value) {
    memcpy(&code->ptr8bit[site], &value, sizeof(value));
  }

  // Where a guest register sits in the file. 32 registers of four bytes, so
  // the furthest is at 124 and a one-byte displacement always reaches it.
  static int8_t Offset(uint32_t reg) { return static_cast<int8_t>(reg * 4); }

  void LoadReg(uint8_t host, uint32_t guest) {
    if (guest == 0) {
      // Reading r0 is reading zero; do not go to memory for it.
      x86::AluRegReg(emitter_, x86::AluOp::kXor, host, host);
      return;
    }
    if (host_of_[guest] >= 0) {
      x86::MovRegReg(emitter_, host, static_cast<uint8_t>(host_of_[guest]));
      return;
    }
    x86::MovRegMem(emitter_, host, kRegsPtr, Offset(guest));
  }

  void StoreReg(uint32_t guest, uint8_t host) {
    if (guest == 0)
      return;   // writes to r0 are discarded
    if (host_of_[guest] >= 0) {
      x86::MovRegReg(emitter_, static_cast<uint8_t>(host_of_[guest]), host);
      dirty_[guest] = true;
      return;
    }
    x86::MovMemReg(emitter_, host, kRegsPtr, Offset(guest));
  }

  // The load delay, resolved at compile time.
  //
  // A load arms a value that reaches its register at the start of the
  // instruction *after* the next one. So the store is emitted after the next
  // instruction's code - by which point that instruction has read everything it
  // reads, and has seen the register's old value, which is the whole point -
  // unless it writes the same register, in which case the load is cancelled.
  //
  // Every Emit path calls this exactly once, at the point where the
  // instruction has finished reading. For a load that is mid-instruction:
  // after the call returns, before the returned value becomes the new pending
  // one.
  void FlushPending(uint32_t destination) {
    if (!pending_active_)
      return;
    pending_active_ = false;
    if (destination != 0 && destination == pending_reg_)
      return;   // the instruction wrote it; the load never lands
    EmitPendingStore();
  }

  void EmitPendingStore() {
    pending_active_ = false;
    StoreReg(pending_reg_, kPending);
  }

  void Emit(const Instruction& instruction) {
    const uint32_t word = instruction.word;
    const uint32_t opcode = word >> 26;

    if (opcode == 0x00) {
      EmitSpecial(instruction);
      return;
    }
    if (opcode == 0x01 || (opcode >= 0x02 && opcode <= 0x07)) {
      EmitBranch(instruction);
      FlushPending(Destination(word));
      return;
    }
    if (opcode >= 0x20 && opcode <= 0x25) {
      EmitLoad(instruction);   // flushes at its own point, mid-instruction
      return;
    }
    if (opcode >= 0x28 && opcode <= 0x2B) {
      EmitStore(instruction);
      FlushPending(Destination(word));
      return;
    }
    EmitImmediate(instruction);
    FlushPending(Destination(word));
  }

  void EmitImmediate(const Instruction& instruction) {
    const uint32_t word = instruction.word;
    const uint32_t opcode = word >> 26;
    const uint32_t rs = (word >> 21) & 0x1F;
    const uint32_t rt = (word >> 16) & 0x1F;
    const uint16_t immediate = static_cast<uint16_t>(word & 0xFFFF);

    switch (opcode) {
      case 0x09:   // addiu rt, rs, imm - sign-extended, and does not trap
        LoadReg(kScratchA, rs);
        x86::AluRegImm(emitter_, x86::AluImmOp::kAdd, kScratchA, SignExtend(immediate));
        StoreReg(rt, kScratchA);
        return;

      case 0x0C:   // andi - zero-extended
        LoadReg(kScratchA, rs);
        x86::AluRegImm(emitter_, x86::AluImmOp::kAnd, kScratchA, immediate);
        StoreReg(rt, kScratchA);
        return;

      case 0x0D:   // ori
        LoadReg(kScratchA, rs);
        x86::AluRegImm(emitter_, x86::AluImmOp::kOr, kScratchA, immediate);
        StoreReg(rt, kScratchA);
        return;

      case 0x0E:   // xori
        LoadReg(kScratchA, rs);
        x86::AluRegImm(emitter_, x86::AluImmOp::kXor, kScratchA, immediate);
        StoreReg(rt, kScratchA);
        return;

      case 0x0F:   // lui
        x86::MovRegImm(emitter_, kScratchA, static_cast<uint32_t>(immediate) << 16);
        StoreReg(rt, kScratchA);
        return;

      case 0x0A:   // slti - signed
        LoadReg(kScratchA, rs);
        x86::AluRegImm(emitter_, x86::AluImmOp::kCmp, kScratchA, SignExtend(immediate));
        EmitSetCondition(x86::Cc::kLess, rt);
        return;

      case 0x0B:   // sltiu - unsigned, but the immediate is still sign-extended
        LoadReg(kScratchA, rs);
        x86::AluRegImm(emitter_, x86::AluImmOp::kCmp, kScratchA, SignExtend(immediate));
        EmitSetCondition(x86::Cc::kBelow, rt);
        return;

      default:
        return;   // Compilable() admitted nothing else
    }
  }

  void EmitSpecial(const Instruction& instruction) {
    const uint32_t word = instruction.word;
    const uint32_t rs = (word >> 21) & 0x1F;
    const uint32_t rt = (word >> 16) & 0x1F;
    const uint32_t rd = (word >> 11) & 0x1F;
    const uint32_t shift = (word >> 6) & 0x1F;
    const uint32_t funct = word & 0x3F;

    // jr and jalr write next_pc and, for jalr, the link register; they are not
    // ALU instructions and take the branch path.
    if (funct == 0x08 || funct == 0x09) {
      LoadReg(kScratchA, rs);   // read rs before writing rd: jalr may be rd == rs
      x86::MovMemReg(emitter_, kScratchA, kStatePtr, kOffNextPc);
      if (funct == 0x09) {
        x86::MovRegImm(emitter_, kScratchA, instruction.pc + 8);
        StoreReg(rd, kScratchA);
      }
      ends_with_branch_ = true;
      successor_count_ = 0;   // a register jump goes somewhere only it knows
      FlushPending(Destination(word));
      return;
    }

    switch (funct) {
      case 0x00:   // sll rd, rt, sa - and the all-zero word, which is nop
        if (word != 0)
          EmitShiftImmediate(x86::ShiftOp::kShl, rd, rt, shift);
        break;    // nop compiles to nothing at all
      case 0x02:   // srl
        EmitShiftImmediate(x86::ShiftOp::kShr, rd, rt, shift);
        break;
      case 0x03:   // sra
        EmitShiftImmediate(x86::ShiftOp::kSar, rd, rt, shift);
        break;

      case 0x04:   // sllv rd, rt, rs
        EmitShiftVariable(x86::ShiftOp::kShl, rd, rt, rs);
        break;
      case 0x06:   // srlv
        EmitShiftVariable(x86::ShiftOp::kShr, rd, rt, rs);
        break;
      case 0x07:   // srav
        EmitShiftVariable(x86::ShiftOp::kSar, rd, rt, rs);
        break;

      case 0x21:   // addu
        EmitAlu(x86::AluOp::kAdd, rd, rs, rt);
        break;
      case 0x23:   // subu
        EmitAlu(x86::AluOp::kSub, rd, rs, rt);
        break;
      case 0x24:   // and
        EmitAlu(x86::AluOp::kAnd, rd, rs, rt);
        break;
      case 0x25:   // or
        EmitAlu(x86::AluOp::kOr, rd, rs, rt);
        break;
      case 0x26:   // xor
        EmitAlu(x86::AluOp::kXor, rd, rs, rt);
        break;

      case 0x27:   // nor rd, rs, rt
        LoadReg(kScratchA, rs);
        LoadReg(kScratchB, rt);
        x86::AluRegReg(emitter_, x86::AluOp::kOr, kScratchA, kScratchB);
        x86::NotReg(emitter_, kScratchA);
        StoreReg(rd, kScratchA);
        break;

      case 0x2A:   // slt
        LoadReg(kScratchA, rs);
        LoadReg(kScratchB, rt);
        x86::AluRegReg(emitter_, x86::AluOp::kCmp, kScratchA, kScratchB);
        EmitSetCondition(x86::Cc::kLess, rd);
        break;

      case 0x2B:   // sltu
        LoadReg(kScratchA, rs);
        LoadReg(kScratchB, rt);
        x86::AluRegReg(emitter_, x86::AluOp::kCmp, kScratchA, kScratchB);
        EmitSetCondition(x86::Cc::kBelow, rd);
        break;

      default:
        break;    // Compilable() admitted nothing else
    }
    FlushPending(Destination(word));
  }

  // A branch, as two addresses and a conditional move. `taken` is computed from
  // the instruction's own pc, so the emitted code carries both answers as
  // immediates and picks one - no host branch, nothing to patch, and the same
  // number of instructions whichever way it goes.
  void EmitBranch(const Instruction& instruction) {
    const uint32_t word = instruction.word;
    const uint32_t opcode = word >> 26;
    const uint32_t rs = (word >> 21) & 0x1F;
    const uint32_t rt = (word >> 16) & 0x1F;
    const uint16_t immediate = static_cast<uint16_t>(word & 0xFFFF);

    ends_with_branch_ = true;

    // j and jal go to a fixed address: the top four bits come from the delay
    // slot's address, which is this instruction's pc plus four.
    if (opcode == 0x02 || opcode == 0x03) {
      const uint32_t target = ((instruction.pc + 4) & 0xF0000000u) |
                              ((word & 0x03FFFFFFu) << 2);
      if (opcode == 0x03) {   // jal links unconditionally, before the jump
        x86::MovRegImm(emitter_, kScratchA, instruction.pc + 8);
        StoreReg(31, kScratchA);
      }
      x86::MovMemImm(emitter_, kStatePtr, kOffNextPc, target);
      successors_[0] = target;
      successor_count_ = 1;
      return;
    }

    const uint32_t taken = instruction.pc + 4 + (SignExtend(immediate) << 2);
    const uint32_t not_taken = instruction.pc + 8;

    // The condition emitted is the one for *not* taking the branch, because
    // the conditional move overwrites the taken address with the other one.
    x86::Cc not_taken_when = x86::Cc::kNotEqual;
    LoadReg(kScratchB, rs);
    if (opcode == 0x04 || opcode == 0x05) {         // beq, bne
      LoadReg(kScratchC, rt);
      x86::AluRegReg(emitter_, x86::AluOp::kCmp, kScratchB, kScratchC);
      not_taken_when = (opcode == 0x04) ? x86::Cc::kNotEqual : x86::Cc::kEqual;
    } else {
      x86::AluRegImm(emitter_, x86::AluImmOp::kCmp, kScratchB, 0);
      if (opcode == 0x06)                           // blez: taken if <= 0
        not_taken_when = x86::Cc::kGreater;
      else if (opcode == 0x07)                      // bgtz: taken if > 0
        not_taken_when = x86::Cc::kLessEqual;
      else if (rt == 0x00)                          // bltz: taken if < 0
        not_taken_when = x86::Cc::kGreaterEqual;
      else                                          // bgez: taken if >= 0
        not_taken_when = x86::Cc::kLess;
    }

    // Neither mov touches the flags the compare set.
    x86::MovRegImm(emitter_, kScratchA, taken);
    x86::MovRegImm(emitter_, kScratchC, not_taken);
    x86::CmovRegReg(emitter_, not_taken_when, kScratchA, kScratchC);
    x86::MovMemReg(emitter_, kScratchA, kStatePtr, kOffNextPc);

    // Both destinations are known here even though which one is taken is not,
    // so a conditional branch gets two link slots and picks between them at run
    // time. This is the case that matters: the back edge of a loop is a
    // conditional branch, and a loop that cannot link is a loop that returns to
    // the dispatcher on every iteration.
    successors_[0] = taken;
    successors_[1] = not_taken;
    successor_count_ = 2;
  }

  // A load: address into arg 2, the callback's context into arg 1, the function
  // pointer out of the state, call. What comes back is the raw value, and the
  // width's sign or zero extension is applied here rather than in the callback,
  // so the callback stays the same shape as Cpu::Load.
  void EmitLoad(const Instruction& instruction) {
    const uint32_t word = instruction.word;
    const uint32_t opcode = word >> 26;
    const uint32_t rs = (word >> 21) & 0x1F;
    const uint32_t rt = (word >> 16) & 0x1F;
    const uint16_t immediate = static_cast<uint16_t>(word & 0xFFFF);

    int8_t function = kOffLoad32;
    if (opcode == 0x20 || opcode == 0x24)
      function = kOffLoad8;
    else if (opcode == 0x21 || opcode == 0x25)
      function = kOffLoad16;

    // A load still on its way to a register is written to it before the access
    // rather than after. It has to be: if this access faults, the interpreter
    // takes over at the exception vector and has no way to be told about a
    // value still in flight. Doing it early is invisible because the compiler
    // refuses this instruction when the register in flight is one it reads -
    // see MemoryOpBlockedByPendingLoad.
    FlushPendingBeforeMemory();

    EmitAddress(rs, immediate);
    EmitCall(function, instruction.pc);
    EmitFaultCheck(code_, block_start_);

    switch (opcode) {
      case 0x20: x86::MovsxRegReg8(emitter_, kScratchA, kScratchA); break;   // lb
      case 0x24: x86::MovzxRegReg8(emitter_, kScratchA, kScratchA); break;   // lbu
      case 0x21: x86::MovsxRegReg16(emitter_, kScratchA, kScratchA); break;  // lh
      case 0x25: x86::MovzxRegReg16(emitter_, kScratchA, kScratchA); break;  // lhu
      default: break;                                                        // lw
    }

    // This is the point at which the instruction has finished reading, so a
    // previous load's value lands here - before this one's takes its place.
    FlushPending(Destination(word));

    if (rt != 0) {
      x86::Mov64RegReg(emitter_, kPending, kScratchA);   // mov rdi, rax
      pending_active_ = true;
      pending_reg_ = rt;
    }
  }

  void EmitStore(const Instruction& instruction) {
    const uint32_t word = instruction.word;
    const uint32_t opcode = word >> 26;
    const uint32_t rs = (word >> 21) & 0x1F;
    const uint32_t rt = (word >> 16) & 0x1F;
    const uint16_t immediate = static_cast<uint16_t>(word & 0xFFFF);

    int8_t function = kOffStore32;
    if (opcode == 0x28)
      function = kOffStore8;
    else if (opcode == 0x29)
      function = kOffStore16;

    FlushPendingBeforeMemory();
    EmitAddress(rs, immediate);
    LoadReg(kArg3, rt);        // the value, whole; the callback narrows it
    EmitCall(function, instruction.pc);
    EmitFaultCheck(code_, block_start_);
  }

  // See EmitLoad. The pending load is written out before a memory access so
  // that a fault leaves the register file in a state the interpreter can pick
  // up from.
  void FlushPendingBeforeMemory() {
    if (pending_active_)
      EmitPendingStore();
  }

  // Whether a memory access has to be left to the interpreter because a load is
  // still in flight to a register it uses. Writing the pending value out early
  // is only invisible when the instruction neither reads nor writes that
  // register - and a real compiler almost never emits the case this rejects,
  // since the load delay slot is exactly what stops it putting a load's result
  // to use in the next instruction.
  static bool MemoryOpBlockedByPendingLoad(uint32_t word, uint32_t pending) {
    if (pending == 0)
      return false;
    const uint32_t rs = (word >> 21) & 0x1F;
    const uint32_t rt = (word >> 16) & 0x1F;
    return pending == rs || pending == rt;
  }

  // The address a load or store works on, in arg 2: rs plus the sign-extended
  // offset, wrapping like the guest's 32-bit add.
  void EmitAddress(uint32_t rs, uint16_t immediate) {
    LoadReg(kScratchC, rs);
    const uint32_t offset = SignExtend(immediate);
    if (offset != 0)
      x86::AluRegImm(emitter_, x86::AluImmOp::kAdd, kScratchC, offset);
  }

  // The context and the function pointer both come out of the state at run
  // time, so nothing about where the host's code lives is baked into the block.
  // The guest pc goes in the last argument register: the host needs it to point
  // an exception at the right instruction.
  void EmitCall(int8_t function_offset, uint32_t pc) {
    x86::MovRegImm(emitter_, kArg4, pc);                             // arg 4
    x86::Mov64RegMem(emitter_, kScratchB, kStatePtr, kOffContext);   // arg 1
    x86::Mov64RegMem(emitter_, kScratchA, kStatePtr, function_offset);
    x86::CallReg(emitter_, kScratchA);
  }

  // After every memory access: if the host raised a guest exception, leave the
  // block at once. The instructions after this one belong to an execution that
  // is no longer happening - the CPU has vectored somewhere else - and running
  // them is how a recompiler produces corruption that looks like a CPU bug.
  //
  // Two instructions and six bytes on the path that does not fault, and the
  // jump that does is recorded so it can be pointed at the block's fault exit
  // once the length of the block is known.
  void EmitFaultCheck(CodeBlock* code, size_t block_start) {
    x86::CmpMemImm8(emitter_, kStatePtr, kOffFault, 0);
    x86::JccRel8(emitter_, x86::Cc::kEqual, 5);   // skip the jump below
    fault_exits_.push_back(code->cursor);
    x86::JmpRel32(emitter_, 0);
    (void)block_start;
  }

  void EmitAlu(x86::AluOp op, uint32_t rd, uint32_t rs, uint32_t rt) {
    LoadReg(kScratchA, rs);
    if (rt == 0) {
      // Operating against zero still has to happen - `or rd, rs, r0` is the
      // usual register move - but it needs no load.
      x86::AluRegReg(emitter_, x86::AluOp::kXor, kScratchB, kScratchB);
    } else {
      LoadReg(kScratchB, rt);
    }
    x86::AluRegReg(emitter_, op, kScratchA, kScratchB);
    StoreReg(rd, kScratchA);
  }

  void EmitShiftImmediate(x86::ShiftOp op, uint32_t rd, uint32_t rt,
                          uint32_t shift) {
    LoadReg(kScratchA, rt);
    x86::ShiftRegImm(emitter_, op, kScratchA, static_cast<uint8_t>(shift));
    StoreReg(rd, kScratchA);
  }

  // The variable shifts take their count from a register. Both the guest and
  // x86 mask that count to five bits for a 32-bit shift, so it passes straight
  // through with no explicit mask - and the count register is ECX, which is
  // where the second scratch already lives.
  void EmitShiftVariable(x86::ShiftOp op, uint32_t rd, uint32_t rt, uint32_t rs) {
    LoadReg(kScratchA, rt);
    LoadReg(kScratchB, rs);   // ECX
    x86::ShiftRegCl(emitter_, op, kScratchA);
    StoreReg(rd, kScratchA);
  }

  void EmitSetCondition(x86::Cc condition, uint32_t destination) {
    x86::SetCc(emitter_, condition, kScratchA);
    x86::MovzxRegReg8(emitter_, kScratchA, kScratchA);
    StoreReg(destination, kScratchA);
  }

  static uint32_t SignExtend(uint16_t value) {
    return static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(value)));
  }

  Emitter* emitter_;

  // The load in flight, as the compiler walks the block. Compile-time state:
  // the emitted code carries only the value, in kPending.
  bool pending_active_ = false;
  uint32_t pending_reg_ = 0;
  bool ends_with_branch_ = false;

  // The register allocation for the block being compiled. `host_of_` is the
  // whole of it as far as the emitter is concerned: -1 means the register lives
  // in memory, which is what every register was before step 6.
  bool allocate_registers_ = true;
  uint32_t minimum_block_instructions_ = kMinimumBlockInstructions;

  // Block linking: where this block can go next, and whether to emit the slots
  // that would let it jump there directly.
  bool link_blocks_ = true;
  uint32_t successors_[2] = {};
  int successor_count_ = 0;

  // The block being emitted, and the jumps out of it that a faulting memory
  // access leaves behind for EmitFaultExit to point somewhere.
  CodeBlock* code_ = nullptr;
  size_t block_start_ = 0;
  std::vector<size_t> fault_exits_;
  int8_t host_of_[32] = {};
  bool dirty_[32] = {};
  uint8_t allocated_[kMaxAllocated] = {};
  int allocated_count_ = 0;
};

}  // namespace rec
}  // namespace emulation
