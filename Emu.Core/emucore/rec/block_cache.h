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

// Where compiled blocks live and how they are thrown away again.
//
// Deliberately knows nothing about the R3000A, the interpreter or anything in
// psx/ - it maps a guest address to a pointer and a cycle count, and tracks
// which pages of guest memory a block was compiled from so a store into one
// can discard it. That is the whole of it, and keeping it that way is what
// lets this be tested on its own long before there is a compiler to fill it.
//
// See Docs/Recompiler-Plan.md.

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace emulation {
namespace rec {

// One compiled run of guest instructions: where it starts, how much guest code
// it covers, what it costs, and where its host code is.
struct Block {
  uint32_t guest_address = 0;   // physical, masked - see BlockCache::Normalise
  uint32_t guest_bytes = 0;     // how much guest code it was compiled from
  uint32_t cycles = 0;          // charged in one lump when the block finishes
  void* code = nullptr;         // host code, owned by whoever emitted it

  // The interpreter is always the fallback, so a block that could not be fully
  // compiled is still worth caching: it records where the compiled part stops.
  uint32_t compiled_instructions = 0;

  // Words outside the block's own range that what was compiled depends on: the first instruction at
  // each place the block goes next, when a load in its delay slot was written out at the end of the
  // block on the strength of what those instructions are. A store into one throws the block away,
  // exactly as a store into the block's own code does. Addresses in any view; Insert normalises them.
  static const int kMaxWatched = 2;
  uint32_t watched[kMaxWatched] = {};
  int watched_count = 0;
};

// A guest address to Block map, and what a store has to throw away.
//
// Invalidation is the part that goes wrong in a recompiler, so it is the part
// that exists first. Two things throw a block away on this machine:
//
//   - a store into a word a block was compiled from, which Cpu::Store checks
//     against `IsCodePage` first and `CollectWritten` second; and
//   - a write to the cache-control register at 0xFFFE0130, which is how
//     software tells the hardware it has replaced code - overlays, which is
//     the common case on the PSX - and which maps to Clear().
//
// The word, not the page. PSX games keep data beside their code - a counter or
// a flag in the same 4 KB as the routine that reads it - and Final Fantasy
// VII's battle writes one such word about sixteen times a frame: discarding
// the page each time recompiled thirty-odd blocks for every write, and with
// the whole cache walked per discard the recompiler ran slower than the
// interpreter (bug 140). The page bitmap stays as the cheap first question
// every store asks; only a store into a page that has code goes on to ask
// about the word.
//
// And each page keeps its own list of the blocks compiled from it, so
// throwing some away looks at those and not at every block there is.
class BlockCache {
 public:
  // 4 KB, which is the granularity the page bitmap below tracks. Small enough
  // that a game rewriting one buffer does not discard the code around it,
  // large enough that the bitmap stays cheap.
  static const uint32_t kPageShift = 12;
  static const uint32_t kPageSize = 1u << kPageShift;

  // RAM is 2 MB and mirrored; the BIOS is 512 KB. Blocks are keyed by the
  // physical address so the three KUSEG/KSEG0/KSEG1 views of one instruction
  // are one block rather than three.
  static uint32_t Normalise(uint32_t address) { return address & 0x1FFFFFFF; }

  void Insert(const Block& block) {
    const uint32_t key = Normalise(block.guest_address);
    Remove(key);   // a block replaced in place leaves nothing of itself behind
    blocks_[key] = block;
    blocks_[key].guest_address = key;

    const uint32_t end = End(blocks_[key]);
    for (uint32_t page = key >> kPageShift; page <= (end - 1) >> kPageShift; ++page) {
      Page& entry = pages_[page];
      entry.blocks.push_back(key);
      MarkWords(&entry, page, key, end);
      MarkCodePage(page);
    }
    // The words it depends on, in whatever pages they are in, which may be ones it has no code in.
    Block& stored = blocks_[key];
    for (int i = 0; i < stored.watched_count; ++i) {
      stored.watched[i] = Normalise(stored.watched[i]);
      const uint32_t page = stored.watched[i] >> kPageShift;
      Page& entry = pages_[page];
      if (std::find(entry.blocks.begin(), entry.blocks.end(), key) == entry.blocks.end())
        entry.blocks.push_back(key);
      MarkWords(&entry, page, stored.watched[i], stored.watched[i] + 4);
      MarkCodePage(page);
    }
  }

  // Takes the block starting at this address out, if there is one.
  bool Remove(uint32_t address) {
    const uint32_t key = Normalise(address);
    const auto it = blocks_.find(key);
    if (it == blocks_.end())
      return false;
    const Block removed = it->second;
    const uint32_t end = End(removed);
    FastSlot& slot = fast_[(key >> 2) & (kFastSlots - 1)];
    if (slot.key == key)
      slot = FastSlot();
    blocks_.erase(it);
    for (uint32_t page = key >> kPageShift; page <= (end - 1) >> kPageShift; ++page)
      ForgetInPage(page, key);
    for (int i = 0; i < removed.watched_count; ++i)
      ForgetInPage(removed.watched[i] >> kPageShift, key);   // a page already done is a no-op
    return true;
  }

  // The blocks compiled from any of these bytes, appended to `out` by their
  // guest address. A store into code asks this before taking them out, since
  // the jumps into them have to be taken apart first.
  void CollectWritten(uint32_t address, uint32_t bytes,
                      std::vector<uint32_t>* out) const {
    if (bytes == 0)
      return;
    const uint32_t begin = Normalise(address);
    const uint32_t end = begin + bytes;
    for (uint32_t page = begin >> kPageShift; page <= (end - 1) >> kPageShift; ++page) {
      if (!IsCodePage(page << kPageShift))
        continue;
      const auto found = pages_.find(page);
      if (found == pages_.end() || !AnyWordMarked(found->second, page, begin, end))
        continue;
      for (const uint32_t key : found->second.blocks) {
        const Block& block = blocks_.at(key);
        bool hit = key < end && begin < End(block);
        for (int i = 0; i < block.watched_count && !hit; ++i)
          hit = block.watched[i] < end && begin < block.watched[i] + 4;
        if (hit && std::find(out->begin(), out->end(), key) == out->end())
          out->push_back(key);
      }
    }
  }

  // Throws away every block compiled from any of these bytes, and nothing
  // else. Returns how many went.
  uint32_t InvalidateRange(uint32_t address, uint32_t bytes) {
    std::vector<uint32_t> going;
    CollectWritten(address, bytes, &going);
    for (const uint32_t key : going)
      Remove(key);
    return static_cast<uint32_t>(going.size());
  }

  // The block starting exactly at this address, or nullptr. A block is only
  // ever entered at its first instruction: jumping into the middle of one
  // compiles a new block from there, which is correct and costs a little
  // duplicated code.
  //
  // Almost every lookup is for a block the last few thousand lookups have already asked about -
  // a game spends its time in loops - so a small table indexed by the address's low bits answers
  // those without hashing: a hit is one compare. A miss goes to the map and fills the slot. A
  // block's address in the map never moves while it is there (node-based), and Remove and Clear
  // empty any slot that points at what they take out.
  const Block* Find(uint32_t address) const {
    const uint32_t key = Normalise(address);
    FastSlot& slot = fast_[(key >> 2) & (kFastSlots - 1)];
    if (slot.key == key)
      return slot.block;
    const auto it = blocks_.find(key);
    if (it == blocks_.end())
      return nullptr;
    slot.key = key;
    slot.block = &it->second;
    return slot.block;
  }

  // Whether any block was compiled from any page this range covers.
  //
  // A DMA moves thousands of words at a time and asking about each one is far
  // too slow, so a bulk transfer asks once about the whole range. Almost every
  // such range is data - an ordering table, a sound buffer - and answers no
  // after a handful of bitmap lookups.
  bool RangeTouchesCode(uint32_t address, uint32_t bytes) const {
    if (bytes == 0)
      return false;
    const uint32_t first = Normalise(address) >> kPageShift;
    const uint32_t last = Normalise(address + bytes - 1) >> kPageShift;
    for (uint32_t page = first; page <= last; ++page) {
      if (IsCodePage(page << kPageShift))
        return true;
    }
    return false;
  }

  // Whether any block was compiled from this page - the one question the store
  // path has to answer on every write, so it is a bitmap lookup and nothing
  // more.
  bool IsCodePage(uint32_t address) const {
    const uint32_t page = Normalise(address) >> kPageShift;
    const size_t word = page >> 6;
    return word < code_pages_.size() &&
           (code_pages_[word] & (1ull << (page & 63))) != 0;
  }

  // Throws away every block compiled from the page this address falls in.
  // Returns how many went, which is what a stats line wants.
  uint32_t InvalidatePage(uint32_t address) {
    const uint32_t page = Normalise(address) >> kPageShift;
    const auto found = pages_.find(page);
    if (found == pages_.end())
      return 0;
    // A copy: taking each block out edits this page's list.
    const std::vector<uint32_t> going = found->second.blocks;
    for (const uint32_t key : going)
      Remove(key);
    return static_cast<uint32_t>(going.size());
  }

  void Clear() {
    blocks_.clear();
    pages_.clear();
    std::fill(code_pages_.begin(), code_pages_.end(), 0ull);
    for (FastSlot& slot : fast_)
      slot = FastSlot();
  }

  // The page bitmap itself, for compiled stores to test as they run. It covers every page of
  // the 512 MB physical space from the start and is never resized, so the pointer holds for the
  // life of the cache; bit n is page n, as IsCodePage reads it.
  const uint64_t* code_pages() const { return code_pages_.data(); }

  size_t size() const { return blocks_.size(); }

 private:
  static const uint32_t kWordsPerPage = kPageSize / 4;

  // One page with code in it: the blocks compiled from it, and which of its
  // words those blocks were compiled from.
  struct Page {
    std::vector<uint32_t> blocks;
    uint64_t words[kWordsPerPage / 64] = {};
  };

  // Where a block's guest code ends, exclusive. A block of no bytes is
  // treated as its first word, so it still has a page to be found in.
  static uint32_t End(const Block& block) {
    return block.guest_address + (block.guest_bytes == 0 ? 4 : block.guest_bytes);
  }

  // The part of [begin, end) inside this page, as word indices within it.
  static void WordsInPage(uint32_t page, uint32_t begin, uint32_t end,
                          uint32_t* first, uint32_t* last) {
    const uint32_t page_begin = page << kPageShift;
    const uint32_t from = begin > page_begin ? begin : page_begin;
    const uint32_t to = end < page_begin + kPageSize ? end : page_begin + kPageSize;
    *first = (from - page_begin) >> 2;
    *last = (to - 1 - page_begin) >> 2;
  }

  static void MarkWords(Page* entry, uint32_t page, uint32_t begin, uint32_t end) {
    uint32_t first, last;
    WordsInPage(page, begin, end, &first, &last);
    for (uint32_t w = first; w <= last; ++w)
      entry->words[w >> 6] |= 1ull << (w & 63);
  }

  static bool AnyWordMarked(const Page& entry, uint32_t page, uint32_t begin,
                            uint32_t end) {
    uint32_t first, last;
    WordsInPage(page, begin, end, &first, &last);
    for (uint32_t w = first; w <= last; ++w) {
      if (entry.words[w >> 6] & (1ull << (w & 63)))
        return true;
    }
    return false;
  }

  // A block has gone from this page: drop it from the list and work the
  // page's words out again from the blocks still there. Rebuilt rather than
  // cleared, because two blocks can share words - one entered in the middle
  // of another - and clearing what one covered would let a later store into
  // the other through unnoticed, which is the failure that has a game running
  // stale code. Only this page's blocks are looked at.
  void ForgetInPage(uint32_t page, uint32_t key) {
    const auto found = pages_.find(page);
    if (found == pages_.end())
      return;
    Page& entry = found->second;
    entry.blocks.erase(std::remove(entry.blocks.begin(), entry.blocks.end(), key),
                       entry.blocks.end());
    if (entry.blocks.empty()) {
      pages_.erase(found);
      ClearCodePage(page);
      return;
    }
    for (uint64_t& bits : entry.words)
      bits = 0;
    for (const uint32_t other : entry.blocks) {
      const Block& block = blocks_.at(other);
      // A block is listed in a page for its own code in it, for a word it depends on in it, or both.
      const uint32_t page_begin = page << kPageShift;
      if (other < page_begin + kPageSize && End(block) > page_begin)
        MarkWords(&entry, page, other, End(block));
      for (int i = 0; i < block.watched_count; ++i) {
        if ((block.watched[i] >> kPageShift) == page)
          MarkWords(&entry, page, block.watched[i], block.watched[i] + 4);
      }
    }
  }

  void MarkCodePage(uint32_t page) {
    const size_t word = page >> 6;
    if (word >= code_pages_.size())
      code_pages_.resize(word + 1, 0);
    code_pages_[word] |= (1ull << (page & 63));
  }

  void ClearCodePage(uint32_t page) {
    const size_t word = page >> 6;
    if (word < code_pages_.size())
      code_pages_[word] &= ~(1ull << (page & 63));
  }

  // Find's front: the block last found at each slot's address, or key 0xFFFFFFFF for none (no
  // normalised address has its top three bits set).
  struct FastSlot {
    uint32_t key = 0xFFFFFFFFu;
    const Block* block = nullptr;
  };
  static const uint32_t kFastSlots = 4096;
  mutable FastSlot fast_[kFastSlots];

  std::unordered_map<uint32_t, Block> blocks_;
  std::unordered_map<uint32_t, Page> pages_;
  // 2048 words of 64 pages of 4 KB: all 512 MB of physical addresses (Normalise's range).
  static const size_t kCodePageWords = (0x20000000u >> kPageShift) / 64;
  std::vector<uint64_t> code_pages_ = std::vector<uint64_t>(kCodePageWords, 0ull);
};

}  // namespace rec
}  // namespace emulation
