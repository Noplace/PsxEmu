// cheats_test - GameShark codes (psx/cheats.h): reading them as people write them, running every
// code type the engine takes against a RAM of its own, and the two cheat-file layouts it reads -
// DuckStation's, which is also its own, and RetroArch's. No machine: the engine only ever sees
// memory, and that is all this gives it.

#include "psx/cheats.h"

#include <cstdio>
#include <string>
#include <vector>

using emulation::psx::Cheat;
using emulation::psx::CheatEngine;
using emulation::psx::CheatLine;
using emulation::psx::CheatMemory;
using emulation::psx::ParseCheatCode;
using emulation::psx::ParseCheatFile;
using emulation::psx::SerialiseCheatFile;

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool condition, const char* what) {
  ++g_checks;
  if (!condition) {
    ++g_failures;
    printf("  FAIL  %s\n", what);
  }
}

void CheckEqual(long long got, long long want, const char* what) {
  ++g_checks;
  if (got != want) {
    ++g_failures;
    printf("  FAIL  %s: got %llx want %llx\n", what, got, want);
  }
}

void Group(const char* name) { printf("%s\n", name); }

// A machine's worth of memory, and a count of what the engine said it changed.
struct Machine {
  std::vector<uint8_t> ram = std::vector<uint8_t>(0x200000, 0);
  std::vector<uint8_t> scratchpad = std::vector<uint8_t>(0x400, 0);
  int writes = 0;
  uint32_t last_offset = 0;
  CheatEngine engine;

  static void Written(void* context, uint32_t offset, uint32_t bytes) {
    Machine* self = static_cast<Machine*>(context);
    self->writes += static_cast<int>(bytes);
    self->last_offset = offset;
  }
  CheatMemory Memory() {
    CheatMemory m;
    m.ram = ram.data();
    m.scratchpad = scratchpad.data();
    m.written = &Written;
    m.context = this;
    return m;
  }
  // One enabled cheat with `code`.
  void Use(const std::string& code) {
    Cheat cheat;
    cheat.enabled = true;
    cheat.code = code;
    std::string error;
    if (!ParseCheatCode(code, &cheat.lines, &error))
      printf("  (could not parse: %s)\n", error.c_str());
    engine.Set({ cheat });
  }
  void Frame(uint16_t buttons = 0) { engine.Apply(Memory(), buttons); }
  uint16_t Read16(uint32_t offset) const {
    return static_cast<uint16_t>(ram[offset] | (ram[offset + 1] << 8));
  }
};

void TestParsing() {
  Group("reading a code");
  std::vector<CheatLine> lines;
  std::string error;
  Check(ParseCheatCode("80012345 0063", &lines, &error) && lines.size() == 1, "one line");
  CheckEqual(lines[0].type(), 0x80, "its type");
  CheckEqual(lines[0].address(), 0x012345, "its address, less the 80");
  CheckEqual(lines[0].value, 0x0063, "its value");

  Check(ParseCheatCode("d0012345 1234\n80012346 00ff\r\n", &lines, &error) && lines.size() == 2,
        "lower case, two lines, a Windows line end");
  Check(ParseCheatCode("80012345+0063+30012347+0001", &lines, &error) && lines.size() == 2,
        "RetroArch's '+' between the halves and the lines");
  CheckEqual(lines[1].first, 0x30012347, "and the second line is whole");
  Check(ParseCheatCode("800123450063 300123470001", &lines, &error) && lines.size() == 2,
        "two lines run together on one row");
  Check(ParseCheatCode("80012345-0063 ; infinite money", &lines, &error) && lines.size() == 1,
        "a dash between the halves, and a comment");

  Check(!ParseCheatCode("80012345 063", &lines, &error), "eleven digits is refused");
  Check(error.find("11 hex digits") != std::string::npos, "saying how many there were");
  Check(!ParseCheatCode("77012345 0063", &lines, &error), "an unknown type is refused");
  Check(error.find("77") != std::string::npos, "naming it");
  Check(!ParseCheatCode("50000302 0001", &lines, &error), "a 50 with nothing after it");
  Check(!ParseCheatCode("8001234G 0063", &lines, &error), "a letter past F");
  Check(!ParseCheatCode("  \n ", &lines, &error), "nothing at all");
}

void TestWrites() {
  Group("writes");
  Machine m;
  m.Use("80012344 BEEF\n30012350 0042");
  m.Frame();
  CheckEqual(m.Read16(0x012344), 0xBEEF, "80 writes 16 bits, low byte first");
  CheckEqual(m.ram[0x012350], 0x42, "30 writes 8 bits");
  CheckEqual(m.writes, 3, "and says so, a byte at a time");
  m.Frame();
  CheckEqual(m.writes, 3, "the same values the next frame are not written again");
  m.ram[0x012350] = 0x07;   // the game changes it
  m.Frame();
  CheckEqual(m.ram[0x012350], 0x42, "put back the frame after the game changes it");
  CheckEqual(m.writes, 4, "with one byte reported");

  Machine mirror;
  mirror.Use("30212345 0011");
  mirror.Frame();
  CheckEqual(mirror.ram[0x012345], 0x11, "an address past 2 MB lands in RAM's mirror");

  Machine pad;
  pad.Use("1F000102 1234");
  pad.Frame();
  CheckEqual(pad.scratchpad[0x102] | (pad.scratchpad[0x103] << 8), 0x1234, "1F writes the scratchpad");
  CheckEqual(pad.writes, 0, "which holds no code, so nothing is reported");

  Machine inc;
  inc.Use("10000100 0002\n21000200 0001");
  inc.ram[0x100] = 0xFF;
  inc.ram[0x101] = 0xFF;
  inc.Frame();
  CheckEqual(inc.Read16(0x100), 0x0001, "10 adds, wrapping at 16 bits");
  CheckEqual(inc.ram[0x200], 0xFF, "21 subtracts, wrapping at 8");
  inc.Frame();
  CheckEqual(inc.Read16(0x100), 0x0003, "every frame, as the cartridge did");
}

void TestConditions() {
  Group("conditions");
  Machine m;
  m.Use("D0000100 0005\n80000200 AAAA\n80000210 BBBB");
  m.Frame();
  CheckEqual(m.Read16(0x200), 0, "D0 false skips the line it guards");
  CheckEqual(m.Read16(0x210), 0xBBBB, "and only that line");
  m.ram[0x100] = 5;
  m.Frame();
  CheckEqual(m.Read16(0x200), 0xAAAA, "D0 true lets it run");

  Machine chain;
  chain.Use("D0000100 0001\nE0000102 0002\n80000200 1111");
  chain.ram[0x100] = 1;
  chain.Frame();
  CheckEqual(chain.Read16(0x200), 0, "two conditions guard one line, and both must hold");
  chain.ram[0x102] = 2;
  chain.Frame();
  CheckEqual(chain.Read16(0x200), 0x1111, "E0 compares 8 bits");

  struct { const char* code; uint16_t have; bool runs; const char* what; } compare[] = {
      { "D1000100 0005", 5, false, "D1: not equal" },   { "D1000100 0005", 4, true, "D1 true" },
      { "D2000100 0005", 4, true, "D2: less" },         { "D2000100 0005", 5, false, "D2 false" },
      { "D3000100 0005", 6, true, "D3: greater" },      { "D3000100 0005", 5, false, "D3 false" },
      { "E1000100 0005", 5, false, "E1: not equal" },   { "E2000100 0005", 4, true, "E2: less" },
      { "E3000100 0005", 4, false, "E3: greater" },
  };
  for (const auto& c : compare) {
    Machine t;
    t.Use(std::string(c.code) + "\n30000300 0001");
    t.ram[0x100] = static_cast<uint8_t>(c.have);
    t.Frame();
    CheckEqual(t.ram[0x300], c.runs ? 1 : 0, c.what);
  }

  // Buttons: the pad's bits (Cross is bit 14) as the cartridge's word (Cross is 0040).
  Machine buttons;
  buttons.Use("D4000000 0040\n30000300 0001");
  buttons.Frame(1 << 14);
  CheckEqual(buttons.ram[0x300], 1, "D4: Cross held is the cartridge's 0040");
  buttons.ram[0x300] = 0;
  buttons.Frame((1 << 14) | (1 << 0));
  CheckEqual(buttons.ram[0x300], 0, "D4 wants exactly those buttons - Select as well is not it");
  Machine select;
  select.Use("D4000000 0100\n30000300 0001");
  select.Frame(1 << 0);
  CheckEqual(select.ram[0x300], 1, "Select is 0100");

  // C0: false skips everything up to the separator, and the code carries on after it.
  Machine c0;
  c0.Use("C0000100 0007\n30000300 0001\n30000301 0001\n00000000 FFFF\n30000302 0001");
  c0.Frame();
  Check(c0.ram[0x300] == 0 && c0.ram[0x301] == 0, "C0 false skips every line to the separator");
  CheckEqual(c0.ram[0x302], 1, "and the lines after it still run");
  c0.ram[0x100] = 7;
  c0.Frame();
  Check(c0.ram[0x300] == 1 && c0.ram[0x301] == 1, "C0 true runs them");

  Machine d5;
  d5.Use("D5000000 0800\n30000300 0001");
  d5.Frame(0);
  CheckEqual(d5.ram[0x300], 0, "D5: not Start, nothing");
  d5.Frame(1 << 3);
  CheckEqual(d5.ram[0x300], 1, "D5: Start (bit 3, the cartridge's 0800) runs the rest");
  Machine d6;
  d6.Use("D6000000 0800\n30000300 0001");
  d6.Frame(1 << 3);
  CheckEqual(d6.ram[0x300], 0, "D6: Start held stops the rest");
  d6.Frame(0);
  CheckEqual(d6.ram[0x300], 1, "D6: otherwise it runs");

  Machine delay;
  delay.Use("C1000000 000A\n30000300 0001");
  delay.Frame();
  delay.Frame();
  CheckEqual(delay.ram[0x300], 0, "C1 000A holds back for the first two frames (x 0.3 is 6 and 3)");
  delay.Frame();
  CheckEqual(delay.ram[0x300], 1, "and lets go on the third (10)");
}

void TestSlideAndCopy() {
  Group("slides and copies");
  Machine slide;
  slide.Use("50000402 0001\n80000100 0010");
  slide.Frame();
  CheckEqual(slide.Read16(0x100), 0x10, "50 then 80: the first write");
  CheckEqual(slide.Read16(0x102), 0x11, "the address up by 2 and the value by 1");
  CheckEqual(slide.Read16(0x106), 0x13, "four writes in all");
  CheckEqual(slide.Read16(0x108), 0, "and no fifth");
  Machine slide8;
  slide8.Use("50000301 0000\n30000200 00AB");
  slide8.Frame();
  Check(slide8.ram[0x200] == 0xAB && slide8.ram[0x202] == 0xAB && slide8.ram[0x203] == 0,
        "50 then 30: three bytes of the same value");

  Machine copy;
  for (int i = 0; i < 4; ++i)
    copy.ram[0x400 + i] = static_cast<uint8_t>(0xA0 + i);
  copy.Use("C2000400 0004\n80000500 0000");
  copy.Frame();
  Check(copy.ram[0x500] == 0xA0 && copy.ram[0x503] == 0xA3 && copy.ram[0x504] == 0,
        "C2 copies that many bytes to the next line's address");
}

void TestEnabled() {
  Group("which cheats run");
  Machine m;
  Cheat on, off;
  on.enabled = true;
  off.enabled = false;
  std::string error;
  ParseCheatCode("30000100 0001", &on.lines, &error);
  ParseCheatCode("30000101 0001", &off.lines, &error);
  m.engine.Set({ on, off });
  m.Frame();
  Check(m.ram[0x100] == 1 && m.ram[0x101] == 0, "only the enabled one");
  m.engine.Set({});
  Check(m.engine.empty(), "none set is empty");
}

void TestFiles() {
  Group("cheat files");
  const std::string duckstation =
      "; a comment\n"
      "[Infinite HP]\n"
      "Type = Gameshark\n"
      "Activation = EndFrame\n"
      "80012345 0063\n"
      "80012347 0063\n"
      "\n"
      "[Max Gold]\n"
      "Enabled = 1\n"
      "80023456 FFFF\n"
      "[Warp Once]\n"
      "Type = Gameshark\n"
      "Activation = Manual\n"
      "80012345 0001\n"
      "[Asm Thing]\n"
      "Type = Assembly\n"
      "80012345 0001\n"
      "[Broken]\n"
      "8001234 0001\n";
  int skipped = -1;
  std::vector<Cheat> cheats = ParseCheatFile(duckstation, &skipped);
  CheckEqual(static_cast<long long>(cheats.size()), 2, "DuckStation's layout: two cheats taken");
  CheckEqual(skipped, 3, "and three left out: a Manual one, an Assembly one, a broken one");
  Check(cheats.size() == 2 && cheats[0].name == "Infinite HP" && cheats[0].lines.size() == 2,
        "the first, by name, with both lines");
  Check(cheats.size() == 2 && !cheats[0].enabled && cheats[1].enabled,
        "off unless it says Enabled = 1");

  const std::string text = SerialiseCheatFile(cheats, "Wild Arms (SCUS-94608)");
  std::vector<Cheat> back = ParseCheatFile(text, &skipped);
  Check(back.size() == 2 && back[0].name == cheats[0].name && back[1].enabled &&
            back[0].lines.size() == 2 && back[1].lines[0].value == 0xFFFF && skipped == 0,
        "written and read back unchanged");
  Check(text.find("Wild Arms (SCUS-94608)") != std::string::npos, "the heading says whose");

  const std::string retroarch =
      "cheats = 2\n\n"
      "cheat0_desc = \"Infinite Lives\"\n"
      "cheat0_code = \"80012345+0009+30012347+0001\"\n"
      "cheat0_enable = true\n\n"
      "cheat1_desc = \"Bad\"\n"
      "cheat1_code = \"ZZZ\"\n"
      "cheat1_enable = false\n";
  cheats = ParseCheatFile(retroarch, &skipped);
  CheckEqual(static_cast<long long>(cheats.size()), 1, "RetroArch's layout: the good one taken");
  CheckEqual(skipped, 1, "and the bad one left out");
  Check(cheats.size() == 1 && cheats[0].name == "Infinite Lives" && cheats[0].enabled &&
            cheats[0].lines.size() == 2,
        "with its name, its switch and both lines");
  Check(cheats.size() == 1 && cheats[0].code == "80012345 0009\n30012347 0001",
        "its '+'s become one line per row");
}

}  // namespace

int main() {
  printf("cheats_test - GameShark codes read, run and filed\n\n");
  TestParsing();
  TestWrites();
  TestConditions();
  TestSlideAndCopy();
  TestEnabled();
  TestFiles();
  printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
