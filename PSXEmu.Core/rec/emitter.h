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

// Executable memory and a cursor to write into it. That is the whole job.
//
// This replaces the vendored RecCore's Emitter, which did the same job in 5,633
// lines of library to supply the six functions this file supplies in a hundred.
// Every actual instruction encoding was already ours in x86_extras.h; what was
// left was a VirtualAlloc wrapper, and it had two defects worth replacing it
// over:
//
//   - **It never freed anything.** `VirtualFree(address, size,
//     MEM_DECOMMIT|MEM_RELEASE)` is an invalid combination - MEM_RELEASE
//     requires a size of zero - so the call failed with ERROR_INVALID_PARAMETER
//     every time, and the return value was not checked. Measured: two hundred
//     allocate/free cycles of 256 KB leaked all 51,200 KB of it.
//   - **Emitting was unchecked.** `*(ptr + cursor++) = byte` with nothing
//     comparing the cursor against the size. The recompiler keeps well clear of
//     the end of an arena by convention, which is not the same as being unable
//     to run off it.
//
// Both are fixed here: the documented free, with its result checked, and an
// emit that refuses to write past the end and says so.

#include <cstdint>
#include <cstring>
#include <vector>
#include <windows.h>

namespace emulation {
namespace rec {

// A run of executable memory, and how far into it the emitter has written.
struct CodeBlock {
  void* address = nullptr;
  uint8_t* ptr8bit = nullptr;
  size_t size = 0;
  size_t cursor = 0;

  // Set when an emit was refused for want of room. A block that overflowed is
  // not a block: whoever is compiling has to throw it away rather than run
  // whatever did fit.
  bool overflowed = false;
};

class Emitter {
 public:
  Emitter() = default;
  ~Emitter() {
    for (CodeBlock* block : blocks_)
      Release(block);
  }

  Emitter(const Emitter&) = delete;
  Emitter& operator=(const Emitter&) = delete;

  // Reserve and commit in one call, which is what VirtualAlloc asks for, and
  // executable because the point of the memory is to be jumped into.
  //
  // The pages are RWX rather than W^X, and that is a decision rather than an
  // oversight: block linking patches the displacement of a jump in already-
  // emitted code, and it does so often - nearly two million times in a
  // 1,500-frame run of one game. Flipping page protection around each of those
  // would cost far more than the linking saves. Doing W^X properly means
  // mapping the same pages twice, writable at one address and executable at
  // another, which is a real piece of work and worth doing only if something
  // actually objects to RWX.
  CodeBlock* create_block(size_t bytes) {
    void* memory = VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT,
                                PAGE_EXECUTE_READWRITE);
    if (memory == nullptr)
      return nullptr;

    CodeBlock* block = new CodeBlock();
    block->address = memory;
    block->ptr8bit = static_cast<uint8_t*>(memory);
    block->size = bytes;
    blocks_.push_back(block);
    return block;
  }

  void destroy_block(CodeBlock* block) {
    if (block == nullptr)
      return;
    for (size_t i = 0; i < blocks_.size(); ++i) {
      if (blocks_[i] == block) {
        blocks_.erase(blocks_.begin() + i);
        break;
      }
    }
    Release(block);
  }

  void set_block(CodeBlock* block) { block_ = block; }
  CodeBlock* block() { return block_; }
  size_t GetCursor() const { return block_ == nullptr ? 0 : block_->cursor; }

  // Refuses rather than writes past the end. The caller finds out through
  // CodeBlock::overflowed, because a byte at a time is not where anyone can do
  // anything useful about it.
  void emit8(uint8_t byte) {
    if (block_ == nullptr)
      return;
    if (block_->cursor >= block_->size) {
      block_->overflowed = true;
      return;
    }
    block_->ptr8bit[block_->cursor++] = byte;
  }

  void emit16(uint16_t bytes) {
    emit8(static_cast<uint8_t>(bytes & 0xFF));
    emit8(static_cast<uint8_t>((bytes >> 8) & 0xFF));
  }

  void emit32(uint32_t bytes) {
    emit16(static_cast<uint16_t>(bytes & 0xFFFF));
    emit16(static_cast<uint16_t>((bytes >> 16) & 0xFFFF));
  }

  void emit64(uint64_t bytes) {
    emit32(static_cast<uint32_t>(bytes & 0xFFFFFFFF));
    emit32(static_cast<uint32_t>((bytes >> 32) & 0xFFFFFFFF));
  }

 private:
  // MEM_RELEASE frees the whole reservation and takes a size of zero. Anything
  // else - a size, or MEM_DECOMMIT alongside it - is ERROR_INVALID_PARAMETER
  // and frees nothing, which is exactly the bug this file exists to not repeat,
  // so the result is checked rather than assumed.
  void Release(CodeBlock* block) {
    if (block->address != nullptr) {
      const BOOL freed = VirtualFree(block->address, 0, MEM_RELEASE);
      if (!freed)
        ++failed_frees_;
    }
    if (block_ == block)
      block_ = nullptr;
    delete block;
  }

  CodeBlock* block_ = nullptr;
  std::vector<CodeBlock*> blocks_;

 public:
  // Zero, or something is wrong with the assumptions above. Exposed so a test
  // can insist on it rather than trust the comment.
  int failed_frees() const { return failed_frees_; }

 private:
  int failed_frees_ = 0;
};

}  // namespace rec
}  // namespace emulation
