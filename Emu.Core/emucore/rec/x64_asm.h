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

// A general x86-64 encoder, for code that is arithmetic rather than plumbing.
//
// x86_extras.h has exactly the forms the block compiler emits, each a function of its own, and is
// right for that: a guest instruction becomes a few fixed shapes. The GTE's commands (gte_compiler.h)
// are a different kind of code - 64-bit multiplies, sign- and zero-extending loads of bytes and
// halfwords at arbitrary offsets, a table indexed by a register, conditional moves and forward
// jumps by the dozen - and writing each of those as a one-off would be most of a assembler anyway.
// So this is one: operands are a register or [base + index*scale + disp], every operation takes an
// operand size (8, 16, 32 or 64 bits), and the encoding is worked out once, in Encode.
//
// What it does not do: RSP-relative addressing beyond what the SIB rule needs, AVX, anything
// floating point. Nothing here wants them.

#include "emucore/rec/emitter.h"

#include <cstdint>
#include <vector>

namespace emulation {
namespace rec {
namespace x64 {

enum Reg : uint8_t {
  rax = 0, rcx, rdx, rbx, rsp, rbp, rsi, rdi, r8, r9, r10, r11, r12, r13, r14, r15,
  kNoReg = 0xFF,
};

// [base + index*scale + disp]. scale is 1, 2, 4 or 8.
struct Mem {
  Reg base = kNoReg;
  int32_t disp = 0;
  Reg index = kNoReg;
  uint8_t scale = 1;
};
inline Mem At(Reg base, int32_t disp = 0) { Mem m; m.base = base; m.disp = disp; return m; }
inline Mem At(Reg base, Reg index, uint8_t scale, int32_t disp = 0) {
  Mem m; m.base = base; m.index = index; m.scale = scale; m.disp = disp; return m;
}

// The condition codes, in the encoding's own order (Jcc is 0F 80+cc, CMOVcc 0F 40+cc).
enum Cond : uint8_t {
  kO = 0, kNO, kB, kAE, kE, kNE, kBE, kA, kS, kNS, kP, kNP, kL, kGE, kLE, kG,
};

// A place in the code a jump goes to, possibly before it is known. Every jump is rel32, so a label
// is never out of reach and nothing has to be re-encoded once the distance is known.
struct Label {
  size_t bound = SIZE_MAX;
  std::vector<size_t> uses;   // the rel32 fields that jump here, by their offset in the code
};

class Asm {
 public:
  explicit Asm(Emitter* emitter) : e_(emitter) {}

  size_t cursor() const { return e_->GetCursor(); }

  // ---- moves ----------------------------------------------------------------------------------
  void mov(int bits, Reg dst, Reg src) { RegReg(bits, 0x8B, dst, src); }
  void mov(int bits, Reg dst, Mem src) { RegMem(bits, {0x8B}, dst, src); }
  void mov(int bits, Mem dst, Reg src) { RegMem(bits, {bits == 8 ? uint8_t(0x88) : uint8_t(0x89)}, src, dst, bits == 8); }
  // mov r32, imm32 (zero-extending into the whole register) or, for 64, the full imm64 form.
  void mov_imm(int bits, Reg dst, uint64_t value) {
    Rex(bits == 64, 0, 0, dst);
    e_->emit8(static_cast<uint8_t>(0xB8 + (dst & 7)));
    if (bits == 64) e_->emit64(value);
    else e_->emit32(static_cast<uint32_t>(value));
  }
  void mov_imm(int bits, Mem dst, uint32_t value) {
    if (bits == 8) {
      RegMem(8, {0xC6}, 0, dst);
      e_->emit8(static_cast<uint8_t>(value));
      return;
    }
    RegMem(bits, {0xC7}, 0, dst);
    if (bits == 16) e_->emit16(static_cast<uint16_t>(value));
    else e_->emit32(value);
  }
  // Sign- and zero-extending loads into a 32- or 64-bit register.
  void movsx16(int bits, Reg dst, Mem src) { RegMem(bits, {0x0F, 0xBF}, dst, src); }
  void movzx16(Reg dst, Mem src) { RegMem(32, {0x0F, 0xB7}, dst, src); }
  void movzx8(Reg dst, Mem src) { RegMem(32, {0x0F, 0xB6}, dst, src); }
  void movsxd(Reg dst, Mem src) { RegMem(64, {0x63}, dst, src); }
  void movsxd(Reg dst, Reg src) { RegReg(64, 0x63, dst, src); }
  void movsx16(int bits, Reg dst, Reg src) { RegReg2(bits, 0xBF, dst, src); }

  // ---- arithmetic ------------------------------------------------------------------------------
  enum Alu : uint8_t { kAdd = 0, kOr = 1, kAnd = 4, kSub = 5, kXor = 6, kCmp = 7 };
  void alu(Alu op, int bits, Reg dst, Reg src) { RegReg(bits, static_cast<uint8_t>(op * 8 + 3), dst, src); }
  void alu(Alu op, int bits, Reg dst, Mem src) { RegMem(bits, {static_cast<uint8_t>(op * 8 + 3)}, dst, src); }
  void alu(Alu op, int bits, Mem dst, Reg src) { RegMem(bits, {static_cast<uint8_t>(op * 8 + 1)}, src, dst); }
  void alu(Alu op, int bits, Reg dst, int32_t imm) {
    if (imm >= -128 && imm <= 127) {
      RegReg(bits, 0x83, static_cast<Reg>(op), dst);
      e_->emit8(static_cast<uint8_t>(imm));
    } else {
      RegReg(bits, 0x81, static_cast<Reg>(op), dst);
      e_->emit32(static_cast<uint32_t>(imm));
    }
  }
  void alu(Alu op, int bits, Mem dst, int32_t imm) {
    if (imm >= -128 && imm <= 127) {
      RegMem(bits, {0x83}, static_cast<Reg>(op), dst);
      e_->emit8(static_cast<uint8_t>(imm));
    } else {
      RegMem(bits, {0x81}, static_cast<Reg>(op), dst);
      e_->emit32(static_cast<uint32_t>(imm));
    }
  }
  void add(int bits, Reg dst, Reg src) { alu(kAdd, bits, dst, src); }
  void sub(int bits, Reg dst, Reg src) { alu(kSub, bits, dst, src); }
  void cmp(int bits, Reg dst, Reg src) { alu(kCmp, bits, dst, src); }
  void xor_(int bits, Reg dst, Reg src) { alu(kXor, bits, dst, src); }
  void test(int bits, Reg a, Reg b) { RegReg(bits, 0x85, b, a); }
  void neg(int bits, Reg r) { RegReg(bits, 0xF7, static_cast<Reg>(3), r); }
  void imul(int bits, Reg dst, Reg src) { RegReg2(bits, 0xAF, dst, src); }
  void imul(int bits, Reg dst, Mem src) { RegMem(bits, {0x0F, 0xAF}, dst, src); }

  enum Shift : uint8_t { kShl = 4, kShr = 5, kSar = 7 };
  void shift(Shift op, int bits, Reg r, uint8_t count) {
    if (count == 0)
      return;
    RegReg(bits, 0xC1, static_cast<Reg>(op), r);
    e_->emit8(count);
  }
  void shift_cl(Shift op, int bits, Reg r) { RegReg(bits, 0xD3, static_cast<Reg>(op), r); }
  void shl(int bits, Reg r, uint8_t n) { shift(kShl, bits, r, n); }
  void shr(int bits, Reg r, uint8_t n) { shift(kShr, bits, r, n); }
  void sar(int bits, Reg r, uint8_t n) { shift(kSar, bits, r, n); }

  void cmov(Cond cc, int bits, Reg dst, Reg src) { RegReg2(bits, static_cast<uint8_t>(0x40 + cc), dst, src); }
  void bsr(int bits, Reg dst, Reg src) { RegReg2(bits, 0xBD, dst, src); }

  // ---- control ---------------------------------------------------------------------------------
  void jcc(Cond cc, Label& to) {
    e_->emit8(0x0F);
    e_->emit8(static_cast<uint8_t>(0x80 + cc));
    Use(to);
  }
  void jmp(Label& to) {
    e_->emit8(0xE9);
    Use(to);
  }
  void jmp(Reg r) { RegReg(32, 0xFF, static_cast<Reg>(4), r); }
  void ret() { e_->emit8(0xC3); }
  void bind(Label& label) {
    label.bound = cursor();
    for (size_t at : label.uses)
      Patch(at, label.bound);
    label.uses.clear();
  }

 private:
  // A REX prefix when anything needs one: a 64-bit operand, a register from r8 up in any of the
  // three fields, or - `byte_regs` - a byte operand, where without REX the encodings 4-7 mean AH,
  // CH, DH and BH rather than SPL to DIL.
  void Rex(bool w, uint8_t reg, uint8_t index, uint8_t base, bool byte_regs = false) {
    const uint8_t rex = static_cast<uint8_t>(0x40 | (w ? 8 : 0) | ((reg & 8) ? 4 : 0) |
                                             ((index != kNoReg && (index & 8)) ? 2 : 0) |
                                             ((base != kNoReg && (base & 8)) ? 1 : 0));
    if (rex != 0x40 || (byte_regs && reg >= 4))
      e_->emit8(rex);
  }

  void Prefix(int bits) {
    if (bits == 16)
      e_->emit8(0x66);
  }

  // op reg, r/m - one opcode byte, the register in ModRM.reg and the other in ModRM.rm.
  void RegReg(int bits, uint8_t opcode, Reg reg, Reg rm) {
    Prefix(bits);
    Rex(bits == 64, reg, kNoReg, rm);
    e_->emit8(opcode);
    e_->emit8(static_cast<uint8_t>(0xC0 | ((reg & 7) << 3) | (rm & 7)));
  }
  // The same with a 0F escape.
  void RegReg2(int bits, uint8_t opcode, Reg reg, Reg rm) {
    Prefix(bits);
    Rex(bits == 64, reg, kNoReg, rm);
    e_->emit8(0x0F);
    e_->emit8(opcode);
    e_->emit8(static_cast<uint8_t>(0xC0 | ((reg & 7) << 3) | (rm & 7)));
  }

  struct Opcode { uint8_t a; uint8_t b = 0; bool two = false;
    Opcode(uint8_t x) : a(x) {}
    Opcode(uint8_t x, uint8_t y) : a(x), b(y), two(true) {} };

  // op reg, [mem] - ModRM, a SIB when the address needs one, and the shortest displacement.
  void RegMem(int bits, Opcode opcode, uint8_t reg, Mem m, bool byte_regs = false) {
    Prefix(bits);
    Rex(bits == 64, reg, m.index, m.base, byte_regs || bits == 8);
    e_->emit8(opcode.a);
    if (opcode.two)
      e_->emit8(opcode.b);
    const bool sib = m.index != kNoReg || (m.base & 7) == 4;   // rsp and r12 need one
    // [rbp] and [r13] with no displacement mean something else, so they always take a disp8.
    uint8_t mod = 2;
    if (m.disp == 0 && (m.base & 7) != 5)
      mod = 0;
    else if (m.disp >= -128 && m.disp <= 127)
      mod = 1;
    e_->emit8(static_cast<uint8_t>((mod << 6) | ((reg & 7) << 3) | (sib ? 4 : (m.base & 7))));
    if (sib) {
      const uint8_t ss = m.scale == 8 ? 3 : m.scale == 4 ? 2 : m.scale == 2 ? 1 : 0;
      const uint8_t index = m.index == kNoReg ? 4 : (m.index & 7);   // 4: no index
      e_->emit8(static_cast<uint8_t>((ss << 6) | (index << 3) | (m.base & 7)));
    }
    if (mod == 1)
      e_->emit8(static_cast<uint8_t>(m.disp));
    else if (mod == 2)
      e_->emit32(static_cast<uint32_t>(m.disp));
  }

  void Use(Label& to) {
    const size_t at = cursor();
    e_->emit32(0);
    if (to.bound != SIZE_MAX)
      Patch(at, to.bound);
    else
      to.uses.push_back(at);
  }
  void Patch(size_t at, size_t target) {
    CodeBlock* block = e_->block();
    if (block == nullptr || at + 4 > block->size)
      return;
    const int32_t rel = static_cast<int32_t>(static_cast<int64_t>(target) - static_cast<int64_t>(at + 4));
    memcpy(block->ptr8bit + at, &rel, 4);
  }

  Emitter* e_;
};

}  // namespace x64
}  // namespace rec
}  // namespace emulation
