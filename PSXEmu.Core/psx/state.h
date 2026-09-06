/*****************************************************************************************************************
* Copyright (c) 2026 Khalid Ali Al-Kooheji                                                                       *
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

// The save-state serialiser. See Docs/Save-States-Plan.md.
//
// One class, one code path for both directions: every component's
// Serialise(StateIO&) calls the same Plain/Bytes/Deque/Str regardless of
// whether the machine is being saved or loaded, so a field can never be
// written by one path and read back by a different one that has drifted
// from it.

#include <cstdint>
#include <cstring>
#include <deque>
#include <string>
#include <type_traits>
#include <vector>

namespace emulation {
namespace psx {

// A save-state file: magic, then a version (bumped whenever any component's
// Serialise changes - old files are refused outright, never migrated), then
// an FNV-1a hash of the BIOS image the state was made against, then the
// mounted disc's path (empty for none - a courtesy copy for a future save
// browser; System::LoadState restores the real one from the payload, not
// from here), then the payload itself: every component's Serialise, called
// in the same fixed order on both sides. See System::SaveState/LoadState.
constexpr char kStateMagic[8] = {'P', 'S', 'X', 'S', 'T', 'A', 'T', 'E'};
constexpr uint32_t kStateVersion = 1;

// FNV-1a, 64-bit. Same algorithm boot_runner.cpp's framebuffer Checksum()
// already used - given a shared home here so the BIOS-identity hash in a
// state file's header uses the same, already-proven implementation.
uint64_t Fnv1a64(const void* data, size_t size);

class StateIO {
 public:
  explicit StateIO(bool saving) : saving_(saving) {}

  bool saving() const { return saving_; }

  // Any trivially-copyable value or struct - the common case for registers,
  // small structs and fixed arrays of either.
  template <typename T>
  void Plain(T& value) {
    static_assert(std::is_trivially_copyable<T>::value,
                  "Plain() needs a trivially-copyable type; use Bytes() for "
                  "anything with pointers, or add a dedicated helper.");
    if (saving_)
      Append(&value, sizeof(T));
    else
      Extract(&value, sizeof(T));
  }

  // A heap buffer the caller owns and knows the size of - Buffer::u8 and
  // friends. Never serialise a Buffer by its struct: its three pointers are
  // aliases into one allocation, and saving them writes stale host
  // addresses that mean nothing in a different process.
  void Bytes(void* data, size_t size) {
    if (saving_)
      Append(data, size);
    else
      Extract(data, size);
  }

  // A count-prefixed deque of a trivially-copyable element type - the
  // CD-ROM's parameter/response FIFOs and pending-response queue.
  template <typename T>
  void Deque(std::deque<T>& d) {
    static_assert(std::is_trivially_copyable<T>::value,
                  "Deque() elements must be trivially copyable.");
    if (saving_) {
      uint32_t count = static_cast<uint32_t>(d.size());
      Plain(count);
      for (T& element : d)
        Plain(element);
    } else {
      uint32_t count = 0;
      Plain(count);
      d.clear();
      for (uint32_t i = 0; i < count; ++i) {
        T element{};
        Plain(element);
        d.push_back(element);
      }
    }
  }

  // A length-prefixed string - disc paths, and nothing else in a plain
  // component's own Serialise (the header carries the top-level disc path
  // and BIOS hash separately).
  void Str(std::string& s) {
    if (saving_) {
      uint32_t length = static_cast<uint32_t>(s.size());
      Plain(length);
      if (length > 0)
        Append(s.data(), length);
    } else {
      uint32_t length = 0;
      Plain(length);
      s.assign(length, '\0');
      if (length > 0)
        Extract(&s[0], length);
    }
  }

  // Loading only: point the cursor at an in-memory copy of the payload
  // (after the header has already been consumed by whoever read the file).
  void BeginLoad(const uint8_t* data, size_t size) {
    buffer_.assign(data, data + size);
    cursor_ = 0;
    truncated_ = false;
  }

  // Saving only: the payload accumulated so far, ready to append to a file
  // after the header.
  const std::vector<uint8_t>& bytes() const { return buffer_; }

  // Set if a load ran past the end of the buffer - a truncated or foreign
  // file. Callers must check this after driving a whole Serialise chain;
  // reading garbage past the end is not an option to report, but is
  // silently answered with zeroed output. Only meaningful while loading.
  bool truncated() const { return truncated_; }

  // A component's Serialise can have a real side effect that fails on load -
  // Cdrom reopening its Disc from a saved path being the one case in this
  // codebase. Every Serialise still returns void and moves bytes
  // unconditionally; a failure is recorded here instead, and the first one
  // wins (later components still run, but System::LoadState reports the
  // original reason, not whatever broke downstream because of it).
  void SetError(const std::string& message) {
    if (error_.empty())
      error_ = message;
  }
  const std::string& error() const { return error_; }

 private:
  void Append(const void* data, size_t size) {
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    buffer_.insert(buffer_.end(), bytes, bytes + size);
  }

  void Extract(void* data, size_t size) {
    if (truncated_ || cursor_ + size > buffer_.size()) {
      truncated_ = true;
      memset(data, 0, size);
      return;
    }
    memcpy(data, buffer_.data() + cursor_, size);
    cursor_ += size;
  }

  bool saving_;
  std::vector<uint8_t> buffer_;
  size_t cursor_ = 0;
  bool truncated_ = false;
  std::string error_;
};

}  // namespace psx
}  // namespace emulation
