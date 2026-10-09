// Finished frames, from the machine's thread to the video thread.
//
// Three slots: the producer fills one, the consumer shows another, and the
// third is the mailbox between them, handed across with one atomic exchange.
// Neither side ever waits for the other. The consumer always gets the newest
// frame; a frame it never got to is replaced and counted - which is exactly
// what should happen when the machine runs faster than the monitor, at 200% on
// a 60 Hz display, and exactly what could not happen while presenting was on
// the machine's own thread and vsync paced it.
//
// DuckStation queues up to two frames between its core and video threads;
// this holds one, which is the least latency there is.
#pragma once

#include "host/doorbell.h"
#include "psx/shared_picture.h"

#include <atomic>
#include <cstdint>
#include <vector>

namespace emulation {
namespace host {

struct VideoFrame {
  // The picture as the GPU resolves it - 32-bit 0xAARRGGBB, width x height,
  // Gpu::framebuffer's own format.
  std::vector<uint32_t> pixels;

  // Or the hardware rasteriser's picture, left on the graphics card (psx/shared_picture.h):
  // width x height, and `pixels` empty. Only for a presenter that said it can take one.
  psx::SharedPicture shared;

  // Or, for View > VRAM, all of VRAM as the GPU holds it: 1024x512
  // 16-bit pixels, converted by the video thread, which has the time to spare
  // where the machine's thread has none.
  std::vector<uint16_t> vram;
  bool is_vram = false;

  int width = 0;
  int height = 0;

  // Which emulated frame this is, counting from 1. The consumer can tell a
  // repeat from a new one, and a test can tell a torn frame from a whole one.
  uint64_t number = 0;
};

class FrameMailbox {
 public:
  // `consumer` is the video thread's doorbell, rung on every publish.
  explicit FrameMailbox(Doorbell* consumer = nullptr) : consumer_(consumer) {}

  FrameMailbox(const FrameMailbox&) = delete;
  FrameMailbox& operator=(const FrameMailbox&) = delete;

  // ---- The producer: the machine's thread --------------------------------

  // The slot to fill. The same one until Publish.
  VideoFrame& back() { return slots_[back_]; }

  // Hands the filled slot over and takes the mailbox's old one back to fill
  // next. If that one held a frame the consumer never took, it is overwritten
  // from here on, and counted as dropped - and true is returned, since back()
  // is then a frame nobody saw rather than one the consumer has finished with.
  bool Publish() {
    const int previous = middle_.exchange(back_ | kFresh, std::memory_order_acq_rel);
    back_ = previous & kSlotMask;
    const bool dropped = (previous & kFresh) != 0;
    if (dropped)
      dropped_.fetch_add(1, std::memory_order_relaxed);
    published_.fetch_add(1, std::memory_order_relaxed);
    if (consumer_ != nullptr)
      consumer_->Ring();
    return dropped;
  }

  // ---- The consumer: the video thread ------------------------------------

  // The newest frame, if one was published since the last call; null if not.
  // What it returns stays valid, and unchanged, until the next TakeNew.
  const VideoFrame* TakeNew() {
    if ((middle_.load(std::memory_order_acquire) & kFresh) == 0)
      return nullptr;
    // Only the consumer ever clears kFresh, so having seen it set, the
    // exchange is guaranteed to return a fresh frame - possibly a newer one
    // than was there a moment ago, which is only better.
    const int previous = middle_.exchange(front_, std::memory_order_acq_rel);
    front_ = previous & kSlotMask;
    has_front_ = true;
    taken_.fetch_add(1, std::memory_order_relaxed);
    return &slots_[front_];
  }

  // The frame last taken, or null if there has not been one - for showing it
  // again after a resize, when nothing new has arrived.
  const VideoFrame* current() const { return has_front_ ? &slots_[front_] : nullptr; }

  // ---- Counters, any thread ----------------------------------------------

  // How long the consumer's last present took, in nanoseconds - which says whether presents are
  // waiting on a full queue (host::Machine::Pace, matching the display). The consumer's to set.
  void NotePresent(int64_t ns) { present_ns_.store(ns, std::memory_order_relaxed); }
  int64_t last_present_ns() const { return present_ns_.load(std::memory_order_relaxed); }

  // The producer asks for the next frame taken not to be presented - one finished too late for
  // its refresh, or one left out to empty a display queue that has filled (host::Machine) - and
  // the consumer asks whether it should skip, once. A frame skipped is counted as dropped: it
  // is never shown.
  void RequestSkip() { skip_.store(true, std::memory_order_relaxed); }
  bool TakeSkip() {
    if (!skip_.exchange(false, std::memory_order_relaxed))
      return false;
    dropped_.fetch_add(1, std::memory_order_relaxed);
    return true;
  }

  uint64_t published() const { return published_.load(std::memory_order_relaxed); }
  uint64_t taken() const { return taken_.load(std::memory_order_relaxed); }
  uint64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }

 private:
  static const int kSlotMask = 3;
  static const int kFresh = 4;   // the mailbox holds a frame nobody has taken

  VideoFrame slots_[3];
  int back_ = 0;                            // the producer's
  int front_ = 1;                           // the consumer's
  bool has_front_ = false;                  // the consumer's
  std::atomic<int> middle_{2};              // the mailbox's slot, plus kFresh
  Doorbell* consumer_;

  std::atomic<uint64_t> published_{0};
  std::atomic<uint64_t> taken_{0};
  std::atomic<uint64_t> dropped_{0};
  std::atomic<int64_t> present_ns_{0};
  std::atomic<bool> skip_{false};
};

}  // namespace host
}  // namespace emulation
