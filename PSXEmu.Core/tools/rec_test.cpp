// rec_test - the recompiler's foundations, before there is a recompiler.
//
// Two things are checked here and nothing else: that the vendored RecCore
// emitter produces x86-64 this process can actually call, and that the block
// cache hands back what was put in it and throws it away when it should.
//
// This is step 1 of Docs/Recompiler-Plan.md. Getting the calling convention
// wrong produces corruption that looks like a CPU bug three layers away, so it
// is worth pinning down while the only code being emitted is four
// instructions long.
//
// Nothing here touches PSXEmu.Core/psx. The recompiler is being built beside
// the interpreter, not into it.

#include "rec/block_cache.h"
#include "rec/block_decoder.h"
#include "rec/block_compiler.h"
#include "rec/recompiler.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <intrin.h>
#include <windows.h>
#include <psapi.h>
#include <map>
#include <memory>

using emulation::rec::Block;
using emulation::rec::BlockCache;
using emulation::rec::BlockDecoder;
using emulation::rec::DecodedBlock;
using emulation::rec::EndReason;
using emulation::rec::Kind;
using emulation::rec::Emitter;
using emulation::rec::CodeBlock;
namespace x86 = emulation::rec::x86;

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

void CheckEqual(int64_t got, int64_t want, const char* what) {
  ++g_checks;
  if (got != want) {
    ++g_failures;
    printf("  FAIL  %s: got %lld want %lld\n", what, static_cast<long long>(got),
           static_cast<long long>(want));
  }
}

// ---------------------------------------------------------------------------
// The emitter
// ---------------------------------------------------------------------------

// The smallest thing that proves the whole path works: allocate executable
// memory, encode two instructions into it, call it, get the answer back.
void TestEmitsACallableFunction() {
  printf("the emitter produces code this process can call\n");

  Emitter emitter;
  CodeBlock* block = emitter.create_block(64);
  Check(block != nullptr && block->address != nullptr, "a block was allocated");
  if (block == nullptr || block->address == nullptr)
    return;

  emitter.set_block(block);
  x86::MovRegImm(&emitter, 0, 42);   // 32-bit write zero-extends into RAX
  x86::Ret(&emitter);

  // Cast the block's own address rather than using execute_block, which only
  // knows about void(*)() - a compiled guest block will take arguments.
  typedef int (*IntFunc)();
  const IntFunc func = reinterpret_cast<IntFunc>(block->address);
  CheckEqual(func(), 42, "it returned what it was told to");

  emitter.destroy_block(block);
}

// The convention matters more than the encoding: Windows x64 passes the first
// four integer arguments in RCX, RDX, R8, R9, and the callee owns RBX, RBP,
// RSI, RDI and R12-R15. A recompiler that keeps guest state in a callee-saved
// register and forgets to preserve it corrupts its caller, and the symptom
// turns up nowhere near the cause.
void TestArgumentsArriveWhereTheyShould() {
  printf("arguments arrive in rcx/rdx and callee-saved registers survive\n");

  Emitter emitter;
  CodeBlock* block = emitter.create_block(64);
  if (block == nullptr || block->address == nullptr) {
    Check(false, "a block was allocated");
    return;
  }

  emitter.set_block(block);
  // int32 add(int32 a, int32 b): a in ECX, b in EDX, result in EAX.
  x86::MovRegReg(&emitter, 0, 1);                       // mov eax, ecx
  x86::AluRegReg(&emitter, x86::AluOp::kAdd, 0, 2);     // add eax, edx
  x86::Ret(&emitter);

  typedef int32_t (*AddFunc)(int32_t, int32_t);
  const AddFunc add = reinterpret_cast<AddFunc>(block->address);
  CheckEqual(add(20, 22), 42, "20 + 22 through emitted code");
  CheckEqual(add(-5, 5), 0, "signs survive");
  CheckEqual(add(2147483647, 1), -2147483648LL,
             "and it wraps like a 32-bit add, which is what the guest does");

  emitter.destroy_block(block);
}

// A guest block will be handed a pointer to the machine's register file and
// will work through it. This is that shape, in miniature: read a field, add to
// it, write it back.
void TestReadsAndWritesThroughAPointer() {
  printf("emitted code reads and writes a context through a pointer\n");

  struct Context {
    uint32_t r0;
    uint32_t r1;
  };

  Emitter emitter;
  CodeBlock* block = emitter.create_block(64);
  if (block == nullptr || block->address == nullptr) {
    Check(false, "a block was allocated");
    return;
  }

  emitter.set_block(block);
  // void add_fields(Context* c): c in RCX. c->r1 += c->r0.
  x86::MovRegMem(&emitter, 0, 1, 0);                        // eax = c->r0
  x86::AluRegMem(&emitter, x86::AluOp::kAdd, 0, 1, 4);      // eax += c->r1
  x86::MovMemReg(&emitter, 0, 1, 4);                        // c->r1 = eax
  x86::Ret(&emitter);

  typedef void (*AddFields)(Context*);
  const AddFields add_fields = reinterpret_cast<AddFields>(block->address);

  Context context = { 7, 35 };
  add_fields(&context);
  CheckEqual(context.r1, 42, "the field was updated in place");
  CheckEqual(context.r0, 7, "and the one it read was left alone");

  emitter.destroy_block(block);
}

// Executable memory that is actually given back, which the emitter this
// replaced did not do: its VirtualFree passed a size alongside MEM_RELEASE,
// which is ERROR_INVALID_PARAMETER, and it ignored the result. Two hundred
// cycles of allocate-and-free leaked every byte.
//
// Checked by watching the process's own committed memory rather than by
// trusting the call: the failure mode being guarded against is a free that
// reports nothing and does nothing.
void TestExecutableMemoryIsGivenBack() {
    printf("executable memory is released, not just dropped\n");

    PROCESS_MEMORY_COUNTERS_EX before = {};
    before.cb = sizeof(before);
    GetProcessMemoryInfo(GetCurrentProcess(),
                         reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&before),
                         sizeof(before));

    Emitter emitter;
    for (int i = 0; i < 200; ++i) {
        CodeBlock* block = emitter.create_block(256 * 1024);
        x86::Ret(&emitter);
        emitter.destroy_block(block);
    }

    PROCESS_MEMORY_COUNTERS_EX after = {};
    after.cb = sizeof(after);
    GetProcessMemoryInfo(GetCurrentProcess(),
                         reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&after),
                         sizeof(after));

    CheckEqual(emitter.failed_frees(), 0, "every release succeeded");
    // 200 x 256 KB is 50 MB if none of it came back. A megabyte of slack covers
    // whatever else the process did meanwhile.
    const long long growth =
        static_cast<long long>(after.PrivateUsage - before.PrivateUsage);
    Check(growth < 1024 * 1024,
          "and fifty megabytes of blocks did not stay committed");
    if (growth >= 1024 * 1024)
        printf("  (grew by %lld KB)\n", growth / 1024);
}

// ---------------------------------------------------------------------------
// The block cache
// ---------------------------------------------------------------------------

Block MakeBlock(uint32_t address, uint32_t bytes) {
  Block block;
  block.guest_address = address;
  block.guest_bytes = bytes;
  block.cycles = bytes / 4;   // one cycle an instruction, near enough here
  block.code = reinterpret_cast<void*>(static_cast<uintptr_t>(0x1000 + address));
  return block;
}

void TestCacheFindsWhatWasPutIn() {
  printf("the block cache returns what was put in it\n");

  BlockCache cache;
  cache.Insert(MakeBlock(0x80001000, 64));
  cache.Insert(MakeBlock(0x80002000, 32));
  CheckEqual(static_cast<int64_t>(cache.size()), 2, "two blocks are held");

  const Block* found = cache.Find(0x80001000);
  Check(found != nullptr, "the first one is found");
  if (found != nullptr)
    CheckEqual(found->cycles, 16, "with its cycle count");

  Check(cache.Find(0x80003000) == nullptr, "an address with no block finds none");
  // Only the entry point, never the middle - jumping into the middle of a block
  // compiles a new one from there.
  Check(cache.Find(0x80001004) == nullptr, "and neither does the middle of one");
}

// The three views of RAM are the same memory, so they must be the same block.
void TestTheThreeViewsOfRamAreOneBlock() {
  printf("kuseg, kseg0 and kseg1 are one block, not three\n");

  BlockCache cache;
  cache.Insert(MakeBlock(0x80001000, 16));   // kseg0
  Check(cache.Find(0x00001000) != nullptr, "found through kuseg");
  Check(cache.Find(0xA0001000) != nullptr, "found through kseg1");
  CheckEqual(static_cast<int64_t>(cache.size()), 1, "and it is still one block");
}

void TestAStoreIntoCodeThrowsItAway() {
  printf("a store into a page a block came from discards it\n");

  BlockCache cache;
  cache.Insert(MakeBlock(0x80001000, 64));
  cache.Insert(MakeBlock(0x80005000, 64));   // a different page

  Check(cache.IsCodePage(0x80001040), "the page holding a block is known as code");
  Check(!cache.IsCodePage(0x80009000), "a page with no code in it is not");

  CheckEqual(cache.InvalidatePage(0x80001800), 1, "one block went");
  Check(cache.Find(0x80001000) == nullptr, "and it is gone");
  Check(cache.Find(0x80005000) != nullptr, "the block in another page stayed");
  Check(!cache.IsCodePage(0x80001000), "the emptied page is no longer code");
  Check(cache.IsCodePage(0x80005000), "the other one still is");
}

// A block that straddles a page boundary has to die if either page is written,
// and must not leave the page it shared with someone else marked clean.
void TestABlockSpanningTwoPages() {
  printf("a block across a page boundary belongs to both\n");

  BlockCache cache;
  // Starts 32 bytes before the boundary and runs 128 bytes, so it covers the
  // end of one page and the start of the next.
  cache.Insert(MakeBlock(0x80001000 - 32, 128));
  Check(cache.IsCodePage(0x80000FF0), "the page it starts in is code");
  Check(cache.IsCodePage(0x80001010), "and the page it ends in is too");

  CheckEqual(cache.InvalidatePage(0x80001010), 1, "writing the second page kills it");
  Check(!cache.IsCodePage(0x80000FF0), "and the first page is clean again");
}

void TestClearThrowsEverythingAway() {
  printf("the cache-control write throws everything away\n");

  BlockCache cache;
  for (uint32_t i = 0; i < 16; ++i)
    cache.Insert(MakeBlock(0x80010000 + i * 0x1000, 64));
  CheckEqual(static_cast<int64_t>(cache.size()), 16, "sixteen blocks are held");

  cache.Clear();
  CheckEqual(static_cast<int64_t>(cache.size()), 0, "and none afterwards");
  Check(!cache.IsCodePage(0x80010000), "no page is code any more");
}

// ---------------------------------------------------------------------------
// The block decoder
// ---------------------------------------------------------------------------

// Assembles just enough MIPS to write a block by hand. Each returns the word,
// so a test reads as the program it is.
uint32_t Special(uint32_t rs, uint32_t rt, uint32_t rd, uint32_t funct) {
  return (rs << 21) | (rt << 16) | (rd << 11) | funct;
}
uint32_t ADDU(uint32_t rd, uint32_t rs, uint32_t rt) { return Special(rs, rt, rd, 0x21); }
uint32_t ADDIU(uint32_t rt, uint32_t rs, uint16_t imm) {
  return (0x09u << 26) | (rs << 21) | (rt << 16) | imm;
}
uint32_t LW(uint32_t rt, uint32_t rs, uint16_t imm) {
  return (0x23u << 26) | (rs << 21) | (rt << 16) | imm;
}
uint32_t SW(uint32_t rt, uint32_t rs, uint16_t imm) {
  return (0x2Bu << 26) | (rs << 21) | (rt << 16) | imm;
}
uint32_t BEQ(uint32_t rs, uint32_t rt, uint16_t offset) {
  return (0x04u << 26) | (rs << 21) | (rt << 16) | offset;
}
uint32_t J(uint32_t target) { return (0x02u << 26) | (target & 0x3FFFFFF); }
uint32_t JR(uint32_t rs) { return Special(rs, 0, 0, 0x08); }
uint32_t MULT(uint32_t rs, uint32_t rt) { return Special(rs, rt, 0, 0x18); }
uint32_t SYSCALL() { return Special(0, 0, 0, 0x0C); }
uint32_t RFE() { return (0x10u << 26) | (0x10u << 21) | 0x10u; }
uint32_t NOP() { return 0; }

// A guest memory of hand-written words, and the fetch the decoder reads it
// through. Anything not written is unmapped, which is how the "there is
// nothing there" path gets tested.
class FakeMemory {
 public:
  void Write(uint32_t pc, const std::vector<uint32_t>& words) {
    for (size_t i = 0; i < words.size(); ++i)
      words_[pc + static_cast<uint32_t>(i) * 4] = words[i];
  }

  emulation::rec::FetchWord Fetch() {
    return [this](uint32_t pc, uint32_t* word) {
      const auto it = words_.find(pc);
      if (it == words_.end())
        return false;
      *word = it->second;
      return true;
    };
  }

 private:
  std::map<uint32_t, uint32_t> words_;
};

void TestStraightLineStopsAtTheCap() {
  printf("a run of straight-line code stops at the length cap\n");

  FakeMemory memory;
  std::vector<uint32_t> program;
  for (int i = 0; i < 100; ++i)
    program.push_back(ADDIU(1, 1, 1));
  memory.Write(0x80001000, program);

  BlockDecoder decoder(memory.Fetch());
  const DecodedBlock block = decoder.Decode(0x80001000, 16);
  CheckEqual(static_cast<int64_t>(block.instructions.size()), 16, "sixteen instructions");
  Check(block.end_reason == EndReason::kLengthCap, "and it stopped because of the cap");
  CheckEqual(block.guest_bytes(), 64, "which is 64 bytes of guest code");
  CheckEqual(block.static_cycles, 16, "one cycle each");

  // The next block picks up exactly where this one left off - no instruction
  // skipped, none repeated. This is the arithmetic that decides whether a
  // recompiler runs the program or something adjacent to it.
  uint32_t next = 0;
  Check(BlockDecoder::StaticFallthrough(block, &next), "it has a known continuation");
  CheckEqual(next, 0x80001040, "at the instruction after the last");
}

void TestABranchTakesItsDelaySlot() {
  printf("a branch ends the block, and its delay slot comes with it\n");

  FakeMemory memory;
  memory.Write(0x80001000, {
      ADDIU(1, 0, 5),      // 1000
      BEQ(1, 2, 4),        // 1004  <- branch
      ADDU(3, 1, 2),       // 1008  <- delay slot, runs either way
      ADDIU(4, 0, 9),      // 100C  <- must NOT be in this block
  });

  BlockDecoder decoder(memory.Fetch());
  const DecodedBlock block = decoder.Decode(0x80001000);
  CheckEqual(static_cast<int64_t>(block.instructions.size()), 3,
             "three instructions: up to the branch, plus the slot");
  Check(block.end_reason == EndReason::kBranchDelaySlot, "and it says why it stopped");
  Check(block.instructions[1].kind == Kind::kBranch, "the branch was recognised");
  Check(block.instructions[2].in_delay_slot, "the last one is marked as the delay slot");
  Check(!block.instructions[1].in_delay_slot, "the branch itself is not");
  CheckEqual(block.instructions.back().pc, 0x80001008, "the block ends at the slot");
}

void TestEveryJumpFormEndsABlock() {
  printf("j, jr and the register forms all end a block the same way\n");

  const uint32_t kJumps[] = { J(0x200400), JR(31), BEQ(0, 0, 8) };
  const char* kNames[] = { "j", "jr", "beq" };

  for (int i = 0; i < 3; ++i) {
    FakeMemory memory;
    memory.Write(0x80002000, { kJumps[i], NOP(), ADDIU(1, 1, 1) });
    BlockDecoder decoder(memory.Fetch());
    const DecodedBlock block = decoder.Decode(0x80002000);
    CheckEqual(static_cast<int64_t>(block.instructions.size()), 2, kNames[i]);
    Check(block.end_reason == EndReason::kBranchDelaySlot, "for the delay-slot reason");
  }
}

void TestSyscallEndsTheBlockWithNoDelaySlot() {
  printf("syscall ends a block immediately - it has no delay slot\n");

  FakeMemory memory;
  memory.Write(0x80003000, { ADDIU(1, 0, 1), SYSCALL(), ADDIU(2, 0, 2) });
  BlockDecoder decoder(memory.Fetch());
  const DecodedBlock block = decoder.Decode(0x80003000);
  CheckEqual(static_cast<int64_t>(block.instructions.size()), 2,
             "the syscall is the last instruction");
  Check(block.end_reason == EndReason::kException, "and the reason is the exception");
  // What follows a syscall in memory is not what runs next - the exception
  // vector is - so including it would compile code that never executes there.
  Check(block.instructions.back().kind == Kind::kSyscall, "the syscall is in the block");
}

void TestReturnFromExceptionEndsTheBlock() {
  printf("rfe ends a block: what runs next is EPC, not the next instruction\n");

  FakeMemory memory;
  memory.Write(0x80004000, { ADDIU(1, 0, 1), RFE(), ADDIU(2, 0, 2) });
  BlockDecoder decoder(memory.Fetch());
  const DecodedBlock block = decoder.Decode(0x80004000);
  CheckEqual(static_cast<int64_t>(block.instructions.size()), 2, "it stops at the rfe");
  Check(block.end_reason == EndReason::kReturnFromException, "for that reason");
}

// Bug 7 is the reason this one is here: getting the return-from-exception path
// wrong cost this project an afternoon of BIOS disassembly, and a recompiler
// that mistakes an rfe for an ordinary cop0 move would do it again.
void TestAnOrdinaryCop0MoveIsNotAnRfe() {
  printf("an ordinary cop0 move is not mistaken for an rfe\n");

  const uint32_t mfc0 = (0x10u << 26) | (0x00u << 21) | (2u << 16) | (12u << 11);
  FakeMemory memory;
  memory.Write(0x80004100, { mfc0, ADDIU(1, 1, 1), NOP(), NOP() });
  BlockDecoder decoder(memory.Fetch());
  const DecodedBlock block = decoder.Decode(0x80004100, 4);
  CheckEqual(static_cast<int64_t>(block.instructions.size()), 4, "the block ran on");
  Check(block.instructions[0].kind == Kind::kCoprocessor, "and it is a coprocessor move");
}

void TestMultiplyMarksTheBlockAsDynamicallyPriced() {
  printf("a block containing mult cannot have its cycles charged up front\n");

  FakeMemory memory;
  memory.Write(0x80005000, { ADDIU(1, 0, 7), MULT(1, 2), ADDU(3, 0, 0), NOP() });
  BlockDecoder decoder(memory.Fetch());
  const DecodedBlock block = decoder.Decode(0x80005000, 4);

  Check(block.dynamic_cost,
        "the block is flagged: mult costs 6, 9 or 13 by operand magnitude");
  Check(block.instructions[1].kind == Kind::kMultiplyDivide, "the mult was recognised");

  // And a block without one is fully priced at compile time, which is the
  // common case and the one worth keeping cheap.
  FakeMemory plain;
  plain.Write(0x80005100, { ADDIU(1, 0, 7), ADDU(3, 1, 2), NOP(), NOP() });
  BlockDecoder plain_decoder(plain.Fetch());
  const DecodedBlock plain_block = plain_decoder.Decode(0x80005100, 4);
  Check(!plain_block.dynamic_cost, "an ordinary block is priced statically");
  CheckEqual(plain_block.static_cycles, 4, "at one cycle an instruction");
}

void TestLoadsAndStoresAreToldApart() {
  printf("loads and stores are classified, since they are what needs a bus\n");

  FakeMemory memory;
  memory.Write(0x80006000, { LW(2, 1, 0), SW(2, 1, 4), NOP(), NOP() });
  BlockDecoder decoder(memory.Fetch());
  const DecodedBlock block = decoder.Decode(0x80006000, 4);
  Check(block.instructions[0].kind == Kind::kLoad, "lw is a load");
  Check(block.instructions[1].kind == Kind::kStore, "sw is a store");
}

void TestUnmappedMemoryEndsTheBlock() {
  printf("a block stops where the memory does\n");

  FakeMemory memory;
  memory.Write(0x80007000, { ADDIU(1, 0, 1), ADDIU(2, 0, 2) });   // and no more
  BlockDecoder decoder(memory.Fetch());
  const DecodedBlock block = decoder.Decode(0x80007000);
  CheckEqual(static_cast<int64_t>(block.instructions.size()), 2, "two instructions");
  Check(block.end_reason == EndReason::kFetchFailed, "and it stopped at the edge");

  // Starting in nothing gives nothing rather than a block of garbage.
  const DecodedBlock empty = decoder.Decode(0x8000F000);
  Check(empty.empty(), "a block starting in unmapped memory is empty");
}

// Entering at the delay slot of a branch is a real thing - an exception
// returns there - and it is an ordinary block start, not a special case.
void TestAblockCanStartAtADelaySlot() {
  printf("a block can start at a delay slot\n");

  FakeMemory memory;
  // Five words, so that entering at the second one still leaves four to decode
  // and the cap is what stops it rather than the end of the program.
  memory.Write(0x80008000,
               { BEQ(1, 2, 4), ADDU(3, 1, 2), ADDIU(4, 0, 1), NOP(), NOP() });
  BlockDecoder decoder(memory.Fetch());
  const DecodedBlock block = decoder.Decode(0x80008004, 4);
  CheckEqual(static_cast<int64_t>(block.instructions.size()), 4,
             "it decodes from there like any other address");
  Check(!block.instructions[0].in_delay_slot,
        "and nothing pretends to know it was one");
}

// ---------------------------------------------------------------------------
// The compiler, checked against an interpreter of the same subset
// ---------------------------------------------------------------------------

// The guest memory a compiled block reaches, and the callbacks it reaches it
// through. In the emulator these are thunks onto Cpu::Load and Cpu::Store; here
// they are a kilobyte of bytes, which is all that is needed to prove the call
// path, the argument order and the widths.
//
// Any address is accepted - the index is masked - so that a sweep over every
// opcode can use whatever register values it likes without arranging for each
// one to be in range. Both sides of every comparison go through these same two
// functions, so the masking cannot hide a disagreement.
// 32 KB, and it holds code as well as data: step 5's engine decodes guest
// instructions out of the same memory a store can write to, which is what makes
// self-modifying code testable at all.
const uint32_t kBusBase = 0x80000000;
const int kBusBytes = 32768;

class FakeBus {
 public:
  FakeBus() {
    for (int i = 0; i < kBusBytes; ++i)
      bytes_[i] = static_cast<uint8_t>(i * 7 + 1);
  }

  // Programs are written as words and read back by the decoder through the
  // same bytes a `sw` reaches.
  void WriteProgram(uint32_t address, const std::vector<uint32_t>& words) {
    for (size_t i = 0; i < words.size(); ++i)
      Write(address + static_cast<uint32_t>(i) * 4, 4, words[i]);
  }

  uint32_t ReadWord(uint32_t address) const { return Read(address, 4); }

  emulation::rec::FetchWord Fetch() {
    return [this](uint32_t pc, uint32_t* word) {
      *word = ReadWord(pc);
      return true;
    };
  }

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

  bool SameAs(const FakeBus& other) const {
    return memcmp(bytes_, other.bytes_, sizeof(bytes_)) == 0;
  }

  int calls = 0;   // how many times the compiled code called out

  // Whether the stack was aligned the way the calling convention promises at
  // every call the compiled code made. A callee is entitled to assume RSP was
  // 16-byte aligned before the call pushed the return address, so the return
  // address itself sits at an address of the form 16n+8. Nothing in these
  // callbacks would notice otherwise - but a real Cpu::Load, compiled with
  // aligned SSE spills, would crash, and it would crash a long way from here.
  bool alignment_ok = true;

  void NoteStackAlignment(void* return_address_slot) {
    if ((reinterpret_cast<uintptr_t>(return_address_slot) % 16) != 8)
      alignment_ok = false;
  }

  // The callbacks, in the shape rec/runtime.h asks for. The intrinsic has to
  // be written in the function that was actually called, not in a helper, so
  // that it names that function's own frame.
  // When this is set, an unaligned word access is treated as a guest exception
  // the way Cpu::Load treats one: the access does not happen, and the block is
  // told to stop where it is.
  emulation::rec::Recompiler* faults_on_unaligned = nullptr;

  static uint32_t Load32(void* c, uint32_t a, uint32_t) {
    Bus(c)->NoteStackAlignment(_AddressOfReturnAddress());
    FakeBus* bus = Bus(c);
    if (bus->faults_on_unaligned != nullptr && (a & 3) != 0) {
      bus->faults_on_unaligned->SetFault();
      return 0;
    }
    return bus->DoRead(a, 4);
  }
  static uint32_t Load16(void* c, uint32_t a, uint32_t) { return Bus(c)->DoRead(a, 2); }
  static uint32_t Load8(void* c, uint32_t a, uint32_t) { return Bus(c)->DoRead(a, 1); }
  static void Store32(void* c, uint32_t a, uint32_t v, uint32_t) {
    Bus(c)->NoteStackAlignment(_AddressOfReturnAddress());
    Bus(c)->DoWrite(a, 4, v);
  }
  static void Store16(void* c, uint32_t a, uint32_t v, uint32_t) { Bus(c)->DoWrite(a, 2, v); }
  static void Store8(void* c, uint32_t a, uint32_t v, uint32_t) { Bus(c)->DoWrite(a, 1, v); }

 private:
  static FakeBus* Bus(void* context) { return static_cast<FakeBus*>(context); }
  static int Index(uint32_t address) {
    return static_cast<int>((address - kBusBase) & (kBusBytes - 1));
  }

  uint32_t DoRead(uint32_t address, int width) {
    ++calls;
    return Read(address, width);
  }
  void DoWrite(uint32_t address, int width, uint32_t value) {
    ++calls;
    Write(address, width, value);
  }

  uint8_t bytes_[kBusBytes];
};

emulation::rec::BlockState MakeState(uint32_t* regs, FakeBus* bus) {
  emulation::rec::BlockState state;
  state.regs = regs;
  state.context = bus;
  state.load32 = &FakeBus::Load32;
  state.load16 = &FakeBus::Load16;
  state.load8 = &FakeBus::Load8;
  state.store32 = &FakeBus::Store32;
  state.store16 = &FakeBus::Store16;
  state.store8 = &FakeBus::Store8;
  return state;
}

void RunBlock(CodeBlock* code, emulation::rec::BlockState* state) {
  reinterpret_cast<void (*)(emulation::rec::BlockState*)>(code->address)(state);
}

// Step 6's register allocator is a switch, and every test below runs with it
// both ways: an allocator that changes an answer is the whole risk of the
// step, and running the suite twice is what makes the claim that it does not.
bool g_allocate_registers = true;
bool g_link_blocks = true;

emulation::rec::BlockCompiler MakeCompiler(Emitter* emitter) {
  emulation::rec::BlockCompiler compiler(emitter);
  compiler.set_allocate_registers(g_allocate_registers);
  compiler.set_link_blocks(g_link_blocks);
  // Allocate however short the block is. In the emulator a block has to be
  // long enough for allocation to pay; here it has to happen at all, or the
  // second pass over the suite would compile the same thing as the first and
  // prove nothing. Almost every test block below is shorter than the threshold.
  compiler.set_minimum_block_instructions(1);
  return compiler;
}

// What the compiled code is measured against. Written from the instruction set
// and from the three functions that make up this core's load pipeline -
// Cpu::AdvanceLoadDelay, Cpu::WriteReg, Cpu::ArmLoad - rather than from the
// compiler: if both were derived from the same reading, a misreading would
// agree with itself.
//
// The compiler resolves the load delay statically, at compile time, and this
// runs it as the hardware does, one stage at a time. That the two agree is the
// point of the exercise.
class Machine {
 public:
  uint32_t r[32] = {};
  uint32_t next_pc = 0;
  FakeBus* bus = nullptr;

  // Every store tells whoever is watching for code being overwritten. In the
  // emulator this is the call `Cpu::Store` will have to make - invalidation
  // cannot be contained inside the recompiler, because only some of a
  // program's stores go through compiled code.
  std::function<void(uint32_t)> on_store;

  void Step(uint32_t pc, uint32_t word);

  // One step as the interpreter takes it: the instruction at `pc`, plus its
  // delay slot when it has one, and the address to continue at. That is the
  // shape `Cpu::ExecuteInstruction` already has - it runs a branch's delay slot
  // as a nested execute - and it is what the recompiler's fallback hook wants.
  uint32_t Run(uint32_t pc) {
    const uint32_t word = bus->ReadWord(pc);
    next_pc = pc + 4;
    Step(pc, word);

    const Kind kind = BlockDecoder::Classify(word);
    if (kind == Kind::kBranch || kind == Kind::kJump) {
      const uint32_t target = next_pc;   // where the branch decided to go
      next_pc = pc + 8;
      Step(pc + 4, bus->ReadWord(pc + 4));   // the delay slot runs either way
      return target;
    }
    return next_pc;
  }

  // Whether a load is still on its way to a register. The recompiler asks this
  // before entering a compiled block.
  bool LoadInFlight() const { return pending_.active || armed_.active; }

  void Stored(uint32_t address) {
    if (on_store)
      on_store(address);
  }

  // The pipeline moves on one stage at the start of every instruction - and
  // once more after the last one, which is where a load issued second-to-last
  // reaches its register.
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

 private:
  struct Load {
    uint32_t reg = 0;
    uint32_t value = 0;
    bool active = false;
  };
  Load pending_;
  Load armed_;

  // A write cancels a load still in flight to the same register: the hardware
  // writes the load back first and the instruction's result second.
  void Write(uint32_t index, uint32_t value) {
    if (index != 0)
      r[index] = value;
    if (pending_.active && pending_.reg == index)
      pending_.active = false;
  }

  // And a second load to the same register discards the first before it ever
  // reaches the register file.
  void Arm(uint32_t index, uint32_t value) {
    if (pending_.active && pending_.reg == index)
      pending_.active = false;
    armed_.reg = index;
    armed_.value = value;
    armed_.active = (index != 0);
  }
};

void Machine::Step(uint32_t pc, uint32_t word) {
  Advance();

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
      case 0x04: Write(rd, r[rt] << (r[rs] & 31)); return;
      case 0x06: Write(rd, r[rt] >> (r[rs] & 31)); return;
      case 0x07: Write(rd, static_cast<uint32_t>(static_cast<int32_t>(r[rt]) >> (r[rs] & 31))); return;
      case 0x08: next_pc = r[rs]; return;                       // jr
      case 0x09: {                                              // jalr
        const uint32_t destination = r[rs];   // read before the link is written
        Write(rd, pc + 8);
        next_pc = destination;
        return;
      }
      // The trapping forms. The compiler refuses these, so the recompiler's
      // fallback has to run them - which is exactly what makes them useful in
      // a test of the fallback. Overflow would raise an exception on hardware;
      // nothing here overflows.
      case 0x20: Write(rd, r[rs] + r[rt]); return;   // add
      case 0x22: Write(rd, r[rs] - r[rt]); return;   // sub

      case 0x21: Write(rd, r[rs] + r[rt]); return;
      case 0x23: Write(rd, r[rs] - r[rt]); return;
      case 0x24: Write(rd, r[rs] & r[rt]); return;
      case 0x25: Write(rd, r[rs] | r[rt]); return;
      case 0x26: Write(rd, r[rs] ^ r[rt]); return;
      case 0x27: Write(rd, ~(r[rs] | r[rt])); return;
      case 0x2A: Write(rd, (static_cast<int32_t>(r[rs]) < static_cast<int32_t>(r[rt])) ? 1u : 0u); return;
      case 0x2B: Write(rd, (r[rs] < r[rt]) ? 1u : 0u); return;
      default: return;
    }
  }

  switch (opcode) {
    case 0x01: {   // bltz, bgez, and the two "and link" forms
      // The link is written first and unconditionally, before the condition is
      // even looked at - which matters when rs is r31 itself. Cpu::BGEZAL does
      // exactly this, and the order is the only part of it worth copying.
      if ((rt & 0x10) != 0)
        Write(31, pc + 8);
      const bool take = ((rt & 1) == 0) ? (static_cast<int32_t>(r[rs]) < 0)
                                        : (static_cast<int32_t>(r[rs]) >= 0);
      next_pc = take ? taken : fall;
      return;
    }
    case 0x02:   // j
      next_pc = ((pc + 4) & 0xF0000000u) | ((word & 0x03FFFFFFu) << 2);
      return;
    case 0x03:   // jal - links whichever way it goes, which is always
      Write(31, pc + 8);
      next_pc = ((pc + 4) & 0xF0000000u) | ((word & 0x03FFFFFFu) << 2);
      return;
    case 0x04: next_pc = (r[rs] == r[rt]) ? taken : fall; return;
    case 0x05: next_pc = (r[rs] != r[rt]) ? taken : fall; return;
    case 0x06: next_pc = (static_cast<int32_t>(r[rs]) <= 0) ? taken : fall; return;
    case 0x07: next_pc = (static_cast<int32_t>(r[rs]) > 0) ? taken : fall; return;

    case 0x08: Write(rt, r[rs] + se); return;   // addi - trapping, so not compiled
    case 0x09: Write(rt, r[rs] + se); return;
    case 0x0A: Write(rt, (static_cast<int32_t>(r[rs]) < static_cast<int32_t>(se)) ? 1u : 0u); return;
    case 0x0B: Write(rt, (r[rs] < se) ? 1u : 0u); return;
    case 0x0C: Write(rt, r[rs] & imm); return;
    case 0x0D: Write(rt, r[rs] | imm); return;
    case 0x0E: Write(rt, r[rs] ^ imm); return;
    case 0x0F: Write(rt, static_cast<uint32_t>(imm) << 16); return;

    case 0x20: Arm(rt, static_cast<uint32_t>(static_cast<int8_t>(bus->Read(address, 1)))); return;
    case 0x24: Arm(rt, bus->Read(address, 1)); return;
    case 0x21: Arm(rt, static_cast<uint32_t>(static_cast<int16_t>(bus->Read(address, 2)))); return;
    case 0x25: Arm(rt, bus->Read(address, 2)); return;
    case 0x23: Arm(rt, bus->Read(address, 4)); return;

    case 0x28: bus->Write(address, 1, r[rt]); Stored(address); return;
    case 0x29: bus->Write(address, 2, r[rt]); Stored(address); return;
    case 0x2B: bus->Write(address, 4, r[rt]); Stored(address); return;
    default: return;
  }
}

uint32_t SUBU(uint32_t rd, uint32_t rs, uint32_t rt) { return Special(rs, rt, rd, 0x23); }
uint32_t AND_(uint32_t rd, uint32_t rs, uint32_t rt) { return Special(rs, rt, rd, 0x24); }
uint32_t OR_(uint32_t rd, uint32_t rs, uint32_t rt) { return Special(rs, rt, rd, 0x25); }
uint32_t XOR_(uint32_t rd, uint32_t rs, uint32_t rt) { return Special(rs, rt, rd, 0x26); }
uint32_t NOR_(uint32_t rd, uint32_t rs, uint32_t rt) { return Special(rs, rt, rd, 0x27); }
uint32_t SLT_(uint32_t rd, uint32_t rs, uint32_t rt) { return Special(rs, rt, rd, 0x2A); }
uint32_t SLTU_(uint32_t rd, uint32_t rs, uint32_t rt) { return Special(rs, rt, rd, 0x2B); }
uint32_t SLL_(uint32_t rd, uint32_t rt, uint32_t sa) { return (rt << 16) | (rd << 11) | (sa << 6); }
uint32_t SRL_(uint32_t rd, uint32_t rt, uint32_t sa) { return (rt << 16) | (rd << 11) | (sa << 6) | 0x02; }
uint32_t SRA_(uint32_t rd, uint32_t rt, uint32_t sa) { return (rt << 16) | (rd << 11) | (sa << 6) | 0x03; }
uint32_t SLLV_(uint32_t rd, uint32_t rt, uint32_t rs) { return Special(rs, rt, rd, 0x04); }
uint32_t SRAV_(uint32_t rd, uint32_t rt, uint32_t rs) { return Special(rs, rt, rd, 0x07); }
uint32_t ANDI_(uint32_t rt, uint32_t rs, uint16_t imm) { return (0x0Cu << 26) | (rs << 21) | (rt << 16) | imm; }
uint32_t ORI_(uint32_t rt, uint32_t rs, uint16_t imm) { return (0x0Du << 26) | (rs << 21) | (rt << 16) | imm; }
uint32_t XORI_(uint32_t rt, uint32_t rs, uint16_t imm) { return (0x0Eu << 26) | (rs << 21) | (rt << 16) | imm; }
uint32_t LUI_(uint32_t rt, uint16_t imm) { return (0x0Fu << 26) | (rt << 16) | imm; }
uint32_t SLTI_(uint32_t rt, uint32_t rs, uint16_t imm) { return (0x0Au << 26) | (rs << 21) | (rt << 16) | imm; }
uint32_t SLTIU_(uint32_t rt, uint32_t rs, uint16_t imm) { return (0x0Bu << 26) | (rs << 21) | (rt << 16) | imm; }
uint32_t ADD_TRAPPING(uint32_t rd, uint32_t rs, uint32_t rt) { return Special(rs, rt, rd, 0x20); }
uint32_t LB(uint32_t rt, uint32_t rs, uint16_t imm) { return (0x20u << 26) | (rs << 21) | (rt << 16) | imm; }
uint32_t LBU(uint32_t rt, uint32_t rs, uint16_t imm) { return (0x24u << 26) | (rs << 21) | (rt << 16) | imm; }
uint32_t LH(uint32_t rt, uint32_t rs, uint16_t imm) { return (0x21u << 26) | (rs << 21) | (rt << 16) | imm; }
uint32_t LHU(uint32_t rt, uint32_t rs, uint16_t imm) { return (0x25u << 26) | (rs << 21) | (rt << 16) | imm; }
uint32_t SB(uint32_t rt, uint32_t rs, uint16_t imm) { return (0x28u << 26) | (rs << 21) | (rt << 16) | imm; }
uint32_t SH(uint32_t rt, uint32_t rs, uint16_t imm) { return (0x29u << 26) | (rs << 21) | (rt << 16) | imm; }
uint32_t BNE(uint32_t rs, uint32_t rt, uint16_t offset) { return (0x05u << 26) | (rs << 21) | (rt << 16) | offset; }
uint32_t BLEZ(uint32_t rs, uint16_t offset) { return (0x06u << 26) | (rs << 21) | offset; }
uint32_t BGTZ(uint32_t rs, uint16_t offset) { return (0x07u << 26) | (rs << 21) | offset; }
uint32_t BLTZ(uint32_t rs, uint16_t offset) { return (0x01u << 26) | (rs << 21) | offset; }
uint32_t BGEZ(uint32_t rs, uint16_t offset) { return (0x01u << 26) | (rs << 21) | (1u << 16) | offset; }
uint32_t BGEZAL(uint32_t rs, uint16_t offset) { return (0x01u << 26) | (rs << 21) | (0x11u << 16) | offset; }
uint32_t JAL(uint32_t target) { return (0x03u << 26) | (target & 0x3FFFFFF); }
uint32_t JALR(uint32_t rd, uint32_t rs) { return Special(rs, 0, rd, 0x09); }
uint32_t LWL(uint32_t rt, uint32_t rs, uint16_t imm) { return (0x22u << 26) | (rs << 21) | (rt << 16) | imm; }

const uint32_t kProgramBase = 0x80001000;

// Runs a program both ways from the same starting registers and the same
// memory, and compares all 32 registers, every byte of memory, and the address
// each says to continue at. This is the differential harness of
// Docs/Recompiler-Plan.md in miniature - the full one runs against the real
// interpreter over real games, and works exactly like this.
// `report` is off for the sweeps, which run this hundreds of times and count
// the whole sweep as one check rather than flooding the output.
bool RunBothWays(const std::vector<uint32_t>& program,
                 const uint32_t initial[32], const char* what,
                 bool report = true) {
  FakeMemory memory;
  memory.Write(kProgramBase, program);
  BlockDecoder decoder(memory.Fetch());
  const DecodedBlock decoded =
      decoder.Decode(kProgramBase, static_cast<uint32_t>(program.size()));

  Emitter emitter;
  CodeBlock* code = emitter.create_block(4096);
  emulation::rec::BlockCompiler compiler = MakeCompiler(&emitter);
  const emulation::rec::CompiledBlock compiled = compiler.Compile(decoded, code);

  uint32_t compiled_regs[32];
  Machine machine;
  for (int i = 0; i < 32; ++i) {
    compiled_regs[i] = initial[i];
    machine.r[i] = initial[i];
  }

  FakeBus compiled_bus;
  FakeBus reference_bus;
  machine.bus = &reference_bus;

  emulation::rec::BlockState state = MakeState(compiled_regs, &compiled_bus);
  RunBlock(code, &state);

  // The interpreter only runs what the compiler claimed, which is the contract
  // between them: it resumes exactly where the compiled code stopped. The
  // extra Advance is the pipeline stage that would happen at the start of the
  // instruction it resumes at.
  for (uint32_t i = 0; i < compiled.compiled; ++i)
    machine.Step(decoded.instructions[i].pc, decoded.instructions[i].word);
  machine.Advance();

  const uint32_t expected_next_pc =
      compiled.ends_with_branch ? machine.next_pc
                                : kProgramBase + compiled.compiled * 4;

  bool same = true;
  for (int i = 0; i < 32 && same; ++i) {
    if (compiled_regs[i] != machine.r[i]) {
      if (report) {
        printf("  FAIL  %s: r%d compiled %08X, interpreted %08X (after %u of %u)\n",
               what, i, compiled_regs[i], machine.r[i], compiled.compiled,
               static_cast<unsigned>(decoded.instructions.size()));
      }
      same = false;
    }
  }
  if (same && !compiled_bus.SameAs(reference_bus)) {
    if (report)
      printf("  FAIL  %s: memory differs\n", what);
    same = false;
  }
  if (same && state.next_pc != expected_next_pc) {
    if (report) {
      printf("  FAIL  %s: next_pc compiled %08X, expected %08X\n", what,
             state.next_pc, expected_next_pc);
    }
    same = false;
  }

  if (report) {
    ++g_checks;
    if (!same)
      ++g_failures;
  }
  emitter.destroy_block(code);
  return same;
}

void TestCompiledArithmeticMatchesTheInterpreter() {
  printf("compiled code agrees with an interpreter of the same subset\n");

  uint32_t initial[32] = {};
  // Values chosen to exercise signs, the top bit, and the boundaries slt and
  // the shifts care about.
  initial[1] = 5;
  initial[2] = 0xFFFFFFFB;          // -5
  initial[3] = 0x7FFFFFFF;
  initial[4] = 0x80000000;
  initial[5] = 1;
  initial[6] = 31;
  initial[7] = 0xDEADBEEF;
  initial[8] = 0x0000FFFF;

  const std::vector<uint32_t> program = {
      ADDU(9, 1, 2),        // 5 + -5
      SUBU(10, 1, 2),       // 5 - -5
      AND_(11, 7, 8),
      OR_(12, 7, 8),
      XOR_(13, 7, 8),
      NOR_(14, 7, 8),
      SLT_(15, 2, 1),       // -5 < 5 signed: 1
      SLTU_(16, 2, 1),      // huge < 5 unsigned: 0
      SLT_(17, 3, 4),       // 7FFFFFFF < 80000000 signed: 0
      SLTU_(18, 3, 4),      // unsigned: 1
      SLL_(19, 7, 4),
      SRL_(20, 7, 4),
      SRA_(21, 4, 4),       // arithmetic: the sign fills in
      SLLV_(22, 7, 6),      // by 31
      SRAV_(23, 4, 6),
      ADDIU(24, 1, 0xFFFF), // + (-1), sign-extended
      ANDI_(25, 7, 0x00FF),
      ORI_(26, 7, 0xF000),
      XORI_(27, 7, 0xFFFF),
      LUI_(28, 0xABCD),
      SLTI_(29, 2, 0x0001), // -5 < 1 signed: 1
      SLTIU_(30, 2, 0x0001),// unsigned: 0
  };

  RunBothWays(program, initial, "arithmetic block");
}

void TestRegisterZeroStaysZero() {
  printf("r0 reads as zero and cannot be written\n");

  uint32_t initial[32] = {};
  initial[1] = 0x12345678;
  const std::vector<uint32_t> program = {
      ADDU(0, 1, 1),      // write to r0 - discarded
      ORI_(0, 1, 0xFFFF), // and again, the immediate way
      ADDU(2, 0, 0),      // r2 = r0 + r0, which is zero
      ADDU(3, 1, 0),      // r3 = r1 + r0 = r1
  };
  Check(RunBothWays(program, initial, "r0 block"), "the two agree");

  // And explicitly, because this is the one that would go unnoticed: r0 is
  // still zero afterwards.
  FakeMemory memory;
  memory.Write(kProgramBase, program);
  BlockDecoder decoder(memory.Fetch());
  const DecodedBlock decoded = decoder.Decode(kProgramBase, 4);
  Emitter emitter;
  CodeBlock* code = emitter.create_block(4096);
  emulation::rec::BlockCompiler compiler = MakeCompiler(&emitter);
  compiler.Compile(decoded, code);

  uint32_t regs[32] = {};
  regs[1] = 0x12345678;
  FakeBus bus;
  emulation::rec::BlockState state = MakeState(regs, &bus);
  RunBlock(code, &state);
  CheckEqual(regs[0], 0, "r0 is still zero");
  CheckEqual(regs[2], 0, "and reading it gave zero");
  CheckEqual(regs[3], 0x12345678, "while r1 came through untouched");
  emitter.destroy_block(code);
}

void TestItStopsAtWhatItCannotCompile() {
  printf("an instruction outside the subset stops the compiled run\n");

  FakeMemory memory;
  memory.Write(kProgramBase, {
      ADDIU(1, 0, 10),          // compiled
      ADDU(2, 1, 1),            // compiled
      ADD_TRAPPING(3, 1, 1),    // NOT compiled - add traps on overflow
      ADDU(4, 1, 1),            // not reached by the compiled code
      NOP(),
  });
  BlockDecoder decoder(memory.Fetch());
  const DecodedBlock decoded = decoder.Decode(kProgramBase, 5);
  Emitter emitter;
  CodeBlock* code = emitter.create_block(4096);
  emulation::rec::BlockCompiler compiler = MakeCompiler(&emitter);
  const emulation::rec::CompiledBlock compiled = compiler.Compile(decoded, code);

  CheckEqual(compiled.compiled, 2, "two instructions were compiled");
  Check(!compiled.complete, "and the block is not complete");

  // The trapping form is the point: compiling `add` as though it were `addu`
  // would silently drop an overflow exception, so it is left to the
  // interpreter rather than approximated.
  uint32_t regs[32] = {};
  FakeBus bus;
  emulation::rec::BlockState state = MakeState(regs, &bus);
  RunBlock(code, &state);
  CheckEqual(regs[1], 10, "the first instruction ran");
  CheckEqual(regs[2], 20, "and the second");
  CheckEqual(regs[3], 0, "the trapping add did not");
  CheckEqual(regs[4], 0, "nor anything after it");

  // And the interpreter is told where to pick up: at the instruction the
  // compiled code stopped before, not after it.
  CheckEqual(state.next_pc, kProgramBase + 8,
             "next_pc is the instruction the interpreter resumes at");
  emitter.destroy_block(code);
}

void TestNopCompilesToNothing() {
  printf("a nop compiles to no host code at all\n");

  Emitter emitter;
  emulation::rec::BlockCompiler compiler = MakeCompiler(&emitter);

  // A single nop is the floor: the prologue, the next_pc store and the tail.
  // Four of them have to come to exactly the same thing, since each emits
  // nothing of its own. (An empty block is not the comparison to make - it has
  // no tail at all, because there is nothing to charge to the budget and
  // nowhere to go next.)
  FakeMemory one;
  one.Write(kProgramBase, { NOP() });
  BlockDecoder one_decoder(one.Fetch());
  CodeBlock* empty_code = emitter.create_block(4096);
  const emulation::rec::CompiledBlock empty =
      compiler.Compile(one_decoder.Decode(kProgramBase, 1), empty_code);

  FakeMemory memory;
  memory.Write(kProgramBase, { NOP(), NOP(), NOP(), NOP() });
  BlockDecoder decoder(memory.Fetch());
  const DecodedBlock decoded = decoder.Decode(kProgramBase, 4);
  CodeBlock* code = emitter.create_block(4096);
  const emulation::rec::CompiledBlock compiled = compiler.Compile(decoded, code);

  CheckEqual(compiled.compiled, 4, "all four count as compiled");
  CheckEqual(compiled.host_bytes, empty.host_bytes,
             "and they emitted no code of their own");
  emitter.destroy_block(code);
  emitter.destroy_block(empty_code);
}

// A block of one instruction repeated with every register as the destination:
// the crude version of what the differential harness will do over real games,
// and it catches an offset that is right for r1 and wrong for r31.
void TestEveryRegisterIsAddressedCorrectly() {
  printf("every one of the 32 registers is reached at the right offset\n");

  uint32_t initial[32] = {};
  for (int i = 0; i < 32; ++i)
    initial[i] = 0x1000 + i;

  std::vector<uint32_t> program;
  for (uint32_t reg = 1; reg < 32; ++reg)
    program.push_back(ADDIU(reg, reg, 1));
  Check(RunBothWays(program, initial, "all registers"), "the two agree");
}

// ---------------------------------------------------------------------------
// Step 4: memory and branches
// ---------------------------------------------------------------------------

void TestLoadsAndStoresGoThroughTheCallbacks() {
  printf("loads and stores reach memory through the host's callbacks\n");

  uint32_t initial[32] = {};
  initial[1] = kBusBase;          // a base to address memory from
  initial[2] = 0xAABBCCDD;
  initial[3] = kBusBase + 512;

  // Every width, loading and storing, with the delay slots filled by nops so
  // that each load's value is in its register before the next one reads it.
  const std::vector<uint32_t> program = {
      SW(2, 1, 0),        // store the whole word
      NOP(),
      LW(4, 1, 0),        // and read it back
      NOP(),
      LB(5, 1, 0),        // the low byte, sign-extended
      NOP(),
      LBU(6, 1, 0),       // and zero-extended
      NOP(),
      LH(7, 1, 0),        // the low half, sign-extended
      NOP(),
      LHU(8, 1, 0),       // and zero-extended
      NOP(),
      SB(2, 3, 4),        // a byte store, at an offset
      NOP(),
      SH(2, 3, 8),        // and a halfword
      NOP(),
      LW(9, 3, 4),        // read back across both of them
      NOP(),
      LW(10, 1, 0xFFFC),  // a negative offset, which is how a stack is read
      NOP(),
  };
  Check(RunBothWays(program, initial, "memory block"), "the two agree");
}

void TestTheValueOfALoadArrivesOneInstructionLate() {
  printf("a load's value reaches its register one instruction late\n");

  uint32_t initial[32] = {};
  initial[1] = kBusBase;
  initial[2] = 0x11111111;
  initial[5] = 0x55555555;

  // The classic: the instruction in the load's delay slot reads the register's
  // *old* value. Compiling this as though the load landed immediately is the
  // single most likely way for a recompiler to diverge from its interpreter,
  // and it would do so silently.
  const std::vector<uint32_t> program = {
      SW(5, 1, 0),        // memory holds 55555555
      NOP(),
      LW(2, 1, 0),        // r2 will become 55555555...
      ADDU(3, 2, 0),      // ...but not yet: r3 gets the old 11111111
      ADDU(4, 2, 0),      // and now it has landed: r4 gets 55555555
  };
  Check(RunBothWays(program, initial, "load delay block"), "the two agree");

  // Stated outright as well, because "the two agree" would also be satisfied
  // by both being wrong in the same way - and they were written from different
  // sources precisely so that they are not.
  FakeMemory memory;
  memory.Write(kProgramBase, program);
  BlockDecoder decoder(memory.Fetch());
  const DecodedBlock decoded = decoder.Decode(kProgramBase, 5);
  Emitter emitter;
  CodeBlock* code = emitter.create_block(4096);
  emulation::rec::BlockCompiler compiler = MakeCompiler(&emitter);
  compiler.Compile(decoded, code);

  uint32_t regs[32] = {};
  for (int i = 0; i < 32; ++i)
    regs[i] = initial[i];
  FakeBus bus;
  emulation::rec::BlockState state = MakeState(regs, &bus);
  RunBlock(code, &state);
  CheckEqual(regs[3], 0x11111111, "the delay slot saw the old value");
  CheckEqual(regs[4], 0x55555555, "and the one after it saw the loaded one");
  CheckEqual(regs[2], 0x55555555, "which is also where the load ended up");
  emitter.destroy_block(code);
}

void TestAWriteInTheDelaySlotCancelsTheLoad() {
  printf("an instruction writing the loaded register cancels the load\n");

  uint32_t initial[32] = {};
  initial[1] = kBusBase;
  initial[5] = 0x55555555;

  // The hardware writes the load back first and the instruction's own result
  // second, so the instruction wins and the loaded value never appears
  // (Cpu::WriteReg). The same for a second load to the same register, which
  // discards the first (Cpu::ArmLoad).
  const std::vector<uint32_t> program = {
      SW(5, 1, 0),
      NOP(),
      LW(2, 1, 0),        // r2 would become 55555555
      ADDIU(2, 0, 0x0007),// but this writes r2, and it wins
      ADDU(3, 2, 0),      // so r3 is 7, not 55555555
      NOP(),
  };
  Check(RunBothWays(program, initial, "cancelled load block"), "the two agree");

  const std::vector<uint32_t> two_loads = {
      SW(5, 1, 0),
      NOP(),
      LW(2, 1, 0),        // discarded before it ever lands...
      LW(2, 1, 4),        // ...by this one, to the same register
      ADDU(3, 2, 0),      // still the old r2: neither has landed yet
      ADDU(4, 2, 0),      // now the second load's value
      NOP(),
  };
  Check(RunBothWays(two_loads, initial, "two loads to one register"),
        "the two agree");
}

void TestALoadAcrossABranchLandsInTheDelaySlot() {
  printf("a load before a branch lands in the branch's delay slot\n");

  uint32_t initial[32] = {};
  initial[1] = kBusBase;
  initial[2] = 0x22222222;

  // Two delay slots interleaved: the load's value arrives at the start of the
  // instruction after the branch - which is the branch's delay slot - so the
  // delay slot sees the loaded value and the branch itself does not.
  //
  // The value loaded is zero and the register's old value is not, and the
  // branch tests exactly that: a load that landed an instruction early would
  // send this branch the other way, and the addresses would differ even though
  // every register still agreed.
  const std::vector<uint32_t> program = {
      SW(0, 1, 0),        // memory holds zero
      NOP(),
      LW(2, 1, 0),
      BEQ(2, 0, 4),       // compares the *old* r2, which is not zero
      ADDU(3, 2, 0),      // the delay slot, and by now the load has landed
  };
  Check(RunBothWays(program, initial, "load across a branch"), "the two agree");
}

void TestEveryBranchFormGoesBothWays() {
  printf("every branch form picks the right address, taken and not\n");

  // Each branch is tried with a register that takes it and one that does not,
  // and the address it chose is compared against the reference. The offsets are
  // deliberately different so that a branch computing the right answer from the
  // wrong offset still fails.
  struct Case {
    uint32_t word;
    const char* name;
  };
  const Case cases[] = {
      { BEQ(1, 2, 0x0004), "beq" },
      { BNE(1, 2, 0x0008), "bne" },
      { BLEZ(1, 0x000C), "blez" },
      { BGTZ(1, 0x0010), "bgtz" },
      { BLTZ(1, 0xFFFC), "bltz backwards" },
      { BGEZ(1, 0x0014), "bgez" },
  };

  const uint32_t values[] = { 0, 1, 0xFFFFFFFF, 0x7FFFFFFF, 0x80000000, 2 };

  for (const Case& test : cases) {
    bool all = true;
    for (uint32_t value : values) {
      uint32_t initial[32] = {};
      initial[1] = value;
      initial[2] = 2;
      const std::vector<uint32_t> program = { test.word, ADDIU(3, 3, 1) };
      if (!RunBothWays(program, initial, test.name, false)) {
        printf("  FAIL  %s with r1 = %08X\n", test.name, value);
        all = false;
      }
    }
    Check(all, test.name);
  }
}

void TestEveryJumpFormPicksTheRightAddress() {
  printf("j, jal, jr and jalr each land where they should\n");

  uint32_t initial[32] = {};
  initial[1] = 0x80004000;
  initial[31] = 0xDEADBEEF;

  Check(RunBothWays({ J(0x0200400 >> 2 | 0x2000), ADDIU(3, 3, 1) }, initial, "j"),
        "j agrees");
  Check(RunBothWays({ JAL(0x00002000), ADDIU(3, 3, 1) }, initial, "jal"),
        "jal agrees, link included");
  Check(RunBothWays({ JR(1), ADDIU(3, 3, 1) }, initial, "jr"), "jr agrees");
  Check(RunBothWays({ JALR(31, 1), ADDIU(3, 3, 1) }, initial, "jalr"),
        "jalr agrees");

  // jalr where the link register is also the source: the destination has to be
  // read before the link is written, or the jump goes to its own return
  // address.
  Check(RunBothWays({ JALR(1, 1), ADDIU(3, 3, 1) }, initial, "jalr r1, r1"),
        "jalr into its own source register agrees");
}

void TestABranchWithoutItsDelaySlotIsNotCompiled() {
  printf("an effect that lands on the next instruction needs that instruction\n");

  // A branch whose delay slot is outside the subset: compiling the branch
  // alone would leave the interpreter to resume at a delay slot with a jump
  // already decided and no way to know it.
  FakeMemory memory;
  memory.Write(kProgramBase, {
      ADDIU(1, 0, 1),
      BEQ(1, 0, 4),
      ADD_TRAPPING(2, 1, 1),    // the delay slot, and not compilable
  });
  BlockDecoder decoder(memory.Fetch());
  Emitter emitter;
  CodeBlock* code = emitter.create_block(4096);
  emulation::rec::BlockCompiler compiler = MakeCompiler(&emitter);
  const emulation::rec::CompiledBlock compiled =
      compiler.Compile(decoder.Decode(kProgramBase, 3), code);
  CheckEqual(compiled.compiled, 1, "the branch was left to the interpreter");
  Check(!compiled.ends_with_branch, "so nothing claimed to know where it goes");

  // And the same rule for a load: its value lands on the next instruction, so
  // without one there is nowhere for it to land.
  FakeMemory second;
  second.Write(kProgramBase, {
      ADDIU(1, 0, 1),
      LW(2, 1, 0),
      LWL(3, 1, 0),             // not compilable: it reads a load in flight
  });
  BlockDecoder second_decoder(second.Fetch());
  CodeBlock* second_code = emitter.create_block(4096);
  const emulation::rec::CompiledBlock second_compiled =
      compiler.Compile(second_decoder.Decode(kProgramBase, 3), second_code);
  CheckEqual(second_compiled.compiled, 1, "the load was left to the interpreter");

  emitter.destroy_block(code);
  emitter.destroy_block(second_code);
}

void TestTheAndLinkBranchesAreLeftAlone() {
  printf("the branch-and-link forms are not compiled\n");

  FakeMemory memory;
  memory.Write(kProgramBase, { BGEZAL(1, 4), NOP() });
  BlockDecoder decoder(memory.Fetch());
  Emitter emitter;
  CodeBlock* code = emitter.create_block(4096);
  emulation::rec::BlockCompiler compiler = MakeCompiler(&emitter);
  const emulation::rec::CompiledBlock compiled =
      compiler.Compile(decoder.Decode(kProgramBase, 2), code);
  CheckEqual(compiled.compiled, 0, "bgezal is the interpreter's");
  emitter.destroy_block(code);
}

// The two halves of the calling convention that a toy callback cannot notice
// and a real one cannot survive.
void TestTheCallingConventionIsHonoured() {
  printf("compiled calls are aligned, and callee-saved registers come back\n");

  uint32_t regs[32] = {};
  regs[1] = kBusBase;
  regs[2] = 0x1234ABCD;

  FakeMemory memory;
  memory.Write(kProgramBase, {
      SW(2, 1, 0),
      LW(3, 1, 0),
      NOP(),
      SW(3, 1, 8),
      LW(4, 1, 8),
      NOP(),
  });
  BlockDecoder decoder(memory.Fetch());
  Emitter emitter;
  CodeBlock* code = emitter.create_block(4096);
  emulation::rec::BlockCompiler compiler = MakeCompiler(&emitter);
  const emulation::rec::CompiledBlock compiled =
      compiler.Compile(decoder.Decode(kProgramBase, 6), code);
  CheckEqual(compiled.compiled, 6, "the whole block compiled");

  FakeBus bus;
  emulation::rec::BlockState state = MakeState(regs, &bus);

  // A caller written in emitted code, because C++ gives no way to say "hold
  // this value in RBX across the call". It loads the three registers the block
  // claims to preserve with known values, calls the block, and returns zero
  // only if all three survived.
  //
  //   int check(BlockState* state, void* block)   - state in RCX, block in RDX
  CodeBlock* caller = emitter.create_block(256);
  emitter.set_block(caller);
  namespace x86 = emulation::rec::x86;
  x86::Push(&emitter, 3);   // this caller has to preserve them for C++ too
  x86::Push(&emitter, 6);
  x86::Push(&emitter, 7);
  x86::SubRspImm8(&emitter, 32);
  x86::MovRegImm(&emitter, 3, 0x11111111);   // rbx
  x86::MovRegImm(&emitter, 6, 0x22222222);   // rsi
  x86::MovRegImm(&emitter, 7, 0x33333333);   // rdi
  x86::Mov64RegReg(&emitter, 0, 2);          // mov rax, rdx - the block
  x86::CallReg(&emitter, 0);                 // state is already in rcx
  x86::AluRegImm(&emitter, x86::AluImmOp::kXor, 3, 0x11111111);
  x86::AluRegImm(&emitter, x86::AluImmOp::kXor, 6, 0x22222222);
  x86::AluRegImm(&emitter, x86::AluImmOp::kXor, 7, 0x33333333);
  x86::AluRegReg(&emitter, x86::AluOp::kXor, 0, 0);
  x86::AluRegReg(&emitter, x86::AluOp::kOr, 0, 3);
  x86::AluRegReg(&emitter, x86::AluOp::kOr, 0, 6);
  x86::AluRegReg(&emitter, x86::AluOp::kOr, 0, 7);
  x86::AddRspImm8(&emitter, 32);
  x86::Pop(&emitter, 7);
  x86::Pop(&emitter, 6);
  x86::Pop(&emitter, 3);
  x86::Ret(&emitter);

  typedef int (*CheckFunc)(emulation::rec::BlockState*, void*);
  const int clobbered =
      reinterpret_cast<CheckFunc>(caller->address)(&state, code->address);
  CheckEqual(clobbered, 0, "rbx, rsi and rdi all came back unchanged");

  CheckEqual(regs[4], 0x1234ABCD, "and the block did its work");
  Check(bus.calls == 4, "all four memory operations went through the callbacks");
  Check(bus.alignment_ok, "the stack was aligned at every call");

  emitter.destroy_block(caller);
  emitter.destroy_block(code);
}

// The one invariant the whole design rests on: what Compilable() admits is
// exactly what the emitter handles. If Compilable said yes to something the
// switch quietly ignores, the block would claim to have executed an
// instruction it never emitted - and the interpreter would resume after it.
void TestWhatItClaimsToCompileIsWhatItCompiles() {
  printf("what Compilable admits is exactly what gets emitted\n");

  uint32_t initial[32] = {};
  initial[1] = kBusBase + 64;
  initial[2] = kBusBase + 16;
  initial[3] = 0x89ABCDEF;
  initial[4] = 0x00000004;
  initial[5] = 0x80000000;

  int admitted = 0;
  int mismatches = 0;
  int disagreements = 0;

  std::vector<uint32_t> words;
  for (uint32_t opcode = 0; opcode < 64; ++opcode) {
    if (opcode == 0x00) {
      for (uint32_t funct = 0; funct < 64; ++funct)
        words.push_back((2u << 21) | (3u << 16) | (4u << 11) | (5u << 6) | funct);
      continue;
    }
    if (opcode == 0x01) {
      for (uint32_t rt = 0; rt < 32; ++rt)
        words.push_back((opcode << 26) | (2u << 21) | (rt << 16) | 0x0008);
      continue;
    }
    words.push_back((opcode << 26) | (2u << 21) | (3u << 16) | 0x0008);
  }

  for (uint32_t word : words) {
    const bool admits = emulation::rec::BlockCompiler::Compilable(word);

    FakeMemory memory;
    memory.Write(kProgramBase, { word, ADDIU(6, 6, 1) });
    BlockDecoder decoder(memory.Fetch());
    Emitter emitter;
    CodeBlock* code = emitter.create_block(4096);
    emulation::rec::BlockCompiler compiler = MakeCompiler(&emitter);
    const emulation::rec::CompiledBlock compiled =
        compiler.Compile(decoder.Decode(kProgramBase, 2), code);
    emitter.destroy_block(code);

    // A word the decoder ends the block on - syscall, break, rfe - has no
    // second instruction to compile, and none of those are in the subset
    // anyway.
    const uint32_t expected = admits ? 2u : 0u;
    if (compiled.compiled != expected) {
      ++mismatches;
      if (mismatches <= 3) {
        printf("  FAIL  %08X: Compilable says %s, compiled %u\n", word,
               admits ? "yes" : "no", compiled.compiled);
      }
      continue;
    }
    if (!admits)
      continue;

    ++admitted;
    if (!RunBothWays({ word, ADDIU(6, 6, 1) }, initial, "swept word", false)) {
      ++disagreements;
      if (disagreements <= 3)
        printf("  FAIL  %08X: compiled code disagrees with the interpreter\n", word);
    }
  }

  CheckEqual(mismatches, 0, "every word is compiled if and only if it is admitted");
  CheckEqual(disagreements, 0, "and every admitted word agrees with the interpreter");
  Check(admitted > 30, "and the sweep actually covered the subset");
}

// ---------------------------------------------------------------------------
// Step 5: the engine, and invalidation
// ---------------------------------------------------------------------------

// A whole small machine: memory holding code and data, an interpreter, and -
// optionally - a recompiler wired to both. Running the same program through it
// twice, once interpreted and once recompiled, is the plan's differential
// harness applied to programs rather than to single blocks: loops, fallbacks
// to the interpreter and code that rewrites itself all appear here and none of
// them appear in a one-block test.
class Engine {
 public:
  FakeBus bus;
  Machine machine;

  Engine() { machine.bus = &bus; }

  // Without this the machine is just the interpreter, which is what the
  // reference run wants.
  void AttachRecompiler() {
    emulation::rec::HostInterface host;
    host.context = &bus;
    host.fetch = bus.Fetch();
    host.load32 = &FakeBus::Load32;
    host.load16 = &FakeBus::Load16;
    host.load8 = &FakeBus::Load8;
    host.store32 = &FakeBus::Store32;
    host.store16 = &FakeBus::Store16;
    host.store8 = &FakeBus::Store8;
    host.interpret = [this](uint32_t pc) { return machine.Run(pc); };
    host.load_in_flight = [this]() { return machine.LoadInFlight(); };
    recompiler_.reset(new emulation::rec::Recompiler(host, machine.r));
    recompiler_->set_allocate_registers(g_allocate_registers);
    recompiler_->set_minimum_block_instructions(1);
    recompiler_->set_link_blocks(g_link_blocks);

    // The interpreter's own stores have to invalidate too.
    emulation::rec::Recompiler* rec = recompiler_.get();
    machine.on_store = [rec](uint32_t address) { rec->NoteStore(address); };
  }

  emulation::rec::Recompiler* recompiler() { return recompiler_.get(); }

  // Runs from `entry` until the program jumps to zero, which is how the
  // programs below end (`jr r0`). The cap is a runaway guard.
  int Run(uint32_t entry, int cap = 200000) {
    uint32_t pc = entry;
    int steps = 0;
    while (pc != 0 && steps < cap) {
      pc = recompiler_ ? recompiler_->Step(pc) : machine.Run(pc);
      ++steps;
    }
    return steps;
  }

 private:
  std::unique_ptr<emulation::rec::Recompiler> recompiler_;
};

typedef void (*SeedMemory)(FakeBus*);

// Runs a whole program both ways and compares every register and every byte of
// memory. `stats_out` is how a test asks what the engine actually did -
// whether it compiled anything, whether it fell back, whether it threw a block
// away - since agreeing with the interpreter while compiling nothing at all
// would otherwise look like a pass.
bool RunProgramBothWays(const std::vector<uint32_t>& program, const char* what,
                        emulation::rec::Recompiler::Stats* stats_out = nullptr,
                        SeedMemory seed = nullptr) {
  Engine reference;
  reference.bus.WriteProgram(kProgramBase, program);
  if (seed)
    seed(&reference.bus);
  const int reference_steps = reference.Run(kProgramBase);

  Engine engine;
  engine.bus.WriteProgram(kProgramBase, program);
  if (seed)
    seed(&engine.bus);
  engine.AttachRecompiler();
  const int engine_steps = engine.Run(kProgramBase);

  if (stats_out)
    *stats_out = engine.recompiler()->stats();

  bool same = true;
  if (reference_steps >= 200000 || engine_steps >= 200000) {
    printf("  FAIL  %s: ran away (%d interpreted, %d recompiled steps)\n", what,
           reference_steps, engine_steps);
    same = false;
  }
  for (int i = 0; i < 32 && same; ++i) {
    if (reference.machine.r[i] != engine.machine.r[i]) {
      printf("  FAIL  %s: r%d interpreted %08X, recompiled %08X\n", what, i,
             reference.machine.r[i], engine.machine.r[i]);
      same = false;
    }
  }
  if (same && !reference.bus.SameAs(engine.bus)) {
    printf("  FAIL  %s: memory differs\n", what);
    same = false;
  }

  ++g_checks;
  if (!same)
    ++g_failures;
  return same;
}

void TestAWholeProgramRunsTheSameWayBothWays() {
  printf("a program with a loop runs identically compiled and interpreted\n");

  // Ten times round a loop that accumulates and stores, then one instruction
  // the compiler refuses - `add` traps on overflow - so the run has to fall
  // back to the interpreter in the middle and carry on afterwards.
  const std::vector<uint32_t> program = {
      ADDIU(1, 0, 10),          // 0: r1 = 10, the counter
      ADDIU(2, 0, 0),           // 1: r2 = 0, the sum
      LUI_(3, 0x8000),          // 2:
      ORI_(3, 3, 0x4000),       // 3: r3 = 0x80004000, where to write
      ADDU(2, 2, 1),            // 4: loop: r2 += r1
      SW(2, 3, 0),              // 5: *r3 = r2
      ADDIU(3, 3, 4),           // 6: r3 += 4
      ADDIU(1, 1, 0xFFFF),      // 7: r1 -= 1
      BNE(1, 0, 0xFFFB),        // 8: if r1 != 0 go back to 4
      NOP(),                    // 9: the delay slot
      ADD_TRAPPING(4, 2, 2),    // 10: interpreted, not compiled
      JR(0),                    // 11: jump to zero, which ends the run
      NOP(),                    // 12: the delay slot
  };

  emulation::rec::Recompiler::Stats stats;
  Check(RunProgramBothWays(program, "loop program", &stats), "the two agree");

  // And the engine did the thing it exists to do, rather than quietly
  // interpreting everything and agreeing with itself.
  Check(stats.blocks_compiled > 0, "blocks were compiled");
  Check(stats.instructions_compiled > stats.blocks_executed,
        "each entry into compiled code ran more than one instruction");
  Check(stats.instructions_compiled > 40,
        "most of the work ran as compiled code");
  Check(stats.instructions_interpreted > 0,
        "and the instruction outside the subset fell back to the interpreter");
}

void TestCodeThatRewritesItselfIsNoticed() {
  printf("a store into compiled code discards it, and the new code runs\n");

  // The instruction at index 7 writes 11 to r4. The loop runs it, overwrites it
  // with one that writes 22, and comes back round. Second time through, r4 must
  // be 22 - and it will not be unless the store threw away the block that had
  // already been compiled from those words.
  //
  // The store also lands on the page the *currently executing* block was
  // compiled from, which is the case that would be a use-after-free if
  // invalidation freed host code instead of just unmapping it.
  //
  // The branch at index 5 is what makes this a test of anything. Without it the
  // loop would re-enter at an address no block starts at, so a block would be
  // compiled afresh from the patched words and the right answer would come out
  // whether or not invalidation worked at all. A block has to start exactly
  // where the loop comes back to, or staleness cannot be observed.
  const uint32_t patch_address = kProgramBase + 28;   // index 7
  const uint32_t patched_word = ADDIU(4, 0, 22);

  const std::vector<uint32_t> program = {
      ADDIU(5, 0, 2),                                  // 0: two passes
      LUI_(1, static_cast<uint16_t>(patch_address >> 16)),
      ORI_(1, 1, static_cast<uint16_t>(patch_address & 0xFFFF)),
      LUI_(2, static_cast<uint16_t>(patched_word >> 16)),
      ORI_(2, 2, static_cast<uint16_t>(patched_word & 0xFFFF)),
      BEQ(0, 0, 1),             // 5: to index 7, so a block starts there
      NOP(),                    // 6: the delay slot
      ADDIU(4, 0, 11),          // 7: this is what gets overwritten
      SW(2, 1, 0),              // 8: overwrite it
      ADDIU(5, 5, 0xFFFF),      // 9: one pass down
      BNE(5, 0, 0xFFFC),        // 10: go back to index 7
      NOP(),                    // 11
      JR(0),                    // 12
      NOP(),                    // 13
  };

  emulation::rec::Recompiler::Stats stats;
  Check(RunProgramBothWays(program, "self-modifying program", &stats),
        "the two agree");
  Check(stats.blocks_invalidated > 0, "blocks were thrown away by the store");

  // Stated outright, because "the two agree" would also be satisfied by both
  // running the old instruction twice.
  Engine engine;
  engine.bus.WriteProgram(kProgramBase, program);
  engine.AttachRecompiler();
  engine.Run(kProgramBase);
  CheckEqual(engine.machine.r[4], 22, "the replaced instruction is what ran");
  CheckEqual(engine.bus.ReadWord(patch_address), patched_word,
             "and the replacement is what is in memory");
}

// The program for the test below, and for the failure it is checking against.
// The store that rewrites the code is in the delay slot of `bgezal`, which the
// compiler does not handle - so the interpreter performs both the branch and
// the store, and the compiled path never sees the write at all.
// As in the test above, the branch at index 5 is what puts a block boundary at
// the address the loop comes back to. Without it nothing stale is ever
// re-entered and the test would pass with invalidation switched off entirely.
std::vector<uint32_t> InterpreterStoreProgram(uint32_t patch_address,
                                              uint32_t patched_word) {
  return {
      ADDIU(5, 0, 2),                                  // 0: two passes
      LUI_(1, static_cast<uint16_t>(patch_address >> 16)),
      ORI_(1, 1, static_cast<uint16_t>(patch_address & 0xFFFF)),
      LUI_(2, static_cast<uint16_t>(patched_word >> 16)),
      ORI_(2, 2, static_cast<uint16_t>(patched_word & 0xFFFF)),
      BEQ(0, 0, 1),             // 5: to index 7, so a block starts there
      NOP(),                    // 6: the delay slot
      ADDIU(4, 0, 11),          // 7: this is what gets overwritten
      BGEZAL(0, 1),             // 8: not compiled - it branches and links
      SW(2, 1, 0),              // 9: its delay slot, run by the interpreter
      ADDIU(5, 5, 0xFFFF),      // 10: one pass down
      BNE(5, 0, 0xFFFB),        // 11: back to index 7
      NOP(),                    // 12
      JR(0),                    // 13
      NOP(),                    // 14
  };
}

void TestAStoreFromTheInterpreterInvalidatesToo() {
  printf("a store the interpreter made discards compiled code as well\n");

  // Only some of a program's stores go through compiled code. If invalidation
  // happened only on the compiled path, a game would run code it has already
  // replaced - so the interpreter has to report its stores too. Here it does
  // that through Machine::on_store, which is the hook Cpu::Store will have to
  // grow when this is wired up for real.
  const uint32_t patch_address = kProgramBase + 28;   // index 7
  const uint32_t patched_word = ADDIU(4, 0, 22);
  const std::vector<uint32_t> program =
      InterpreterStoreProgram(patch_address, patched_word);

  emulation::rec::Recompiler::Stats stats;
  Check(RunProgramBothWays(program, "interpreter-store program", &stats),
        "the two agree");
  Check(stats.blocks_invalidated > 0,
        "and the interpreter's store threw a block away");

  Engine engine;
  engine.bus.WriteProgram(kProgramBase, program);
  engine.AttachRecompiler();
  engine.Run(kProgramBase);
  CheckEqual(engine.machine.r[4], 22, "the replaced instruction is what ran");

  // And the same run with nothing reporting the interpreter's stores, which is
  // what makes the check above mean something: it produces the stale answer.
  Engine deaf;
  deaf.bus.WriteProgram(kProgramBase, program);
  deaf.AttachRecompiler();
  deaf.machine.on_store = nullptr;
  deaf.Run(kProgramBase);
  CheckEqual(deaf.machine.r[4], 11,
             "and without that hook it runs the code it has already replaced");
}

void TestTheCacheControlWriteThrowsEverythingAway() {
  printf("the cache-control write empties the cache and releases the arenas\n");

  const std::vector<uint32_t> program = {
      ADDIU(1, 0, 4),
      ADDU(2, 2, 1),
      ADDIU(1, 1, 0xFFFF),
      BNE(1, 0, 0xFFFD),
      NOP(),
      JR(0),
      NOP(),
  };

  Engine engine;
  engine.bus.WriteProgram(kProgramBase, program);
  engine.AttachRecompiler();
  engine.Run(kProgramBase);

  const uint64_t compiled_before = engine.recompiler()->stats().blocks_compiled;
  Check(compiled_before > 0, "something was compiled");
  Check(engine.recompiler()->cache().size() > 0, "and cached");
  Check(engine.recompiler()->arena_count() > 0, "in an arena");

  engine.recompiler()->Reset();
  CheckEqual(static_cast<int64_t>(engine.recompiler()->cache().size()), 0,
             "the cache-control write emptied the cache");
  Check(engine.recompiler()->arena_count() > 0,
        "but the arenas are still there, since a block may have been running");

  // Running again reclaims the memory at the top of the next step - the safe
  // point - and compiles everything afresh.
  engine.machine.r[2] = 0;
  engine.Run(kProgramBase);
  Check(engine.recompiler()->stats().blocks_compiled > compiled_before,
        "and the next run compiled it all over again");
  CheckEqual(engine.machine.r[2], 10, "with the same answer as before");
}

void TestABlockIsNotEnteredWithALoadInFlight() {
  printf("a load left in flight by the interpreter is not lost\n");

  // The branch at index 3 has a load in its delay slot. A load needs somewhere
  // for its value to land, and there is nowhere inside this block, so neither
  // the branch nor the load is compiled - the interpreter runs both, and the
  // loaded value is still on its way when the branch's target comes up.
  //
  // Index 5 must see the old r5 and index 6 the loaded one. If the engine
  // entered a compiled block at index 5, both would see the old value: the
  // block resolved its load delays when it was compiled and cannot be told
  // about this one.
  const std::vector<uint32_t> program = {
      LUI_(1, 0x8000),          // 0:
      ORI_(1, 1, 0x4000),       // 1: r1 = 0x80004000
      ADDIU(5, 0, 7),           // 2: r5 = 7
      BEQ(0, 0, 1),             // 3: always taken, to index 5
      LW(5, 1, 0),              // 4: the delay slot, and a load
      ADDU(6, 5, 0),            // 5: sees the OLD r5
      ADDU(7, 5, 0),            // 6: sees the loaded value
      JR(0),                    // 7
      NOP(),                    // 8
  };

  struct Seed {
    static void Write(FakeBus* bus) { bus->Write(0x80004000, 4, 0x12345678); }
  };

  Check(RunProgramBothWays(program, "load in flight across a block boundary",
                           nullptr, &Seed::Write),
        "the two agree");

  Engine engine;
  engine.bus.WriteProgram(kProgramBase, program);
  Seed::Write(&engine.bus);
  engine.AttachRecompiler();
  engine.Run(kProgramBase);
  CheckEqual(engine.machine.r[6], 7, "the delay of the load was respected");
  CheckEqual(engine.machine.r[7], 0x12345678, "and its value still arrived");
}

void TestAnUncompilableInstructionIsCachedAsOne() {
  printf("an address the compiler can do nothing with is not decoded twice\n");

  // A block starting on an instruction outside the subset compiles to nothing.
  // Caching that fact - as an entry with no code - is what stops the engine
  // decoding and compiling the same thing on every visit round a loop.
  const std::vector<uint32_t> program = {
      ADDIU(1, 0, 3),
      ADD_TRAPPING(2, 2, 1),    // 1: never compiled
      ADDIU(1, 1, 0xFFFF),
      BNE(1, 0, 0xFFFD),        // 3: back to index 1
      NOP(),
      JR(0),
      NOP(),
  };

  Engine engine;
  engine.bus.WriteProgram(kProgramBase, program);
  engine.AttachRecompiler();
  engine.Run(kProgramBase);

  CheckEqual(engine.machine.r[2], 6, "the program ran: 3 + 2 + 1");
  const emulation::rec::Recompiler::Stats& stats = engine.recompiler()->stats();
  Check(stats.instructions_interpreted >= 3,
        "the trapping add was interpreted every time round");
  // Three visits to the same uncompilable address, and the block starting there
  // is compiled - as a marker - exactly once.
  Check(stats.blocks_compiled <= 4,
        "but its address was only ever compiled once");
  Check(engine.recompiler()->cache().Find(kProgramBase + 4) != nullptr,
        "because the answer is cached rather than recomputed");
}

void TestManyBlocksShareOneArena() {
  printf("blocks are packed into one arena rather than a page each\n");

  // A long run of straight-line code becomes many blocks - the decoder caps a
  // block at 64 instructions - and all of them should fit in one arena. A
  // VirtualAlloc per block would spend a 4 KB page on 60 bytes of code.
  std::vector<uint32_t> program;
  for (int i = 0; i < 300; ++i)
    program.push_back(ADDIU(1, 1, 1));
  program.push_back(JR(0));
  program.push_back(NOP());

  Engine engine;
  engine.bus.WriteProgram(kProgramBase, program);
  engine.AttachRecompiler();
  engine.Run(kProgramBase);

  CheckEqual(engine.machine.r[1], 300, "all 300 instructions ran");
  Check(engine.recompiler()->stats().blocks_compiled >= 4,
        "which took several blocks");
  CheckEqual(static_cast<int64_t>(engine.recompiler()->arena_count()), 1,
             "and they all fit in one arena");
}

// ---------------------------------------------------------------------------
// Block linking
// ---------------------------------------------------------------------------

// The loop from the engine tests, small enough to reason about: four times
// round, then out.
std::vector<uint32_t> CountingLoop() {
  return {
      ADDIU(1, 0, 4),           // 0
      ADDU(2, 2, 1),            // 1: loop top
      ADDIU(1, 1, 0xFFFF),      // 2
      BNE(1, 0, 0xFFFD),        // 3: back to index 1
      NOP(),                    // 4
      JR(0),                    // 5
      NOP(),                    // 6
  };
}

void TestALoopStaysInsideCompiledCode() {
  printf("a linked loop runs without returning to the dispatcher\n");

  Engine engine;
  engine.bus.WriteProgram(kProgramBase, CountingLoop());
  engine.AttachRecompiler();
  engine.Run(kProgramBase);

  const emulation::rec::Recompiler::Stats& stats = engine.recompiler()->stats();
  CheckEqual(engine.machine.r[2], 10, "the loop produced 4+3+2+1");
  Check(stats.links_made > 0, "links were made");

  // The point of the whole exercise: one entry into compiled code now buys
  // several blocks' worth of work instead of one.
  Check(stats.instructions_compiled >= stats.blocks_executed * 3,
        "and each dispatch ran several blocks' worth of instructions");

  // And with linking off, the same program dispatches far more often. This is
  // the comparison that says the mechanism is doing anything at all.
  Engine unlinked;
  unlinked.bus.WriteProgram(kProgramBase, CountingLoop());
  unlinked.AttachRecompiler();
  unlinked.recompiler()->set_link_blocks(false);
  unlinked.Run(kProgramBase);
  CheckEqual(unlinked.machine.r[2], 10, "unlinked, it produces the same answer");
  CheckEqual(static_cast<int64_t>(unlinked.recompiler()->stats().links_made), 0,
             "and makes no links");
  Check(unlinked.recompiler()->stats().blocks_executed >
            stats.blocks_executed,
        "and returns to the dispatcher more often");
}

void TestTheBudgetBoundsAChain() {
  printf("the budget stops a chain of linked blocks from running forever\n");

  // Without a budget this program never returns: it is an unconditional loop
  // with no exit, exactly the shape of a game's main loop, and linked blocks
  // would jump between each other with nothing to interrupt them.
  const std::vector<uint32_t> forever = {
      ADDIU(1, 1, 1),           // 0
      BEQ(0, 0, 0xFFFE),        // 1: always taken, back to index 0
      NOP(),                    // 2
  };

  Engine engine;
  engine.bus.WriteProgram(kProgramBase, forever);
  engine.AttachRecompiler();
  engine.recompiler()->set_budget(30);

  uint32_t pc = kProgramBase;
  for (int i = 0; i < 10; ++i)
    pc = engine.recompiler()->Step(pc);

  const emulation::rec::Recompiler::Stats& stats = engine.recompiler()->stats();
  Check(pc != 0, "it is still going, as an endless loop should be");
  CheckEqual(static_cast<int64_t>(stats.blocks_executed), 10,
             "ten entries into compiled code, one per Step");

  // Each entry runs the budget's worth, give or take the block it was inside
  // when the budget ran out - the check is at the end of a block, not the
  // middle of one.
  Check(stats.instructions_compiled >= 10 * 30,
        "each entry ran at least its budget");
  if (stats.instructions_compiled >= 10 * 30 + 10 * 8) {
    printf("  (budget probe: %llu instructions over 10 entries)\n",
           static_cast<unsigned long long>(stats.instructions_compiled));
  }
  Check(stats.instructions_compiled < 10 * 30 + 10 * 8,
        "and overshot by at most one block each time");
  CheckEqual(engine.machine.r[1],
             static_cast<uint32_t>(stats.instructions_compiled / 3),
             "and the guest counted once per trip round the loop");
}

void TestAStoreBreaksTheLinksIntoABlock() {
  printf("a store into a linked block takes the jumps into it apart first\n");

  // This is the failure block linking makes possible and invalidation alone
  // does not catch: block A holds a jump straight into block B's code. When a
  // store replaces B's guest instructions, dropping B from the cache is not
  // enough - A's jump still points at B's host code, which is still there and
  // still perfectly runnable. Nothing crashes. The game just runs the code it
  // replaced.
  const uint32_t patch_address = kProgramBase + 28;   // index 7
  const uint32_t patched_word = ADDIU(4, 0, 22);

  const std::vector<uint32_t> program = {
      ADDIU(5, 0, 2),
      LUI_(1, static_cast<uint16_t>(patch_address >> 16)),
      ORI_(1, 1, static_cast<uint16_t>(patch_address & 0xFFFF)),
      LUI_(2, static_cast<uint16_t>(patched_word >> 16)),
      ORI_(2, 2, static_cast<uint16_t>(patched_word & 0xFFFF)),
      BEQ(0, 0, 1),             // 5: to index 7 - so a block starts there, and
      NOP(),                    // 6:    this block links to it
      ADDIU(4, 0, 11),          // 7: overwritten
      SW(2, 1, 0),              // 8: overwrite it
      ADDIU(5, 5, 0xFFFF),      // 9
      BNE(5, 0, 0xFFFC),        // 10: back to index 7, linking to it again
      NOP(),                    // 11
      JR(0),                    // 12
      NOP(),                    // 13
  };

  Engine engine;
  engine.bus.WriteProgram(kProgramBase, program);
  engine.AttachRecompiler();
  engine.Run(kProgramBase);

  CheckEqual(engine.machine.r[4], 22, "the replaced instruction is what ran");
  Check(engine.recompiler()->stats().links_made > 0, "links were made");
  Check(engine.recompiler()->stats().links_broken > 0,
        "and the store took them apart again");
}

void TestAFaultingAccessStopsTheBlock() {
  printf("an access that raises an exception stops the block where it is\n");

  // The load at index 1 is unaligned, which on this machine is an address
  // error. Everything after it belongs to an execution that is no longer
  // happening - the CPU has vectored elsewhere - so none of it may run. A
  // recompiler that carried on here would produce corruption a long way from
  // the cause, which is why the check is emitted after every access rather
  // than argued about.
  const std::vector<uint32_t> program = {
      ADDIU(3, 0, 5),           // 0: runs
      LW(2, 0, 0x4001),         // 1: unaligned - faults
      ADDIU(4, 0, 7),           // 2: must NOT run
      ADDIU(5, 0, 9),           // 3: must NOT run
      JR(0),                    // 4
      NOP(),                    // 5
  };

  Engine engine;
  engine.bus.WriteProgram(kProgramBase, program);
  engine.AttachRecompiler();
  engine.bus.faults_on_unaligned = engine.recompiler();

  const uint32_t next = engine.recompiler()->Step(kProgramBase);

  CheckEqual(engine.machine.r[3], 5, "the instruction before the fault ran");
  CheckEqual(engine.machine.r[2], 0, "the faulting load delivered nothing");
  CheckEqual(engine.machine.r[4], 0, "the instruction after it did not run");
  CheckEqual(engine.machine.r[5], 0, "nor the one after that");
  Check(next == emulation::rec::Recompiler::kFaulted,
        "and the engine was told the block faulted rather than finished");
  CheckEqual(static_cast<int64_t>(engine.recompiler()->stats().faults), 1,
             "one fault was counted");
}

// Everything that compiles or runs guest code. Called twice - once with step
// 6's register allocator off, once with it on - because the allocator's whole
// risk is that it changes an answer somewhere, and the only convincing way to
// say it does not is to ask every question again.
void RunEverythingThatCompiles() {
  TestCompiledArithmeticMatchesTheInterpreter();
  TestRegisterZeroStaysZero();
  TestItStopsAtWhatItCannotCompile();
  TestNopCompilesToNothing();
  TestEveryRegisterIsAddressedCorrectly();

  TestLoadsAndStoresGoThroughTheCallbacks();
  TestTheValueOfALoadArrivesOneInstructionLate();
  TestAWriteInTheDelaySlotCancelsTheLoad();
  TestALoadAcrossABranchLandsInTheDelaySlot();
  TestEveryBranchFormGoesBothWays();
  TestEveryJumpFormPicksTheRightAddress();
  TestABranchWithoutItsDelaySlotIsNotCompiled();
  TestTheAndLinkBranchesAreLeftAlone();
  TestTheCallingConventionIsHonoured();
  TestWhatItClaimsToCompileIsWhatItCompiles();

  TestAWholeProgramRunsTheSameWayBothWays();
  TestCodeThatRewritesItselfIsNoticed();
  TestAStoreFromTheInterpreterInvalidatesToo();
  TestTheCacheControlWriteThrowsEverythingAway();
  TestABlockIsNotEnteredWithALoadInFlight();
  TestAnUncompilableInstructionIsCachedAsOne();
  TestManyBlocksShareOneArena();
}

// The two switches the last two steps added, in every combination. Linking
// changes what runs between blocks and allocation changes what runs inside
// them, and the pair interacting is exactly the kind of thing neither one's
// own tests would catch.
void RunTheMatrix() {
  static const bool kOff = false;
  static const bool kOn = true;
  const bool settings[4][2] = {
      { kOff, kOff }, { kOff, kOn }, { kOn, kOff }, { kOn, kOn },
  };
  for (const auto& setting : settings) {
    g_link_blocks = setting[0];
    g_allocate_registers = setting[1];
    printf("\n--- linking %s, allocation %s ---\n",
           g_link_blocks ? "on" : "off",
           g_allocate_registers ? "on" : "off");
    RunEverythingThatCompiles();
  }
}

// The allocator has to actually be doing something, or running the suite twice
// proves only that nothing happened twice.
void TestTheAllocatorAllocates() {
  printf("the allocator caches the registers a block leans on\n");

  // Long enough to be worth allocating - rec_bench puts the break-even at
  // around eleven instructions - and all of it working through r1 and r2.
  std::vector<uint32_t> program;
  for (int i = 0; i < 8; ++i) {
    program.push_back(ADDIU(1, 1, 1));
    program.push_back(ADDU(2, 2, 1));
  }
  FakeMemory memory;
  memory.Write(kProgramBase, program);
  BlockDecoder decoder(memory.Fetch());
  const DecodedBlock decoded =
      decoder.Decode(kProgramBase, static_cast<uint32_t>(program.size()));

  Emitter emitter;
  CodeBlock* off_code = emitter.create_block(4096);
  emulation::rec::BlockCompiler off(&emitter);
  off.set_allocate_registers(false);
  const emulation::rec::CompiledBlock without = off.Compile(decoded, off_code);

  CodeBlock* on_code = emitter.create_block(4096);
  emulation::rec::BlockCompiler on(&emitter);
  on.set_allocate_registers(true);
  const emulation::rec::CompiledBlock with = on.Compile(decoded, on_code);

  CheckEqual(without.registers_allocated, 0, "with the allocator off, nothing is cached");
  CheckEqual(with.registers_allocated, 2, "with it on, both busy registers are");

  // Not smaller, and that is the point worth writing down: `mov eax, r12d` is
  // three bytes and so is `mov eax, [rsi+4]`. Allocation does not shrink the
  // code, it removes memory operations - and whether that is worth anything on
  // a machine with store-to-load forwarding is a question for rec_bench, not
  // for a byte count. All that is checked here is that the prologue and
  // epilogue it adds stay bounded.
  Check(with.host_bytes <= without.host_bytes + 40,
        "and what it adds around the block is a fixed, small cost");

  // A register touched once is not worth a host register, a push and a load,
  // however long the block is.
  std::vector<uint32_t> sparse_program;
  for (uint32_t reg = 1; reg <= 16; ++reg)
    sparse_program.push_back(ADDIU(reg, 0, static_cast<uint16_t>(reg)));
  FakeMemory sparse;
  sparse.Write(kProgramBase, sparse_program);
  BlockDecoder sparse_decoder(sparse.Fetch());
  CodeBlock* sparse_code = emitter.create_block(4096);
  const emulation::rec::CompiledBlock sparse_block = on.Compile(
      sparse_decoder.Decode(kProgramBase,
                            static_cast<uint32_t>(sparse_program.size())),
      sparse_code);
  CheckEqual(sparse_block.registers_allocated, 0,
             "a register used once is left in memory");

  // And a block too short to amortise the entry cost allocates nothing at all,
  // whatever its registers look like. This is the measured part of step 6:
  // below about eleven instructions allocation is a loss, so it is not done.
  FakeMemory brief;
  brief.Write(kProgramBase, {
      ADDIU(1, 1, 1), ADDU(2, 2, 1), ADDU(2, 2, 1),
      ADDIU(1, 1, 1), ADDU(2, 2, 1), ADDIU(1, 1, 1),
  });
  BlockDecoder brief_decoder(brief.Fetch());
  CodeBlock* brief_code = emitter.create_block(4096);
  const emulation::rec::CompiledBlock brief_block =
      on.Compile(brief_decoder.Decode(kProgramBase, 6), brief_code);
  CheckEqual(brief_block.registers_allocated, 0,
             "and a short block is left alone however busy its registers are");

  emitter.destroy_block(off_code);
  emitter.destroy_block(on_code);
  emitter.destroy_block(sparse_code);
  emitter.destroy_block(brief_code);
}

}  // namespace

int main() {
  printf("rec_test - emitter, block cache, decoder, compiler, engine\n");
  printf("           (Docs/Recompiler-Plan.md steps 1 to 6)\n\n");

  TestEmitsACallableFunction();
  TestArgumentsArriveWhereTheyShould();
  TestReadsAndWritesThroughAPointer();
  TestExecutableMemoryIsGivenBack();
  TestCacheFindsWhatWasPutIn();
  TestTheThreeViewsOfRamAreOneBlock();
  TestAStoreIntoCodeThrowsItAway();
  TestABlockSpanningTwoPages();
  TestClearThrowsEverythingAway();

  TestStraightLineStopsAtTheCap();
  TestABranchTakesItsDelaySlot();
  TestEveryJumpFormEndsABlock();
  TestSyscallEndsTheBlockWithNoDelaySlot();
  TestReturnFromExceptionEndsTheBlock();
  TestAnOrdinaryCop0MoveIsNotAnRfe();
  TestMultiplyMarksTheBlockAsDynamicallyPriced();
  TestLoadsAndStoresAreToldApart();
  TestUnmappedMemoryEndsTheBlock();
  TestAblockCanStartAtADelaySlot();

  RunTheMatrix();

  printf("\n");
  g_link_blocks = true;
  g_allocate_registers = true;
  TestTheAllocatorAllocates();
  TestALoopStaysInsideCompiledCode();
  TestTheBudgetBoundsAChain();
  TestAStoreBreaksTheLinksIntoABlock();
  TestAFaultingAccessStopsTheBlock();

  printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
