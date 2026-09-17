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

// The x86-64 forms the vendored RecCore does not have.
//
// RecCore supplies what this needs most - a block of executable memory, an
// emit cursor, and the register numbering - but its instruction coverage is
// uneven: there is ADD, AND, OR, MOV, CMP and RET, and no SUB, XOR, NOT, SHR,
// SAR, SETcc or MOVZX. Those are needed to compile even the simplest third of
// the R3000A's instruction set, and step 4 of the plan needed more still -
// CALL, CMOVcc, PUSH/POP, the stack adjustment a call's shadow space wants,
// and the sign- and zero-extending moves a byte or halfword load ends with.
// All of it is written here rather than added to the vendored copy: the
// library's README asks for it that way round, so that the diff against
// upstream stays readable.
//
// Everything is the 32-bit form on the low half of a register, which is what
// the guest's registers are, and which zero-extends into the full 64-bit
// register exactly as the hardware does. The exceptions are the handful of
// REX.W forms for moving pointers around. All of it emits through
// reccore::Emitter, so a caller mixes these and RecCore's own freely.
//
// The one addressing form used is [base + disp8], because that is all a
// register file of 32 words needs: the furthest is at offset 124, and the
// block's own state is smaller than that again.

#include "lib/reccore/reccore.h"

#include <cstdint>

namespace emulation {
namespace rec {
namespace x86 {

// The ModRM byte, and the SIB that [RSP]-based addressing would need - not
// used here, since no base register is RSP.
inline uint8_t ModRM(uint8_t mod, uint8_t reg, uint8_t rm) {
  return static_cast<uint8_t>((mod << 6) | ((reg & 7) << 3) | (rm & 7));
}

// The REX prefix, emitted only when it changes something: R8-R15 in either
// operand, or a 64-bit operand size. Without this, `mov r8d, [rsi+8]` would
// encode as `mov eax, [rsi+8]` - the same three bytes, a different register,
// and no diagnostic. Emitting it unconditionally would be correct too, but it
// would make every byte count in the tests depend on registers that happen to
// be low.
inline void EmitRex(reccore::Emitter* e, bool wide, uint8_t reg, uint8_t rm) {
  const uint8_t rex = static_cast<uint8_t>(0x40 | (wide ? 8 : 0) |
                                           ((reg >= 8) ? 4 : 0) |
                                           ((rm >= 8) ? 1 : 0));
  if (rex != 0x40)
    e->emit8(rex);
}

// op r32, [base + disp8]  and  op [base + disp8], r32, for the ALU opcodes
// whose encoding is uniform: the /r form with the direction bit deciding which
// operand is memory.
inline void EmitRegMem8(reccore::Emitter* e, uint8_t opcode, uint8_t reg,
                        uint8_t base, int8_t displacement, bool wide = false) {
  EmitRex(e, wide, reg, base);
  e->emit8(opcode);
  e->emit8(ModRM(1, reg, base));
  e->emit8(static_cast<uint8_t>(displacement));
}

inline void EmitRegReg(reccore::Emitter* e, uint8_t opcode, uint8_t reg,
                       uint8_t rm, bool wide = false) {
  EmitRex(e, wide, reg, rm);
  e->emit8(opcode);
  e->emit8(ModRM(3, reg, rm));
}

// mov r32, [base + disp8]
inline void MovRegMem(reccore::Emitter* e, uint8_t reg, uint8_t base,
                      int8_t displacement) {
  EmitRegMem8(e, 0x8B, reg, base, displacement);
}

// mov [base + disp8], r32
inline void MovMemReg(reccore::Emitter* e, uint8_t reg, uint8_t base,
                      int8_t displacement) {
  EmitRegMem8(e, 0x89, reg, base, displacement);
}

// mov r32, r32 - what a guest register kept in a host register costs to read
// or write, instead of the memory access it replaces.
inline void MovRegReg(reccore::Emitter* e, uint8_t dest, uint8_t src) {
  EmitRegReg(e, 0x8B, dest, src);
}

// mov r32, imm32
inline void MovRegImm(reccore::Emitter* e, uint8_t reg, uint32_t value) {
  EmitRex(e, false, 0, reg);
  e->emit8(static_cast<uint8_t>(0xB8 + (reg & 7)));
  e->emit32(value);
}

// mov dword [base + disp8], imm32 - how a block writes a constant guest
// address into its state without going through a register.
inline void MovMemImm(reccore::Emitter* e, uint8_t base, int8_t displacement,
                      uint32_t value) {
  EmitRex(e, false, 0, base);
  e->emit8(0xC7);
  e->emit8(ModRM(1, 0, base));
  e->emit8(static_cast<uint8_t>(displacement));
  e->emit32(value);
}

// The ALU group, register-to-register: dest = dest OP source.
enum class AluOp : uint8_t {
  kAdd = 0x03,
  kSub = 0x2B,
  kAnd = 0x23,
  kOr = 0x0B,
  kXor = 0x33,
  kCmp = 0x3B,
};

// op r32, [base + disp8] - the memory operand is the source, which is the
// shape every guest ALU instruction takes here: the accumulator is a host
// register and the other operand comes out of the register file.
inline void AluRegMem(reccore::Emitter* e, AluOp op, uint8_t reg, uint8_t base,
                      int8_t displacement) {
  EmitRegMem8(e, static_cast<uint8_t>(op), reg, base, displacement);
}

inline void AluRegReg(reccore::Emitter* e, AluOp op, uint8_t dest, uint8_t src) {
  EmitRegReg(e, static_cast<uint8_t>(op), dest, src);
}

// The immediate forms share opcode 0x81 and differ by the /digit in ModRM.reg.
enum class AluImmOp : uint8_t {
  kAdd = 0,
  kOr = 1,
  kAnd = 4,
  kSub = 5,
  kXor = 6,
  kCmp = 7,
};

inline void AluRegImm(reccore::Emitter* e, AluImmOp op, uint8_t reg,
                      uint32_t value) {
  e->emit8(0x81);
  e->emit8(ModRM(3, static_cast<uint8_t>(op), reg));
  e->emit32(value);
}

// not r32 - the /2 form of group 0xF7.
inline void NotReg(reccore::Emitter* e, uint8_t reg) {
  e->emit8(0xF7);
  e->emit8(ModRM(3, 2, reg));
}

// The shift group, by an immediate count: 0xC1 with the /digit choosing which.
enum class ShiftOp : uint8_t {
  kShl = 4,
  kShr = 5,
  kSar = 7,
};

inline void ShiftRegImm(reccore::Emitter* e, ShiftOp op, uint8_t reg,
                        uint8_t count) {
  e->emit8(0xC1);
  e->emit8(ModRM(3, static_cast<uint8_t>(op), reg));
  e->emit8(count);
}

// The shift group, by CL, for the guest's variable shifts.
inline void ShiftRegCl(reccore::Emitter* e, ShiftOp op, uint8_t reg) {
  e->emit8(0xD3);
  e->emit8(ModRM(3, static_cast<uint8_t>(op), reg));
}

inline void MovzxRegReg8(reccore::Emitter* e, uint8_t dest, uint8_t src) {
  EmitRex(e, false, dest, src);
  e->emit8(0x0F);
  e->emit8(0xB6);
  e->emit8(ModRM(3, dest, src));
}

inline void MovsxRegReg8(reccore::Emitter* e, uint8_t dest, uint8_t src) {
  EmitRex(e, false, dest, src);
  e->emit8(0x0F);
  e->emit8(0xBE);
  e->emit8(ModRM(3, dest, src));
}

inline void MovzxRegReg16(reccore::Emitter* e, uint8_t dest, uint8_t src) {
  e->emit8(0x0F);
  e->emit8(0xB7);
  e->emit8(ModRM(3, dest, src));
}

inline void MovsxRegReg16(reccore::Emitter* e, uint8_t dest, uint8_t src) {
  e->emit8(0x0F);
  e->emit8(0xBF);
  e->emit8(ModRM(3, dest, src));
}

// mov r64, r64 - the REX.W form, for moving the incoming argument pointer out
// of the register the shift instructions need.
inline void Mov64RegReg(reccore::Emitter* e, uint8_t dest, uint8_t src) {
  EmitRex(e, true, src, dest);
  e->emit8(0x89);
  e->emit8(ModRM(3, src, dest));
}

// mov r64, [base + disp8] - for pulling a pointer out of the block's state:
// the register file, the callback context, a function address.
inline void Mov64RegMem(reccore::Emitter* e, uint8_t dest, uint8_t base,
                        int8_t displacement) {
  EmitRegMem8(e, 0x8B, dest, base, displacement, true);
}

// The condition codes, as the low nibble shared by Jcc, SETcc and CMOVcc: the
// three groups are 0x70+cc, 0x0F 0x90+cc and 0x0F 0x40+cc. Keeping one enum
// for all three is what stops a branch and the SETcc that was supposed to
// mirror it from drifting apart.
enum class Cc : uint8_t {
  kBelow = 0x2,          // unsigned <
  kAboveEqual = 0x3,
  kEqual = 0x4,
  kNotEqual = 0x5,
  kSign = 0x8,           // negative
  kNotSign = 0x9,
  kLess = 0xC,           // signed <
  kGreaterEqual = 0xD,
  kLessEqual = 0xE,
  kGreater = 0xF,
};

// setcc r8, then movzx r32, r8 - how a comparison becomes the 0 or 1 that slt
// writes to a register.
inline void SetCc(reccore::Emitter* e, Cc condition, uint8_t reg) {
  EmitRex(e, false, 0, reg);
  e->emit8(0x0F);
  e->emit8(static_cast<uint8_t>(0x90 + static_cast<uint8_t>(condition)));
  e->emit8(ModRM(3, 0, reg));
}

// cmovcc r32, r32 - a branch without a branch. Every guest branch here
// resolves to "one of two addresses", which is a compare and a conditional
// move; no jump is emitted and nothing has to be patched afterwards.
inline void CmovRegReg(reccore::Emitter* e, Cc condition, uint8_t dest,
                       uint8_t src) {
  EmitRex(e, false, dest, src);
  e->emit8(0x0F);
  e->emit8(static_cast<uint8_t>(0x40 + static_cast<uint8_t>(condition)));
  e->emit8(ModRM(3, dest, src));
}

// call r64 - the /2 form of group 0xFF. The address comes out of a register
// because it is read from the block's state at run time; an immediate call
// would bake in a displacement that only holds while the code stays put.
inline void CallReg(reccore::Emitter* e, uint8_t reg) {
  EmitRex(e, false, 0, reg);
  e->emit8(0xFF);
  e->emit8(ModRM(3, 2, reg));
}

inline void Push(reccore::Emitter* e, uint8_t reg) {
  EmitRex(e, false, 0, reg);
  e->emit8(static_cast<uint8_t>(0x50 + (reg & 7)));
}

inline void Pop(reccore::Emitter* e, uint8_t reg) {
  EmitRex(e, false, 0, reg);
  e->emit8(static_cast<uint8_t>(0x58 + (reg & 7)));
}

// sub rsp, imm8 / add rsp, imm8 - the shadow space a called function is
// entitled to write, and its release.
inline void SubRspImm8(reccore::Emitter* e, uint8_t bytes) {
  e->emit8(0x48);
  e->emit8(0x83);
  e->emit8(ModRM(3, 5, 4));   // /5 = sub, rm = RSP
  e->emit8(bytes);
}

inline void AddRspImm8(reccore::Emitter* e, uint8_t bytes) {
  e->emit8(0x48);
  e->emit8(0x83);
  e->emit8(ModRM(3, 0, 4));   // /0 = add
  e->emit8(bytes);
}

// cmp dword [base + disp8], imm8 - testing a flag in the block's state without
// spending a register on it.
inline void CmpMemImm8(reccore::Emitter* e, uint8_t base, int8_t displacement,
                       uint8_t value) {
  EmitRex(e, false, 0, base);
  e->emit8(0x83);
  e->emit8(ModRM(1, 7, base));   // /7 = cmp
  e->emit8(static_cast<uint8_t>(displacement));
  e->emit8(value);
}

// add dword [base + disp8], imm8 - a block adding its cycle cost to the
// running total on its way out.
inline void AddMemImm8(reccore::Emitter* e, uint8_t base, int8_t displacement,
                       uint8_t value) {
  EmitRex(e, false, 0, base);
  e->emit8(0x83);
  e->emit8(ModRM(1, 0, base));   // /0 = add
  e->emit8(static_cast<uint8_t>(displacement));
  e->emit8(value);
}

// sub dword [base + disp8], imm8 - the block's budget decrement, and the only
// instruction here that both reads and writes memory. The immediate is a byte
// because a block is at most 64 instructions long.
inline void SubMemImm8(reccore::Emitter* e, uint8_t base, int8_t displacement,
                       uint8_t value) {
  EmitRex(e, false, 0, base);
  e->emit8(0x83);
  e->emit8(ModRM(1, 5, base));   // /5 = sub
  e->emit8(static_cast<uint8_t>(displacement));
  e->emit8(value);
}

// The jumps that make linking possible. Displacements are from the end of the
// instruction, and the rel32 ones are written as zero and filled in afterwards:
// a link's target is not known when the block that jumps to it is compiled,
// and may change when a store throws that target away.
inline void JccRel8(reccore::Emitter* e, Cc condition, int8_t displacement) {
  e->emit8(static_cast<uint8_t>(0x70 + static_cast<uint8_t>(condition)));
  e->emit8(static_cast<uint8_t>(displacement));
}

inline void JmpRel32(reccore::Emitter* e, int32_t displacement) {
  e->emit8(0xE9);
  e->emit32(static_cast<uint32_t>(displacement));
}

inline void Ret(reccore::Emitter* e) { e->emit8(0xC3); }

}  // namespace x86
}  // namespace rec
}  // namespace emulation
