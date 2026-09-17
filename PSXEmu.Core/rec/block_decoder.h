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

// Where a compiled block starts and stops, and what is in it.
//
// Step 2 of Docs/Recompiler-Plan.md: the boundary rules, the delay slots and
// the cycle accounting, with no code generation behind them yet. Getting this
// wrong is how a recompiler ends up executing an instruction twice or not at
// all, and it is far easier to debug here - where a block is a vector anyone
// can print - than inside emitted machine code.
//
// Knows nothing about psx/: guest words arrive through a callback, so this
// decodes a hand-written sequence in a test exactly as it will decode RAM.
//
// What a block is, on this machine: instructions from an entry point up to and
// including the first branch or jump *and its delay slot*, because the delay
// slot runs whichever way the branch goes and belongs to the block that
// contains the branch. A block also ends at anything that leaves through an
// exception - syscall, break - and at a length cap, so a run of straight-line
// code does not compile into one enormous block.

#include <cstdint>
#include <functional>
#include <vector>

namespace emulation {
namespace rec {

// What an instruction is, to the extent the decoder cares. This is not a
// disassembler: it is the minimum needed to find the end of a block, know what
// a compiler would have to emit, and know what it costs.
enum class Kind : uint8_t {
  kAluImmediate,   // addiu, ori, slti...
  kAluRegister,    // addu, and, sltu, shifts...
  kMultiplyDivide, // mult, multu, div, divu - cost depends on the operands
  kLoad,
  kStore,
  kBranch,         // conditional; has a delay slot
  kJump,           // j, jal, jr, jalr; has a delay slot
  kSyscall,        // syscall, break - leaves through the exception vector
  kCoprocessor,    // cop0/cop2 moves and commands
  kOther,          // decoded but not classified; the compiler will bail to the
                   // interpreter on these rather than guess
};

struct Instruction {
  uint32_t pc = 0;
  uint32_t word = 0;
  Kind kind = Kind::kOther;
  uint8_t cycles = 1;        // the static part; see DecodedBlock::dynamic_cost
  bool in_delay_slot = false;
};

// Why the decoder stopped, which is the thing worth asserting in a test - two
// blocks can have the same length for very different reasons.
enum class EndReason : uint8_t {
  kBranchDelaySlot,   // a branch or jump, and its delay slot, are the last two
  kException,         // syscall or break
  kReturnFromException,
  kLengthCap,
  kFetchFailed,       // the fetch callback said there is nothing there
};

struct DecodedBlock {
  uint32_t start_pc = 0;
  std::vector<Instruction> instructions;
  EndReason end_reason = EndReason::kLengthCap;

  // The sum of the static per-instruction costs. It is *not* the whole cost:
  // mult and div take 6, 9, 13 or 36 cycles depending on the magnitude of
  // their operands (see cpu_test's `muldelay`), which is not knowable until
  // the block runs. A block containing one cannot have its cycles charged
  // entirely at compile time, and this flag is how the compiler knows to emit
  // the runtime accounting for it.
  uint32_t static_cycles = 0;
  bool dynamic_cost = false;

  uint32_t guest_bytes() const {
    return static_cast<uint32_t>(instructions.size()) * 4;
  }
  bool empty() const { return instructions.empty(); }
};

// Reads one aligned guest word. Returns false if there is nothing mapped
// there, which ends the block rather than inventing an instruction.
typedef std::function<bool(uint32_t pc, uint32_t* word)> FetchWord;

class BlockDecoder {
 public:
  // Long enough that ordinary straight-line runs are one block, short enough
  // that a block stays cheap to throw away when its page is written. DuckStation
  // and PCSX-R both sit in this range.
  static const uint32_t kMaxInstructions = 64;

  explicit BlockDecoder(FetchWord fetch) : fetch_(fetch) {}

  DecodedBlock Decode(uint32_t start_pc, uint32_t max_instructions = kMaxInstructions) const {
    DecodedBlock block;
    block.start_pc = start_pc;
    if (max_instructions == 0)
      return block;

    uint32_t pc = start_pc;
    bool ending_after_delay_slot = false;

    for (uint32_t index = 0; index < max_instructions; ++index) {
      uint32_t word = 0;
      if (!fetch_(pc, &word)) {
        block.end_reason = EndReason::kFetchFailed;
        return block;
      }

      Instruction instruction;
      instruction.pc = pc;
      instruction.word = word;
      instruction.kind = Classify(word);
      // One cycle an instruction, flat - and that is a measurement, not an
      // assumption that was never checked.
      //
      // The obvious refinement is wrong: Cpu::LW calls Tick() twice where
      // Cpu::ADDU calls it once, so charging loads two looks obviously more
      // faithful. Tried, and it runs the machine visibly fast - the BIOS shell
      // drew 379 primitives in 400 frames instead of 1157, because each frame
      // was consuming cycles the interpreter would not have spent. Flat
      // reproduces the interpreter's pacing on that run exactly, framebuffer
      // checksum included.
      //
      // Which says the interpreter's real cost per instruction is not the count
      // of Tick() calls in its handler, and working out what it actually is -
      // rather than guessing again - is what the cycle model needs before it
      // can be called accurate. See Docs/Recompiler-Plan.md.
      instruction.cycles = 1;
      instruction.in_delay_slot = ending_after_delay_slot;
      block.instructions.push_back(instruction);
      block.static_cycles += instruction.cycles;
      if (instruction.kind == Kind::kMultiplyDivide)
        block.dynamic_cost = true;

      // The delay slot of the branch that ended the block has now been taken,
      // so the block is complete. A branch *in* a delay slot is undefined on
      // the R3000A and no assembler emits one; it stops here either way rather
      // than starting a second delay slot.
      if (ending_after_delay_slot) {
        block.end_reason = EndReason::kBranchDelaySlot;
        return block;
      }

      switch (instruction.kind) {
        case Kind::kBranch:
        case Kind::kJump:
          // One more instruction - the delay slot - then stop.
          ending_after_delay_slot = true;
          break;

        case Kind::kSyscall:
          // Leaves through the exception vector; whatever follows it in memory
          // is not what runs next.
          block.end_reason = EndReason::kException;
          return block;

        default:
          if (IsReturnFromException(word)) {
            block.end_reason = EndReason::kReturnFromException;
            return block;
          }
          break;
      }

      pc += 4;
    }

    // Ran out of room. The block is valid and simply stops; the next one starts
    // where this left off.
    block.end_reason = ending_after_delay_slot ? EndReason::kBranchDelaySlot
                                               : EndReason::kLengthCap;
    return block;
  }

  // The address execution continues at when a block ends, for the cases where
  // that is knowable without running it: a straight-line block that hit the cap
  // continues at the instruction after its last. A branch's target depends on
  // the machine's state, so it is not answered here.
  static bool StaticFallthrough(const DecodedBlock& block, uint32_t* next_pc) {
    if (block.empty() || block.end_reason != EndReason::kLengthCap)
      return false;
    *next_pc = block.instructions.back().pc + 4;
    return true;
  }

  static Kind Classify(uint32_t word) {
    const uint32_t opcode = word >> 26;
    switch (opcode) {
      case 0x00: {   // SPECIAL
        const uint32_t funct = word & 0x3F;
        switch (funct) {
          case 0x08:   // jr
          case 0x09:   // jalr
            return Kind::kJump;
          case 0x0C:   // syscall
          case 0x0D:   // break
            return Kind::kSyscall;
          case 0x18:   // mult
          case 0x19:   // multu
          case 0x1A:   // div
          case 0x1B:   // divu
            return Kind::kMultiplyDivide;
          default:
            return Kind::kAluRegister;
        }
      }
      case 0x01:   // REGIMM: bltz, bgez, bltzal, bgezal
        return Kind::kBranch;
      case 0x02:   // j
      case 0x03:   // jal
        return Kind::kJump;
      case 0x04:   // beq
      case 0x05:   // bne
      case 0x06:   // blez
      case 0x07:   // bgtz
        return Kind::kBranch;
      case 0x08:   // addi
      case 0x09:   // addiu
      case 0x0A:   // slti
      case 0x0B:   // sltiu
      case 0x0C:   // andi
      case 0x0D:   // ori
      case 0x0E:   // xori
      case 0x0F:   // lui
        return Kind::kAluImmediate;
      case 0x10:   // cop0
      case 0x11:   // cop1
      case 0x12:   // cop2 - the GTE
      case 0x13:   // cop3
        return Kind::kCoprocessor;
      case 0x20:   // lb
      case 0x21:   // lh
      case 0x22:   // lwl
      case 0x23:   // lw
      case 0x24:   // lbu
      case 0x25:   // lhu
      case 0x26:   // lwr
      case 0x32:   // lwc2
        return Kind::kLoad;
      case 0x28:   // sb
      case 0x29:   // sh
      case 0x2A:   // swl
      case 0x2B:   // sw
      case 0x2E:   // swr
      case 0x3A:   // swc2
        return Kind::kStore;
      default:
        return Kind::kOther;
    }
  }

  // cop0 with rs = 0x10 and funct = 0x10. Returns from an exception, so what
  // runs next is EPC and not the next instruction.
  static bool IsReturnFromException(uint32_t word) {
    return (word >> 26) == 0x10 && ((word >> 21) & 0x1F) == 0x10 &&
           (word & 0x3F) == 0x10;
  }

 private:
  FetchWord fetch_;
};

}  // namespace rec
}  // namespace emulation
