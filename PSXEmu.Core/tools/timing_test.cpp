// timing_test - bus timing against a real console (Docs/CPU-Timing-Plan.md, phase 3).
//
//   timing_test [bios]
//
// Runs JaCzekanski's cpu/access-time (test/test suite/cpu/access-time) - it times 8, 16 and 32-bit
// reads from every region of the memory map with a root counter and prints cycles per access -
// and sets what this emulator measures beside the psx.log the suite ships, which is the same
// table from a real console.
//
// Two different questions, answered separately:
//
//   - How close to the console is it? Every cell within kTolerance of the console's counts as a
//     match, and the count of matches is printed. Not a pass/fail: phase 3 is the work of raising
//     it, and until it is done most cells are expected to differ.
//   - Has it changed? The emulator is deterministic, so every cell is also checked against
//     kBaseline, what this build measured when it was recorded - exactly. Any change fails, which
//     is the point: a change to bus timing should be deliberate, with the baseline updated
//     beside it and the match count read again.
//
// Each cell is the cost of one load beyond a nop, averaged over 100 of them - the test subtracts a
// loop of nops from a loop of nops and loads. kTolerance is a quarter of a cycle because that is
// the console's own scatter: the on-die registers share one decoder and one cost and read 2.92 to
// 3.18, and RAM reads 5.03 to 5.21 with its refresh cycles landing in some loops and not others.

#include "psx/psx.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using emulation::psx::System;

namespace {

int g_checks = 0;
int g_failures = 0;

const char kExe[] = "test/test suite/cpu/access-time/access-time.exe";
const char kLog[] = "test/test suite/cpu/access-time/psx.log";
const double kTolerance = 0.25;
const int kFrameLimit = 3000;

struct Row {
  std::string name;
  uint32_t address = 0;
  double cycles[3] = {};   // 8, 16, 32-bit
};

// What this build measures - update it with any deliberate change to bus timing, in the same
// commit. Recorded 2026-09-19 after phase 3's second step (bug 77): the slow regions' costs come
// from the memory-control registers by psx-spx's formula. Exact for the BIOS ROM and expansions 1
// and 3; above the console for expansion 2 and the SPU and one below it for the CD-ROM, which is
// the formula's own error. The SPU's 32-bit cell is an lwl/lwr pair - 1F801DAA is not
// word-aligned - each charged a whole word, where the console seems to read only the halfword
// each needs.
const Row kBaseline[] = {
  { "RAM",        0x80000000, { 5.01, 5.01, 5.01 } },
  { "BIOS",       0xbfc00000, { 7.01, 13.01, 25.01 } },
  { "SCRATCHPAD", 0x1f800000, { 0.99, 0.99, 0.99 } },
  { "EXPANSION1", 0x1f000000, { 7.01, 13.01, 25.01 } },
  { "EXPANSION2", 0x1f802000, { 15.00, 29.00, 57.00 } },
  { "EXPANSION3", 0x1fa00000, { 6.01, 6.01, 10.01 } },
  { "DMAC_CTRL",  0x1f8010f0, { 3.00, 3.00, 3.00 } },
  { "JOY_STAT",   0x1f801044, { 3.00, 3.00, 3.00 } },
  { "SIO_STAT",   0x1f801054, { 3.00, 3.00, 3.00 } },
  { "RAM_SIZE",   0x1f801060, { 3.00, 3.00, 3.00 } },
  { "I_STAT",     0x1f801070, { 3.00, 3.00, 3.00 } },
  { "TIMER0_VAL", 0x1f801100, { 3.00, 3.00, 3.00 } },
  { "CDROM_STAT", 0x1f801800, { 7.00, 13.00, 25.00 } },
  { "GPUSTAT",    0x1f801814, { 3.00, 3.00, 3.00 } },
  { "MDECSTAT",   0x1f801824, { 3.00, 3.00, 3.00 } },
  { "SPUCNT",     0x1f801daa, { 21.00, 21.00, 82.00 } },
  { "CACHECTRL",  0xfffe0130, { 1.01, 1.01, 1.01 } },
};

// One cell. The test prints `cycles / 100` and `cycles % 100` either side of a dot, with no
// leading zero on the remainder - so "5.3" is 503 cycles over 100 reads, 5.03, not 5.30, and
// "12.94" is 12.94. Read as a decimal, every one-digit remainder comes out ten times too big.
double ParseCell(const char* text) {
  int whole = 0, remainder = 0;
  if (sscanf(text, "%d.%d", &whole, &remainder) != 2)
    return -1.0;
  return whole + remainder / 100.0;
}

// The table's rows, from text in the program's own format:
//   RAM        (0x80000000)    5.21     5.3      5.14
std::vector<Row> ParseTable(const std::string& text) {
  std::vector<Row> rows;
  size_t start = 0;
  while (start < text.size()) {
    size_t end = text.find('\n', start);
    if (end == std::string::npos)
      end = text.size();
    const std::string line = text.substr(start, end - start);
    start = end + 1;
    char name[32] = {}, cells[3][16] = {};
    unsigned address = 0;
    Row row;
    if (sscanf(line.c_str(), "%31s (0x%x) %15s %15s %15s", name, &address, cells[0], cells[1],
               cells[2]) == 5) {
      row.name = name;
      row.address = address;
      for (int w = 0; w < 3; ++w)
        row.cycles[w] = ParseCell(cells[w]);
      rows.push_back(row);
    }
  }
  return rows;
}

bool ReadFile(const char* path, std::string* text) {
  FILE* fp = fopen(path, "rb");
  if (fp == nullptr)
    return false;
  char buffer[4096];
  size_t read;
  while ((read = fread(buffer, 1, sizeof(buffer), fp)) > 0)
    text->append(buffer, read);
  fclose(fp);
  return true;
}

// Boots the BIOS, side-loads the test the way boot_runner --exe --auto-boot does, and runs it
// until it prints "Done." or the frame limit passes. What it printed through the BIOS comes back.
bool RunTest(const std::string& bios, std::string* console, int* frames) {
  // On the heap: a System is far too big for a thread's stack.
  std::unique_ptr<System> system = std::make_unique<System>();
  if (system->Initialize(bios.c_str()) != 0)
    return false;
  system->set_auto_boot_exe(true, kExe);
  const auto& kernel = system->kernel().stats();
  while (static_cast<int>(system->gpu().frame_count()) < kFrameLimit) {
    system->StepInstructionUnarmed();
    if (kernel.tty_length > 5 &&
        std::string(kernel.tty, kernel.tty_length).find("Done.") != std::string::npos)
      break;
  }
  *frames = static_cast<int>(system->gpu().frame_count());
  console->assign(kernel.tty, kernel.tty_length);
  // What the BIOS left in the memory-control registers, which set the slow regions' timing.
  const auto& io = system->io().io;
  printf("memory control as the test found it:\n"
         "  exp1 %08X  exp3 %08X  bios %08X  spu %08X  cdrom %08X  exp2 %08X  com %08X\n\n",
         io.exp1_delay, io.exp3_delay, io.bios_rom, io.spu_delay, io.cdrom_delay,
         io.exp2_delay, io.com_delay);
  system->Deinitialize();
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string bios = (argc > 1) ? argv[1] : "bios/SCPH1001.BIN";
  printf("timing_test - bus timing against a real console (cpu/access-time)\n\n");

  std::string reference_text;
  if (!ReadFile(kLog, &reference_text)) {
    printf("no %s - run from the repository root\n", kLog);
    return 1;
  }
  const std::vector<Row> console = ParseTable(reference_text);

  std::string output;
  int frames = 0;
  if (!RunTest(bios, &output, &frames)) {
    printf("could not boot %s\n", bios.c_str());
    return 1;
  }
  const std::vector<Row> measured = ParseTable(output);
  ++g_checks;
  if (output.find("Done.") == std::string::npos) {
    ++g_failures;
    printf("  FAIL  the test did not finish within %d frames\n", kFrameLimit);
  }
  ++g_checks;
  if (measured.size() != std::size(kBaseline) || console.size() != std::size(kBaseline)) {
    ++g_failures;
    printf("  FAIL  expected %zu rows, got %zu from the emulator and %zu from psx.log\n",
           std::size(kBaseline), measured.size(), console.size());
    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return 1;
  }

  printf("finished at frame %d. Cycles per read, 8/16/32-bit; * is within %.2f of the console\n\n",
         frames, kTolerance);
  printf("  %-11s %-12s  %-22s  %-22s\n", "region", "address", "console", "this emulator");
  int matches = 0, cells = 0;
  for (size_t r = 0; r < measured.size(); ++r) {
    const Row& ours = measured[r];
    const Row& theirs = console[r];
    char mine[64] = {}, real[64] = {};
    int at_mine = 0, at_real = 0;
    for (int w = 0; w < 3; ++w) {
      const bool match = std::fabs(ours.cycles[w] - theirs.cycles[w]) <= kTolerance + 1e-9;
      matches += match ? 1 : 0;
      ++cells;
      at_real += snprintf(real + at_real, sizeof(real) - at_real, "%6.2f ", theirs.cycles[w]);
      at_mine += snprintf(mine + at_mine, sizeof(mine) - at_mine, "%6.2f%s", ours.cycles[w],
                          match ? "*" : " ");
    }
    printf("  %-11s (%08X)  %-22s  %-22s\n", ours.name.c_str(), ours.address, real, mine);

    // Against the recorded baseline: the same region, and the same numbers exactly.
    const Row& base = kBaseline[r];
    ++g_checks;
    bool same = ours.name == base.name && ours.address == base.address;
    for (int w = 0; w < 3; ++w)
      same = same && std::fabs(ours.cycles[w] - base.cycles[w]) < 1e-6;
    if (!same) {
      ++g_failures;
      printf("  FAIL  %s changed from the baseline (%.2f %.2f %.2f)\n", base.name.c_str(),
             base.cycles[0], base.cycles[1], base.cycles[2]);
    }
  }
  printf("\n%d of %d cells within %.2f cycles of the console\n", matches, cells, kTolerance);
  printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
