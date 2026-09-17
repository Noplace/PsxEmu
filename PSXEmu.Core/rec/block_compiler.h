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
//     which is a compare and a conditional move into `next_pc`. Nothing is
//     patched, no host branch is emitted, and block linking - jumping straight
//     into the next block - is a later step that has to earn its place with a
//     measurement.
//
// The emitted function is
//
//     void block(BlockState* state);
//
// Guest registers live in memory and are loaded and stored around each
// operation - no allocation yet, deliberately.
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

#include "lib/reccore/reccore.h"
#include "rec/block_decoder.h"
#include "rec/runtime.h"
#include "rec/x86_extras.h"

#include <cstddef>
#include <cstdint>
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

  // Windows x64: 32 bytes of shadow space, and RSP 16-byte aligned at the call.
  // Three pushes leave RSP 16-aligned, so the shadow space is all that is
  // needed and it happens to be a multiple of 16 itself.
  static const uint8_t kStackBytes = 32;

  explicit BlockCompiler(reccore::Emitter* emitter) : emitter_(emitter) {}

  CompiledBlock Compile(const DecodedBlock& block, reccore::CodeBlock* code) {
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

    pending_active_ = false;
    pending_reg_ = 0;
    ends_with_branch_ = false;

    const uint32_t count = CompilablePrefix(block);

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
    }

    EmitEpilogue();

    result.compiled = count;
    result.host_bytes = static_cast<uint32_t>(code->cursor - start);
    result.complete = (count == block.instructions.size());
    result.ends_with_branch = ends_with_branch_;
    return result;
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

  // How far into the block the compiled code gets. Computed backwards, because
  // whether an instruction can be compiled depends on whether the one after it
  // can be: a branch needs its delay slot, and a load needs somewhere to land.
  static uint32_t CompilablePrefix(const DecodedBlock& block) {
    const size_t n = block.instructions.size();
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

  void EmitPrologue() {
    x86::Push(emitter_, kStatePtr);
    x86::Push(emitter_, kRegsPtr);
    x86::Push(emitter_, kPending);
    x86::SubRspImm8(emitter_, kStackBytes);
    x86::Mov64RegReg(emitter_, kStatePtr, 1);              // mov rbx, rcx
    x86::Mov64RegMem(emitter_, kRegsPtr, kStatePtr, kOffRegs);
  }

  void EmitEpilogue() {
    x86::AddRspImm8(emitter_, kStackBytes);
    x86::Pop(emitter_, kPending);
    x86::Pop(emitter_, kRegsPtr);
    x86::Pop(emitter_, kStatePtr);
    x86::Ret(emitter_);
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
    x86::MovRegMem(emitter_, host, kRegsPtr, Offset(guest));
  }

  void StoreReg(uint32_t guest, uint8_t host) {
    if (guest == 0)
      return;   // writes to r0 are discarded
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
    x86::MovMemReg(emitter_, kPending, kRegsPtr, Offset(pending_reg_));
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

    EmitAddress(rs, immediate);
    EmitCall(function);

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

    EmitAddress(rs, immediate);
    LoadReg(kArg3, rt);        // the value, whole; the callback narrows it
    EmitCall(function);
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
  void EmitCall(int8_t function_offset) {
    x86::Mov64RegMem(emitter_, kScratchB, kStatePtr, kOffContext);   // arg 1
    x86::Mov64RegMem(emitter_, kScratchA, kStatePtr, function_offset);
    x86::CallReg(emitter_, kScratchA);
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

  reccore::Emitter* emitter_;

  // The load in flight, as the compiler walks the block. Compile-time state:
  // the emitted code carries only the value, in kPending.
  bool pending_active_ = false;
  uint32_t pending_reg_ = 0;
  bool ends_with_branch_ = false;
};

}  // namespace rec
}  // namespace emulation
