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
// See Docs/Recompiler-Plan.md. Nothing in the emulator calls any of this yet.

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
};

// A guest address to Block map with page-granular invalidation.
//
// Invalidation is the part that goes wrong in a recompiler, so it is the part
// that exists first. Two things throw a block away on this machine:
//
//   - a store into a page a block was compiled from, which Cpu::Store will
//     check against `IsCodePage` once this is wired up; and
//   - a write to the cache-control register at 0xFFFE0130, which is how
//     software tells the hardware it has replaced code - overlays, which is
//     the common case on the PSX - and which maps to Clear().
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
    blocks_[key] = block;
    blocks_[key].guest_address = key;

    const uint32_t first = key >> kPageShift;
    const uint32_t last =
        (key + (block.guest_bytes == 0 ? 0 : block.guest_bytes - 1)) >> kPageShift;
    for (uint32_t page = first; page <= last; ++page)
      MarkCodePage(page);
  }

  // The block starting exactly at this address, or nullptr. A block is only
  // ever entered at its first instruction: jumping into the middle of one
  // compiles a new block from there, which is correct and costs a little
  // duplicated code.
  const Block* Find(uint32_t address) const {
    const auto it = blocks_.find(Normalise(address));
    return (it == blocks_.end()) ? nullptr : &it->second;
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
    if (!IsCodePage(address))
      return 0;

    uint32_t removed = 0;
    for (auto it = blocks_.begin(); it != blocks_.end();) {
      const uint32_t first = it->second.guest_address >> kPageShift;
      const uint32_t bytes = it->second.guest_bytes;
      const uint32_t last =
          (it->second.guest_address + (bytes == 0 ? 0 : bytes - 1)) >> kPageShift;
      if (page >= first && page <= last) {
        it = blocks_.erase(it);
        ++removed;
      } else {
        ++it;
      }
    }

    // The page is only clear once nothing is left in it. A block that spans two
    // pages keeps both marked, so this rebuilds rather than just clearing the
    // bit - wrongly clearing it would let a later store through unnoticed,
    // which is the failure mode that produces a game running stale code.
    RebuildCodePages();
    return removed;
  }

  void Clear() {
    blocks_.clear();
    code_pages_.clear();
  }

  size_t size() const { return blocks_.size(); }

 private:
  void MarkCodePage(uint32_t page) {
    const size_t word = page >> 6;
    if (word >= code_pages_.size())
      code_pages_.resize(word + 1, 0);
    code_pages_[word] |= (1ull << (page & 63));
  }

  void RebuildCodePages() {
    code_pages_.clear();
    for (const auto& entry : blocks_) {
      const uint32_t bytes = entry.second.guest_bytes;
      const uint32_t first = entry.second.guest_address >> kPageShift;
      const uint32_t last =
          (entry.second.guest_address + (bytes == 0 ? 0 : bytes - 1)) >> kPageShift;
      for (uint32_t page = first; page <= last; ++page)
        MarkCodePage(page);
    }
  }

  std::unordered_map<uint32_t, Block> blocks_;
  std::vector<uint64_t> code_pages_;
};

}  // namespace rec
}  // namespace emulation
