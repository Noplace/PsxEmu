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
#include "emucore/state/rewind_buffer.h"

#include "zlib.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace emulation {
namespace state {

namespace {

// zlib at level 1, the fastest: what is compressed is mostly zeros, the bytes that did not change
// in a page that did. `out` is resized to what came out.
void Compress(const uint8_t* in, size_t size, std::vector<uint8_t>* out, size_t header) {
  z_stream z = {};
  deflateInit(&z, 1);
  out->resize(header + deflateBound(&z, static_cast<uLong>(size)));
  z.next_in = const_cast<Bytef*>(in);
  z.avail_in = static_cast<uInt>(size);
  z.next_out = out->data() + header;
  z.avail_out = static_cast<uInt>(out->size() - header);
  deflate(&z, Z_FINISH);
  out->resize(header + z.total_out);
  deflateEnd(&z);
}

bool Decompress(const uint8_t* in, size_t size, uint8_t* out, size_t out_size) {
  z_stream z = {};
  if (inflateInit(&z) != Z_OK)
    return false;
  z.next_in = const_cast<Bytef*>(in);
  z.avail_in = static_cast<uInt>(size);
  z.next_out = out;
  z.avail_out = static_cast<uInt>(out_size);
  const int result = inflate(&z, Z_FINISH);
  const bool whole = result == Z_STREAM_END && z.total_out == out_size;
  inflateEnd(&z);
  return whole;
}

size_t PageLength(size_t page, size_t size) {
  const size_t start = page * RewindBuffer::kPage;
  return std::min(RewindBuffer::kPage, size - start);
}

}  // namespace

RewindBuffer::RewindBuffer(size_t budget_bytes) : budget_(budget_bytes) {
  worker_ = std::thread([this] { Work(); });
}

RewindBuffer::~RewindBuffer() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stop_ = true;
  }
  wake_.notify_all();
  worker_.join();
}

void RewindBuffer::set_budget(size_t bytes) {
  std::lock_guard<std::mutex> lock(mutex_);
  budget_ = bytes;
}

void RewindBuffer::Push(std::vector<uint8_t>* payload) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (has_pending_)
      ++stats_.replaced;
    pending_.swap(*payload);
    has_pending_ = true;
    // Whatever came back - the push it replaced, or nothing - and a spare if that was nothing.
    if (payload->capacity() == 0)
      payload->swap(spare_);
  }
  wake_.notify_one();
}

bool RewindBuffer::Pop(std::vector<uint8_t>* out) {
  std::unique_lock<std::mutex> lock(mutex_);
  idle_.wait(lock, [this] { return !working_ && !has_pending_; });
  if (newest_.empty())
    return false;
  *out = newest_;
  if (!deltas_.empty()) {
    const Delta delta = std::move(deltas_.back());
    deltas_.pop_back();
    delta_bytes_ -= delta.data.size();
    ApplyDelta(delta, &newest_);
  }
  return true;
}

void RewindBuffer::WaitIdle() {
  std::unique_lock<std::mutex> lock(mutex_);
  idle_.wait(lock, [this] { return !working_ && !has_pending_; });
}

void RewindBuffer::Clear() {
  std::unique_lock<std::mutex> lock(mutex_);
  idle_.wait(lock, [this] { return !working_; });
  has_pending_ = false;
  pending_.clear();
  newest_.clear();
  deltas_.clear();
  delta_bytes_ = 0;
}

size_t RewindBuffer::count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return newest_.empty() ? 0 : deltas_.size() + 1;
}

size_t RewindBuffer::bytes() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return newest_.size() + delta_bytes_;
}

RewindBuffer::Stats RewindBuffer::stats() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return stats_;
}

void RewindBuffer::Work() {
  std::unique_lock<std::mutex> lock(mutex_);
  for (;;) {
    wake_.wait(lock, [this] { return stop_ || has_pending_; });
    if (stop_)
      return;
    std::vector<uint8_t> incoming;
    incoming.swap(pending_);
    has_pending_ = false;
    if (newest_.empty()) {
      newest_.swap(incoming);
      idle_.notify_all();
      continue;
    }
    // Pop and Clear wait for this, and Push touches only pending_: newest_ is the worker's
    // until working_ is cleared.
    working_ = true;
    lock.unlock();
    const auto start = std::chrono::steady_clock::now();
    Delta delta = MakeDelta(newest_, incoming);
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    lock.lock();
    ++stats_.deltas;
    stats_.delta_bytes += delta.data.size();
    stats_.delta_ms += ms;
    delta_bytes_ += delta.data.size();
    deltas_.push_back(std::move(delta));
    newest_.swap(incoming);
    spare_.swap(incoming);   // the state that was newest: a buffer for the next Push
    TrimLocked();
    working_ = false;
    idle_.notify_all();
  }
}

RewindBuffer::Delta RewindBuffer::MakeDelta(const std::vector<uint8_t>& older,
                                            const std::vector<uint8_t>& newer) {
  Delta delta;
  delta.older_size = static_cast<uint32_t>(older.size());
  // A state can change size - the CD-ROM's queues, a port's device - and then there is nothing to
  // line its pages up against: the older one is kept whole, compressed.
  if (older.size() != newer.size()) {
    delta.whole = true;
    Compress(older.data(), older.size(), &delta.data, 0);
    return delta;
  }
  // The pages that changed, and their XOR against the newer state's, which is mostly zeros.
  std::vector<uint32_t> pages;
  scratch_.clear();
  const size_t page_count = (older.size() + kPage - 1) / kPage;
  for (size_t page = 0; page < page_count; ++page) {
    const size_t start = page * kPage;
    const size_t length = PageLength(page, older.size());
    if (memcmp(&older[start], &newer[start], length) == 0)
      continue;
    pages.push_back(static_cast<uint32_t>(page));
    const size_t at = scratch_.size();
    scratch_.resize(at + length);
    for (size_t i = 0; i < length; ++i)
      scratch_[at + i] = older[start + i] ^ newer[start + i];
  }
  // [page count][the pages][the XORs, compressed]
  const size_t header = sizeof(uint32_t) * (pages.size() + 1);
  Compress(scratch_.data(), scratch_.size(), &delta.data, header);
  const uint32_t count = static_cast<uint32_t>(pages.size());
  memcpy(delta.data.data(), &count, sizeof(count));
  if (!pages.empty())
    memcpy(delta.data.data() + sizeof(count), pages.data(), sizeof(uint32_t) * pages.size());
  return delta;
}

void RewindBuffer::ApplyDelta(const Delta& delta, std::vector<uint8_t>* newest) {
  if (delta.whole) {
    newest->resize(delta.older_size);
    Decompress(delta.data.data(), delta.data.size(), newest->data(), newest->size());
    return;
  }
  uint32_t count = 0;
  memcpy(&count, delta.data.data(), sizeof(count));
  const uint8_t* pages = delta.data.data() + sizeof(count);
  const size_t header = sizeof(uint32_t) * (count + 1);
  size_t xored = 0;
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t page = 0;
    memcpy(&page, pages + sizeof(uint32_t) * i, sizeof(page));
    xored += PageLength(page, newest->size());
  }
  unpacked_.resize(xored);
  Decompress(delta.data.data() + header, delta.data.size() - header, unpacked_.data(), xored);
  size_t at = 0;
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t page = 0;
    memcpy(&page, pages + sizeof(uint32_t) * i, sizeof(page));
    const size_t start = page * kPage;
    const size_t length = PageLength(page, newest->size());
    for (size_t b = 0; b < length; ++b)
      (*newest)[start + b] ^= unpacked_[at + b];
    at += length;
  }
}

void RewindBuffer::TrimLocked() {
  while (!deltas_.empty() && newest_.size() + delta_bytes_ > budget_) {
    delta_bytes_ -= deltas_.front().data.size();
    deltas_.pop_front();
  }
}

}  // namespace state
}  // namespace emulation
