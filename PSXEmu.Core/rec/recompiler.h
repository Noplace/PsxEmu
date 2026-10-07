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
//
// Step 7 added block linking, and two more of the same kind:
//
//   - **A link into a block that has been thrown away is worse than a stale
//     block.** The cache entry goes, but the host code it pointed at is still
//     there and still runnable, so a jump straight into it does not crash -
//     it quietly runs the guest instructions that have just been replaced.
//     `incoming_` is indexed by target for exactly this: when a block is about
//     to go, every jump into it is sent back to its own block's `ret` first.
//   - **A chain of linked blocks would never come back.** A guest loop living
//     entirely in compiled code would jump around inside itself forever, and
//     the emulator would never take an interrupt or draw a frame again. Every
//     block charges its length to `BlockState::budget` on the way out and
//     returns when it runs out; the host sets the budget to however long it
//     can afford not to hear from the CPU.
//
// And loads that read RAM themselves (HostInterface::ram_bytes) one more:
//
//   - **Whether they may is decided per step, not per compile.** What makes a
//     RAM load more than a read and a stall - in the emulator PGXP, a debugger
//     watchpoint, the write queue, an isolated cache - can change between any
//     two steps. Each load checks `BlockState::ram` as it runs and calls out
//     when it is null, so switching costs nothing; recompiling on every change
//     would cost more than the reads save.

#include "rec/emitter.h"
#include "rec/block_cache.h"
#include "rec/block_compiler.h"
#include "rec/block_decoder.h"
#include "rec/runtime.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <unordered_map>
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

  // Told of every register copy compiled code makes, while set_track_moves is on.
  MoveFn move = nullptr;

  // Main RAM, for loads compiled to read it directly: the first `ram_bytes` of
  // physical memory, reached through KUSEG, KSEG0 or KSEG1, and what reading it
  // costs beyond the instruction's own cycle - the stall load32/16/8 would have
  // charged. Which memory, and whether it may be read at all right now, is
  // set_ram's. Zero bytes compiles every load as a call, as before.
  uint32_t ram_bytes = 0;
  uint8_t ram_read_cycles = 0;

  // How far RAM's mirrors extend, when it is mirrored: the physical addresses below this
  // are the same `ram_bytes` over and over (the console's 2 MB across its first 8 MB), and
  // compiled loads and stores reach them directly too. Zero, the default, means no mirrors.
  // `ram_bytes` has to be a power of two for this to mean anything.
  uint32_t ram_window_bytes = 0;

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
  // The same question as a plain function, handed `context`, which the recompiler asks on every
  // step and so prefers when it is set: a std::function is a call through a thunk to a lambda.
  bool (*load_in_flight_fn)(void* context) = nullptr;
};

class Recompiler {
 public:
  struct Stats {
    uint64_t blocks_compiled = 0;
    uint64_t blocks_executed = 0;           // entries into compiled code
    uint64_t instructions_compiled = 0;     // executed as compiled code
    uint64_t instructions_interpreted = 0;  // steps taken by the interpreter
    uint64_t blocks_invalidated = 0;
    uint64_t host_bytes = 0;                // code emitted, cumulative
    uint64_t links_made = 0;
    uint64_t links_broken = 0;
    uint64_t blocks_with_allocation = 0;   // blocks that cached any register
    uint64_t faults = 0;                   // accesses that raised an exception
    uint64_t cycles_compiled = 0;          // what compiled code owes the machine
    uint64_t ram_reads_direct = 0;         // loads that read RAM without a call

    // `blocks_executed` counts entries into compiled code, not blocks run: a
    // chain of linked blocks is one entry. So instructions_compiled divided by
    // blocks_executed is how much work each dispatch buys, which is the number
    // linking exists to raise.
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
    state_.move = &MoveThunk;
    compiler_.set_direct_ram(host.ram_bytes, host.ram_read_cycles, host.ram_window_bytes);
    state_.code_pages = cache_.code_pages();
  }

  ~Recompiler() {
    for (CodeBlock* arena : arenas_)
      emitter_.destroy_block(arena);
  }

  Recompiler(const Recompiler&) = delete;
  Recompiler& operator=(const Recompiler&) = delete;

  // Runs whatever is at `pc` - a compiled block, or one interpreted
  // instruction - and returns where to continue.
  uint32_t Step(uint32_t pc) {
    // Cleared first, so that a step which interprets rather than running a
    // block does not leave the previous chain's cycles lying around for the
    // host to charge a second time. An interpreted instruction ticks the
    // machine itself; it owes nothing here.
    last_cycles_ = 0;

    if (reclaim_pending_ && executing_ == 0)
      Reclaim();

    // Never enter compiled code with a load still on its way to a register.
    if (host_.load_in_flight_fn != nullptr ? host_.load_in_flight_fn(host_.context)
                                           : (host_.load_in_flight && host_.load_in_flight()))
      return Interpret(pc);

    const Block* found = cache_.Find(pc);
    if (found == nullptr)
      found = Compile(pc);
    if (found == nullptr || found->code == nullptr)
      return Interpret(pc);

    // What is needed of it, taken now: running the block can store into its own
    // page, which discards the cache entry this points at. The host code survives
    // that - arenas outlive invalidation - but the Block does not.
    void* const code = found->code;
    const uint32_t guest_bytes = found->guest_bytes;

    // What comes back may be several blocks later: a block linked to the next
    // jumps straight to it rather than returning here. The budget is what
    // bounds that, and what is left of it is how much ran.
    state_.next_pc = pc + guest_bytes;
    state_.budget = budget_;
    state_.fault = 0;
    state_.extra_cycles = 0;
    ++executing_;
    reinterpret_cast<void (*)(BlockState*)>(code)(&state_);
    --executing_;

    ++stats_.blocks_executed;
    stats_.instructions_compiled +=
        static_cast<uint64_t>(budget_ - state_.budget);
    // One cycle an instruction. Not because that is exactly what the
    // interpreter charges - see block_decoder.h - but because it is what the
    // measurements say reproduces its pacing, and a model nobody has measured
    // is worse than a flat one everybody can see. On top of that, the stall of
    // every load that read RAM directly: through the callback it would have
    // charged that itself.
    last_cycles_ = static_cast<uint32_t>(budget_ - state_.budget) + state_.extra_cycles;
    stats_.cycles_compiled += last_cycles_;
    stall_cycles_direct_ += state_.extra_cycles;   // divided into reads by stats(), not here

    // A memory access raised a guest exception and the block stopped where it
    // was. Whoever raised it has already moved the CPU's pc, so there is
    // nothing here to say about where to go next - the caller asks the machine.
    if (state_.fault != 0) {
      ++stats_.faults;
      return kFaulted;
    }
    return state_.next_pc;
  }

  // What Step returns when compiled code stopped because the host raised an
  // exception. Not an address: the host's own pc is the answer.
  static const uint32_t kFaulted = 0xFFFFFFFFu;

  // Called by a load or store callback that has just raised a guest exception.
  // The block stops at that instruction and runs nothing after it; Step then
  // returns kFaulted, and where execution goes next is the machine's own pc.
  void SetFault() { state_.fault = 1; }

  // Everything that writes guest memory has to come through here - not just
  // the CPU's stores.
  //
  // A DMA writes RAM directly, without going anywhere near Cpu::Store, and on
  // this machine that is how a game loads an overlay: the CD channel drops new
  // code into RAM and jumps to it. Compiled code built from whatever was there
  // before has to go, and nothing else in the system would have said so.
  //
  // Only the blocks compiled from the bytes written go - not the rest of the
  // page, which on this machine is as often data as code (bug 140).
  void NoteStoreRange(uint32_t address, uint32_t bytes) {
    if (!cache_.RangeTouchesCode(address, bytes))
      return;

    // Which blocks are about to go, so the jumps into them can be taken apart
    // first. A link left pointing at a block whose guest words have changed is
    // the worst failure this design can produce: the code is still there and
    // still runnable, so nothing crashes - it just quietly runs what the game
    // has already replaced.
    going_.clear();
    cache_.CollectWritten(address, bytes, &going_);
    for (const uint32_t target : going_) {
      BreakLinksTo(target);
      DropLinksFrom(target);
      cache_.Remove(target);
    }
    stats_.blocks_invalidated += going_.size();
  }

  // Every guest store has to come through here, including the interpreter's.
  // Cheap when it is not a code page, which is almost always.
  void NoteStore(uint32_t address) { NoteStoreRange(address & ~3u, 4); }

  // The write to the cache-control register at 0xFFFE0130: software saying it
  // has replaced code, which on this machine is usually an overlay arriving off
  // the disc. Everything goes.
  //
  // The memory is not released here - this can be called from a store inside a
  // block that is still running. It is released at the top of the next Step,
  // by which point nothing is inside one.
  void Reset() {
    cache_.Clear();
    incoming_.clear();   // the code those links live in is about to be released
    outgoing_.clear();
    reclaim_pending_ = true;
  }

  // The budget a single entry into compiled code is given, in guest
  // instructions. In the emulator this would be the cycles until the next
  // scheduled event; here it is simply how long a chain of linked blocks may
  // run before the dispatcher gets a look in.
  void set_budget(int32_t instructions) { budget_ = instructions; }
  int32_t budget() const { return budget_; }

  void set_link_blocks(bool on) {
    link_blocks_ = on;
    compiler_.set_link_blocks(on);
  }

  // Step 6's allocator, for the A/B. Compiled blocks already in the cache keep
  // whatever they were compiled with, so flip this before anything runs.
  void set_allocate_registers(bool on) { compiler_.set_allocate_registers(on); }

  // Whether compiled register copies call HostInterface::move - the emulator's
  // PGXP, while it is on. Blocks carry the choice they were compiled with, so a
  // change throws them all away, as software replacing its code does.
  void set_track_moves(bool on) {
    if (on == track_moves_)
      return;
    track_moves_ = on;
    compiler_.set_track_moves(on);
    Reset();
  }

  // The RAM compiled loads read directly (HostInterface::ram_bytes), or nullptr
  // for every load to go through the callbacks. Read by the loads themselves as
  // they run, so the host can change it before any Step and nothing has to be
  // compiled again - the emulator does, whenever something makes a RAM load
  // more than a read and a stall (psx/recompiler_bridge.h).
  void set_ram(uint8_t* ram) { state_.ram = ram; }

  // The same for stores: the RAM compiled stores write directly (to a page without compiled
  // code in it), or nullptr for every store to call out. Separate from set_ram because a store
  // has more to be plain about - the emulator withholds it for a watched address as well.
  void set_ram_store(uint8_t* ram) { state_.ram_store = ram; }

  // For tests: see BlockCompiler::set_minimum_block_instructions.
  void set_minimum_block_instructions(uint32_t instructions) {
    compiler_.set_minimum_block_instructions(instructions);
  }

  // Cycles charged by the most recent Step, for a host that has to hand them
  // on to the rest of the machine.
  uint32_t last_cycles() const { return last_cycles_; }

  const Stats& stats() const {
    // Every direct read owed the same stall, so the reads are the stalls divided by it - worked
    // out here, since Step runs far more often than anyone asks.
    stats_.ram_reads_direct =
        host_.ram_read_cycles != 0 ? stall_cycles_direct_ / host_.ram_read_cycles : 0;
    return stats_;
  }
  const BlockCache& cache() const { return cache_; }
  size_t arena_count() const { return arenas_.size(); }

  // How many jumps between blocks are being kept track of. Each live block's
  // own, and no more: a block thrown away takes its jumps with it.
  size_t links_held() const {
    size_t held = 0;
    for (const auto& entry : incoming_)
      held += entry.second.size();
    return held;
  }

 private:
  // One patchable jump at the end of one block, and where it goes when it is
  // not pointing at anything.
  struct Link {
    uint32_t owner = 0;          // the guest address of the block it lives in
    uint8_t* site = nullptr;     // the rel32 itself
    uint8_t* after = nullptr;    // the instruction after it, which rel32 is from
    int32_t unlinked = 0;
  };

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

    CodeBlock* arena = ArenaWithRoom();
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
      if (compiled.registers_allocated > 0)
        ++stats_.blocks_with_allocation;
    }

    cache_.Insert(block);

    if (block.code != nullptr && link_blocks_) {
      RecordLinks(pc, compiled);
      // This block may be what some earlier block has been waiting to jump to.
      Relink(pc);
    }
    return cache_.Find(pc);
  }

  // Remember every slot this block has, indexed by where it wants to go, and
  // point the ones whose destination already exists straight at it.
  //
  // Both ends are kept by the block cache's own key - the address normalised,
  // as Find takes it - since that is what a store reports going, and a jump
  // to 8000101C has to be found when the block at 0000101C goes.
  void RecordLinks(uint32_t pc, const CompiledBlock& compiled) {
    const uint32_t owner = BlockCache::Normalise(pc);
    DropLinksFrom(owner);   // anything left over from a block that was here before
    uint8_t* const code = static_cast<uint8_t*>(compiled.code);
    for (int i = 0; i < compiled.link_count; ++i) {
      const CompiledBlock::LinkSlot& slot = compiled.links[i];
      const uint32_t target = BlockCache::Normalise(slot.target);
      Link link;
      link.owner = owner;
      link.site = code + slot.site;
      link.after = code + slot.after;
      link.unlinked = slot.unlinked;
      incoming_[target].push_back(link);
      outgoing_[owner].push_back(target);
      Point(link, target);
    }
  }

  // Forget the jumps out of a block that is going. Its host code stays where it
  // is until the next Reset, but nothing will enter it again, so its slots need
  // no repointing - and keeping them did harm: every recompile of a block added
  // its slots again beside the dead ones, and each later break or relink of the
  // target walked them all. In Final Fantasy VII's battle, whose blocks were
  // recompiled every frame (bug 140), that came to 71,000 slots rewritten per
  // store, on average.
  void DropLinksFrom(uint32_t address) {
    const uint32_t owner = BlockCache::Normalise(address);
    const auto out = outgoing_.find(owner);
    if (out == outgoing_.end())
      return;
    for (const uint32_t target : out->second) {
      const auto in = incoming_.find(target);
      if (in == incoming_.end())
        continue;
      std::vector<Link>& links = in->second;
      links.erase(std::remove_if(links.begin(), links.end(),
                                 [owner](const Link& link) { return link.owner == owner; }),
                  links.end());
      if (links.empty())
        incoming_.erase(in);
    }
    outgoing_.erase(out);
  }

  // Point every slot that wants this address at the block now sitting there.
  void Relink(uint32_t target) {
    const auto it = incoming_.find(BlockCache::Normalise(target));
    if (it == incoming_.end())
      return;
    for (const Link& link : it->second)
      Point(link, target);
  }

  void Point(const Link& link, uint32_t target) {
    const Block* block = cache_.Find(target);
    if (block == nullptr || block->code == nullptr)
      return;   // nothing there yet, or nothing but a marker to interpret

    // A rel32 jump reaches 2 GB. Two arenas in one process are almost always
    // far closer than that, but "almost always" is not a thing to encode into
    // a jump, so a link that would not reach is simply not made.
    const intptr_t delta = static_cast<uint8_t*>(block->code) - link.after;
    if (delta > INT32_MAX || delta < INT32_MIN)
      return;

    const int32_t displacement = static_cast<int32_t>(delta);
    memcpy(link.site, &displacement, sizeof(displacement));
    ++stats_.links_made;
  }

  // Send every jump into this address back to its own block's `ret`. Called
  // just before the block there is thrown away.
  void BreakLinksTo(uint32_t target) {
    const auto it = incoming_.find(BlockCache::Normalise(target));
    if (it == incoming_.end())
      return;
    for (const Link& link : it->second) {
      memcpy(link.site, &link.unlinked, sizeof(link.unlinked));
      ++stats_.links_broken;
    }
  }

  CodeBlock* ArenaWithRoom() {
    if (arenas_.empty() ||
        arenas_.back()->cursor + kMaxBlockBytes > arenas_.back()->size) {
      arenas_.push_back(emitter_.create_block(kArenaBytes));
    }
    return arenas_.back();
  }

  void Reclaim() {
    reclaim_pending_ = false;
    for (CodeBlock* arena : arenas_)
      emitter_.destroy_block(arena);
    arenas_.clear();
  }

  static Recompiler* Self(void* context) {
    return static_cast<Recompiler*>(context);
  }

  static void MoveThunk(void* c, uint32_t to, uint32_t from) {
    Recompiler* self = Self(c);
    if (self->host_.move != nullptr)
      self->host_.move(self->host_.context, to, from);
  }
  static uint32_t LoadThunk32(void* c, uint32_t a, uint32_t pc) {
    Recompiler* self = Self(c);
    return self->host_.load32(self->host_.context, a, pc);
  }
  static uint32_t LoadThunk16(void* c, uint32_t a, uint32_t pc) {
    Recompiler* self = Self(c);
    return self->host_.load16(self->host_.context, a, pc);
  }
  static uint32_t LoadThunk8(void* c, uint32_t a, uint32_t pc) {
    Recompiler* self = Self(c);
    return self->host_.load8(self->host_.context, a, pc);
  }

  // A store goes to memory first and invalidates second. The other order would
  // discard the block and then let the write that discarded it land, which
  // is the same thing here - but not once a store can fault.
  static void StoreThunk32(void* c, uint32_t a, uint32_t v, uint32_t pc) {
    Recompiler* self = Self(c);
    self->host_.store32(self->host_.context, a, v, pc);
    self->NoteStore(a);
  }
  static void StoreThunk16(void* c, uint32_t a, uint32_t v, uint32_t pc) {
    Recompiler* self = Self(c);
    self->host_.store16(self->host_.context, a, v, pc);
    self->NoteStore(a);
  }
  static void StoreThunk8(void* c, uint32_t a, uint32_t v, uint32_t pc) {
    Recompiler* self = Self(c);
    self->host_.store8(self->host_.context, a, v, pc);
    self->NoteStore(a);
  }

  HostInterface host_;
  Emitter emitter_;
  BlockDecoder decoder_;
  BlockCompiler compiler_;
  BlockCache cache_;
  BlockState state_;
  std::vector<CodeBlock*> arenas_;
  mutable Stats stats_;
  uint64_t stall_cycles_direct_ = 0;
  int executing_ = 0;
  bool reclaim_pending_ = false;

  // Jumps indexed by the guest address they want to reach, which is the
  // direction invalidation needs: "this block is going, who points at it?"
  std::unordered_map<uint32_t, std::vector<Link>> incoming_;
  // And the other way: each block's targets, so its slots can be found again
  // in `incoming_` when it goes.
  std::unordered_map<uint32_t, std::vector<uint32_t>> outgoing_;
  // Scratch for NoteStoreRange, kept to save an allocation per store into code.
  std::vector<uint32_t> going_;
  bool link_blocks_ = true;
  bool track_moves_ = false;

  // Long enough that a hot loop stays inside compiled code, short enough that
  // the host still hears from the CPU promptly.
  int32_t budget_ = 1024;
  uint32_t last_cycles_ = 0;
};

}  // namespace rec
}  // namespace emulation
