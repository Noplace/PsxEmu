#pragma once

// A MIPS R3000A disassembler: one instruction word to one line of text.
//
// Header-only and free of the rest of the core, so a harness can use it without a System. Two
// users: boot_runner's traces and --dis, and the debugger's window (psx/debugger.h, which
// disassembles on the machine's thread and hands the text over in a snapshot).
//
// Every encoding the CPU executes has a name here, including the ones assemblers never write -
// the REGIMM aliases the hardware decodes from rt's low bit and bits 4-1 (bug 68) - so a
// disassembly never says "unknown" about an instruction that runs.

#include <cstdint>
#include <cstdio>

namespace emulation {
namespace psx {

inline const char* RegisterName(uint32_t index) {
  static const char* kNames[32] = {
    "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3",
    "t0",   "t1", "t2", "t3", "t4", "t5", "t6", "t7",
    "s0",   "s1", "s2", "s3", "s4", "s5", "s6", "s7",
    "t8",   "t9", "k0", "k1", "gp", "sp", "fp", "ra",
  };
  return kNames[index & 31];
}

// The Cop0 registers this CPU has, by number; the rest print as cop0rN.
inline const char* Cop0Name(uint32_t index) {
  switch (index & 31) {
    case 3:  return "bpc";
    case 5:  return "bda";
    case 6:  return "jumpdest";
    case 7:  return "dcic";
    case 8:  return "badvaddr";
    case 9:  return "bdam";
    case 11: return "bpcm";
    case 12: return "sr";
    case 13: return "cause";
    case 14: return "epc";
    case 15: return "prid";
    default: return nullptr;
  }
}

// The GTE's commands, by the low six bits of a cop2 command word.
inline const char* GteCommandName(uint32_t code) {
  switch (code & 0x3F) {
    case 0x01: return "rtps";
    case 0x06: return "nclip";
    case 0x0C: return "op";
    case 0x10: return "dpcs";
    case 0x11: return "intpl";
    case 0x12: return "mvmva";
    case 0x13: return "ncds";
    case 0x14: return "cdp";
    case 0x16: return "ncdt";
    case 0x1B: return "nccs";
    case 0x1C: return "cc";
    case 0x1E: return "ncs";
    case 0x20: return "nct";
    case 0x28: return "sqr";
    case 0x29: return "dcpl";
    case 0x2A: return "dpct";
    case 0x2D: return "avsz3";
    case 0x2E: return "avsz4";
    case 0x30: return "rtpt";
    case 0x3D: return "gpf";
    case 0x3E: return "gpl";
    case 0x3F: return "ncct";
    default:   return nullptr;
  }
}

// Whether `code` is a branch or jump - an instruction with a delay slot after it.
inline bool HasDelaySlot(uint32_t code) {
  const uint32_t op = code >> 26;
  if (op == 0x00)
    return (code & 0x3F) == 0x08 || (code & 0x3F) == 0x09;   // jr, jalr
  return op >= 0x01 && op <= 0x07;                             // REGIMM, j, jal, beq..bgtz
}

// Where a branch or jump at `pc` goes, if that is known from the word alone - every one except
// jr and jalr, whose target is in a register.
inline bool StaticTarget(uint32_t pc, uint32_t code, uint32_t* target) {
  const uint32_t op = code >> 26;
  if (op == 0x02 || op == 0x03) {
    *target = ((pc + 4) & 0xF0000000) | ((code & 0x03FFFFFF) << 2);
    return true;
  }
  if (op == 0x01 || (op >= 0x04 && op <= 0x07)) {
    *target = pc + 4 + (static_cast<uint32_t>(static_cast<int16_t>(code)) << 2);
    return true;
  }
  return false;
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
  uint32_t target = 0;
  StaticTarget(pc, code, &target);

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
    case 0x01: {
      // The hardware decodes only rt's low bit (gez or ltz) and whether bits 4-1 are 1000b (link),
      // so every rt value is some branch. The canonical four get their names; the rest are shown
      // as what they do, marked as aliases.
      const bool link = (rt & 0x1E) == 0x10;
      const char* name = (rt & 1) ? (link ? "bgezal" : "bgez") : (link ? "bltzal" : "bltz");
      const bool canonical = rt == 0x00 || rt == 0x01 || rt == 0x10 || rt == 0x11;
      snprintf(out, size, "%-7s %s, 0x%08X%s", name, RegisterName(rs), target,
               canonical ? "" : "   ; alias");
      return;
    }
    case 0x02: snprintf(out, size, "j       0x%08X", target); return;
    case 0x03: snprintf(out, size, "jal     0x%08X", target); return;
    case 0x04:
      if (rs == 0 && rt == 0) {
        snprintf(out, size, "b       0x%08X", target);
        return;
      }
      snprintf(out, size, "beq     %s, %s, 0x%08X", RegisterName(rs), RegisterName(rt), target);
      return;
    case 0x05: snprintf(out, size, "bne     %s, %s, 0x%08X", RegisterName(rs), RegisterName(rt), target); return;
    case 0x06: snprintf(out, size, "blez    %s, 0x%08X", RegisterName(rs), target); return;
    case 0x07: snprintf(out, size, "bgtz    %s, 0x%08X", RegisterName(rs), target); return;
    case 0x08: snprintf(out, size, "addi    %s, %s, %d", RegisterName(rt), RegisterName(rs), simm); return;
    case 0x09: snprintf(out, size, "addiu   %s, %s, %d", RegisterName(rt), RegisterName(rs), simm); return;
    case 0x0A: snprintf(out, size, "slti    %s, %s, %d", RegisterName(rt), RegisterName(rs), simm); return;
    case 0x0B: snprintf(out, size, "sltiu   %s, %s, %d", RegisterName(rt), RegisterName(rs), simm); return;
    case 0x0C: snprintf(out, size, "andi    %s, %s, 0x%04X", RegisterName(rt), RegisterName(rs), imm); return;
    case 0x0D: snprintf(out, size, "ori     %s, %s, 0x%04X", RegisterName(rt), RegisterName(rs), imm); return;
    case 0x0E: snprintf(out, size, "xori    %s, %s, 0x%04X", RegisterName(rt), RegisterName(rs), imm); return;
    case 0x0F: snprintf(out, size, "lui     %s, 0x%04X", RegisterName(rt), imm); return;
    case 0x10: {
      const char* name = Cop0Name(rd);
      char reg[16];
      if (name != nullptr)
        snprintf(reg, sizeof(reg), "%s", name);
      else
        snprintf(reg, sizeof(reg), "cop0r%u", rd);
      switch (rs) {
        case 0x00: snprintf(out, size, "mfc0    %s, %s", RegisterName(rt), reg); return;
        case 0x04: snprintf(out, size, "mtc0    %s, %s", RegisterName(rt), reg); return;
        case 0x10: snprintf(out, size, "rfe"); return;
        default:   snprintf(out, size, "cop0?rs=0x%02X", rs); return;
      }
    }
    case 0x12:
      if (rs & 0x10) {
        const char* name = GteCommandName(code);
        if (name != nullptr)
          snprintf(out, size, "%-7s 0x%07X", name, code & 0x1FFFFFF);
        else
          snprintf(out, size, "cop2    0x%07X", code & 0x1FFFFFF);
        return;
      }
      switch (rs) {
        case 0x00: snprintf(out, size, "mfc2    %s, cop2r%u", RegisterName(rt), rd); return;
        case 0x02: snprintf(out, size, "cfc2    %s, cop2r%u", RegisterName(rt), rd + 32); return;
        case 0x04: snprintf(out, size, "mtc2    %s, cop2r%u", RegisterName(rt), rd); return;
        case 0x06: snprintf(out, size, "ctc2    %s, cop2r%u", RegisterName(rt), rd + 32); return;
        default:   snprintf(out, size, "cop2?rs=0x%02X", rs); return;
      }
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

}  // namespace psx
}  // namespace emulation
