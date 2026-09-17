/*****************************************************************************************************************
* Copyright (c) 2012 Khalid Ali Al-Kooheji                                                                       *
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

// Step 5 of Docs/Recompiler-Plan.md: the engine that ties the pieces together,
// and the invalidation that is the part a recompiler gets wrong.
//
// The loop is: look the address up, compile it if it is not there, run it, and
// hand back the address to continue at. Anything the compiler cannot do is the
// interpreter's, one instruction at a time, and that is not a failure mode -
// it is the design. A block that compiles nine of its ten instructions runs
// nine compiled and one interpreted, and the tenth's address comes back from
// the block itself.
//
// Still nothing from psx/ here. The host supplies memory access, an
// interpreter and a word fetch through HostInterface; the emulator will fill
// those with Cpu's, and `rec_test` fills them with a small machine of its own.
// That is what the plan means by switching the recompiler on at the end rather
// than growing it into the core.
//
// Three things in here are less obvious than the loop, and each exists because
// of a way this goes wrong:
//
//   - **A store can delete the block that is running.** Invalidation removes
//     the cache entry, so the host code it pointed at must stay valid anyway:
//     arenas are never freed while anything might be inside one. A block that
//     overwrites its own page keeps running to its end, which is what the
//     hardware does too - the instructions are already in flight.
//   - **The interpreter's stores have to invalidate as well.** Only some of a
//     program's stores go through compiled code. This is the one hook the
//     emulator itself will have to grow: `NoteStore`, called from Cpu::Store.
//     It cannot be contained in here, and pretending otherwise would leave a
//     game running code it has already replaced.
//   - **A compiled block cannot be entered with a load in flight.** The
//     compiler resolves the load delay slot when it compiles (see
//     block_compiler.h), so a block has no way to be told that the interpreter
//     left a value on its way to a register. When the host says one is in
//     flight, this interprets instead - at most two instructions, and then the
//     pipeline is empty again.

#include "lib/reccore/reccore.h"
#include "rec/block_cache.h"
#include "rec/block_compiler.h"
#include "rec/block_decoder.h"
#include "rec/runtime.h"

#include <cstdint>
#include <functional>
#include <vector>

namespace emulation {
namespace rec {

// What the host machine has to provide. Everything the recompiler cannot do
// for itself, and nothing it can.
struct HostInterface {
  void* context = nullptr;      // handed back to the memory callbacks

  // Reads one aligned guest word for decoding. Returning false ends the block,
  // and an address with nothing there is left to the interpreter to fault on.
  FetchWord fetch;

  Load32Fn load32 = nullptr;
  Load16Fn load16 = nullptr;
  Load8Fn load8 = nullptr;
  Store32Fn store32 = nullptr;
  Store16Fn store16 = nullptr;
  Store8Fn store8 = nullptr;

  // Runs the instruction at `pc` and returns the address of the next one.
  //
  // "The instruction" includes its delay slot when it has one: a branch and
  // the instruction after it are one step, because the slot runs before the
  // branch takes effect. This core's Cpu::ExecuteInstruction already works
  // that way - it runs the slot as a nested execute - so the hook is the
  // interpreter as it stands, not a new mode of it.
  std::function<uint32_t(uint32_t pc)> interpret;

  // Whether a load is still on its way to a register. See the note above: a
  // compiled block cannot be entered while one is. If this is empty the
  // recompiler assumes there never is, which is only true of a host with no
  // load delay at all.
  std::function<bool()> load_in_flight;
};

class Recompiler {
 public:
  struct Stats {
    uint64_t blocks_compiled = 0;
    uint64_t blocks_executed = 0;
    uint64_t instructions_compiled = 0;     // executed as compiled code
    uint64_t instructions_interpreted = 0;  // steps taken by the interpreter
    uint64_t blocks_invalidated = 0;
    uint64_t host_bytes = 0;                // code emitted, cumulative
  };

  // One arena holds many blocks. 256 KB is a few thousand of them, and the
  // page-per-block a VirtualAlloc each would cost is the reason not to.
  static const size_t kArenaBytes = 256 * 1024;

  // Comfortably more than a 64-instruction block can emit. Checked rather than
  // assumed: running off the end of an arena would corrupt the block next to
  // it, and it would do so silently.
  static const size_t kMaxBlockBytes = 16 * 1024;

  Recompiler(const HostInterface& host, uint32_t* regs)
      : host_(host),
        decoder_(host.fetch),
        compiler_(&emitter_) {
    state_.regs = regs;
    state_.context = this;
    state_.load32 = &LoadThunk32;
    state_.load16 = &LoadThunk16;
    state_.load8 = &LoadThunk8;
    state_.store32 = &StoreThunk32;
    state_.store16 = &StoreThunk16;
    state_.store8 = &StoreThunk8;
  }

  ~Recompiler() {
    for (reccore::CodeBlock* arena : arenas_)
      emitter_.destroy_block(arena);
  }

  Recompiler(const Recompiler&) = delete;
  Recompiler& operator=(const Recompiler&) = delete;

  // Runs whatever is at `pc` - a compiled block, or one interpreted
  // instruction - and returns where to continue.
  uint32_t Step(uint32_t pc) {
    if (reclaim_pending_ && executing_ == 0)
      Reclaim();

    // Never enter compiled code with a load still on its way to a register.
    if (host_.load_in_flight && host_.load_in_flight())
      return Interpret(pc);

    const Block* found = cache_.Find(pc);
    if (found == nullptr)
      found = Compile(pc);
    if (found == nullptr || found->code == nullptr)
      return Interpret(pc);

    // By value: running the block can store into its own page, which discards
    // the cache entry this points at. The host code survives that - arenas
    // outlive invalidation - but the Block does not.
    const Block block = *found;

    state_.next_pc = pc + block.guest_bytes;
    ++executing_;
    reinterpret_cast<void (*)(BlockState*)>(block.code)(&state_);
    --executing_;

    ++stats_.blocks_executed;
    stats_.instructions_compiled += block.compiled_instructions;
    return state_.next_pc;
  }

  // Every guest store has to come through here, including the interpreter's.
  // Cheap when it is not a code page, which is almost always.
  void NoteStore(uint32_t address) {
    if (!cache_.IsCodePage(address))
      return;
    stats_.blocks_invalidated += cache_.InvalidatePage(address);
  }

  // The write to the cache-control register at 0xFFFE0130: software saying it
  // has replaced code, which on this machine is usually an overlay arriving off
  // the disc. Everything goes.
  //
  // The memory is not released here - this can be called from a store inside a
  // block that is still running. It is released at the top of the next Step,
  // by which point nothing is inside one.
  void Reset() {
    cache_.Clear();
    reclaim_pending_ = true;
  }

  const Stats& stats() const { return stats_; }
  const BlockCache& cache() const { return cache_; }
  size_t arena_count() const { return arenas_.size(); }

 private:
  uint32_t Interpret(uint32_t pc) {
    ++stats_.instructions_interpreted;
    return host_.interpret(pc);
  }

  // Compiles the block at `pc` and caches it. A block the compiler could do
  // nothing with is still cached - as a marker with no code - so that the next
  // visit does not decode it all over again to reach the same answer.
  const Block* Compile(uint32_t pc) {
    const DecodedBlock decoded = decoder_.Decode(pc);
    if (decoded.empty())
      return nullptr;   // nothing mapped there; the interpreter will fault

    reccore::CodeBlock* arena = ArenaWithRoom();
    const size_t before = arena->cursor;
    const CompiledBlock compiled = compiler_.Compile(decoded, arena);

    Block block;
    block.guest_address = pc;
    block.compiled_instructions = compiled.compiled;
    if (compiled.compiled == 0) {
      // Give the arena its bytes back: a prologue and epilogue were emitted
      // for a block that will never run.
      arena->cursor = before;
      block.code = nullptr;
      block.guest_bytes = 4;
      block.cycles = 1;
    } else {
      block.code = compiled.code;
      // Only the compiled prefix is code this block depends on, so only that
      // much of the page map needs to be watched for stores.
      block.guest_bytes = compiled.compiled * 4;
      block.cycles = compiled.compiled;   // one each; see the timing note below
      ++stats_.blocks_compiled;
      stats_.host_bytes += compiled.host_bytes;
    }

    cache_.Insert(block);
    return cache_.Find(pc);
  }

  reccore::CodeBlock* ArenaWithRoom() {
    if (arenas_.empty() ||
        arenas_.back()->cursor + kMaxBlockBytes > arenas_.back()->size) {
      arenas_.push_back(emitter_.create_block(kArenaBytes));
    }
    return arenas_.back();
  }

  void Reclaim() {
    reclaim_pending_ = false;
    for (reccore::CodeBlock* arena : arenas_)
      emitter_.destroy_block(arena);
    arenas_.clear();
  }

  static Recompiler* Self(void* context) {
    return static_cast<Recompiler*>(context);
  }

  static uint32_t LoadThunk32(void* c, uint32_t a) {
    Recompiler* self = Self(c);
    return self->host_.load32(self->host_.context, a);
  }
  static uint32_t LoadThunk16(void* c, uint32_t a) {
    Recompiler* self = Self(c);
    return self->host_.load16(self->host_.context, a);
  }
  static uint32_t LoadThunk8(void* c, uint32_t a) {
    Recompiler* self = Self(c);
    return self->host_.load8(self->host_.context, a);
  }

  // A store goes to memory first and invalidates second. The other order would
  // discard the block and then let the write that discarded it land, which
  // is the same thing here - but not once a store can fault.
  static void StoreThunk32(void* c, uint32_t a, uint32_t v) {
    Recompiler* self = Self(c);
    self->host_.store32(self->host_.context, a, v);
    self->NoteStore(a);
  }
  static void StoreThunk16(void* c, uint32_t a, uint32_t v) {
    Recompiler* self = Self(c);
    self->host_.store16(self->host_.context, a, v);
    self->NoteStore(a);
  }
  static void StoreThunk8(void* c, uint32_t a, uint32_t v) {
    Recompiler* self = Self(c);
    self->host_.store8(self->host_.context, a, v);
    self->NoteStore(a);
  }

  HostInterface host_;
  reccore::Emitter emitter_;
  BlockDecoder decoder_;
  BlockCompiler compiler_;
  BlockCache cache_;
  BlockState state_;
  std::vector<reccore::CodeBlock*> arenas_;
  Stats stats_;
  int executing_ = 0;
  bool reclaim_pending_ = false;
};

}  // namespace rec
}  // namespace emulation
