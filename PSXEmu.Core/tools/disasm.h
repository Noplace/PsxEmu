// Minimal MIPS R3000A disassembler for the headless harnesses.
//
// Deliberately kept in tools/ rather than the core: the core's own
// debug_assist is a _DEBUG-only CSV logger, and a harness needs to disassemble
// in a release build without dragging that in.
#pragma once

#include <cstdint>
#include <cstdio>

namespace tools {

inline const char* RegisterName(uint32_t index) {
  static const char* kNames[32] = {
    "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3",
    "t0",   "t1", "t2", "t3", "t4", "t5", "t6", "t7",
    "s0",   "s1", "s2", "s3", "s4", "s5", "s6", "s7",
    "t8",   "t9", "k0", "k1", "gp", "sp", "fp", "ra",
  };
  return kNames[index & 31];
}

// Writes a human-readable form of `code`, executing at `pc`, into `out`.
inline void Disassemble(uint32_t pc, uint32_t code, char* out, size_t size) {
  const uint32_t op    = code >> 26;
  const uint32_t rs    = (code >> 21) & 0x1F;
  const uint32_t rt    = (code >> 16) & 0x1F;
  const uint32_t rd    = (code >> 11) & 0x1F;
  const uint32_t shamt = (code >> 6) & 0x1F;
  const uint32_t funct = code & 0x3F;
  const uint16_t imm   = static_cast<uint16_t>(code);
  const int32_t simm   = static_cast<int16_t>(code);
  const uint32_t target = (pc & 0xF0000000) | ((code & 0x03FFFFFF) << 2);
  const uint32_t branch = pc + 4 + (simm << 2);

  if (code == 0) {
    snprintf(out, size, "nop");
    return;
  }

  switch (op) {
    case 0x00:
      switch (funct) {
        case 0x00: snprintf(out, size, "sll     %s, %s, %u", RegisterName(rd), RegisterName(rt), shamt); return;
        case 0x02: snprintf(out, size, "srl     %s, %s, %u", RegisterName(rd), RegisterName(rt), shamt); return;
        case 0x03: snprintf(out, size, "sra     %s, %s, %u", RegisterName(rd), RegisterName(rt), shamt); return;
        case 0x04: snprintf(out, size, "sllv    %s, %s, %s", RegisterName(rd), RegisterName(rt), RegisterName(rs)); return;
        case 0x06: snprintf(out, size, "srlv    %s, %s, %s", RegisterName(rd), RegisterName(rt), RegisterName(rs)); return;
        case 0x07: snprintf(out, size, "srav    %s, %s, %s", RegisterName(rd), RegisterName(rt), RegisterName(rs)); return;
        case 0x08: snprintf(out, size, "jr      %s", RegisterName(rs)); return;
        case 0x09: snprintf(out, size, "jalr    %s, %s", RegisterName(rd), RegisterName(rs)); return;
        case 0x0C: snprintf(out, size, "syscall"); return;
        case 0x0D: snprintf(out, size, "break"); return;
        case 0x10: snprintf(out, size, "mfhi    %s", RegisterName(rd)); return;
        case 0x11: snprintf(out, size, "mthi    %s", RegisterName(rs)); return;
        case 0x12: snprintf(out, size, "mflo    %s", RegisterName(rd)); return;
        case 0x13: snprintf(out, size, "mtlo    %s", RegisterName(rs)); return;
        case 0x18: snprintf(out, size, "mult    %s, %s", RegisterName(rs), RegisterName(rt)); return;
        case 0x19: snprintf(out, size, "multu   %s, %s", RegisterName(rs), RegisterName(rt)); return;
        case 0x1A: snprintf(out, size, "div     %s, %s", RegisterName(rs), RegisterName(rt)); return;
        case 0x1B: snprintf(out, size, "divu    %s, %s", RegisterName(rs), RegisterName(rt)); return;
        case 0x20: snprintf(out, size, "add     %s, %s, %s", RegisterName(rd), RegisterName(rs), RegisterName(rt)); return;
        case 0x21: snprintf(out, size, "addu    %s, %s, %s", RegisterName(rd), RegisterName(rs), RegisterName(rt)); return;
        case 0x22: snprintf(out, size, "sub     %s, %s, %s", RegisterName(rd), RegisterName(rs), RegisterName(rt)); return;
        case 0x23: snprintf(out, size, "subu    %s, %s, %s", RegisterName(rd), RegisterName(rs), RegisterName(rt)); return;
        case 0x24: snprintf(out, size, "and     %s, %s, %s", RegisterName(rd), RegisterName(rs), RegisterName(rt)); return;
        case 0x25: snprintf(out, size, "or      %s, %s, %s", RegisterName(rd), RegisterName(rs), RegisterName(rt)); return;
        case 0x26: snprintf(out, size, "xor     %s, %s, %s", RegisterName(rd), RegisterName(rs), RegisterName(rt)); return;
        case 0x27: snprintf(out, size, "nor     %s, %s, %s", RegisterName(rd), RegisterName(rs), RegisterName(rt)); return;
        case 0x2A: snprintf(out, size, "slt     %s, %s, %s", RegisterName(rd), RegisterName(rs), RegisterName(rt)); return;
        case 0x2B: snprintf(out, size, "sltu    %s, %s, %s", RegisterName(rd), RegisterName(rs), RegisterName(rt)); return;
        default:   snprintf(out, size, "special?funct=0x%02X", funct); return;
      }
    case 0x01:
      switch (rt) {
        case 0x00: snprintf(out, size, "bltz    %s, 0x%08X", RegisterName(rs), branch); return;
        case 0x01: snprintf(out, size, "bgez    %s, 0x%08X", RegisterName(rs), branch); return;
        case 0x10: snprintf(out, size, "bltzal  %s, 0x%08X", RegisterName(rs), branch); return;
        case 0x11: snprintf(out, size, "bgezal  %s, 0x%08X", RegisterName(rs), branch); return;
        default:   snprintf(out, size, "regimm?rt=0x%02X", rt); return;
      }
    case 0x02: snprintf(out, size, "j       0x%08X", target); return;
    case 0x03: snprintf(out, size, "jal     0x%08X", target); return;
    case 0x04: snprintf(out, size, "beq     %s, %s, 0x%08X", RegisterName(rs), RegisterName(rt), branch); return;
    case 0x05: snprintf(out, size, "bne     %s, %s, 0x%08X", RegisterName(rs), RegisterName(rt), branch); return;
    case 0x06: snprintf(out, size, "blez    %s, 0x%08X", RegisterName(rs), branch); return;
    case 0x07: snprintf(out, size, "bgtz    %s, 0x%08X", RegisterName(rs), branch); return;
    case 0x08: snprintf(out, size, "addi    %s, %s, %d", RegisterName(rt), RegisterName(rs), simm); return;
    case 0x09: snprintf(out, size, "addiu   %s, %s, %d", RegisterName(rt), RegisterName(rs), simm); return;
    case 0x0A: snprintf(out, size, "slti    %s, %s, %d", RegisterName(rt), RegisterName(rs), simm); return;
    case 0x0B: snprintf(out, size, "sltiu   %s, %s, %d", RegisterName(rt), RegisterName(rs), simm); return;
    case 0x0C: snprintf(out, size, "andi    %s, %s, 0x%04X", RegisterName(rt), RegisterName(rs), imm); return;
    case 0x0D: snprintf(out, size, "ori     %s, %s, 0x%04X", RegisterName(rt), RegisterName(rs), imm); return;
    case 0x0E: snprintf(out, size, "xori    %s, %s, 0x%04X", RegisterName(rt), RegisterName(rs), imm); return;
    case 0x0F: snprintf(out, size, "lui     %s, 0x%04X", RegisterName(rt), imm); return;
    case 0x10:
      switch (rs) {
        case 0x00: snprintf(out, size, "mfc0    %s, cop0r%u", RegisterName(rt), rd); return;
        case 0x04: snprintf(out, size, "mtc0    %s, cop0r%u", RegisterName(rt), rd); return;
        case 0x10: snprintf(out, size, "rfe"); return;
        default:   snprintf(out, size, "cop0?rs=0x%02X", rs); return;
      }
    case 0x12: snprintf(out, size, "cop2    0x%07X", code & 0x1FFFFFF); return;
    case 0x20: snprintf(out, size, "lb      %s, %d(%s)", RegisterName(rt), simm, RegisterName(rs)); return;
    case 0x21: snprintf(out, size, "lh      %s, %d(%s)", RegisterName(rt), simm, RegisterName(rs)); return;
    case 0x22: snprintf(out, size, "lwl     %s, %d(%s)", RegisterName(rt), simm, RegisterName(rs)); return;
    case 0x23: snprintf(out, size, "lw      %s, %d(%s)", RegisterName(rt), simm, RegisterName(rs)); return;
    case 0x24: snprintf(out, size, "lbu     %s, %d(%s)", RegisterName(rt), simm, RegisterName(rs)); return;
    case 0x25: snprintf(out, size, "lhu     %s, %d(%s)", RegisterName(rt), simm, RegisterName(rs)); return;
    case 0x26: snprintf(out, size, "lwr     %s, %d(%s)", RegisterName(rt), simm, RegisterName(rs)); return;
    case 0x28: snprintf(out, size, "sb      %s, %d(%s)", RegisterName(rt), simm, RegisterName(rs)); return;
    case 0x29: snprintf(out, size, "sh      %s, %d(%s)", RegisterName(rt), simm, RegisterName(rs)); return;
    case 0x2A: snprintf(out, size, "swl     %s, %d(%s)", RegisterName(rt), simm, RegisterName(rs)); return;
    case 0x2B: snprintf(out, size, "sw      %s, %d(%s)", RegisterName(rt), simm, RegisterName(rs)); return;
    case 0x2E: snprintf(out, size, "swr     %s, %d(%s)", RegisterName(rt), simm, RegisterName(rs)); return;
    case 0x32: snprintf(out, size, "lwc2    cop2r%u, %d(%s)", rt, simm, RegisterName(rs)); return;
    case 0x3A: snprintf(out, size, "swc2    cop2r%u, %d(%s)", rt, simm, RegisterName(rs)); return;
    default:   snprintf(out, size, "?op=0x%02X", op); return;
  }
}

}  // namespace tools
