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
//
//     Bar one case, added later and measured (Docs/Recompiler-Plan.md, "Loads
//     from RAM read it"): a load from main RAM, which is most of them, is a read
//     and a fixed stall and nothing else, and the call cost more than both. So
//     while the host allows it (set_direct_ram, BlockState::ram) a load reads
//     RAM itself and adds the stall to what the chain owes, and only falls back
//     on the call for anything else - another region, a misaligned address, the
//     host saying not now.
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

  // Guest addresses outside the block whose first instruction the compiled code is only right for:
  // set when the block ends in a load in a branch's delay slot that it delivers itself, on the
  // strength of what the instruction at each place it goes next does not touch. The engine has the
  // block thrown away when any of them is written (Block::watched).
  uint32_t watched[2] = {};
  int watched_count = 0;
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
  static const uint8_t kArg3 = 8;          // R8D - a store's value, a load's guest pc
  static const uint8_t kArg4 = 9;          // R9D - a store's guest pc
  static const uint8_t kScratchD = 10;     // R10 - a direct store's pointer to the code pages

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

  // On, every register copy - addu/or with r0, addiu/ori with 0 - also calls
  // BlockState::move, for a host keeping something beside each register. Off,
  // not a byte of it is emitted.
  void set_track_moves(bool on) { track_moves_ = on; }

  // Loads from the first `bytes` of physical memory - main RAM - read
  // BlockState::ram directly instead of calling out, and add `read_cycles` to
  // BlockState::extra_cycles for each read: the stall the callback would have
  // charged. Zero bytes, the default, compiles every load as a call. Blocks keep
  // what they were compiled with; the host switches between the two at run time
  // through BlockState::ram, not through this.
  //
  // `window_bytes` is how far the physical addresses that reach RAM extend: the same `bytes`
  // seen again and again, as the console's 2 MB is across its first 8 MB. Zero means no
  // mirrors - RAM is only its own first `bytes`. Whatever the host says, `bytes` has to be a
  // power of two and the window a multiple of it, since a mirror is found by masking.
  //
  // Stores to the same memory are written directly too, from BlockState::ram_store, unless
  // the page they land on has compiled code in it (BlockState::code_pages).
  void set_direct_ram(uint32_t bytes, uint8_t read_cycles, uint32_t window_bytes = 0) {
    direct_ram_bytes_ = bytes;
    direct_ram_read_cycles_ = read_cycles;
    direct_ram_window_ = window_bytes > bytes ? window_bytes : bytes;
  }

  // The scratchpad, which is memory too and nothing else: the `bytes` from physical address `base`
  // (0x1F800000, 1 KB), read and written by compiled code the same way RAM is, from
  // BlockState::scratchpad - at no stall, with no mirror and no code in it to notice a store to.
  // Zero bytes, the default, leaves every access to it a call. Taken only where RAM is: it is the same
  // pointers' being null that says the host wants every access to go through its callbacks.
  void set_direct_scratchpad(uint32_t base, uint32_t bytes) {
    direct_scratch_base_ = base;
    direct_scratch_bytes_ = bytes;
  }

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
    block_pc_ = block.start_pc;
    fault_exits_.clear();

    pending_active_ = false;
    pending_reg_ = 0;
    ends_with_branch_ = false;
    successor_count_ = 0;
    rel8_out_of_reach_ = false;

    const uint32_t count = CompilablePrefix(block);
    PlanAllocation(block, count);

    EmitPrologue();
    // A block with a memory access says where it starts, so a call that goes past RAM can work out
    // how far into the chain the instruction making it is (HostInterface::sync).
    for (uint32_t i = 0; i < count; ++i) {
      if (IsMemoryWord(block.instructions[i].word)) {
        x86::MovMemImm(emitter_, kStatePtr, kOffBlockPc, block.start_pc);
        break;
      }
    }
    for (uint32_t i = 0; i < count; ++i)
      Emit(block.instructions[i]);

    // A load is the last instruction of the compiled prefix only as the delay slot of the branch that
    // ends the block, when CompilablePrefix has seen that the instructions it goes to cannot tell the
    // difference (SlotLoadIsDeliverable); then this is what delivers it. Otherwise it needs a follower
    // and this does not fire. It is here because the alternative to a redundant store is a value that
    // silently never reaches its register.
    if (pending_active_)
      EmitPendingStore();
    result.watched_count = 0;
    if (EndsWithSlotLoad(block, count)) {
      for (int i = 0; i < block.successor_count; ++i)
        result.watched[result.watched_count++] = block.successors[i].pc;
    }

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

    // A jump that could not reach (PatchRel8) means the code is wrong, so none
    // of it is claimed.
    result.compiled = rel8_out_of_reach_ ? 0 : count;
    result.host_bytes = static_cast<uint32_t>(code->cursor - start);
    result.complete = !rel8_out_of_reach_ && (count == block.instructions.size());
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
  //
  // The base subset: what needs nothing from the host beyond memory. The instructions of
  // IsSpecialOp are compiled too, but only when the host can answer for them (set_special_ops),
  // which is what CanCompile asks.
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

  // The instructions that are compiled when the host provides for them (set_special_ops): the
  // trapping add and addi, which have an exception to raise, and the multiply and divide unit,
  // which has state of its own (HI and LO) and a clock.
  static bool IsSpecialOp(uint32_t word) {
    const uint32_t opcode = word >> 26;
    if (opcode == 0x08)   // addi
      return true;
    if (opcode != 0x00)
      return false;
    switch (word & 0x3F) {
      case 0x10: case 0x11: case 0x12: case 0x13:   // mfhi, mthi, mflo, mtlo
      case 0x18: case 0x19: case 0x1A: case 0x1B:   // mult, multu, div, divu
      case 0x20:                                    // add
        return true;
      default:
        return false;
    }
  }

  static bool IsBranchOrJump(uint32_t word) {
    const uint32_t opcode = word >> 26;
    if (opcode == 0x00)
      return (word & 0x3F) == 0x08 || (word & 0x3F) == 0x09;
    return opcode == 0x01 || (opcode >= 0x02 && opcode <= 0x07);
  }

  // lwl, lwr, swl and swr: an unaligned word's two halves, each touching the bytes of one aligned word
  // from the addressed byte to one end of it. Compiled for RAM and left, for everything else, to the
  // interpreter (EmitUnalignedBail).
  static bool IsUnalignedOp(uint32_t word) {
    const uint32_t opcode = word >> 26;
    return opcode == 0x22 || opcode == 0x26 || opcode == 0x2A || opcode == 0x2E;
  }

  // Coprocessor 2, the GTE. A command is cop2 with bit 25 set; the moves are selected by rs
  // (mfc2 0, cfc2 2, mtc2 4, ctc2 6); lwc2 and swc2 are opcodes of their own. Cop0 and the
  // other coprocessors are not here: cop0 is rare in game code, and a compiled mtc0 could isolate
  // the cache or change the interrupt mask in the middle of a chain, which nothing here is ready
  // for.
  static bool IsGteCommand(uint32_t word) {
    return (word >> 26) == 0x12 && (word & (1u << 25)) != 0;
  }
  static bool IsGteMove(uint32_t word) {
    if ((word >> 26) != 0x12 || (word & (1u << 25)) != 0)
      return false;
    const uint32_t rs = (word >> 21) & 0x1F;
    return rs == 0 || rs == 2 || rs == 4 || rs == 6;
  }
  static bool IsGteMemory(uint32_t word) {
    const uint32_t opcode = word >> 26;
    return opcode == 0x32 || opcode == 0x3A;   // lwc2, swc2
  }

  // Whether this compiler admits the instruction - Compilable, IsSpecialOp when the host has said
  // it can answer for those, and the GTE's when it has said it can answer for them. The register
  // moves, lwc2 and swc2 are left to the interpreter while the host tracks PGXP's shadows
  // (set_track_moves): the interpreter carries one with each of them and compiled code does not.
  // A command carries none, so it compiles either way.
  bool CanCompile(uint32_t word) const {
    if (Compilable(word) || (special_ops_ && IsSpecialOp(word)))
      return true;
    // lwl, lwr, swl and swr, which have a fast path for RAM and leave the rest to the interpreter - so
    // they need RAM to be one of the things this compiler can reach directly.
    if (IsUnalignedOp(word) && direct_ram_bytes_ != 0)
      return true;
    if (!gte_ops_)
      return false;
    return IsGteCommand(word) || ((IsGteMove(word) || IsGteMemory(word)) && !track_moves_);
  }

  // Compile the GTE's instructions, calling BlockState::special for them. Off, the default, they
  // stay the interpreter's.
  void set_gte_ops(bool on) { gte_ops_ = on; }

  // Compile the trapping add and addi and the multiply/divide unit, calling BlockState::special
  // for what compiled code cannot do itself. Off, the default, they stay the interpreter's.
  void set_special_ops(bool on) { special_ops_ = on; }

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
      if (funct == 0x11 || funct == 0x13 || (funct >= 0x18 && funct <= 0x1B))
        return 0;                         // mthi, mtlo and mult/div write HI and LO, not rd
      return (word >> 11) & 0x1F;         // rd, including jalr's
    }
    if (opcode == 0x12) {                 // mfc2 and cfc2 load a register; nothing else of cop2 does
      const uint32_t rs = (word >> 21) & 0x1F;
      return ((word & (1u << 25)) == 0 && (rs == 0 || rs == 2)) ? (word >> 16) & 0x1F : 0;
    }
    switch (opcode) {
      case 0x03:                          // jal
        return 31;
      case 0x08: case 0x09: case 0x0A: case 0x0B:
      case 0x0C: case 0x0D: case 0x0E: case 0x0F:
      case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26:
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
    if (opcode == 0x12)                        // mfc2 and cfc2 deliver late, as a load does
      return Destination(word) != 0;
    switch (opcode) {
      case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25: case 0x26:
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
  static const int8_t kOffMove = static_cast<int8_t>(offsetof(BlockState, move));
  static const int8_t kOffRam = static_cast<int8_t>(offsetof(BlockState, ram));
  static const int8_t kOffExtraCycles =
      static_cast<int8_t>(offsetof(BlockState, extra_cycles));
  // The cast above would wrap a field past 127 into a negative displacement
  // without a word, and the code would read whatever sits before the state.
  static_assert(offsetof(BlockState, extra_cycles) <= 127,
                "every BlockState field has to be in reach of a disp8");
  static const int8_t kOffRamStore = static_cast<int8_t>(offsetof(BlockState, ram_store));
  static const int8_t kOffCodePages = static_cast<int8_t>(offsetof(BlockState, code_pages));
  static const int8_t kOffSpecial = static_cast<int8_t>(offsetof(BlockState, special));
  static const int8_t kOffBlockPc = static_cast<int8_t>(offsetof(BlockState, block_pc));
  // Past the first 128 bytes of BlockState, so it is addressed with a 32-bit displacement.
  static const int32_t kOffBailValue = static_cast<int32_t>(offsetof(BlockState, bail_value));
  static const int32_t kOffScratchpad = static_cast<int32_t>(offsetof(BlockState, scratchpad));
  static_assert(offsetof(BlockState, block_pc) <= 127 && offsetof(BlockState, ram_store) <= 127 &&
                offsetof(BlockState, code_pages) <= 127 &&
                offsetof(BlockState, special) <= 127,
                "every BlockState field has to be in reach of a disp8");

  // How far into the block the compiled code gets. Computed backwards, because
  // whether an instruction can be compiled depends on whether the one after it
  // can be: a branch needs its delay slot, and a load needs somewhere to land.
  static bool IsMemoryWord(uint32_t word) {
    const uint32_t opcode = word >> 26;
    return (opcode >= 0x20 && opcode <= 0x26) || (opcode >= 0x28 && opcode <= 0x2E) ||
           opcode == 0x32 || opcode == 0x3A;   // lwc2 and swc2 reach memory too
  }

  static bool IsLoadWord(uint32_t word) {
    const uint32_t opcode = word >> 26;
    return opcode >= 0x20 && opcode <= 0x26;   // including lwl and lwr
  }

  // A load, or mfc2/cfc2 into a register, which delivers its value an instruction late.
  static bool IsLateLoad(uint32_t word) {
    return NeedsFollower(word) && !IsBranchOrJump(word);
  }

  // Whether an instruction names register `reg` in any of the three fields registers go in. Wider than
  // "reads it": a jump's target bits and a coprocessor's fields are caught too, which only ever costs a
  // block it could have compiled.
  static bool NamesRegister(uint32_t word, uint32_t reg) {
    return ((word >> 21) & 0x1F) == reg || ((word >> 16) & 0x1F) == reg ||
           ((word >> 11) & 0x1F) == reg;
  }

  // A load in a branch's delay slot lands after the instruction the branch goes to - which is in another
  // block - so a block that ends with one can only write it out itself, at its end, which is before that
  // instruction instead of after: and that is the same thing only if that instruction cannot tell, by
  // reading the register, or writing it (it would win in one order and lose in the other). With one or
  // two places to go and a known word at each, that is a question the compiler can answer; with jr and
  // jalr, which go anywhere, it cannot, and the answer is no.
  static bool SlotLoadIsDeliverable(const DecodedBlock& block) {
    if (block.successor_count == 0 || block.instructions.empty())
      return false;
    const uint32_t reg = Destination(block.instructions.back().word);
    if (reg == 0)
      return false;
    for (int i = 0; i < block.successor_count; ++i) {
      const DecodedBlock::Successor& successor = block.successors[i];
      if (!successor.valid || NamesRegister(successor.word, reg))
        return false;
    }
    return true;
  }

  // Whether the compiled prefix ends in such a load, and so the block depends on its successors' words.
  static bool EndsWithSlotLoad(const DecodedBlock& block, uint32_t count) {
    const size_t size = block.instructions.size();
    return count == size && size >= 2 && IsBranchOrJump(block.instructions[size - 2].word) &&
           IsLateLoad(block.instructions[size - 1].word);
  }

  uint32_t CompilablePrefix(const DecodedBlock& block) const {
    size_t n = block.instructions.size();

    // A memory access whose fault would have to be taken while a load is still
    // in flight to a register it uses cannot be compiled: the value is written
    // out before the access so that a fault leaves a state the interpreter can
    // resume from, and that is only invisible when the access does not touch
    // the register. Stop the block before the load rather than before the
    // access, since a load needs a follower either way.
    for (size_t i = 1; i < n; ++i) {
      const uint32_t previous = block.instructions[i - 1].word;
      // What leaves a value in flight: a load, and mfc2 and cfc2, which deliver late the same way.
      const bool late_gte = IsGteMove(previous) && Destination(previous) != 0;
      if (!IsLoadWord(previous) && !late_gte)
        continue;
      const uint32_t pending = late_gte ? Destination(previous) : (previous >> 16) & 0x1F;
      const uint32_t word = block.instructions[i].word;
      if (IsMemoryWord(word) && MemoryOpBlockedByPendingLoad(word, pending)) {
        n = i - 1;
        break;
      }
    }

    std::vector<bool> ok(n, false);
    for (size_t i = n; i-- > 0;) {
      const uint32_t word = block.instructions[i].word;
      bool can = CanCompile(word);
      // Leaving the block at one of these in a branch's delay slot goes back to the branch, for the
      // interpreter to run with its slot; that is the same as having run it once only if running it
      // twice changes nothing, and `jalr rd, rs` with rd == rs reads the register it has already
      // overwritten.
      if (can && i > 0 && IsUnalignedOp(word)) {
        const uint32_t before = block.instructions[i - 1].word;
        if ((before >> 26) == 0 && (before & 0x3F) == 0x09 &&
            ((before >> 21) & 0x1F) == ((before >> 11) & 0x1F))
          can = false;
      }
      if (can && NeedsFollower(word)) {
        bool follower = (i + 1 < n) && ok[i + 1];
        // The one load that may be the last instruction of a block: the delay slot of the branch that
        // ends it, delivered by the block itself when the instruction at each place it goes next
        // (SlotLoadIsDeliverable) cannot tell.
        if (!follower && i + 1 == n && n == block.instructions.size() && IsLateLoad(word) &&
            i > 0 && IsBranchOrJump(block.instructions[i - 1].word) &&
            SlotLoadIsDeliverable(block))
          follower = true;
        can = follower;
      }
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
  //
  // Every rel8 here jumps forward over a short stretch of fixed code, but
  // "short" is a fact about today's encodings, not a guarantee. One that would
  // not reach marks the whole block as not compiled - Compile then claims none
  // of it and the engine leaves it to the interpreter - rather than leaving a
  // jump that lands somewhere else.
  void PatchRel8(CodeBlock* code, size_t jump_at, size_t target) {
    const ptrdiff_t displacement = static_cast<ptrdiff_t>(target) -
                                   static_cast<ptrdiff_t>(jump_at + 2);
    if (displacement < -128 || displacement > 127)
      rel8_out_of_reach_ = true;
    code->ptr8bit[jump_at + 1] =
        static_cast<uint8_t>(static_cast<int8_t>(displacement));
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
    if (opcode == 0x22 || opcode == 0x26) {
      EmitLwlLwr(instruction);   // a load: arms the load delay itself
      return;
    }
    if (opcode == 0x2A || opcode == 0x2E) {
      EmitSwlSwr(instruction);
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
    if (opcode == 0x12) {
      EmitCop2(instruction);   // flushes and arms the load delay itself, as a load does
      return;
    }
    if (opcode == 0x32) {
      EmitLwc2(instruction);
      return;
    }
    if (opcode == 0x3A) {
      EmitSwc2(instruction);
      FlushPending(Destination(word));
      return;
    }
    EmitImmediate(instruction);
    FlushPending(Destination(word));
  }

  // The way out of the block for lwl, lwr, swl and swr when the access is not one compiled code can
  // do: not main RAM the host has given, a page with compiled code in it, an address in no view of RAM.
  // The block leaves *before* the instruction, having charged what it has run, and says where the
  // interpreter picks up (BlockState::fault = 2, which Recompiler::Step reads as "interpret next_pc
  // once", and which is how the interpreter's exact timing, exceptions and watches come with it
  // rather than being written again here). In a branch's delay slot that is the branch, which the
  // interpreter runs again with its slot; the branch has done nothing that running it twice would
  // show, which CompilablePrefix saw to.
  void EmitUnalignedBail(const Instruction& instruction, bool deliver_pending = false) {
    // A load in flight that lwl or lwr was to merge into goes with the block as a register and a value.
    // It is not written to the register file: the instruction after the interpreter's lwl would see it
    // there, and the merge is to replace it. The interpreter still has to read it as the register's
    // value in flight, so Recompiler::Step hands it to the host to put back in the pipeline.
    uint32_t fault = BlockState::kBail;
    if (deliver_pending && pending_active_) {
      x86::MovMemRegDisp32(emitter_, kPending, kStatePtr, kOffBailValue);
      fault |= pending_reg_ << BlockState::kBailRegShift;
    }
    const uint32_t before = instruction.in_delay_slot ? 1u : 0u;
    const uint32_t executed = ((instruction.pc - block_pc_) >> 2) - before;
    if (executed != 0)
      x86::SubMemImm8(emitter_, kStatePtr, kOffBudget, static_cast<uint8_t>(executed));
    x86::MovMemImm(emitter_, kStatePtr, kOffNextPc, instruction.pc - before * 4);
    x86::MovMemImm(emitter_, kStatePtr, kOffFault, fault);
    fault_exits_.push_back(code_->cursor);
    x86::JmpRel32(emitter_, 0);
  }

  // lwl and lwr. The aligned word is read as lw reads one, with its stall; the bytes from the addressed
  // one to the word's end replace the same count of rt's, and rt is read as the register is about to be -
  // a load that was in flight to it lands first, which is how the usual `lwl t0, 3(a0)` / `lwr t0, 0(a0)`
  // pair works with nothing between. The lane is the address's low two bits, known only when it runs,
  // so the shifts take their count from CL: lwl keeps 0x00FFFFFF >> (8 * lane) of rt and ors in the word
  // shifted left by 24 - 8 * lane, lwr the word shifted right by 8 * lane over the top bytes
  // 0xFFFFFF00 << (24 - 8 * lane) of rt; and 24 - n is n ^ 24 for the four multiples of 8 there are.
  void EmitLwlLwr(const Instruction& instruction) {
    const uint32_t word = instruction.word;
    const bool left = (word >> 26) == 0x22;
    const uint32_t rs = (word >> 21) & 0x1F;
    const uint32_t rt = (word >> 16) & 0x1F;
    const uint16_t immediate = static_cast<uint16_t>(word & 0xFFFF);

    // A load in flight to rt is what this merges into, read from where it is: it is not written to the
    // register, because the merge replaces it - the interpreter drops the first of two loads to a register
    // (ArmLoad), so the instruction after sees the register as it was before both, and writing it out
    // early would show it the first. Any other load in flight lands now, as before a memory access.
    const bool forwarded = pending_active_ && pending_reg_ == rt;
    if (!forwarded)
      FlushPendingBeforeMemory();
    EmitAddress(rs, immediate);

    size_t to_bail[16];
    int to_bail_count = 0;
    auto jump_to_bail = [&](x86::Cc condition) {
      to_bail[to_bail_count++] = code_->cursor;
      x86::JccRel32(emitter_, condition, 0);
    };
    size_t to_scratch[2];
    int to_scratch_count = 0;
    auto jump_to_scratch = [&](x86::Cc condition) {
      if (direct_scratch_bytes_ == 0) {
        jump_to_bail(condition);
        return;
      }
      to_scratch[to_scratch_count++] = code_->cursor;
      x86::JccRel32(emitter_, condition, 0);
    };
    EmitDirectRamPrologue(kOffRam, jump_to_bail, jump_to_scratch);
    if (direct_ram_window_ > direct_ram_bytes_)
      x86::AluRegImm(emitter_, x86::AluImmOp::kAnd, kScratchB, direct_ram_bytes_ - 1);
    x86::AluRegImm(emitter_, x86::AluImmOp::kAnd, kScratchB, 0xFFFFFFFCu);   // the aligned word
    x86::Add64RegReg(emitter_, kScratchA, kScratchB);
    x86::MovRegMem(emitter_, kScratchA, kScratchA, 0);
    if (direct_ram_read_cycles_ != 0)
      x86::AddMemImm8(emitter_, kStatePtr, kOffExtraCycles, direct_ram_read_cycles_);

    const size_t merge = code_->cursor;                                      // where the scratchpad's read rejoins
    x86::MovRegReg(emitter_, kScratchB, kScratchC);                          // CL = 8 * lane
    x86::AluRegImm(emitter_, x86::AluImmOp::kAnd, kScratchB, 3);
    x86::ShiftRegImm(emitter_, x86::ShiftOp::kShl, kScratchB, 3);
    if (forwarded)
      x86::MovRegReg(emitter_, kArg3, kPending);
    else
      LoadReg(kArg3, rt);
    if (left) {
      x86::MovRegImm(emitter_, kArg4, 0x00FFFFFFu);
      x86::ShiftRegCl(emitter_, x86::ShiftOp::kShr, kArg4);                  // what of rt is kept
      x86::AluRegReg(emitter_, x86::AluOp::kAnd, kArg3, kArg4);
      x86::AluRegImm(emitter_, x86::AluImmOp::kXor, kScratchB, 24);
      x86::ShiftRegCl(emitter_, x86::ShiftOp::kShl, kScratchA);              // the word, moved up
    } else {
      x86::ShiftRegCl(emitter_, x86::ShiftOp::kShr, kScratchA);              // the word, moved down
      x86::AluRegImm(emitter_, x86::AluImmOp::kXor, kScratchB, 24);
      x86::MovRegImm(emitter_, kArg4, 0xFFFFFF00u);
      x86::ShiftRegCl(emitter_, x86::ShiftOp::kShl, kArg4);                  // what of rt is kept
      x86::AluRegReg(emitter_, x86::AluOp::kAnd, kArg3, kArg4);
    }
    x86::AluRegReg(emitter_, x86::AluOp::kOr, kScratchA, kArg3);

    const size_t past_bail = code_->cursor;
    x86::JmpRel8(emitter_, 0);
    if (to_scratch_count != 0) {
      // The scratchpad: the aligned word, no stall, and the same merge.
      for (int i = 0; i < to_scratch_count; ++i)
        PatchJcc32(to_scratch[i], code_->cursor);
      EmitScratchpadAddress(jump_to_bail, true);
      x86::MovRegMem(emitter_, kScratchA, kScratchA, 0);
      JumpBack8(merge);
    }
    for (int i = 0; i < to_bail_count; ++i)
      PatchJcc32(to_bail[i], code_->cursor);
    EmitUnalignedBail(instruction, forwarded);
    PatchRel8(code_, past_bail, code_->cursor);

    // The merged word arrives an instruction late, as a load's does.
    FlushPending(rt);
    if (rt != 0) {
      x86::Mov64RegReg(emitter_, kPending, kScratchA);
      pending_active_ = true;
      pending_reg_ = rt;
    }
  }

  // swl and swr: the aligned word is read (with no stall, which is how the interpreter's read-to-merge
  // is charged), rt's bytes from one end to the lane replace the same count of its own, and the word is
  // written back - to a page with no compiled code, as sw writes one. The merge, by lane = 8 * lane in CL:
  // swl keeps 0xFFFFFF00 << (8 * lane) of the word and ors in rt >> (24 - 8 * lane); swr keeps
  // 0x00FFFFFF >> (24 - 8 * lane) and ors in rt << (8 * lane).
  void EmitSwlSwr(const Instruction& instruction) {
    const uint32_t word = instruction.word;
    const bool left = (word >> 26) == 0x2A;
    const uint32_t rs = (word >> 21) & 0x1F;
    const uint32_t rt = (word >> 16) & 0x1F;
    const uint16_t immediate = static_cast<uint16_t>(word & 0xFFFF);

    FlushPendingBeforeMemory();
    EmitAddress(rs, immediate);

    size_t to_bail[16];
    int to_bail_count = 0;
    auto jump_to_bail = [&](x86::Cc condition) {
      to_bail[to_bail_count++] = code_->cursor;
      x86::JccRel32(emitter_, condition, 0);
    };
    size_t to_scratch[2];
    int to_scratch_count = 0;
    auto jump_to_scratch = [&](x86::Cc condition) {
      if (direct_scratch_bytes_ == 0) {
        jump_to_bail(condition);
        return;
      }
      to_scratch[to_scratch_count++] = code_->cursor;
      x86::JccRel32(emitter_, condition, 0);
    };
    EmitDirectRamPrologue(kOffRamStore, jump_to_bail, jump_to_scratch);
    x86::MovRegReg(emitter_, kArg4, kScratchB);                              // the page's bit, as sw reads it
    x86::ShiftRegImm(emitter_, x86::ShiftOp::kShr, kArg4, BlockCache::kPageShift);
    x86::Mov64RegMem(emitter_, kScratchD, kStatePtr, kOffCodePages);
    x86::BtMemReg(emitter_, kScratchD, kArg4);
    jump_to_bail(x86::Cc::kBelow);                                           // set: code lives here
    if (direct_ram_window_ > direct_ram_bytes_)
      x86::AluRegImm(emitter_, x86::AluImmOp::kAnd, kScratchB, direct_ram_bytes_ - 1);
    x86::AluRegImm(emitter_, x86::AluImmOp::kAnd, kScratchB, 0xFFFFFFFCu);   // the aligned word
    x86::Add64RegReg(emitter_, kScratchA, kScratchB);
    x86::MovRegMem(emitter_, kScratchD, kScratchA, 0);                       // what is there

    const size_t merge = code_->cursor;                                      // where the scratchpad's read rejoins
    x86::MovRegReg(emitter_, kScratchB, kScratchC);                          // CL = 8 * lane
    x86::AluRegImm(emitter_, x86::AluImmOp::kAnd, kScratchB, 3);
    x86::ShiftRegImm(emitter_, x86::ShiftOp::kShl, kScratchB, 3);
    LoadReg(kArg3, rt);
    if (left) {
      x86::MovRegImm(emitter_, kArg4, 0xFFFFFF00u);
      x86::ShiftRegCl(emitter_, x86::ShiftOp::kShl, kArg4);                  // what of the word is kept
      x86::AluRegReg(emitter_, x86::AluOp::kAnd, kScratchD, kArg4);
      x86::AluRegImm(emitter_, x86::AluImmOp::kXor, kScratchB, 24);
      x86::ShiftRegCl(emitter_, x86::ShiftOp::kShr, kArg3);                  // rt, moved down
    } else {
      x86::MovRegImm(emitter_, kArg4, 0x00FFFFFFu);
      x86::AluRegImm(emitter_, x86::AluImmOp::kXor, kScratchB, 24);
      x86::ShiftRegCl(emitter_, x86::ShiftOp::kShr, kArg4);                  // what of the word is kept
      x86::AluRegReg(emitter_, x86::AluOp::kAnd, kScratchD, kArg4);
      x86::AluRegImm(emitter_, x86::AluImmOp::kXor, kScratchB, 24);          // back to 8 * lane
      x86::ShiftRegCl(emitter_, x86::ShiftOp::kShl, kArg3);                  // rt, moved up
    }
    x86::AluRegReg(emitter_, x86::AluOp::kOr, kScratchD, kArg3);
    x86::MovMem32Reg(emitter_, kScratchA, kScratchD);

    const size_t past_bail = code_->cursor;
    x86::JmpRel8(emitter_, 0);
    if (to_scratch_count != 0) {
      // The scratchpad: no page to look up, the word read the same way, the merge and the write shared.
      for (int i = 0; i < to_scratch_count; ++i)
        PatchJcc32(to_scratch[i], code_->cursor);
      EmitScratchpadAddress(jump_to_bail, true);
      x86::MovRegMem(emitter_, kScratchD, kScratchA, 0);
      JumpBack8(merge);
    }
    for (int i = 0; i < to_bail_count; ++i)
      PatchJcc32(to_bail[i], code_->cursor);
    EmitUnalignedBail(instruction);
    PatchRel8(code_, past_bail, code_->cursor);
  }

  // Coprocessor 2. A command and the register moves go to BlockState::special with what the host
  // needs - the register number or the instruction in arg 4, the value to write in arg 3. A command
  // and a read have to wait for the command before them by the machine's clock, which the host does
  // from `elapsed`; mfc2 and cfc2 then deliver late, as a load does, so they arm the load delay the
  // way EmitLoad does.
  void EmitCop2(const Instruction& instruction) {
    const uint32_t word = instruction.word;
    const uint32_t rs = (word >> 21) & 0x1F;
    const uint32_t rt = (word >> 16) & 0x1F;
    const uint32_t rd = (word >> 11) & 0x1F;

    if (IsGteCommand(word)) {
      x86::MovRegImm(emitter_, kArg4, word);
      EmitSpecialCall(instruction, kSpecialGteCommand);
      FlushPending(0);
      return;
    }
    switch (rs) {
      case 0x00:   // mfc2
      case 0x02:   // cfc2
        x86::MovRegImm(emitter_, kArg4, rd);
        EmitSpecialCall(instruction, rs == 0x00 ? kSpecialGteMfc2 : kSpecialGteCfc2);
        FlushPending(rt);   // a load already in flight lands now, unless this writes its register
        if (rt != 0) {
          x86::Mov64RegReg(emitter_, kPending, kScratchA);
          pending_active_ = true;
          pending_reg_ = rt;
        }
        return;
      case 0x04:   // mtc2
      case 0x06:   // ctc2
        LoadReg(kArg3, rt);
        x86::MovRegImm(emitter_, kArg4, rd);
        EmitSpecialCall(instruction, rs == 0x04 ? kSpecialGteMtc2 : kSpecialGteCtc2);
        FlushPending(0);
        return;
      default:
        return;   // CanCompile admitted nothing else
    }
  }

  // lwc2: a word loaded as lw loads one, the way EmitLoad loads it, and then written to GTE data
  // register rt instead of to a register of the CPU's - at once, with no delay slot to wait out.
  void EmitLwc2(const Instruction& instruction) {
    const uint32_t word = instruction.word;
    const uint32_t rs = (word >> 21) & 0x1F;
    const uint32_t rt = (word >> 16) & 0x1F;
    const uint16_t immediate = static_cast<uint16_t>(word & 0xFFFF);

    FlushPendingBeforeMemory();
    EmitAddress(rs, immediate);
    size_t past_call = 0;
    if (direct_ram_bytes_ != 0)
      past_call = EmitDirectRamRead(0x23);
    EmitCall(kOffLoad32, instruction.pc, kArg3);   // (context, address, pc)
    EmitFaultCheck(code_, block_start_);
    if (direct_ram_bytes_ != 0)
      PatchRel8(code_, past_call, code_->cursor);

    x86::MovRegReg(emitter_, kArg3, kScratchA);    // the word
    x86::MovRegImm(emitter_, kArg4, rt);           // the GTE register
    EmitSpecialCall(instruction, kSpecialGteLoad);
  }

  // swc2: GTE data register rt, read first - the address is worked out after, since the read is a
  // call that does not keep EDX - and stored as sw stores a word, the way EmitStore stores it.
  void EmitSwc2(const Instruction& instruction) {
    const uint32_t word = instruction.word;
    const uint32_t rs = (word >> 21) & 0x1F;
    const uint32_t rt = (word >> 16) & 0x1F;
    const uint16_t immediate = static_cast<uint16_t>(word & 0xFFFF);

    FlushPendingBeforeMemory();
    x86::MovRegImm(emitter_, kArg4, rt);
    EmitSpecialCall(instruction, kSpecialGteStore);
    x86::MovRegReg(emitter_, kArg3, kScratchA);    // the value, whole
    EmitAddress(rs, immediate);
    size_t past_call = 0;
    if (direct_ram_bytes_ != 0)
      past_call = EmitDirectRamWrite(0x2B);
    EmitCall(kOffStore32, instruction.pc, kArg4);  // (context, address, value, pc)
    EmitFaultCheck(code_, block_start_);
    if (direct_ram_bytes_ != 0)
      PatchRel8(code_, past_call, code_->cursor);
  }

  void EmitImmediate(const Instruction& instruction) {
    const uint32_t word = instruction.word;
    const uint32_t opcode = word >> 26;
    const uint32_t rs = (word >> 21) & 0x1F;
    const uint32_t rt = (word >> 16) & 0x1F;
    const uint16_t immediate = static_cast<uint16_t>(word & 0xFFFF);

    switch (opcode) {
      case 0x08:   // addi - addiu, but a signed overflow raises an exception instead
        LoadReg(kScratchA, rs);
        x86::AluRegImm(emitter_, x86::AluImmOp::kAdd, kScratchA, SignExtend(immediate));
        EmitOverflowCheck(instruction, rt);
        return;

      case 0x09:   // addiu rt, rs, imm - sign-extended, and does not trap
        LoadReg(kScratchA, rs);
        x86::AluRegImm(emitter_, x86::AluImmOp::kAdd, kScratchA, SignExtend(immediate));
        StoreReg(rt, kScratchA);
        if (immediate == 0)
          EmitMove(rt, rs);
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
        if (immediate == 0)
          EmitMove(rt, rs);
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

      case 0x20:   // add - addu, but a signed overflow raises an exception instead
        LoadReg(kScratchA, rs);
        LoadReg(kScratchB, rt);
        x86::AluRegReg(emitter_, x86::AluOp::kAdd, kScratchA, kScratchB);
        EmitOverflowCheck(instruction, rd);
        break;

      case 0x10: case 0x12:   // mfhi, mflo
        EmitSpecialCall(instruction, funct);
        StoreReg(rd, kScratchA);
        break;
      case 0x11: case 0x13:   // mthi, mtlo
        LoadReg(kArg3, rs);
        EmitSpecialCall(instruction, funct);
        break;
      case 0x18: case 0x19: case 0x1A: case 0x1B:   // mult, multu, div, divu
        LoadReg(kArg3, rs);
        LoadReg(kArg4, rt);
        EmitSpecialCall(instruction, funct);
        break;

      case 0x21:   // addu
        EmitAlu(x86::AluOp::kAdd, rd, rs, rt);
        EmitMoveIfCopy(rd, rs, rt);
        break;
      case 0x23:   // subu
        EmitAlu(x86::AluOp::kSub, rd, rs, rt);
        break;
      case 0x24:   // and
        EmitAlu(x86::AluOp::kAnd, rd, rs, rt);
        break;
      case 0x25:   // or
        EmitAlu(x86::AluOp::kOr, rd, rs, rt);
        EmitMoveIfCopy(rd, rs, rt);
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

  // The end of a trapping add: the sum is in EAX and the flags are the add's. No overflow, and it
  // is stored to `destination`; overflow, and the host raises the exception and the block leaves,
  // by the same exit a faulting memory access takes, with the destination untouched.
  //
  // The call is off to one side and falls through to a jump out, so the common path is the add,
  // one jump not taken and the store. A load still in flight is written to its register on the
  // way out, as a memory access does before it can fault - only on this path, where it is the
  // state the interpreter has to resume from, and not on the one that carries on, where the load
  // lands after the add as it should.
  void EmitOverflowCheck(const Instruction& instruction, uint32_t destination) {
    const size_t no_overflow = code_->cursor;
    x86::JccRel8(emitter_, x86::Cc::kNoOverflow, 0);

    if (pending_active_)
      StoreReg(pending_reg_, kPending);
    x86::MovRegImm(emitter_, kScratchC, kSpecialOverflow);                // arg 2: the operation
    x86::MovRegImm(emitter_, kArg4, instruction.in_delay_slot ? 1 : 0);   // arg 4: a delay slot?
    EmitCall(kOffSpecial, instruction.pc, kArg3);                         // arg 3: where
    fault_exits_.push_back(code_->cursor);
    x86::JmpRel32(emitter_, 0);                                           // the host set the fault

    PatchRel8(code_, no_overflow, code_->cursor);
    StoreReg(destination, kScratchA);
  }

  // A call to BlockState::special for the multiply and divide unit: the operation and how far into
  // the block this is in arg 2, the operands already in arg 3 and 4 (R8D and R9D), and a read's
  // value coming back in EAX. Nothing live is in a volatile register between instructions, and the
  // callee does not touch the guest registers - it takes values and returns one.
  void EmitSpecialCall(const Instruction& instruction, uint32_t funct) {
    const uint32_t index = (instruction.pc - block_pc_) >> 2;
    x86::MovRegImm(emitter_, kScratchC, funct | (index << 8));
    EmitCall(kOffSpecial, instruction.pc, kScratchD);   // the pc register is not one it reads
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
    size_t past_call = 0;
    if (direct_ram_bytes_ != 0)
      past_call = EmitDirectRamRead(opcode);
    EmitCall(function, instruction.pc, kArg3);   // (context, address, pc)
    EmitFaultCheck(code_, block_start_);
    if (direct_ram_bytes_ != 0)
      PatchRel8(code_, past_call, code_->cursor);

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
    size_t past_call = 0;
    if (direct_ram_bytes_ != 0)
      past_call = EmitDirectRamWrite(opcode);
    EmitCall(function, instruction.pc, kArg4);   // (context, address, value, pc)
    EmitFaultCheck(code_, block_start_);
    if (direct_ram_bytes_ != 0)
      PatchRel8(code_, past_call, code_->cursor);
  }

  // A load reading main RAM itself, with the address in EDX: the value lands in
  // EAX, zero-extended as a callback returns it, and the stall the callback
  // would have charged goes on BlockState::extra_cycles. The call EmitLoad
  // emits next is the way out for everything else, and the returned offset is
  // the jump over it, for EmitLoad to point past the call once it is emitted.
  //
  // The read is only right when Cpu::Load would have done nothing but read, so
  // anything it would decode differently goes to the call:
  //   - the host saying not now - BlockState::ram is null;
  //   - an address outside KUSEG's first 512 MB, KSEG0 and KSEG1, the three
  //     views of the physical memory (KSEG2, and the rest of KUSEG, are the
  //     callback's to decode);
  //   - a physical address at or past the end of RAM - another region, or a
  //     mirror;
  //   - a misaligned word or halfword, an address error the callback raises.
  //
  // Only EAX, ECX and EDX are touched, which a call would have clobbered
  // anyway, and only forward rel8 jumps are emitted - PatchRel8 checks them.
  size_t EmitDirectRamRead(uint32_t opcode) {
    size_t to_call[16];
    int to_call_count = 0;
    auto jump_to_call = [&](x86::Cc condition) {
      to_call[to_call_count++] = code_->cursor;
      x86::JccRel8(emitter_, condition, 0);
    };

    size_t to_scratch[2];
    int to_scratch_count = 0;
    auto jump_to_scratch = [&](x86::Cc condition) {
      if (direct_scratch_bytes_ == 0) {
        jump_to_call(condition);
        return;
      }
      to_scratch[to_scratch_count++] = code_->cursor;
      x86::JccRel8(emitter_, condition, 0);
    };
    EmitDirectRamPrologue(kOffRam, jump_to_call, jump_to_scratch);

    if (opcode == 0x23 || opcode == 0x21 || opcode == 0x25) {
      x86::TestRegImm(emitter_, kScratchC, opcode == 0x23 ? 3 : 1);
      jump_to_call(x86::Cc::kNotEqual);
    }

    if (direct_ram_window_ > direct_ram_bytes_)                  // a mirror is the same bytes
      x86::AluRegImm(emitter_, x86::AluImmOp::kAnd, kScratchB, direct_ram_bytes_ - 1);
    x86::Add64RegReg(emitter_, kScratchA, kScratchB);            // RAM + physical
    if (opcode == 0x20 || opcode == 0x24)
      x86::MovzxRegMem8(emitter_, kScratchA, kScratchA, 0);
    else if (opcode == 0x21 || opcode == 0x25)
      x86::MovzxRegMem16(emitter_, kScratchA, kScratchA, 0);
    else
      x86::MovRegMem(emitter_, kScratchA, kScratchA, 0);
    if (direct_ram_read_cycles_ != 0) {
      x86::AddMemImm8(emitter_, kStatePtr, kOffExtraCycles,
                      direct_ram_read_cycles_);
    }

    const size_t past_call = code_->cursor;
    x86::JmpRel8(emitter_, 0);
    if (to_scratch_count != 0) {
      // The scratchpad: the same read at no stall, so no cycles are added (Cpu::Load charges 0 there),
      // and back to the jump over the call that RAM's read just made.
      for (int i = 0; i < to_scratch_count; ++i)
        PatchRel8(code_, to_scratch[i], code_->cursor);
      if (opcode == 0x23 || opcode == 0x21 || opcode == 0x25) {
        x86::TestRegImm(emitter_, kScratchC, opcode == 0x23 ? 3 : 1);
        jump_to_call(x86::Cc::kNotEqual);
      }
      EmitScratchpadAddress(jump_to_call);
      if (opcode == 0x20 || opcode == 0x24)
        x86::MovzxRegMem8(emitter_, kScratchA, kScratchA, 0);
      else if (opcode == 0x21 || opcode == 0x25)
        x86::MovzxRegMem16(emitter_, kScratchA, kScratchA, 0);
      else
        x86::MovRegMem(emitter_, kScratchA, kScratchA, 0);
      JumpBack8(past_call);
    }
    for (int i = 0; i < to_call_count; ++i)
      PatchRel8(code_, to_call[i], code_->cursor);
    return past_call;
  }

  // What a direct read and a direct write both start with, the address being in EDX: RAM's
  // base into RAX, or out through `jump_to_call` if the host gave none (`ram_offset` is which
  // of BlockState's two); out again unless the address is in KUSEG's first 512 MB, KSEG0 or
  // KSEG1; and the physical address into ECX, out unless it is inside RAM's window.
  //
  // The last check, "inside RAM's window", leaves through `jump_out_of_window` rather than
  // `jump_to_call`: that is where the scratchpad is tried, when it is compiled (EmitScratchpad).
  template <typename JumpToCall, typename JumpOutOfWindow>
  void EmitDirectRamPrologue(int8_t ram_offset, JumpToCall& jump_to_call,
                             JumpOutOfWindow& jump_out_of_window) {
    x86::Mov64RegMem(emitter_, kScratchA, kStatePtr, ram_offset);   // mov rax, [rbx+ram]
    x86::Test64RegReg(emitter_, kScratchA);
    jump_to_call(x86::Cc::kEqual);

    x86::MovRegReg(emitter_, kScratchB, kScratchC);              // the segment
    x86::ShiftRegImm(emitter_, x86::ShiftOp::kShr, kScratchB, 29);
    const size_t kuseg = code_->cursor;
    x86::JccRel8(emitter_, x86::Cc::kEqual, 0);                  // the shift set ZF
    x86::AluRegImm(emitter_, x86::AluImmOp::kCmp, kScratchB, 4);
    const size_t kseg0 = code_->cursor;
    x86::JccRel8(emitter_, x86::Cc::kEqual, 0);
    x86::AluRegImm(emitter_, x86::AluImmOp::kCmp, kScratchB, 5);
    jump_to_call(x86::Cc::kNotEqual);
    PatchRel8(code_, kuseg, code_->cursor);
    PatchRel8(code_, kseg0, code_->cursor);

    x86::MovRegReg(emitter_, kScratchB, kScratchC);              // the physical address
    x86::AluRegImm(emitter_, x86::AluImmOp::kAnd, kScratchB, 0x1FFFFFFF);
    x86::AluRegImm(emitter_, x86::AluImmOp::kCmp, kScratchB, direct_ram_window_);
    jump_out_of_window(x86::Cc::kAboveEqual);
  }

  // What a direct access does past RAM's window, when the scratchpad is compiled: the physical address
  // still in ECX, minus the scratchpad's base, has to be inside its bytes, and the host has to have
  // given it - then RAX is where it is, and the caller reads or writes there. Anything else is the
  // caller's way out, as in the prologue. Misses in two instructions what is not the scratchpad.
  // Only called with the prologue's jumps as they were: RAM given, a view of the physical memory.
  // `word_aligned` takes the offset down to its word, for the unaligned pairs.
  template <typename JumpToCall>
  void EmitScratchpadAddress(JumpToCall& jump_to_call, bool word_aligned = false) {
    x86::AluRegImm(emitter_, x86::AluImmOp::kSub, kScratchB, direct_scratch_base_);
    x86::AluRegImm(emitter_, x86::AluImmOp::kCmp, kScratchB, direct_scratch_bytes_);
    jump_to_call(x86::Cc::kAboveEqual);
    if (word_aligned)
      x86::AluRegImm(emitter_, x86::AluImmOp::kAnd, kScratchB, 0xFFFFFFFCu);
    x86::Mov64RegMemDisp32(emitter_, kScratchA, kStatePtr, kOffScratchpad);
    x86::Test64RegReg(emitter_, kScratchA);
    jump_to_call(x86::Cc::kEqual);
    x86::Add64RegReg(emitter_, kScratchA, kScratchB);
  }

  // Points a JccRel32 emitted at `site` at `target`.
  void PatchJcc32(size_t site, size_t target) {
    PatchRel32(code_, site + 2,
               static_cast<int32_t>(static_cast<ptrdiff_t>(target) - static_cast<ptrdiff_t>(site + 6)));
  }

  // A jump back to `target`, which is behind it (PatchRel8 takes either direction).
  void JumpBack8(size_t target) {
    const size_t site = code_->cursor;
    x86::JmpRel8(emitter_, 0);
    PatchRel8(code_, site, target);
  }

  // A store writing main RAM itself, with the address in EDX and the value in R8D - EmitStore's
  // arguments - and the same shape as EmitDirectRamRead: the returned offset is the jump over
  // the call EmitStore emits next, which stays the way out for everything else.
  //
  // Right only when Cpu::Store would have done nothing but write, so it also goes to the call
  // for what a read would not mind:
  //   - the host saying not now - BlockState::ram_store is null;
  //   - a misaligned word or halfword, an address error the callback raises;
  //   - a page that compiled code was built from (BlockState::code_pages): the callback
  //     writes it and discards whatever blocks the word belonged to, which is not something
  //     to do from here. Data beside code in the same 4 KB takes the call as well, as it
  //     always did.
  //
  // EAX, ECX, EDX, R9 and R10 are touched, all of which a call would have clobbered anyway;
  // R8 holds the value and is left alone until the write.
  size_t EmitDirectRamWrite(uint32_t opcode) {
    size_t to_call[16];
    int to_call_count = 0;
    auto jump_to_call = [&](x86::Cc condition) {
      to_call[to_call_count++] = code_->cursor;
      x86::JccRel8(emitter_, condition, 0);
    };

    size_t to_scratch[2];
    int to_scratch_count = 0;
    auto jump_to_scratch = [&](x86::Cc condition) {
      if (direct_scratch_bytes_ == 0) {
        jump_to_call(condition);
        return;
      }
      to_scratch[to_scratch_count++] = code_->cursor;
      x86::JccRel8(emitter_, condition, 0);
    };
    EmitDirectRamPrologue(kOffRamStore, jump_to_call, jump_to_scratch);

    if (opcode == 0x2B || opcode == 0x29) {
      x86::TestRegImm(emitter_, kScratchC, opcode == 0x2B ? 3 : 1);
      jump_to_call(x86::Cc::kNotEqual);
    }

    // The page's bit, from the physical address as the cache keys it - mirrors included, as
    // BlockCache::IsCodePage reads it.
    x86::MovRegReg(emitter_, kArg4, kScratchB);                  // mov r9d, ecx
    x86::ShiftRegImm(emitter_, x86::ShiftOp::kShr, kArg4, BlockCache::kPageShift);
    x86::Mov64RegMem(emitter_, kScratchD, kStatePtr, kOffCodePages);   // mov r10, [rbx+pages]
    x86::BtMemReg(emitter_, kScratchD, kArg4);                   // bt [r10], r9
    jump_to_call(x86::Cc::kBelow);                               // set: there is code here

    if (direct_ram_window_ > direct_ram_bytes_)
      x86::AluRegImm(emitter_, x86::AluImmOp::kAnd, kScratchB, direct_ram_bytes_ - 1);
    x86::Add64RegReg(emitter_, kScratchA, kScratchB);            // RAM + physical
    if (opcode == 0x28)
      x86::MovMem8Reg(emitter_, kScratchA, kArg3);
    else if (opcode == 0x29)
      x86::MovMem16Reg(emitter_, kScratchA, kArg3);
    else
      x86::MovMem32Reg(emitter_, kScratchA, kArg3);

    const size_t past_call = code_->cursor;
    x86::JmpRel8(emitter_, 0);
    if (to_scratch_count != 0) {
      // The scratchpad, which no compiled code is ever fetched from: no page to look up.
      for (int i = 0; i < to_scratch_count; ++i)
        PatchRel8(code_, to_scratch[i], code_->cursor);
      if (opcode == 0x2B || opcode == 0x29) {
        x86::TestRegImm(emitter_, kScratchC, opcode == 0x2B ? 3 : 1);
        jump_to_call(x86::Cc::kNotEqual);
      }
      EmitScratchpadAddress(jump_to_call);
      if (opcode == 0x28)
        x86::MovMem8Reg(emitter_, kScratchA, kArg3);
      else if (opcode == 0x29)
        x86::MovMem16Reg(emitter_, kScratchA, kArg3);
      else
        x86::MovMem32Reg(emitter_, kScratchA, kArg3);
      JumpBack8(past_call);
    }
    for (int i = 0; i < to_call_count; ++i)
      PatchRel8(code_, to_call[i], code_->cursor);
    return past_call;
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
    // lwl and lwr merge into rt as the register will be, so a load still arriving there is what
    // they are meant to see - the usual back-to-back pair - and only an address register in flight
    // is in the way. (A store's rt is its value, read as it is now: in the way too.)
    const uint32_t opcode = word >> 26;
    if (opcode == 0x22 || opcode == 0x26)
      return pending == rs;
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
  // The guest pc goes in the callback's last argument - `pc_register`, the
  // third for a load and the fourth for a store, whose third is the value: the
  // host needs it to point an exception at the right instruction. Until phase 6
  // of Docs/Hardware-Renderer-Plan.md every call put it in the fourth, so a load
  // was handed whatever R8 held (bug 126).
  void EmitCall(int8_t function_offset, uint32_t pc, uint8_t pc_register) {
    x86::MovRegImm(emitter_, pc_register, pc);
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

  // BlockState::move(context, to, from), after the copy itself, while the host
  // tracks moves. Nothing is live in the volatile registers between guest
  // instructions - the pending load and allocated registers are callee-saved -
  // so the call needs no saving around it.
  void EmitMove(uint32_t to, uint32_t from) {
    if (!track_moves_ || to == 0)
      return;
    x86::MovRegImm(emitter_, kScratchC, to);                         // arg 2
    x86::MovRegImm(emitter_, kArg3, from);                           // arg 3
    x86::Mov64RegMem(emitter_, kScratchB, kStatePtr, kOffContext);   // arg 1
    x86::Mov64RegMem(emitter_, kScratchA, kStatePtr, kOffMove);
    x86::CallReg(emitter_, kScratchA);
  }

  // addu and or are copies when either operand is r0 - rt first, as the
  // interpreter's ADDU and OR decide it.
  void EmitMoveIfCopy(uint32_t rd, uint32_t rs, uint32_t rt) {
    if (rt == 0)
      EmitMove(rd, rs);
    else if (rs == 0)
      EmitMove(rd, rt);
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
  bool track_moves_ = false;
  uint32_t successors_[2] = {};
  int successor_count_ = 0;

  // set_direct_ram: how much of physical memory loads may read themselves, and
  // the cycles each read owes. Zero bytes compiles every load as a call.
  uint32_t direct_ram_bytes_ = 0;
  uint8_t direct_ram_read_cycles_ = 0;
  uint32_t direct_ram_window_ = 0;
  uint32_t direct_scratch_base_ = 0;
  uint32_t direct_scratch_bytes_ = 0;

  // set_special_ops, and where the block being compiled starts - an instruction's index in it is
  // what a call to BlockState::special reports.
  bool special_ops_ = false;
  bool gte_ops_ = false;
  uint32_t block_pc_ = 0;

  // Set by PatchRel8 when a jump would not reach, for Compile to see.
  bool rel8_out_of_reach_ = false;

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
