// hle_test - the HLE BIOS (psx/hle_bios.h), driven by a program of its own.
//
// Each group assembles a small PS-EXE with psx/mips_assembler.h that calls kernel functions the
// way a game does - through A0h, B0h and C0h with the number in t1 - and stores what they return
// in RAM. The machine boots it on the HLE BIOS, runs until the program writes its done marker,
// and the checks read the results back. Expected values come from psx-spx's "Kernel (BIOS)"
// chapters, quirks and all, and from the SCPH-1001 kernel's RAM as boot_runner shows it.
//
//   hle_test

#include "psx/psx.h"
#include "psx/mips_assembler.h"
#include "psx/mc_directory.h"

#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

using emulation::psx::MipsAssembler;
using emulation::psx::System;
using namespace emulation::psx;   // the register names

namespace {

int g_checks = 0;
int g_failures = 0;

void Group(const char* name) { printf("%s\n", name); }

void Check(bool condition, const char* what) {
  ++g_checks;
  if (!condition) {
    ++g_failures;
    printf("  FAIL  %s\n", what);
  }
}

void CheckEqual(uint32_t got, uint32_t want, const char* what) {
  ++g_checks;
  if (got != want) {
    ++g_failures;
    printf("  FAIL  %s: got %u (0x%X) want %u (0x%X)\n", what, got, got, want, want);
  }
}

const uint32_t kText = 0x80010000;       // the program
const uint32_t kCallbacks = 0x80020000;  // functions the kernel calls back
const uint32_t kData = 0x80030000;       // strings and arrays
const uint32_t kResults = 0x80040000;    // what the program stores
const uint32_t kDone = 0x80041000;       // the done marker
const uint32_t kImageEnd = 0x80042000;

std::string TempPath(const char* name) {
  char directory[MAX_PATH];
  GetTempPathA(sizeof(directory), directory);
  return std::string(directory) + name;
}

// A program under construction: its code, its callbacks and its data, all in one PS-EXE.
struct Program {
  MipsAssembler main{kText};
  MipsAssembler callbacks{kCallbacks};
  std::vector<uint8_t> data;
  uint32_t next_result = kResults;

  Program() {
    main.Addiu(kSp, kSp, -0x40);   // room for a fifth and sixth argument
  }

  uint32_t String(const std::string& text) {
    const uint32_t at = kData + static_cast<uint32_t>(data.size());
    data.insert(data.end(), text.begin(), text.end());
    data.push_back(0);
    while (data.size() % 4) data.push_back(0);
    return at;
  }
  uint32_t Words(const std::vector<uint32_t>& words) {
    const uint32_t at = kData + static_cast<uint32_t>(data.size());
    for (uint32_t w : words)
      for (int i = 0; i < 4; ++i) data.push_back(static_cast<uint8_t>(w >> (i * 8)));
    return at;
  }
  uint32_t Space(uint32_t bytes) {
    const uint32_t at = kData + static_cast<uint32_t>(data.size());
    data.resize(data.size() + ((bytes + 3) & ~3u), 0);
    return at;
  }

  // vector(number)(args...), v0 stored to a fresh result slot whose address is returned.
  uint32_t Call(uint32_t vector, uint32_t number, std::vector<uint32_t> args) {
    for (size_t i = 4; i < args.size(); ++i) {
      main.Li32(kT0, args[i]);
      main.Sw(kT0, static_cast<int16_t>(0x10 + (i - 4) * 4), kSp);
    }
    for (size_t i = 0; i < args.size() && i < 4; ++i)
      main.Li32(kA0 + static_cast<uint32_t>(i), args[i]);
    main.Li32(kT2, vector);
    main.Li32(kT1, number);
    main.Jalr(kT2);
    main.Nop();
    const uint32_t slot = next_result;
    next_result += 4;
    main.Li32(kT0, slot);
    main.Sw(kV0, 0, kT0);
    return slot;
  }

  // The bytes of a PS-X EXE with all three pieces in it.
  std::vector<uint8_t> Build() {
    main.Li32(kT0, kDone);
    main.Li32(kT1, 0xD0E);
    main.Sw(kT1, 0, kT0);
    const int loop = main.NewLabel();
    main.Bind(loop);
    main.Beq(kR0, kR0, loop);
    main.Nop();
    main.Finish();
    callbacks.Finish();

    const uint32_t size = kImageEnd - kText;
    std::vector<uint8_t> image(0x800 + size, 0);
    memcpy(&image[0], "PS-X EXE", 8);
    auto put = [&](uint32_t offset, uint32_t value) { memcpy(&image[offset], &value, 4); };
    put(0x10, kText);   // pc0
    put(0x18, kText);   // t_addr
    put(0x1C, size);    // t_size
    memcpy(&image[0x800], main.words().data(), main.words().size() * 4);
    memcpy(&image[0x800 + (kCallbacks - kText)], callbacks.words().data(),
           callbacks.words().size() * 4);
    if (!data.empty())
      memcpy(&image[0x800 + (kData - kText)], data.data(), data.size());
    return image;
  }
};

// Boots `program` on the HLE BIOS and runs it to its done marker. `prepare` gets the machine
// first, for a memory card.
bool Run(System& system, Program& program, const std::function<void(System&)>& prepare = {}) {
  const std::string path = TempPath("hle_test_program.exe");
  const std::vector<uint8_t> image = program.Build();
  FILE* fp = fopen(path.c_str(), "wb");
  if (fp == nullptr)
    return false;
  fwrite(image.data(), 1, image.size(), fp);
  fclose(fp);

  system.config().hle_bios = true;
  if (system.Initialize("") != 0)
    return false;
  if (prepare)
    prepare(system);
  system.set_auto_boot_exe(true, path);
  for (int i = 0; i < 20000000; ++i) {
    system.StepInstruction();
    uint32_t done;
    memcpy(&done, system.ram() + (kDone & 0x1FFFFF), 4);
    if (done == 0xD0E)
      return true;
  }
  return false;
}

uint32_t Ram32(System& system, uint32_t address) {
  uint32_t value;
  memcpy(&value, system.ram() + (address & 0x1FFFFF), 4);
  return value;
}

std::string RamString(System& system, uint32_t address) {
  return std::string(reinterpret_cast<const char*>(system.ram() + (address & 0x1FFFFF)));
}

// ---- The groups ----------------------------------------------------------------------------------

void TestKernelLayout() {
  Group("the kernel's RAM, laid out as SCPH-1001's");
  System* system = new System();
  Program program;
  const uint32_t date = program.Call(0xA0, 0xB4, { 0 });       // GetSystemInfo(0)
  const uint32_t ctable = program.Call(0xB0, 0x56, {});        // GetC0Table
  const uint32_t btable = program.Call(0xB0, 0x57, {});        // GetB0Table
  Check(Run(*system, program), "the program ran to its end");
  CheckEqual(Ram32(*system, date), 0x19951204, "GetSystemInfo(0): the kernel's date");
  CheckEqual(Ram32(*system, ctable), 0x674, "GetC0Table");
  CheckEqual(Ram32(*system, btable), 0x874, "GetB0Table");
  CheckEqual(Ram32(*system, 0x80), 0x3C1A0000, "exception vector: lui k0, 0");
  CheckEqual(Ram32(*system, 0x84), 0x275A0C80, "exception vector: addiu k0, k0, 0C80h");
  CheckEqual(Ram32(*system, 0x00), 0x3C1A0000, "the copy at address 0");
  CheckEqual(Ram32(*system, 0xA4), 0x250805C4, "A0h vector to 5C4h");
  CheckEqual(Ram32(*system, 0x674 + 6 * 4), 0xC80, "C(06h) is the handler itself");
  CheckEqual(Ram32(*system, 0xC80 + 0x10), 0x241A0100, "handler +10h: addiu k0, zero, 100h");
  CheckEqual(Ram32(*system, 0xC80 + 0x28), 0xAF410004, "handler +28h: sw at, 4(k0)");
  CheckEqual(Ram32(*system, 0xC80 + 0x70), 0, "handler +70h: the early card hook, empty");
  CheckEqual(Ram32(*system, 0x100), 0xA000E004, "ExCBs at E004h");
  CheckEqual(Ram32(*system, 0x108), 0xA000E1EC, "the PCB at E1ECh");
  CheckEqual(Ram32(*system, 0x110), 0xA000E1F4, "TCBs at E1F4h");
  CheckEqual(Ram32(*system, 0x120), 0xA000E028, "EvCBs at E028h");
  CheckEqual(Ram32(*system, 0x140), 0x8648, "FCBs at 8648h");
  CheckEqual(Ram32(*system, 0x150), 0x6EE0, "DCBs at 6EE0h");
  CheckEqual(Ram32(*system, 0x60), 2, "RAM size 2 MB");
  delete system;
}

void TestStrings() {
  Group("strings and memory, with psx-spx's bugs");
  System* system = new System();
  Program p;
  const uint32_t hello = p.String("hello");
  const uint32_t aaab = p.String("aaab");
  const uint32_t aab = p.String("aab");
  const uint32_t ab = p.String("ab");
  const uint32_t abc = p.String("abc");
  const uint32_t abd = p.String("abd");
  const uint32_t mem1 = p.String("abcx");
  const uint32_t mem2 = p.String("abdx");
  const uint32_t hex = p.String("0x1F");
  const uint32_t octal = p.String("010");
  const uint32_t negative = p.String("  -42z");
  const uint32_t buffer = p.Space(32);
  const uint32_t strlen_r = p.Call(0xA0, 0x1B, { hello });
  const uint32_t strcmp_r = p.Call(0xA0, 0x17, { abc, abd });
  const uint32_t strstr_found = p.Call(0xA0, 0x24, { aaab, ab });
  const uint32_t strstr_bug = p.Call(0xA0, 0x24, { aaab, aab });
  const uint32_t memcmp_r = p.Call(0xA0, 0x2D, { mem1, mem2, 4 });
  const uint32_t strcpy_r = p.Call(0xA0, 0x19, { buffer, hello });
  const uint32_t strcat_r = p.Call(0xA0, 0x15, { buffer, abc });
  const uint32_t atoi_hex = p.Call(0xA0, 0x10, { hex });
  const uint32_t atoi_oct = p.Call(0xA0, 0x10, { octal });
  const uint32_t strtol_r = p.Call(0xA0, 0x0D, { negative, 0, 10 });
  const uint32_t todigit_r = p.Call(0xA0, 0x0A, { 'z' });
  const uint32_t todigit_bad = p.Call(0xA0, 0x0A, { '!' });
  const uint32_t toupper_r = p.Call(0xA0, 0x25, { 'q' });
  Check(Run(*system, p), "the program ran to its end");
  CheckEqual(Ram32(*system, strlen_r), 5, "strlen(\"hello\")");
  CheckEqual(Ram32(*system, strcmp_r), static_cast<uint32_t>('c' - 'd'), "strcmp(abc, abd)");
  CheckEqual(Ram32(*system, strstr_found), aaab + 2, "strstr(aaab, ab)");
  CheckEqual(Ram32(*system, strstr_bug), 0, "strstr(aaab, aab) misses - no step back");
  CheckEqual(Ram32(*system, memcmp_r), static_cast<uint32_t>('x' - 'x'),
             "memcmp answers with the bytes after the mismatch");
  CheckEqual(Ram32(*system, strcpy_r), buffer, "strcpy returns dst");
  CheckEqual(Ram32(*system, strcat_r), buffer, "strcat returns dst");
  Check(RamString(*system, buffer) == "helloabc", "strcpy then strcat");
  CheckEqual(Ram32(*system, atoi_hex), 31, "atoi(\"0x1F\")");
  CheckEqual(Ram32(*system, atoi_oct), 8, "atoi(\"010\") is octal");
  CheckEqual(Ram32(*system, strtol_r), static_cast<uint32_t>(-42), "strtol(\"  -42z\")");
  CheckEqual(Ram32(*system, todigit_r), 35, "todigit('z')");
  CheckEqual(Ram32(*system, todigit_bad), 9999999, "todigit('!')");
  CheckEqual(Ram32(*system, toupper_r), 'Q', "toupper('q')");
  delete system;
}

void TestHeapAndRand() {
  Group("the heap and rand");
  System* system = new System();
  Program p;
  p.Call(0xA0, 0x39, { 0x80100000, 0x1000 });               // InitHeap
  const uint32_t a = p.Call(0xA0, 0x33, { 0x10 });          // malloc
  const uint32_t b = p.Call(0xA0, 0x33, { 0x20 });
  p.Call(0xA0, 0x30, { 1 });                                // srand(1)
  const uint32_t r = p.Call(0xA0, 0x2F, {});                // rand
  const uint32_t too_big = p.Call(0xA0, 0x33, { 0x2000 });
  Check(Run(*system, p), "the program ran to its end");
  CheckEqual(Ram32(*system, a), 0x80100004, "the first block, after its header");
  CheckEqual(Ram32(*system, b), 0x80100018, "the second, after the first and a header");
  CheckEqual(Ram32(*system, too_big), 0, "more than the heap holds");
  const uint32_t x = 1u * 0x41C64E6D + 0x3039;
  CheckEqual(Ram32(*system, r), (x >> 16) & 0x7FFF, "rand after srand(1)");
  delete system;
}

void TestEvents() {
  Group("events, a callback event and a thread");
  System* system = new System();
  Program p;
  const uint32_t counter = p.Space(4);
  const uint32_t marker = p.Space(4);
  // The callback: ++counter.
  const uint32_t callback = p.callbacks.pc();
  p.callbacks.Li32(kT0, counter);
  p.callbacks.Lw(kT1, 0, kT0);
  p.callbacks.Nop();
  p.callbacks.Addiu(kT1, kT1, 1);
  p.callbacks.Sw(kT1, 0, kT0);
  p.callbacks.Jr(kRa);
  p.callbacks.Nop();
  // The thread: marker = 7777h, then back to the first thread.
  const uint32_t thread = p.callbacks.pc();
  p.callbacks.Li32(kT0, marker);
  p.callbacks.Li32(kT1, 0x7777);
  p.callbacks.Sw(kT1, 0, kT0);
  p.callbacks.Li32(kA0, 0xFF000000);
  p.callbacks.Li32(kT2, 0xB0);
  p.callbacks.Li32(kT1, 0x10);
  p.callbacks.Jalr(kT2);
  p.callbacks.Nop();
  const int hang = p.callbacks.NewLabel();
  p.callbacks.Bind(hang);
  p.callbacks.Beq(kR0, kR0, hang);
  p.callbacks.Nop();

  const uint32_t event = p.Call(0xB0, 0x08, { 0xF3000001, 2, 0x2000, 0 });   // OpenEvent
  // The handle is in its slot: use it from there.
  auto with_event = [&](uint32_t number) {
    p.main.Li32(kT0, event);
    p.main.Lw(kA0, 0, kT0);
    p.main.Li32(kT2, 0xB0);
    p.main.Li32(kT1, number);
    p.main.Jalr(kT2);
    p.main.Nop();
    const uint32_t slot = p.next_result;
    p.next_result += 4;
    p.main.Li32(kT0, slot);
    p.main.Sw(kV0, 0, kT0);
    return slot;
  };
  with_event(0x0C);                                         // EnableEvent
  const uint32_t before = with_event(0x0B);                 // TestEvent: busy
  p.Call(0xB0, 0x07, { 0xF3000001, 2 });                    // DeliverEvent
  const uint32_t ready = with_event(0x0B);                  // TestEvent: ready
  const uint32_t after = with_event(0x0B);                  // and busy again
  const uint32_t cb_event = p.Call(0xB0, 0x08, { 0xF3000002, 4, 0x1000, callback });
  p.main.Li32(kT0, cb_event);
  p.main.Lw(kA0, 0, kT0);
  p.main.Li32(kT2, 0xB0);
  p.main.Li32(kT1, 0x0C);
  p.main.Jalr(kT2);
  p.main.Nop();
  p.Call(0xB0, 0x07, { 0xF3000002, 4 });
  p.Call(0xB0, 0x07, { 0xF3000002, 4 });
  const uint32_t handle = p.Call(0xB0, 0x0E, { thread, 0x801F0000, 0 });    // OpenThread
  p.main.Li32(kT0, handle);
  p.main.Lw(kA0, 0, kT0);
  p.main.Li32(kT2, 0xB0);
  p.main.Li32(kT1, 0x10);                                   // ChangeThread
  p.main.Jalr(kT2);
  p.main.Nop();
  const uint32_t back = p.next_result;
  p.next_result += 4;
  p.main.Li32(kT0, back);
  p.main.Sw(kV0, 0, kT0);
  Check(Run(*system, p), "the program ran to its end");
  CheckEqual(Ram32(*system, event), 0xF1000005,
             "the first free event: five are the kernel's CD-ROM ones");
  CheckEqual(Ram32(*system, before), 0, "TestEvent before delivery");
  CheckEqual(Ram32(*system, ready), 1, "TestEvent after delivery");
  CheckEqual(Ram32(*system, after), 0, "TestEvent takes the delivery");
  CheckEqual(Ram32(*system, counter), 2, "a callback event's function, once per delivery");
  CheckEqual(Ram32(*system, handle), 0xFF000001, "OpenThread: the first free TCB");
  CheckEqual(Ram32(*system, marker), 0x7777, "the thread ran");
  CheckEqual(Ram32(*system, back), 1, "ChangeThread returns 1 when the first thread resumes");
  delete system;
}

void TestSearch() {
  Group("qsort, lsearch and bsearch, through a callback");
  System* system = new System();
  Program p;
  // The compare: *a - *b.
  const uint32_t compare = p.callbacks.pc();
  p.callbacks.Lw(kT0, 0, kA0);
  p.callbacks.Lw(kT1, 0, kA1);
  p.callbacks.Nop();
  p.callbacks.Subu(kV0, kT0, kT1);
  p.callbacks.Jr(kRa);
  p.callbacks.Nop();
  const uint32_t array = p.Words({ 50, 3, 99, 7, 3, 42, 1, 20 });
  const uint32_t key = p.Words({ 42 });
  const uint32_t missing = p.Words({ 43 });
  const uint32_t unsorted_key = p.Words({ 7 });
  const uint32_t lsearch_r = p.Call(0xA0, 0x35, { unsorted_key, array, 8, 4, compare });
  p.Call(0xA0, 0x31, { array, 8, 4, compare });
  const uint32_t bsearch_r = p.Call(0xA0, 0x36, { key, array, 8, 4, compare });
  const uint32_t bsearch_none = p.Call(0xA0, 0x36, { missing, array, 8, 4, compare });
  Check(Run(*system, p), "the program ran to its end");
  CheckEqual(Ram32(*system, lsearch_r), array + 3 * 4, "lsearch finds 7 where it was");
  const uint32_t sorted[8] = { 1, 3, 3, 7, 20, 42, 50, 99 };
  bool in_order = true;
  for (int i = 0; i < 8; ++i)
    in_order &= Ram32(*system, array + i * 4) == sorted[i];
  Check(in_order, "qsort sorts");
  CheckEqual(Ram32(*system, bsearch_r), array + 5 * 4, "bsearch finds 42");
  CheckEqual(Ram32(*system, bsearch_none), 0, "bsearch does not find 43");
  delete system;
}

void TestCardFiles() {
  Group("memory card files: create, write, read back, list, delete");
  System* system = new System();
  Program p;
  const std::string card_path = TempPath("hle_test_card.mcd");
  const uint32_t name = p.String("bu00:BASLUS-00000TEST");
  const uint32_t pattern = p.String("bu00:BAS*");
  const uint32_t data = p.Space(0x80);
  const uint32_t back = p.Space(0x80);
  const uint32_t dirent = p.Space(0x28);
  for (uint32_t i = 0; i < 0x80; i += 4)
    memcpy(&p.data[data - kData + i], &i, 4);
  auto fd_call = [&](uint32_t fd_slot, uint32_t number, uint32_t a1, uint32_t a2) {
    p.main.Li32(kT0, fd_slot);
    p.main.Lw(kA0, 0, kT0);
    p.main.Li32(kA1, a1);
    p.main.Li32(kA2, a2);
    p.main.Li32(kT2, 0xB0);
    p.main.Li32(kT1, number);
    p.main.Jalr(kT2);
    p.main.Nop();
    const uint32_t slot = p.next_result;
    p.next_result += 4;
    p.main.Li32(kT0, slot);
    p.main.Sw(kV0, 0, kT0);
    return slot;
  };
  const uint32_t fd = p.Call(0xB0, 0x32, { name, 0x00010203 });   // create, one block
  const uint32_t wrote = fd_call(fd, 0x35, data, 0x80);
  fd_call(fd, 0x36, 0, 0);                                           // close
  const uint32_t fd2 = p.Call(0xB0, 0x32, { name, 1 });
  const uint32_t read = fd_call(fd2, 0x34, back, 0x80);
  fd_call(fd2, 0x36, 0, 0);
  const uint32_t again = p.Call(0xB0, 0x32, { name, 0x00010203 });  // already there
  const uint32_t error = p.Call(0xB0, 0x54, {});                    // GetLastError
  const uint32_t found = p.Call(0xB0, 0x42, { pattern, dirent });   // firstfile
  const uint32_t deleted = p.Call(0xB0, 0x45, { name });
  const uint32_t gone = p.Call(0xB0, 0x42, { pattern, dirent + 0 });
  Check(Run(*system, p, [&](System& s) { s.mc(0).CreateFile(card_path.c_str()); }),
        "the program ran to its end");
  CheckEqual(Ram32(*system, fd), 2, "the first file is handle 2: 0 and 1 are the console");
  CheckEqual(Ram32(*system, wrote), 0x80, "a sector written");
  CheckEqual(Ram32(*system, read), 0x80, "and read");
  Check(memcmp(system->ram() + (back & 0x1FFFFF), system->ram() + (data & 0x1FFFFF), 0x80) == 0,
        "what came back is what went");
  CheckEqual(Ram32(*system, again), 0xFFFFFFFF, "creating it twice fails");
  CheckEqual(Ram32(*system, error), 0x11, "with 'file already exists'");
  CheckEqual(Ram32(*system, found), dirent, "firstfile finds it");
  CheckEqual(Ram32(*system, deleted), 1, "and it deletes");
  CheckEqual(Ram32(*system, gone), 0, "after which firstfile finds nothing");
  const std::vector<mcdir::Save> saves = mcdir::List(system->mc(0).data(), true);
  Check(saves.size() == 1 && saves[0].deleted && saves[0].filename == "BASLUS-00000TEST",
        "the card's own directory agrees: one save, deleted");
  system->mc(0).Eject();
  remove(card_path.c_str());
  delete system;
}

void TestConsole() {
  Group("printf and the console");
  System* system = new System();
  Program p;
  const uint32_t format = p.String("x=%d %s %04X\n");
  const uint32_t word = p.String("ok");
  p.Call(0xA0, 0x3F, { format, 42, word, 0xBEEF });
  Check(Run(*system, p), "the program ran to its end");
  const auto& stats = system->kernel().stats();
  const std::string tty(stats.tty, stats.tty_length);
  Check(tty.find("x=42 ok BEEF\n") != std::string::npos, "printf's text on the console");
  delete system;
}

void TestPatches() {
  Group("a game's patch to the pad driver, as Resident Evil 2 writes it");
  System* system = new System();
  Program p;
  const uint32_t table = p.Call(0xB0, 0x57, {});   // GetB0Table
  // psx-spx "patch_no_pad_card_auto_ack": r2 = B(5Bh), then nine zeroes from B(5Bh)+62Ch.
  p.main.Lw(kV0, 0x5B * 4, kV0);
  p.main.Nop();
  p.main.Addiu(kT2, kR0, 9);
  p.main.Addi(kV1, kV0, 0x62C);
  const int loop = p.main.NewLabel();
  p.main.Bind(loop);
  p.main.Addiu(kT2, kT2, -1);
  p.main.Sw(kR0, 0, kV1);
  p.main.Bne(kT2, kR0, loop);
  p.main.Addiu(kV1, kV1, 4);
  Check(Run(*system, p), "the program ran to its end");
  CheckEqual(Ram32(*system, table), 0x874, "GetB0Table");
  Check(system->hle().pad_ack_removed(), "the patch is understood: vblank left to the game");
  Check(system->hle().messages().empty(), "and nothing reported as unknown");
  delete system;
}

void TestFont() {
  Group("the character sets: Krom2RawAdd as the SCPH-1001 answers, and a glyph there");
  System* system = new System();
  Program p;
  const uint32_t a = p.Call(0xB0, 0x51, { 0x8260 });       // full-width A
  const uint32_t kanji = p.Call(0xB0, 0x51, { 0x889F });   // the first level-1 kanji
  const uint32_t last = p.Call(0xB0, 0x51, { 0x9872 });    // the last
  const uint32_t gap = p.Call(0xB0, 0x51, { 0x81AD });     // an unassigned cell
  const uint32_t offset = p.Call(0xB0, 0x53, { 0x8280 });  // past a gap: 8281h is glyph 183
  Check(Run(*system, p), "the program ran to its end");
  CheckEqual(Ram32(*system, a), 0xBFC66000 + 157 * 30, "8260h, where the real BIOS has it");
  CheckEqual(Ram32(*system, kanji), 0xBFC69D68, "889Fh, charset 3's first");
  CheckEqual(Ram32(*system, last), 0xBFC69D68 + 2964 * 30, "9872h, its last");
  CheckEqual(Ram32(*system, gap), 0xFFFFFFFF, "an unassigned code has none");
  CheckEqual(Ram32(*system, offset), 0xFFFFFFFF, "nor does 8280h");
  // The glyph itself: the ROM read back through the bus, not empty.
  int lit = 0;
  for (int i = 0; i < 30; ++i)
    lit += system->bios()[0x66000 + 157 * 30 + i] != 0;
  Check(lit > 10, "and a glyph is drawn there");
  delete system;
}

void TestShell() {
  Group("the shell, with nothing to boot");
  System* system = new System();
  system->config().hle_bios = true;
  Check(system->Initialize("") == 0, "the machine starts with no dump and no disc");
  // Its intro runs three seconds; past it is the menu, drawn every frame.
  for (int i = 0; i < 80000000 && system->gpu().frame_count() < 260; ++i)
    system->StepInstruction();
  CheckEqual(Ram32(*system, emulation::psx::HleBios::kShellVars), 1, "on the menu after the intro");
  int width = 0, height = 0;
  const uint32_t* pixels = system->gpu().framebuffer(width, height);
  int lit = 0;
  for (int i = 0; i < width * height; ++i)
    lit += (pixels[i] & 0xFFFFFF) != 0;
  CheckEqual(static_cast<uint32_t>(width), 320, "a 320-wide display");
  Check(lit > width * height / 2, "and something drawn on it");
  delete system;
}

}  // namespace

int main() {
  printf("hle_test - the HLE BIOS, from a program of its own\n\n");
  TestKernelLayout();
  TestStrings();
  TestHeapAndRand();
  TestEvents();
  TestSearch();
  TestCardFiles();
  TestConsole();
  TestPatches();
  TestFont();
  TestShell();
  printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
