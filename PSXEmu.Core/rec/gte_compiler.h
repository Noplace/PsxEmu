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

// The GTE's commands as x64 - the second of the recompiler's two ways of running one.
//
// The first (GteLayout::commands) calls the host's C++ for the command, chosen when the block is
// compiled. This one writes the command out as machine code instead, once per distinct command
// word, and the block calls that. A word says everything that varies between two runs of the same
// command - the shift (sf), the limit (lm), and for MVMVA which matrix, vector and translation - so
// all of it is decided here, at emit time, and what runs is the arithmetic for exactly that word:
// no test of sf or lm, no switch on MVMVA's fields, no call between the steps.
//
// It is the same arithmetic as psx/gte.cpp, step for step, and has to stay so - gte_test's `x64`
// group runs every command, every sf and lm and every MVMVA form, from thousands of states chosen to
// sit on the edges (the 44-bit accumulator, IR's and the colours' limits, the divide's overflow),
// through both and compares every register. The things that are easy to get subtly wrong are named
// where they happen: each accumulator checked after every partial sum, MAC values truncated to 32
// bits before saturation, IR3's flag in RTPS judged on the value shifted by 12, IR0 from the full
// depth sum, the divide's table, the far-colour MVMVA.
//
// One difference of form: gte.cpp works out all three rows of a step and then writes MAC1-3 and
// IR1-3; this writes each row's MAC as it goes and the IRs at the end. That is the same, because no
// row of any step reads another row's MAC - the inputs are the matrices, the vectors, IR (written
// last) and, for GPL, the row's own MAC.
//
// PGXP is the host's: RTPS, RTPT and NCLIP read and write its unrounded positions, so while the
// host has it on (GteFields::pgxp) those three jump to the host's own function for the command.
//
// Calling convention: the host's, a GteCommandFn - the GTE object in RCX, the word in EDX. The code
// is a leaf and touches only volatile registers: R10 holds the GTE, R11 the FLAG being built, and
// RAX, RCX, RDX, R8 and R9 are scratch.

#include "rec/emitter.h"
#include "rec/runtime.h"
#include "rec/x64_asm.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace emulation {
namespace rec {

class GteCompiler {
 public:
  explicit GteCompiler(const GteLayout* layout) : layout_(layout) {}
  ~GteCompiler() {
    for (CodeBlock* arena : arenas_)
      emitter_.destroy_block(arena);
  }
  GteCompiler(const GteCompiler&) = delete;
  GteCompiler& operator=(const GteCompiler&) = delete;

  // Whether the host has said enough about its GTE for anything to be emitted.
  bool available() const { return layout_ != nullptr && layout_->fields.divide_table != nullptr; }

  // The bits of a command word the command depends on: the opcode, lm, the MVMVA translation,
  // vector and matrix, and sf. The rest (bits 20-24, the "fake" command number) change nothing.
  static uint32_t Key(uint32_t word) { return word & 0x000FE43Fu; }

  // The emitted function for this word - written the first time it is asked for - or null when
  // nothing can be (no layout, or no room).
  GteCommandFn Function(uint32_t word) {
    if (!available())
      return nullptr;
    const uint32_t key = Key(word);
    auto found = cache_.find(key);
    if (found != cache_.end())
      return found->second;
    CodeBlock* arena = ArenaWithRoom();
    if (arena == nullptr)
      return nullptr;
    emitter_.set_block(arena);
    const size_t start = arena->cursor;
    Generator generator(&emitter_, layout_, key);
    generator.Generate();
    GteCommandFn function = nullptr;
    if (!arena->overflowed) {
      function = reinterpret_cast<GteCommandFn>(arena->ptr8bit + start);
      bytes_ += arena->cursor - start;
    }
    cache_[key] = function;
    return function;
  }

  size_t functions() const { return cache_.size(); }
  size_t bytes() const { return bytes_; }

 private:
  static const size_t kArenaBytes = 256 * 1024;
  // NCCT, the longest, is a few KB; this is well clear of it, and checked.
  static const size_t kMaxCommandBytes = 16 * 1024;

  CodeBlock* ArenaWithRoom() {
    if (arenas_.empty() || arenas_.back()->cursor + kMaxCommandBytes > arenas_.back()->size) {
      CodeBlock* arena = emitter_.create_block(kArenaBytes);
      if (arena == nullptr)
        return nullptr;
      arenas_.push_back(arena);
    }
    return arenas_.back();
  }

  // FLAG's bits, as gte.cpp names them.
  static const uint32_t kFlagMac1Positive = 1u << 30;
  static const uint32_t kFlagMac1Negative = 1u << 27;
  static const uint32_t kFlagIr1Saturated = 1u << 24;
  static const uint32_t kFlagColourR = 1u << 21;
  static const uint32_t kFlagSz3Saturated = 1u << 18;
  static const uint32_t kFlagDivideOverflow = 1u << 17;
  static const uint32_t kFlagMac0Positive = 1u << 16;
  static const uint32_t kFlagMac0Negative = 1u << 15;
  static const uint32_t kFlagSx2Saturated = 1u << 14;
  static const uint32_t kFlagSy2Saturated = 1u << 13;
  static const uint32_t kFlagIr0Saturated = 1u << 12;
  static const uint32_t kFlagErrorMask = 0x7F87E000u;

  class Generator {
   public:
    Generator(Emitter* emitter, const GteLayout* layout, uint32_t word)
        : a_(emitter), layout_(layout), f_(layout->fields), word_(word),
          opcode_(word & 0x3F), sf_((word & (1u << 19)) ? 12 : 0), lm_((word & (1u << 10)) != 0) {}

    void Generate() {
      using namespace x64;
      // The three commands PGXP is part of are the host's while it is on.
      const bool pgxp_aware = opcode_ == 0x01 || opcode_ == 0x30 || opcode_ == 0x06;
      Label host;
      if (pgxp_aware) {
        a_.movzx8(rax, At(rcx, f_.pgxp));
        a_.test(32, rax, rax);
        a_.jcc(kNE, host);
      }
      a_.mov(64, G, rcx);
      a_.xor_(32, F, F);                                   // FLAG starts clear
      a_.mov_imm(8, At(G, f_.lm), lm_ ? 1 : 0);            // as Execute leaves them
      a_.mov_imm(32, At(G, f_.sf), static_cast<uint32_t>(sf_));
      a_.alu(Asm::kAdd, 64, At(G, f_.commands), 1);
      a_.alu(Asm::kAdd, 32, At(G, f_.executed + static_cast<int32_t>(opcode_) * 4), 1);

      switch (opcode_) {
        case 0x01: Rtps(0, true); break;
        case 0x06: Nclip(); break;
        case 0x0C: Op(); break;
        case 0x10: DepthCue(f_.rgbc); break;                 // DPCS
        case 0x11: Intpl(); break;
        case 0x12: Mvmva(); break;
        case 0x13: Ncds(0); break;
        case 0x14: Cdp(); break;
        case 0x16: Ncds(0); Ncds(1); Ncds(2); break;         // NCDT
        case 0x1B: Nccs(0); break;
        case 0x1C: Cc(); break;
        case 0x1E: Ncs(0); break;
        case 0x20: Ncs(0); Ncs(1); Ncs(2); break;            // NCT
        case 0x28: Sqr(); break;
        case 0x29: Dcpl(); break;
        case 0x2A:                                           // DPCT: the oldest colour, three times
          DepthCue(f_.rgb_fifo); DepthCue(f_.rgb_fifo); DepthCue(f_.rgb_fifo);
          break;
        case 0x2D: Avsz(f_.zsf3, 1); break;
        case 0x2E: Avsz(f_.zsf4, 0); break;
        case 0x30: Rtps(0, false); Rtps(1, false); Rtps(2, true); break;   // RTPT
        case 0x3D: Gpf(); break;
        case 0x3E: Gpl(); break;
        case 0x3F: Nccs(0); Nccs(1); Nccs(2); break;         // NCCT
        default:
          a_.alu(Asm::kAdd, 64, At(G, f_.unknown_commands), 1);
          break;
      }

      // Bit 31 is the OR of the error bits.
      Label clean;
      a_.mov(32, rax, F);
      a_.alu(Asm::kAnd, 32, rax, static_cast<int32_t>(kFlagErrorMask));
      a_.jcc(kE, clean);
      a_.alu(Asm::kOr, 32, F, static_cast<int32_t>(0x80000000u));
      a_.bind(clean);
      a_.mov(32, At(G, f_.flag), F);
      a_.ret();

      if (pgxp_aware) {
        a_.bind(host);   // RCX and EDX are still the GTE and the word
        a_.mov_imm(64, rax, reinterpret_cast<uint64_t>(layout_->commands[opcode_]));
        a_.jmp(rax);
      }
    }

   private:
    static constexpr x64::Reg G = x64::r10;     // the GTE
    static constexpr x64::Reg F = x64::r11;     // FLAG, as the command builds it
    static constexpr x64::Reg Acc = x64::r8;    // a row's 64-bit sum
    static constexpr x64::Reg T = x64::r9;      // a second value kept across a step

    // Where an operand comes from. Everything the commands multiply is an int16 or a byte of the
    // register file, an int32 translation, or one of MVMVA's garbage matrix entries.
    struct Src {
      enum Kind : uint8_t { kZero, kS16, kU8, kS32, kRgbR16, kNegRgbR16 } kind = kZero;
      int32_t off = 0;
    };
    static Src S16(int32_t off) { Src s; s.kind = Src::kS16; s.off = off; return s; }
    static Src S32(int32_t off) { Src s; s.kind = Src::kS32; s.off = off; return s; }
    static Src Zero() { return Src(); }

    // Offsets of the register file's parts.
    int32_t V(int vertex, int axis) const { return f_.v + (vertex * 3 + axis) * 2; }
    int32_t Ir(int i) const { return f_.ir + i * 2; }
    int32_t Mac(int i) const { return f_.mac + i * 4; }
    int32_t Matrix(int m, int row, int column) const { return f_.matrix + ((m * 3 + row) * 3 + column) * 2; }
    int32_t Vector(int t, int row) const { return f_.translation + (t * 3 + row) * 4; }
    int32_t Sz(int i) const { return f_.sz + i * 2; }
    int32_t Sxy(int i) const { return f_.sxy + i * 4; }
    int32_t Fifo(int i) const { return f_.rgb_fifo + i * 4; }

    void Load(x64::Reg r, Src s) {
      using namespace x64;
      switch (s.kind) {
        case Src::kS16: a_.movsx16(64, r, At(G, s.off)); break;
        case Src::kU8: a_.movzx8(r, At(G, s.off)); break;
        case Src::kS32: a_.movsxd(r, At(G, s.off)); break;
        case Src::kRgbR16:      // int16(R) * 16
          a_.movzx8(r, At(G, f_.rgbc));
          a_.shl(32, r, 4);
          break;
        case Src::kNegRgbR16:   // int16(-int16(R) * 16)
          a_.movzx8(r, At(G, f_.rgbc));
          a_.shl(32, r, 4);
          a_.neg(64, r);
          break;
        default: a_.xor_(32, r, r); break;
      }
    }

    // RAX = a * b, 64 bits signed.
    void Product(Src a, Src b) {
      using namespace x64;
      Load(rax, a);
      Load(rdx, b);
      a_.imul(64, rax, rdx);
    }

    void Flag(uint32_t bits) {
      if (bits != 0)
        a_.alu(x64::Asm::kOr, 32, F, static_cast<int32_t>(bits));
    }

    // ---- saturation and overflow (gte.cpp's) ----------------------------------------------------

    // CheckMac: the 44-bit accumulator. Flags an overflow either way and wraps `r` to 44 bits,
    // sign-extended - which is the value unchanged exactly when it did not overflow. Uses RAX.
    void CheckMac(int index, x64::Reg r) {
      using namespace x64;
      Label ok, negative;
      a_.mov(64, rax, r);
      a_.shl(64, rax, 20);
      a_.sar(64, rax, 20);
      a_.cmp(64, rax, r);
      a_.jcc(kE, ok);
      a_.test(64, r, r);
      a_.jcc(kS, negative);
      Flag(kFlagMac1Positive >> (index - 1));
      a_.jmp(ok);
      a_.bind(negative);
      Flag(kFlagMac1Negative >> (index - 1));
      a_.bind(ok);
      a_.mov(64, r, rax);
    }

    // CheckMac0: flags a sum outside 32 bits; the value kept is its low half. Uses RAX.
    void CheckMac0(x64::Reg r) {
      using namespace x64;
      Label ok, negative;
      a_.movsxd(rax, r);
      a_.cmp(64, rax, r);
      a_.jcc(kE, ok);
      a_.test(64, r, r);
      a_.jcc(kS, negative);
      Flag(kFlagMac0Positive);
      a_.jmp(ok);
      a_.bind(negative);
      Flag(kFlagMac0Negative);
      a_.bind(ok);
    }

    // The 32-bit value in `r` limited to lo..hi, setting `flag` (if any) when it had to be.
    void Clamp(x64::Reg r, int32_t lo, int32_t hi, uint32_t flag) {
      using namespace x64;
      Label above_low, done;
      a_.alu(Asm::kCmp, 32, r, lo);
      a_.jcc(kGE, above_low);
      Flag(flag);
      a_.mov_imm(32, r, static_cast<uint32_t>(lo));
      a_.jmp(done);
      a_.bind(above_low);
      a_.alu(Asm::kCmp, 32, r, hi);
      a_.jcc(kLE, done);
      Flag(flag);
      a_.mov_imm(32, r, static_cast<uint32_t>(hi));
      a_.bind(done);
    }

    // The flag alone, for a value outside lo..hi.
    void FlagIfOutside(x64::Reg r, int32_t lo, int32_t hi, uint32_t flag) {
      using namespace x64;
      Label outside, done;
      a_.alu(Asm::kCmp, 32, r, lo);
      a_.jcc(kL, outside);
      a_.alu(Asm::kCmp, 32, r, hi);
      a_.jcc(kLE, done);
      a_.bind(outside);
      Flag(flag);
      a_.bind(done);
    }

    int32_t IrFloor(bool lm) const { return lm ? 0 : -0x8000; }

    // One row of SetMacAndIr's first half: MACn = int32(CheckMac(n, r) >> sf).
    void SetMac(int index, x64::Reg r) {
      using namespace x64;
      CheckMac(index, r);
      a_.sar(64, r, static_cast<uint8_t>(sf_));
      a_.mov(32, At(G, Mac(index)), r);
    }

    // And its second: IRn = SaturateIr(n, MACn, lm).
    void IrFromMac(int index, bool lm) {
      using namespace x64;
      a_.mov(32, rax, At(G, Mac(index)));
      Clamp(rax, IrFloor(lm), 0x7FFF, kFlagIr1Saturated >> (index - 1));
      a_.mov(16, At(G, Ir(index)), rax);
    }
    void IrsFromMacs(bool lm) {
      IrFromMac(1, lm);
      IrFromMac(2, lm);
      IrFromMac(3, lm);
    }

    // ---- shared steps --------------------------------------------------------------------------

    // TranslatedDot into Acc: translation*1000h + row.vector, the accumulator checked after each
    // of the first two partial sums. The final check is the caller's.
    void TranslatedDot(int index, Src translation, const Src row[3], const Src vector[3]) {
      using namespace x64;
      if (translation.kind == Src::kZero) {
        a_.xor_(32, Acc, Acc);
      } else {
        Load(Acc, translation);
        a_.shl(64, Acc, 12);
      }
      for (int i = 0; i < 3; ++i) {
        Product(row[i], vector[i]);
        a_.add(64, Acc, rax);
        if (i < 2)
          CheckMac(index, Acc);
      }
    }

    void MatrixRows(int m, Src rows[3][3]) {
      for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
          rows[r][c] = S16(Matrix(m, r, c));
    }
    Src Translation(int t, int row) { return t < 3 ? S32(Vector(t, row)) : Zero(); }

    // MultiplyMatrixByVector: matrix times vector plus translation (3: none), into MAC and IR.
    void MatrixVector(const Src rows[3][3], const Src vector[3], int translation, bool lm) {
      for (int r = 0; r < 3; ++r) {
        TranslatedDot(r + 1, Translation(translation, r), rows[r], vector);
        SetMac(r + 1, Acc);
      }
      IrsFromMacs(lm);
    }

    void VertexSrc(int vertex, Src out[3]) {
      for (int i = 0; i < 3; ++i)
        out[i] = S16(V(vertex, i));
    }
    void IrSrc(Src out[3]) {
      for (int i = 0; i < 3; ++i)
        out[i] = S16(Ir(i + 1));
    }

    // PushColourFromMac: the colour FIFO moves on, the newest entry is MAC1-3 >> 4 as bytes (and
    // CODE), and IR1-3 come from MAC1-3 again.
    void PushColourFromMac() {
      using namespace x64;
      a_.mov(32, rax, At(G, Fifo(1)));
      a_.mov(32, At(G, Fifo(0)), rax);
      a_.mov(32, rax, At(G, Fifo(2)));
      a_.mov(32, At(G, Fifo(1)), rax);
      for (int i = 1; i <= 3; ++i) {
        a_.mov(32, rax, At(G, Mac(i)));
        a_.sar(32, rax, 4);
        Clamp(rax, 0, 0xFF, kFlagColourR >> (i - 1));
        a_.mov(8, At(G, Fifo(2) + i - 1), rax);
      }
      a_.movzx8(rax, At(G, f_.rgbc + 3));
      a_.mov(8, At(G, Fifo(2) + 3), rax);
      IrsFromMacs(lm_);
    }

    // The far-colour step every depth-cued command shares. `produce(i)` leaves row i's input (x, y
    // or z of gte.cpp's InterpolateFarColour) in T, as a 64-bit value; it may use RAX and RDX.
    template <typename Produce>
    void InterpolateFarColour(Produce produce) {
      using namespace x64;
      for (int i = 1; i <= 3; ++i) {
        produce(i);
        Load(Acc, S32(Vector(2, i - 1)));
        a_.shl(64, Acc, 12);
        a_.sub(64, Acc, T);                     // FC*1000h - input
        CheckMac(i, Acc);
        a_.sar(64, Acc, static_cast<uint8_t>(sf_));
        a_.mov(32, rax, Acc);
        Clamp(rax, -0x8000, 0x7FFF, kFlagIr1Saturated >> (i - 1));   // lm off, whatever the command said
        a_.movsxd(rax, rax);
        a_.movsx16(64, rdx, At(G, Ir(0)));
        a_.imul(64, rax, rdx);
        a_.add(64, rax, T);                     // + input
        a_.mov(64, Acc, rax);
        SetMac(i, Acc);
      }
    }

    // Row i's (colour byte * IRi) << 4, into `r` - the surface colour step. Uses RDX.
    void ColourTimesIr(x64::Reg r, int i) {
      using namespace x64;
      a_.movzx8(r, At(G, f_.rgbc + i - 1));
      a_.movsx16(64, rdx, At(G, Ir(i)));
      a_.imul(64, r, rdx);
      a_.shl(64, r, 4);
    }

    // ---- commands ------------------------------------------------------------------------------

    // The perspective divide, H / SZ3, into EAX: the Newton-Raphson reciprocal off the table, or
    // 1FFFFh and the overflow flag when H >= SZ3*2. Uses RCX, RDX and R9.
    void Divide() {
      using namespace x64;
      Label in_range, done;
      a_.movzx16(rdx, At(G, f_.h));            // numerator
      a_.movzx16(T, At(G, Sz(3)));             // denominator
      a_.mov(32, rax, T);
      a_.add(32, rax, rax);
      a_.cmp(32, rdx, rax);
      a_.jcc(kB, in_range);
      Flag(kFlagDivideOverflow);
      a_.mov_imm(32, rax, 0x1FFFF);
      a_.jmp(done);
      a_.bind(in_range);
      a_.bsr(32, rax, T);                      // the denominator is not zero here
      a_.mov_imm(32, rcx, 15);
      a_.sub(32, rcx, rax);                    // its leading zeroes in 16 bits
      a_.shift_cl(Asm::kShl, 32, rdx);
      a_.shift_cl(Asm::kShl, 32, T);
      a_.mov(32, rax, T);
      a_.alu(Asm::kSub, 32, rax, 0x7FC0);
      a_.shr(32, rax, 7);
      a_.mov_imm(64, rcx, reinterpret_cast<uint64_t>(f_.divide_table));
      a_.movzx8(rax, At(rcx, rax, 1));
      a_.alu(Asm::kAdd, 32, rax, 0x101);       // u
      a_.imul(32, T, rax);
      a_.mov_imm(32, rcx, 0x2000080);
      a_.sub(32, rcx, T);
      a_.shr(32, rcx, 8);                      // d = (2000080h - d*u) >> 8
      a_.imul(32, rcx, rax);
      a_.alu(Asm::kAdd, 32, rcx, 0x80);
      a_.shr(32, rcx, 8);                      // d = (80h + d*u) >> 8
      a_.mov(32, rax, rdx);
      a_.imul(64, rax, rcx);
      a_.alu(Asm::kAdd, 64, rax, 0x8000);
      a_.shr(64, rax, 16);
      a_.mov_imm(32, rcx, 0x1FFFF);
      a_.cmp(64, rax, rcx);
      a_.cmov(kA, 64, rax, rcx);
      a_.bind(done);
    }

    void Rtps(int vertex, bool compute_ir0) {
      using namespace x64;
      Src rows[3][3], v[3];
      MatrixRows(0, rows);
      VertexSrc(vertex, v);
      for (int r = 0; r < 3; ++r) {
        TranslatedDot(r + 1, S32(Vector(0, r)), rows[r], v);
        CheckMac(r + 1, Acc);
        if (r == 2) {                          // z >> 12, whatever sf says, for IR3's flag and SZ3
          a_.mov(64, T, Acc);
          a_.sar(64, T, 12);
        }
        a_.sar(64, Acc, static_cast<uint8_t>(sf_));
        a_.mov(32, At(G, Mac(r + 1)), Acc);
      }
      IrFromMac(1, lm_);
      IrFromMac(2, lm_);
      FlagIfOutside(T, -0x8000, 0x7FFF, kFlagIr1Saturated >> 2);
      a_.mov(32, rax, At(G, Mac(3)));
      Clamp(rax, IrFloor(lm_), 0x7FFF, 0);     // IR3: the ordinary value, no flag
      a_.mov(16, At(G, Ir(3)), rax);

      a_.mov(32, rax, T);
      Clamp(rax, 0, 0xFFFF, kFlagSz3Saturated);
      // PushScreenZ
      for (int i = 0; i < 3; ++i) {
        a_.movzx16(rdx, At(G, Sz(i + 1)));
        a_.mov(16, At(G, Sz(i)), rdx);
      }
      a_.mov(16, At(G, Sz(3)), rax);

      Divide();
      a_.mov(32, T, rax);

      a_.movsx16(64, rax, At(G, Ir(1)));
      a_.imul(64, rax, T);
      a_.movsxd(rdx, At(G, f_.ofx));
      a_.add(64, rax, rdx);
      a_.mov(64, Acc, rax);
      CheckMac0(Acc);
      a_.sar(64, Acc, 16);
      a_.mov(32, rax, Acc);
      Clamp(rax, -0x400, 0x3FF, kFlagSx2Saturated);
      a_.mov(32, rcx, rax);                    // screen x

      a_.movsx16(64, rax, At(G, Ir(2)));
      a_.imul(64, rax, T);
      a_.movsxd(rdx, At(G, f_.ofy));
      a_.add(64, rax, rdx);
      a_.mov(64, Acc, rax);
      CheckMac0(Acc);
      a_.sar(64, Acc, 16);
      a_.mov(32, rax, Acc);
      Clamp(rax, -0x400, 0x3FF, kFlagSy2Saturated);

      // PushScreenXy
      a_.mov(32, rdx, At(G, Sxy(1)));
      a_.mov(32, At(G, Sxy(0)), rdx);
      a_.mov(32, rdx, At(G, Sxy(2)));
      a_.mov(32, At(G, Sxy(1)), rdx);
      a_.mov(16, At(G, Sxy(2)), rcx);
      a_.mov(16, At(G, Sxy(2) + 2), rax);

      if (compute_ir0) {
        a_.movsx16(64, rax, At(G, f_.dqa));
        a_.imul(64, rax, T);
        a_.movsxd(rdx, At(G, f_.dqb));
        a_.add(64, rax, rdx);
        a_.mov(64, Acc, rax);
        CheckMac0(Acc);
        a_.mov(32, At(G, Mac(0)), Acc);
        a_.sar(64, Acc, 12);                   // IR0 from the full sum, not from MAC0
        a_.mov(32, rax, Acc);
        Clamp(rax, 0, 0x1000, kFlagIr0Saturated);
        a_.mov(16, At(G, Ir(0)), rax);
      }
    }

    void Nclip() {
      using namespace x64;
      auto x = [this](int i) { return S16(Sxy(i)); };
      auto y = [this](int i) { return S16(Sxy(i) + 2); };
      Product(x(0), y(1)); a_.mov(64, Acc, rax);
      Product(x(1), y(2)); a_.add(64, Acc, rax);
      Product(x(2), y(0)); a_.add(64, Acc, rax);
      Product(x(0), y(2)); a_.sub(64, Acc, rax);
      Product(x(1), y(0)); a_.sub(64, Acc, rax);
      Product(x(2), y(1)); a_.sub(64, Acc, rax);
      CheckMac0(Acc);
      a_.mov(32, At(G, Mac(0)), Acc);
    }

    // AVSZ3 (first 1, SZ1-SZ3) and AVSZ4 (first 0, all four).
    void Avsz(int32_t factor, int first) {
      using namespace x64;
      a_.movzx16(rax, At(G, Sz(first)));
      for (int i = first + 1; i <= 3; ++i) {
        a_.movzx16(rdx, At(G, Sz(i)));
        a_.add(32, rax, rdx);
      }
      a_.movsx16(64, rdx, At(G, factor));
      a_.imul(64, rax, rdx);
      a_.mov(64, Acc, rax);
      CheckMac0(Acc);
      a_.mov(32, At(G, Mac(0)), Acc);
      a_.sar(64, Acc, 12);
      a_.mov(32, rax, Acc);
      Clamp(rax, 0, 0xFFFF, kFlagSz3Saturated);
      a_.mov(16, At(G, f_.otz), rax);
    }

    void Mvmva() {
      using namespace x64;
      const int m = static_cast<int>((word_ >> 17) & 3);
      const int vi = static_cast<int>((word_ >> 15) & 3);
      const int t = static_cast<int>((word_ >> 13) & 3);
      Src v[3];
      if (vi < 3)
        VertexSrc(vi, v);
      else
        IrSrc(v);
      Src rows[3][3];
      if (m < 3) {
        MatrixRows(m, rows);
      } else {
        // Not a matrix: the one the hardware reads, built from R, IR0, RT13 and RT22.
        rows[0][0].kind = Src::kNegRgbR16;
        rows[0][1].kind = Src::kRgbR16;
        rows[0][2] = S16(Ir(0));
        for (int c = 0; c < 3; ++c) {
          rows[1][c] = S16(Matrix(0, 0, 2));
          rows[2][c] = S16(Matrix(0, 1, 1));
        }
      }
      if (t != 2) {
        MatrixVector(rows, v, t, lm_);
        return;
      }
      // The far colour as translation: the translation and first product only set flags, and the
      // result is the other two products alone.
      for (int r = 0; r < 3; ++r) {
        Load(Acc, S32(Vector(2, r)));
        a_.shl(64, Acc, 12);
        Product(rows[r][0], v[0]);
        a_.add(64, Acc, rax);
        CheckMac(r + 1, Acc);
        a_.sar(64, Acc, static_cast<uint8_t>(sf_));
        a_.mov(32, rax, Acc);
        Clamp(rax, -0x8000, 0x7FFF, kFlagIr1Saturated >> r);
      }
      for (int r = 0; r < 3; ++r) {
        Product(rows[r][1], v[1]);
        a_.mov(64, Acc, rax);
        Product(rows[r][2], v[2]);
        a_.add(64, Acc, rax);
        SetMac(r + 1, Acc);
      }
      IrsFromMacs(lm_);
    }

    void Op() {
      using namespace x64;
      const Src d1 = S16(Matrix(0, 0, 0)), d2 = S16(Matrix(0, 1, 1)), d3 = S16(Matrix(0, 2, 2));
      const Src ir1 = S16(Ir(1)), ir2 = S16(Ir(2)), ir3 = S16(Ir(3));
      const Src plus[3][2] = { { d2, ir3 }, { d3, ir1 }, { d1, ir2 } };
      const Src minus[3][2] = { { d3, ir2 }, { d1, ir3 }, { d2, ir1 } };
      for (int r = 0; r < 3; ++r) {
        Product(plus[r][0], plus[r][1]);
        a_.mov(64, Acc, rax);
        Product(minus[r][0], minus[r][1]);
        a_.sub(64, Acc, rax);
        SetMac(r + 1, Acc);
      }
      IrsFromMacs(lm_);
    }

    void Sqr() {
      using namespace x64;
      for (int r = 1; r <= 3; ++r) {
        Product(S16(Ir(r)), S16(Ir(r)));
        a_.mov(64, Acc, rax);
        SetMac(r, Acc);
      }
      IrsFromMacs(lm_);
    }

    void Gpf() {
      using namespace x64;
      for (int r = 1; r <= 3; ++r) {
        Product(S16(Ir(0)), S16(Ir(r)));
        a_.mov(64, Acc, rax);
        SetMac(r, Acc);
      }
      IrsFromMacs(lm_);
      PushColourFromMac();
    }

    void Gpl() {
      using namespace x64;
      for (int r = 1; r <= 3; ++r) {
        a_.movsxd(Acc, At(G, Mac(r)));
        a_.shl(64, Acc, static_cast<uint8_t>(sf_));   // the only command to shift MAC up first
        Product(S16(Ir(0)), S16(Ir(r)));
        a_.add(64, Acc, rax);
        SetMac(r, Acc);
      }
      IrsFromMacs(lm_);
      PushColourFromMac();
    }

    // DPCS (from RGBC) and each pass of DPCT (from the oldest FIFO entry): colour << 16, toward the
    // far colour.
    void DepthCue(int32_t colour) {
      using namespace x64;
      InterpolateFarColour([this, colour](int i) {
        a_.movzx8(T, At(G, colour + i - 1));
        a_.shl(32, T, 16);
      });
      PushColourFromMac();
    }

    void Intpl() {
      using namespace x64;
      InterpolateFarColour([this](int i) {
        a_.movsx16(64, T, At(G, Ir(i)));
        a_.shl(64, T, 12);
      });
      PushColourFromMac();
    }

    void Dcpl() {
      InterpolateFarColour([this](int i) { ColourTimesIr(T, i); });
      PushColourFromMac();
    }

    // The lighting chain's first two steps: light direction, then light colour plus background.
    void Light(int vertex) {
      Src rows[3][3], v[3];
      MatrixRows(1, rows);
      VertexSrc(vertex, v);
      MatrixVector(rows, v, 3, lm_);
      LightColour();
    }
    void LightColour() {
      Src rows[3][3], v[3];
      MatrixRows(2, rows);
      IrSrc(v);
      MatrixVector(rows, v, 1, lm_);
    }

    // SetMacAndIr((colour * IR) << 4).
    void SurfaceColour() {
      for (int r = 1; r <= 3; ++r) {
        ColourTimesIr(Acc, r);
        SetMac(r, Acc);
      }
      IrsFromMacs(lm_);
    }

    void Ncs(int vertex) {
      Light(vertex);
      PushColourFromMac();
    }

    void Nccs(int vertex) {
      Light(vertex);
      SurfaceColour();
      PushColourFromMac();
    }

    void Ncds(int vertex) {
      Light(vertex);
      InterpolateFarColour([this](int i) { ColourTimesIr(T, i); });
      PushColourFromMac();
    }

    void Cc() {
      LightColour();
      SurfaceColour();
      PushColourFromMac();
    }

    void Cdp() {
      LightColour();
      InterpolateFarColour([this](int i) { ColourTimesIr(T, i); });
      PushColourFromMac();
    }

    x64::Asm a_;
    const GteLayout* layout_;
    const GteFields& f_;
    uint32_t word_;
    uint32_t opcode_;
    int sf_;
    bool lm_;
  };

  const GteLayout* layout_;
  Emitter emitter_;
  std::vector<CodeBlock*> arenas_;
  std::unordered_map<uint32_t, GteCommandFn> cache_;
  size_t bytes_ = 0;
};

}  // namespace rec
}  // namespace emulation
