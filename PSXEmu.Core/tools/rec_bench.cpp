// rec_bench - how fast the recompiler's output actually is.
//
// Docs/Recompiler-Plan.md makes step 6, register allocation, conditional:
// "then, and only then, register allocation, if the measurements say the
// load/store traffic is what is left to win". This is that measurement, and
// the answer it gives decides whether the allocator stays on.
//
// Two numbers come out of it:
//
//   - **Compiled against interpreted.** The interpreter here is rec_test's
//     small reference one, not this project's real Cpu, so the ratio is not a
//     prediction of what the emulator would gain. A real interpreter does more
//     per instruction - timing, the load pipeline, interrupt checks - so if
//     anything this understates the gap. What it does measure honestly is the
//     throughput of the emitted code.
//   - **Allocated against not.** Both compiled by the same compiler from the
//     same blocks, differing only in whether guest registers live in host
//     registers or in memory. That comparison is exact, and it is the one step
//     6 turns on.
//
// Nothing here touches PSXEmu.Core/psx.

#include "rec/block_compiler.h"
#include "rec/block_decoder.h"
#include "rec/recompiler.h"
#include "rec/runtime.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <windows.h>

namespace {

const uint32_t kBase = 0x80000000;
const int kBusBytes = 32768;
const uint32_t kProgramBase = 0x80001000;
const uint32_t kDataBase = 0x80004000;

class Memory {
 public:
  Memory() { memset(bytes_, 0, sizeof(bytes_)); }

  uint32_t Read(uint32_t address, int width) const {
    uint32_t value = 0;
    for (int i = 0; i < width; ++i)
      value |= static_cast<uint32_t>(bytes_[Index(address + i)]) << (i * 8);
    return value;
  }
  void Write(uint32_t address, int width, uint32_t value) {
    for (int i = 0; i < width; ++i)
      bytes_[Index(address + i)] = static_cast<uint8_t>(value >> (i * 8));
  }
  uint32_t ReadWord(uint32_t address) const { return Read(address, 4); }

  void WriteProgram(uint32_t address, const std::vector<uint32_t>& words) {
    for (size_t i = 0; i < words.size(); ++i)
      Write(address + static_cast<uint32_t>(i) * 4, 4, words[i]);
  }

  emulation::rec::FetchWord Fetch() {
    return [this](uint32_t pc, uint32_t* word) {
      *word = ReadWord(pc);
      return true;
    };
  }

  static uint32_t Load32(void* c, uint32_t a, uint32_t) { return Of(c)->Read(a, 4); }
  static uint32_t Load16(void* c, uint32_t a, uint32_t) { return Of(c)->Read(a, 2); }
  static uint32_t Load8(void* c, uint32_t a, uint32_t) { return Of(c)->Read(a, 1); }
  static void Store32(void* c, uint32_t a, uint32_t v, uint32_t) { Of(c)->Write(a, 4, v); }
  static void Store16(void* c, uint32_t a, uint32_t v, uint32_t) { Of(c)->Write(a, 2, v); }
  static void Store8(void* c, uint32_t a, uint32_t v, uint32_t) { Of(c)->Write(a, 1, v); }

 private:
  static Memory* Of(void* context) { return static_cast<Memory*>(context); }
  static int Index(uint32_t address) {
    return static_cast<int>((address - kBase) & (kBusBytes - 1));
  }
  uint8_t bytes_[kBusBytes];
};

// The same reference interpreter rec_test measures against, cut down to what
// the benchmark programs use. Deliberately straightforward - a switch and a
// register file - because an interpreter tuned for this comparison would be
// measuring the tuning.
class Interpreter {
 public:
  uint32_t r[32] = {};
  Memory* memory = nullptr;
  uint64_t instructions = 0;

  uint32_t Run(uint32_t pc) {
    const uint32_t word = memory->ReadWord(pc);
    next_pc_ = pc + 4;
    Step(pc, word);
    const emulation::rec::Kind kind = emulation::rec::BlockDecoder::Classify(word);
    if (kind == emulation::rec::Kind::kBranch ||
        kind == emulation::rec::Kind::kJump) {
      const uint32_t target = next_pc_;
      next_pc_ = pc + 8;
      Step(pc + 4, memory->ReadWord(pc + 4));
      return target;
    }
    return next_pc_;
  }

  bool LoadInFlight() const { return pending_.active || armed_.active; }

 private:
  struct Load {
    uint32_t reg = 0;
    uint32_t value = 0;
    bool active = false;
  };
  Load pending_;
  Load armed_;
  uint32_t next_pc_ = 0;

  void Advance() {
    if (pending_.active) {
      if (pending_.reg != 0)
        r[pending_.reg] = pending_.value;
      pending_.active = false;
    }
    if (armed_.active) {
      pending_ = armed_;
      armed_.active = false;
    }
  }
  void Write(uint32_t index, uint32_t value) {
    if (index != 0)
      r[index] = value;
    if (pending_.active && pending_.reg == index)
      pending_.active = false;
  }
  void Arm(uint32_t index, uint32_t value) {
    if (pending_.active && pending_.reg == index)
      pending_.active = false;
    armed_.reg = index;
    armed_.value = value;
    armed_.active = (index != 0);
  }

  void Step(uint32_t pc, uint32_t word) {
    Advance();
    ++instructions;

    const uint32_t opcode = word >> 26;
    const uint32_t rs = (word >> 21) & 0x1F;
    const uint32_t rt = (word >> 16) & 0x1F;
    const uint32_t rd = (word >> 11) & 0x1F;
    const uint32_t sa = (word >> 6) & 0x1F;
    const uint16_t imm = static_cast<uint16_t>(word & 0xFFFF);
    const uint32_t se =
        static_cast<uint32_t>(static_cast<int32_t>(static_cast<int16_t>(imm)));
    const uint32_t taken = pc + 4 + (se << 2);
    const uint32_t fall = pc + 8;
    const uint32_t address = r[rs] + se;

    if (opcode == 0) {
      switch (word & 0x3F) {
        case 0x00: Write(rd, r[rt] << sa); return;
        case 0x02: Write(rd, r[rt] >> sa); return;
        case 0x03: Write(rd, static_cast<uint32_t>(static_cast<int32_t>(r[rt]) >> sa)); return;
        case 0x08: next_pc_ = r[rs]; return;
        case 0x21: Write(rd, r[rs] + r[rt]); return;
        case 0x23: Write(rd, r[rs] - r[rt]); return;
        case 0x24: Write(rd, r[rs] & r[rt]); return;
        case 0x25: Write(rd, r[rs] | r[rt]); return;
        case 0x26: Write(rd, r[rs] ^ r[rt]); return;
        case 0x2A: Write(rd, (static_cast<int32_t>(r[rs]) < static_cast<int32_t>(r[rt])) ? 1u : 0u); return;
        case 0x2B: Write(rd, (r[rs] < r[rt]) ? 1u : 0u); return;
        default: return;
      }
    }
    switch (opcode) {
      case 0x04: next_pc_ = (r[rs] == r[rt]) ? taken : fall; return;
      case 0x05: next_pc_ = (r[rs] != r[rt]) ? taken : fall; return;
      case 0x06: next_pc_ = (static_cast<int32_t>(r[rs]) <= 0) ? taken : fall; return;
      case 0x07: next_pc_ = (static_cast<int32_t>(r[rs]) > 0) ? taken : fall; return;
      case 0x09: Write(rt, r[rs] + se); return;
      case 0x0C: Write(rt, r[rs] & imm); return;
      case 0x0D: Write(rt, r[rs] | imm); return;
      case 0x0F: Write(rt, static_cast<uint32_t>(imm) << 16); return;
      case 0x23: Arm(rt, memory->Read(address, 4)); return;
      case 0x24: Arm(rt, memory->Read(address, 1)); return;
      case 0x2B: memory->Write(address, 4, r[rt]); return;
      case 0x28: memory->Write(address, 1, r[rt]); return;
      default: return;
    }
  }
};

// ---------------------------------------------------------------------------

uint32_t Special(uint32_t rs, uint32_t rt, uint32_t rd, uint32_t funct) {
  return (rs << 21) | (rt << 16) | (rd << 11) | funct;
}
uint32_t ADDU(uint32_t rd, uint32_t rs, uint32_t rt) { return Special(rs, rt, rd, 0x21); }
uint32_t XOR_(uint32_t rd, uint32_t rs, uint32_t rt) { return Special(rs, rt, rd, 0x26); }
uint32_t SLTU_(uint32_t rd, uint32_t rs, uint32_t rt) { return Special(rs, rt, rd, 0x2B); }
uint32_t SLL_(uint32_t rd, uint32_t rt, uint32_t sa) { return (rt << 16) | (rd << 11) | (sa << 6); }
uint32_t ADDIU(uint32_t rt, uint32_t rs, uint16_t imm) {
  return (0x09u << 26) | (rs << 21) | (rt << 16) | imm;
}
uint32_t ANDI_(uint32_t rt, uint32_t rs, uint16_t imm) {
  return (0x0Cu << 26) | (rs << 21) | (rt << 16) | imm;
}
uint32_t LUI_(uint32_t rt, uint16_t imm) { return (0x0Fu << 26) | (rt << 16) | imm; }
uint32_t ORI_(uint32_t rt, uint32_t rs, uint16_t imm) {
  return (0x0Du << 26) | (rs << 21) | (rt << 16) | imm;
}
uint32_t LW(uint32_t rt, uint32_t rs, uint16_t imm) {
  return (0x23u << 26) | (rs << 21) | (rt << 16) | imm;
}
uint32_t SW(uint32_t rt, uint32_t rs, uint16_t imm) {
  return (0x2Bu << 26) | (rs << 21) | (rt << 16) | imm;
}
uint32_t BNE(uint32_t rs, uint32_t rt, uint16_t offset) {
  return (0x05u << 26) | (rs << 21) | (rt << 16) | offset;
}
uint32_t JR(uint32_t rs) { return Special(rs, 0, 0, 0x08); }
uint32_t NOP() { return 0; }

struct Program {
  const char* name;
  const char* what;
  std::vector<uint32_t> words;
};

// Register-only arithmetic: the case allocation should help most, since every
// operand of every instruction is a register the block uses repeatedly.
Program ArithmeticLoop(uint32_t iterations) {
  return { "arithmetic",
           "register-only work, the case allocation should suit best",
           {
               ADDIU(1, 0, static_cast<uint16_t>(iterations)),  // 0: counter
               ADDU(2, 2, 1),          // 1: loop body, all registers
               XOR_(3, 3, 2),
               SLL_(4, 3, 3),
               ADDU(5, 5, 4),
               SLTU_(6, 5, 2),
               ADDU(2, 2, 6),
               ADDIU(1, 1, 0xFFFF),    // 7: counter down
               BNE(1, 0, 0xFFF8),      // 8: back to index 1
               NOP(),                  // 9
               JR(0),                  // 10
               NOP(),                  // 11
           } };
}

// A loop that touches memory every iteration, which is what real code does and
// where the call out to the host's load and store dominates everything else.
Program MemoryLoop(uint32_t iterations) {
  return { "memory",
           "a load and a store every iteration, as real code does",
           {
               ADDIU(1, 0, static_cast<uint16_t>(iterations)),
               LUI_(7, static_cast<uint16_t>(kDataBase >> 16)),
               ORI_(7, 7, static_cast<uint16_t>(kDataBase & 0xFFFF)),
               LW(2, 7, 0),            // 3: loop body
               NOP(),                  // 4: the load delay slot
               ADDU(2, 2, 1),
               SW(2, 7, 0),
               ANDI_(3, 2, 0x00FF),
               ADDIU(1, 1, 0xFFFF),
               BNE(1, 0, 0xFFF9),      // 9: back to index 3
               NOP(),
               JR(0),
               NOP(),
           } };
}

// A loop whose body is one long block rather than a handful of instructions.
//
// This one exists to answer a question the first two raise rather than settle:
// allocation costs a fixed amount per *block entry* - the pushes, the loads,
// the write-backs - and saves a little per *instruction*. With a nine
// instruction block the fixed cost has nothing to amortise over. If that is the
// explanation, a fifty-instruction block should show allocation winning, and if
// it does not then the explanation is wrong.
Program BlockOfSize(uint32_t iterations, int quads) {
  std::vector<uint32_t> words;
  words.push_back(ADDIU(1, 0, static_cast<uint16_t>(iterations)));
  const uint32_t body = static_cast<uint32_t>(words.size());
  for (int i = 0; i < quads; ++i) {
    words.push_back(ADDU(2, 2, 3));
    words.push_back(XOR_(3, 3, 4));
    words.push_back(ADDU(4, 4, 2));
    words.push_back(ADDIU(2, 2, 1));
  }
  words.push_back(ADDIU(1, 1, 0xFFFF));

  const uint32_t branch = static_cast<uint32_t>(words.size());
  const int32_t offset = static_cast<int32_t>(body) -
                         (static_cast<int32_t>(branch) + 1);
  words.push_back(BNE(1, 0, static_cast<uint16_t>(offset)));
  words.push_back(NOP());
  words.push_back(JR(0));
  words.push_back(NOP());

  return { "long block",
           "a fifty-instruction body, so the block entry cost amortises",
           words };
}

Program LongBlock(uint32_t iterations) { return BlockOfSize(iterations, 12); }

double Seconds(LARGE_INTEGER start, LARGE_INTEGER end, LARGE_INTEGER frequency) {
  return static_cast<double>(end.QuadPart - start.QuadPart) /
         static_cast<double>(frequency.QuadPart);
}

struct Result {
  double seconds = 0;
  uint64_t instructions = 0;
  uint32_t checksum = 0;
  uint64_t blocks_compiled = 0;
  int registers_allocated = 0;
};

uint32_t Checksum(const uint32_t regs[32]) {
  uint32_t sum = 0;
  for (int i = 0; i < 32; ++i)
    sum = sum * 31 + regs[i];
  return sum;
}

// Three rounds, and the fastest counts. A single run of anything this short is
// mostly noise - the first version of this benchmark reported 0.000 seconds and
// a speedup computed from it, which is worth nothing - and the minimum of
// several rounds is the least contaminated by whatever else the machine was
// doing.
const int kRounds = 3;

Result RunInterpreted(const Program& program, int repeats) {
  Memory memory;
  memory.WriteProgram(kProgramBase, program.words);
  Interpreter interpreter;
  interpreter.memory = &memory;

  LARGE_INTEGER frequency, start, end;
  QueryPerformanceFrequency(&frequency);

  Result result;
  result.seconds = 1e30;
  uint64_t before = 0;
  for (int round = 0; round < kRounds; ++round) {
    before = interpreter.instructions;
    QueryPerformanceCounter(&start);
    for (int i = 0; i < repeats; ++i) {
      memset(interpreter.r, 0, sizeof(interpreter.r));
      memory.Write(kDataBase, 4, 0);
      uint32_t pc = kProgramBase;
      while (pc != 0)
        pc = interpreter.Run(pc);
    }
    QueryPerformanceCounter(&end);
    const double seconds = Seconds(start, end, frequency);
    if (seconds < result.seconds) {
      result.seconds = seconds;
      result.instructions = interpreter.instructions - before;
    }
  }
  result.checksum = Checksum(interpreter.r);
  return result;
}

Result RunCompiled(const Program& program, bool allocate, int repeats,
                   bool link = true) {
  Memory memory;
  memory.WriteProgram(kProgramBase, program.words);
  Interpreter fallback;
  fallback.memory = &memory;

  emulation::rec::HostInterface host;
  host.context = &memory;
  host.fetch = memory.Fetch();
  host.load32 = &Memory::Load32;
  host.load16 = &Memory::Load16;
  host.load8 = &Memory::Load8;
  host.store32 = &Memory::Store32;
  host.store16 = &Memory::Store16;
  host.store8 = &Memory::Store8;
  host.interpret = [&fallback](uint32_t pc) { return fallback.Run(pc); };
  host.load_in_flight = [&fallback]() { return fallback.LoadInFlight(); };

  emulation::rec::Recompiler recompiler(host, fallback.r);
  recompiler.set_allocate_registers(allocate);
  recompiler.set_link_blocks(link);

  // Compile everything first, so the measurement is of running the code and
  // not of emitting it. A second run over the same program finds every block
  // already in the cache.
  uint32_t warm = kProgramBase;
  int guard = 0;
  while (warm != 0 && guard++ < 100000000)
    warm = recompiler.Step(warm);
  const uint64_t compiled = recompiler.stats().blocks_compiled;

  LARGE_INTEGER frequency, start, end;
  QueryPerformanceFrequency(&frequency);

  Result result;
  result.seconds = 1e30;
  for (int round = 0; round < kRounds; ++round) {
    const uint64_t before = recompiler.stats().instructions_compiled +
                            recompiler.stats().instructions_interpreted;
    QueryPerformanceCounter(&start);
    for (int i = 0; i < repeats; ++i) {
      memset(fallback.r, 0, sizeof(fallback.r));
      memory.Write(kDataBase, 4, 0);
      uint32_t pc = kProgramBase;
      while (pc != 0)
        pc = recompiler.Step(pc);
    }
    QueryPerformanceCounter(&end);
    const double seconds = Seconds(start, end, frequency);
    if (seconds < result.seconds) {
      result.seconds = seconds;
      result.instructions = recompiler.stats().instructions_compiled +
                            recompiler.stats().instructions_interpreted - before;
    }
  }

  result.checksum = Checksum(fallback.r);
  result.blocks_compiled = compiled;
  result.registers_allocated =
      static_cast<int>(recompiler.stats().blocks_with_allocation);
  return result;
}

double Rate(const Result& result) {
  return static_cast<double>(result.instructions) / result.seconds;
}

// The speedup column is a ratio of rates, not of wall-clock times: the
// interpreter runs a tenth of the repeats, so its seconds are not comparable
// with anything. Dividing the times instead is how the first version of this
// managed to report the compiled code as slower than the interpreter.
void Report(const char* label, const Result& result, double baseline_rate) {
  printf("  %-24s %8.3f s   %9.1f M inst/s", label, result.seconds,
         Rate(result) / 1e6);
  if (baseline_rate > 0)
    printf("   %5.2fx", Rate(result) / baseline_rate);
  printf("\n");
}

void Measure(const Program& program, int repeats) {
  printf("\n%s - %s\n", program.name, program.what);

  // The interpreter is an order of magnitude slower, so it runs proportionally
  // fewer repeats. Both are reported as instructions a second, which is what
  // makes them comparable regardless of how much work each did.
  const Result interpreted = RunInterpreted(program, (repeats / 10) + 1);
  const Result unlinked = RunCompiled(program, false, repeats, false);
  const Result memory_registers = RunCompiled(program, false, repeats, true);
  const Result allocated = RunCompiled(program, true, repeats, true);

  Report("interpreted", interpreted, 0);
  Report("compiled, no linking", unlinked, Rate(interpreted));
  Report("compiled, linked", memory_registers, Rate(interpreted));
  Report("compiled, linked + allocd", allocated, Rate(interpreted));

  printf("  linking on vs off:       %.2fx\n",
         Rate(memory_registers) / Rate(unlinked));
  printf("  allocation on vs off:    %.2fx\n",
         Rate(allocated) / Rate(memory_registers));

  // The numbers are worthless if the three runs did not compute the same
  // thing, so say so rather than printing a speedup for different work.
  if (interpreted.checksum != memory_registers.checksum ||
      interpreted.checksum != allocated.checksum ||
      interpreted.checksum != unlinked.checksum) {
    printf("  *** MISMATCH: %08X interpreted, %08X in memory, %08X allocated\n",
           interpreted.checksum, memory_registers.checksum, allocated.checksum);
  } else {
    printf("  all three agree (register checksum %08X, %llu guest instructions)\n",
           interpreted.checksum,
           static_cast<unsigned long long>(interpreted.instructions));
  }
}

// Where allocation starts paying, measured rather than guessed.
//
// Its cost is per block entry and its saving is per instruction, so there is a
// block length below which it is a loss. Finding that length is what sets the
// threshold in BlockCompiler, and it is the only defensible way to set it.
void SweepBlockLength(uint32_t iterations, int repeats) {
  printf("\nallocation against block length\n");
  printf("  %-12s %12s %12s %8s\n", "instructions", "in memory", "allocated",
         "ratio");

  static const int kQuads[] = { 1, 2, 3, 4, 6, 8, 12, 15 };
  for (int quads : kQuads) {
    const Program program = BlockOfSize(iterations, quads);
    // Keep the work per measurement roughly constant as the body grows.
    const int scaled = (repeats * 12) / quads;
    const Result off = RunCompiled(program, false, scaled);
    const Result on = RunCompiled(program, true, scaled);
    printf("  %-12d %9.1f M/s %9.1f M/s %7.2fx  %s\n",
           quads * 4 + 3,   // the body, the counter decrement and the branch
           Rate(off) / 1e6, Rate(on) / 1e6, Rate(on) / Rate(off),
           on.registers_allocated > 0 ? "allocated" : "(too short to allocate)");
    if (off.checksum != on.checksum)
      printf("  *** MISMATCH at %d instructions\n", quads * 4 + 3);
  }
}

}  // namespace

int main(int argc, char** argv) {
  uint32_t iterations = 20000;
  if (argc > 1)
    iterations = static_cast<uint32_t>(atoi(argv[1]));
  // The counter is a sign-extended 16-bit immediate, so anything above 32767
  // starts negative and counts away from zero forever.
  if (iterations == 0 || iterations > 30000)
    iterations = 20000;

  int repeats = 3000;
  if (argc > 2)
    repeats = atoi(argv[2]);
  if (repeats < 1)
    repeats = 3000;

  printf("rec_bench - recompiled against interpreted, and step 6's allocator\n");
  printf("            %u iterations per loop, %d repeats, best of %d rounds\n",
         iterations, repeats, kRounds);

  Measure(ArithmeticLoop(iterations), repeats);
  Measure(MemoryLoop(iterations), repeats);
  Measure(LongBlock(iterations), repeats);
  SweepBlockLength(iterations, repeats);

  printf("\nThe interpreter here is a small reference one, not this project's\n");
  printf("Cpu, so the compiled/interpreted ratio is not a prediction for the\n");
  printf("emulator. The allocator comparison is exact: same compiler, same\n");
  printf("blocks, registers in host registers or in memory.\n");
  return 0;
}
