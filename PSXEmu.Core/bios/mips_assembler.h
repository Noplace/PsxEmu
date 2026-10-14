#pragma once

// A very small MIPS I assembler, for the few pieces of the HLE BIOS (psx/hle_bios.h) that have to
// be real code in guest RAM rather than a host function: the exception vector, the A0h/B0h/C0h
// dispatchers and the exception handler. Games read, verify and patch those by address, so they
// are written out instruction by instruction, not trapped.
//
// Only what those pieces use. Branch targets are labels, resolved by Finish().

#include <cstdint>
#include <vector>

namespace emulation {
namespace psx {

enum MipsRegister : uint32_t {
  kR0 = 0, kAt = 1, kV0 = 2, kV1 = 3, kA0 = 4, kA1 = 5, kA2 = 6, kA3 = 7,
  kT0 = 8, kT1 = 9, kT2 = 10, kT3 = 11, kT4 = 12, kT5 = 13, kT6 = 14, kT7 = 15,
  kS0 = 16, kS1 = 17, kS2 = 18, kS3 = 19, kS4 = 20, kS5 = 21, kS6 = 22, kS7 = 23,
  kT8 = 24, kT9 = 25, kK0 = 26, kK1 = 27, kGp = 28, kSp = 29, kFp = 30, kRa = 31,
};

class MipsAssembler {
 public:
  explicit MipsAssembler(uint32_t origin) : origin_(origin) {}

  // Where the next instruction goes.
  uint32_t pc() const { return origin_ + static_cast<uint32_t>(words_.size()) * 4; }
  uint32_t origin() const { return origin_; }
  const std::vector<uint32_t>& words() const { return words_; }

  void Word(uint32_t word) { words_.push_back(word); }
  void Nop() { Word(0); }
  void Nops(int count) { for (int i = 0; i < count; ++i) Nop(); }

  // ALU, immediate.
  void Addi(uint32_t rt, uint32_t rs, int16_t imm) { I(0x08, rs, rt, imm); }
  void Addiu(uint32_t rt, uint32_t rs, int16_t imm) { I(0x09, rs, rt, imm); }
  void Andi(uint32_t rt, uint32_t rs, uint16_t imm) { I(0x0C, rs, rt, imm); }
  void Ori(uint32_t rt, uint32_t rs, uint16_t imm) { I(0x0D, rs, rt, imm); }
  void Lui(uint32_t rt, uint16_t imm) { I(0x0F, 0, rt, imm); }

  // A 32-bit constant in two instructions, always two - so code laid out to fixed offsets stays
  // the length it was written to be.
  void Li32(uint32_t rt, uint32_t value) {
    Lui(rt, static_cast<uint16_t>(value >> 16));
    Ori(rt, rt, static_cast<uint16_t>(value));
  }
  // The same as lui + addiu, which is how the Sony kernel writes an address: the form a game
  // decoding one expects (the low half sign-extends, so the high half is rounded up for it).
  void La(uint32_t rt, uint32_t address) {
    Lui(rt, static_cast<uint16_t>((address + 0x8000) >> 16));
    Addiu(rt, rt, static_cast<int16_t>(address & 0xFFFF));
  }

  // ALU, register.
  void Addu(uint32_t rd, uint32_t rs, uint32_t rt) { R(rs, rt, rd, 0, 0x21); }
  void Add(uint32_t rd, uint32_t rs, uint32_t rt) { R(rs, rt, rd, 0, 0x20); }
  void Subu(uint32_t rd, uint32_t rs, uint32_t rt) { R(rs, rt, rd, 0, 0x23); }
  void Or(uint32_t rd, uint32_t rs, uint32_t rt) { R(rs, rt, rd, 0, 0x25); }
  void Sll(uint32_t rd, uint32_t rt, uint32_t sa) { R(0, rt, rd, sa, 0x00); }
  void Srl(uint32_t rd, uint32_t rt, uint32_t sa) { R(0, rt, rd, sa, 0x02); }
  void Mfhi(uint32_t rd) { R(0, 0, rd, 0, 0x10); }
  void Mthi(uint32_t rs) { R(rs, 0, 0, 0, 0x11); }
  void Mflo(uint32_t rd) { R(0, 0, rd, 0, 0x12); }
  void Mtlo(uint32_t rs) { R(rs, 0, 0, 0, 0x13); }
  void Move(uint32_t rd, uint32_t rs) { Addu(rd, rs, kR0); }

  // Memory.
  void Lw(uint32_t rt, int16_t offset, uint32_t base) { I(0x23, base, rt, offset); }
  void Sw(uint32_t rt, int16_t offset, uint32_t base) { I(0x2B, base, rt, offset); }

  // Jumps. J and Jal take an absolute target in the same 256 MB region.
  void Jr(uint32_t rs) { R(rs, 0, 0, 0, 0x08); }
  void Jalr(uint32_t rs, uint32_t rd = kRa) { R(rs, 0, rd, 0, 0x09); }
  void J(uint32_t target) { Word((0x02u << 26) | ((target >> 2) & 0x03FFFFFF)); }
  void Jal(uint32_t target) { Word((0x03u << 26) | ((target >> 2) & 0x03FFFFFF)); }

  // Branches to a label, bound before or after.
  int NewLabel() {
    labels_.push_back(kUnbound);
    return static_cast<int>(labels_.size()) - 1;
  }
  void Bind(int label) { labels_[label] = pc(); }
  void Beq(uint32_t rs, uint32_t rt, int label) { Branch(0x04, rs, rt, label); }
  void Bne(uint32_t rs, uint32_t rt, int label) { Branch(0x05, rs, rt, label); }

  // Coprocessor 0.
  void Mfc0(uint32_t rt, uint32_t rd) { Word((0x10u << 26) | (0u << 21) | (rt << 16) | (rd << 11)); }
  void Mtc0(uint32_t rt, uint32_t rd) { Word((0x10u << 26) | (4u << 21) | (rt << 16) | (rd << 11)); }
  void Rfe() { Word(0x42000010); }

  // Resolves every branch. False if one names a label that was never bound, or one too far away.
  bool Finish() {
    for (const Fixup& fixup : fixups_) {
      const uint32_t target = labels_[fixup.label];
      if (target == kUnbound)
        return false;
      const uint32_t at = origin_ + fixup.index * 4;
      const int32_t offset = static_cast<int32_t>(target - (at + 4)) / 4;
      if (offset < -32768 || offset > 32767)
        return false;
      words_[fixup.index] |= static_cast<uint16_t>(offset);
    }
    fixups_.clear();
    return true;
  }

 private:
  static const uint32_t kUnbound = 0xFFFFFFFFu;
  struct Fixup {
    uint32_t index;
    int label;
  };

  void I(uint32_t op, uint32_t rs, uint32_t rt, uint32_t imm) {
    Word((op << 26) | (rs << 21) | (rt << 16) | (imm & 0xFFFF));
  }
  void R(uint32_t rs, uint32_t rt, uint32_t rd, uint32_t sa, uint32_t funct) {
    Word((rs << 21) | (rt << 16) | (rd << 11) | (sa << 6) | funct);
  }
  void Branch(uint32_t op, uint32_t rs, uint32_t rt, int label) {
    fixups_.push_back(Fixup{ static_cast<uint32_t>(words_.size()), label });
    I(op, rs, rt, 0);
  }

  uint32_t origin_;
  std::vector<uint32_t> words_;
  std::vector<uint32_t> labels_;
  std::vector<Fixup> fixups_;
};

}  // namespace psx
}  // namespace emulation
