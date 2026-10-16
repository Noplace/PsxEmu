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

// Rewind's store (Docs/Rewind-and-Run-Ahead-Plan.md, phase 5): the payloads of the states kept
// in memory every few frames, as many as a memory budget holds.
//
// The newest is kept whole. Every older one is kept as what it differs by from the one after
// it - a reverse delta: the 4 KB pages that changed, XORed against the newer state's and
// compressed with zlib - so stepping back one state is applying one delta to the newest, never a
// walk forward from a keyframe, and dropping the oldest when the budget is full is freeing one
// delta that nothing depends on. A state is a frame or two of a game apart from the last, so a
// delta is a few dozen pages: 20-130 KB where the whole is 4.3 MB.
//
// Push and Pop are the machine thread's. Working out a delta - comparing 4 MB, XORing and
// compressing what changed, a few milliseconds - is a worker thread's, so taking a state costs
// the machine only the save. If the machine pushes again before the worker has done the last,
// the one waiting is replaced: the store keeps fewer states, never a broken chain.

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace emulation {
namespace psx {

class RewindBuffer {
 public:
  explicit RewindBuffer(size_t budget_bytes);
  ~RewindBuffer();
  RewindBuffer(const RewindBuffer&) = delete;
  RewindBuffer& operator=(const RewindBuffer&) = delete;

  // The memory the store may use, deltas and the newest whole state together. A smaller budget
  // drops the oldest states at the next push.
  void set_budget(size_t bytes);

  // The newest state's payload, taken out of `payload` - which is handed back a spare buffer of
  // about the size, so the caller saves into it next time without allocating.
  void Push(std::vector<uint8_t>* payload);

  // Steps back: the newest state into `out`, and the one before it becomes the newest. The
  // oldest is handed out again and again rather than given up, so a rewind held past the start
  // stays there. Waits for the worker to finish what it has. False with nothing stored.
  bool Pop(std::vector<uint8_t>* out);

  // Forgets everything: the machine has gone somewhere a step back would not lead - a reset, a
  // boot, a state loaded from a file.
  void Clear();

  // Waits until everything pushed has been made the newest state. For a harness that needs every
  // push kept rather than replaced by the next.
  void WaitIdle();

  // States held, the newest included, and the bytes they take. Any thread.
  size_t count() const;
  size_t bytes() const;

  // What the worker has done, for measuring: deltas made, their compressed bytes in all, and the
  // time spent making them.
  struct Stats {
    uint64_t deltas = 0;
    uint64_t delta_bytes = 0;
    double delta_ms = 0.0;
    uint64_t replaced = 0;   // pushes the worker never got to, replaced by the next
  };
  Stats stats() const;

  static const size_t kPage = 4096;

 private:
  struct Delta {
    uint32_t older_size = 0;   // the older state's payload size
    bool whole = false;        // the sizes differed: `data` is the older state, compressed
    std::vector<uint8_t> data;
  };

  void Work();
  // The older state as a delta against the newer one. Worker only.
  Delta MakeDelta(const std::vector<uint8_t>& older, const std::vector<uint8_t>& newer);
  // The state before `newest`, from `newest` and its delta. Machine thread, under the lock.
  void ApplyDelta(const Delta& delta, std::vector<uint8_t>* newest);
  void TrimLocked();

  mutable std::mutex mutex_;
  std::condition_variable wake_;   // the worker: something pending, or stop
  std::condition_variable idle_;   // Pop: the worker has nothing in hand
  std::vector<uint8_t> pending_;   // the newest push, not yet the newest state
  bool has_pending_ = false;
  bool working_ = false;
  bool stop_ = false;
  std::vector<uint8_t> newest_;
  std::deque<Delta> deltas_;       // oldest first
  size_t delta_bytes_ = 0;
  size_t budget_ = 0;
  std::vector<uint8_t> spare_;     // a buffer to hand back to Push
  std::vector<uint8_t> scratch_;   // the worker's: XORed pages before compressing
  std::vector<uint8_t> unpacked_;  // Pop's: a delta decompressed
  Stats stats_;
  std::thread worker_;
};

}  // namespace psx
}  // namespace emulation
