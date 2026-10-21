// Sound, from the machine's thread to the audio thread, with no lock on either
// side.
//
// Single producer - the machine, once a frame, in a burst of about 17 ms of
// samples - and single consumer - the audio thread, pulling a few milliseconds
// at a time at the sound device's own pace. What sits in between is the slack
// that lets the two run on different clocks: the machine's is the frame limiter
// and the host's steady_clock, the device's is its own crystal. The machine
// steers its resampling by Available() to keep the slack near a target, the
// same half-percent trim App::PumpAudio used against the device's queue before
// this existed; the ring is only the buffer being measured.
//
// This is DuckStation's shape too: its CoreAudioStream is a ring with atomic
// read and write positions that the backend's own thread drains.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

namespace emulation {
namespace host {

class SampleRing {
 public:
  static const int kChannels = 2;

  // Rounded up to a power of two, so a position wraps with a mask.
  explicit SampleRing(int capacity_frames) {
    uint32_t capacity = 1;
    while (capacity < static_cast<uint32_t>(capacity_frames))
      capacity <<= 1;
    mask_ = capacity - 1;
    buffer_.assign(static_cast<size_t>(capacity) * kChannels, 0);
  }

  SampleRing(const SampleRing&) = delete;
  SampleRing& operator=(const SampleRing&) = delete;

  int capacity() const { return static_cast<int>(mask_ + 1); }

  // Either side. Exact from the side that asks for its own purposes - the
  // producer can only see it grow by its own writes, the consumer only shrink
  // by its own reads - and a lower (producer) or upper (consumer) bound on
  // what the other side is about to do.
  int Available() const {
    const uint64_t written = write_.load(std::memory_order_acquire);
    const uint64_t read = read_.load(std::memory_order_acquire);
    return static_cast<int>(written - read);
  }

  // ---- The producer: the machine's thread --------------------------------

  // Writes as many of `frames` as fit and returns how many. The rest are
  // dropped - the newest, not the oldest, because the oldest are already
  // queued behind the device and dropping them would be a jump backwards in
  // the middle of the sound - and counted.
  int Write(const int16_t* samples, int frames) {
    const int n = Reserve(frames);
    const uint64_t written = write_.load(std::memory_order_relaxed);
    CopyIn(written, samples, n);
    write_.store(written + n, std::memory_order_release);
    return n;
  }

  // The same, with silence. Used to put the ring back at its target after a
  // pause, so the rate trim - which moves half a percent at most - does not
  // spend five seconds climbing back up from empty.
  int WriteSilence(int frames) {
    const int n = Reserve(frames);
    const uint64_t written = write_.load(std::memory_order_relaxed);
    CopyIn(written, nullptr, n);
    write_.store(written + n, std::memory_order_release);
    return n;
  }

  // Whether the producer is running. The consumer only counts a short read as
  // an underrun while it is: a paused machine produces nothing, and the audio
  // thread filling that with silence is the design, not a fault.
  void set_producing(bool producing) {
    producing_.store(producing, std::memory_order_release);
  }
  bool producing() const { return producing_.load(std::memory_order_acquire); }

  // The level the producer steers for. The consumer reads it to know when the
  // ring is so far past it that waiting for the producer's half-percent trim
  // to bring it down - twenty seconds, from full - would just be latency.
  void set_target_frames(int frames) {
    target_.store(frames, std::memory_order_relaxed);
  }
  int target_frames() const { return target_.load(std::memory_order_relaxed); }

  // ---- The consumer: the audio thread ------------------------------------

  // Reads up to `frames` and returns how many.
  int Read(int16_t* out, int frames) {
    const uint64_t read = read_.load(std::memory_order_relaxed);
    const uint64_t written = write_.load(std::memory_order_acquire);
    const int n = std::min(frames, static_cast<int>(written - read));
    if (n <= 0)
      return 0;
    const uint32_t start = static_cast<uint32_t>(read) & mask_;
    const int first = std::min(n, static_cast<int>(mask_ + 1 - start));
    std::memcpy(out, &buffer_[static_cast<size_t>(start) * kChannels],
                static_cast<size_t>(first) * kChannels * sizeof(int16_t));
    if (n > first) {
      std::memcpy(out + static_cast<size_t>(first) * kChannels, &buffer_[0],
                  static_cast<size_t>(n - first) * kChannels * sizeof(int16_t));
    }
    read_.store(read + n, std::memory_order_release);
    return n;
  }

  // Throws away the oldest `frames`, up to what is there, and returns how many
  // went. For the consumer to catch up when the ring has run far past its
  // target: a device that took a while to open, or a machine run unpaced. The
  // alternative is playing everything that piled up, late, forever.
  int Skip(int frames) {
    const uint64_t read = read_.load(std::memory_order_relaxed);
    const uint64_t written = write_.load(std::memory_order_acquire);
    const int n = std::min(frames, static_cast<int>(written - read));
    if (n <= 0)
      return 0;
    read_.store(read + n, std::memory_order_release);
    skipped_.fetch_add(static_cast<uint64_t>(n), std::memory_order_relaxed);
    return n;
  }

  // What the consumer had to make up with silence because the ring was short
  // while the producer was running - each one an audible gap.
  void NoteShort(int frames) {
    short_.fetch_add(static_cast<uint64_t>(frames), std::memory_order_relaxed);
  }

  // ---- Counters, any thread ----------------------------------------------

  uint64_t dropped_frames() const { return dropped_.load(std::memory_order_relaxed); }
  uint64_t short_frames() const { return short_.load(std::memory_order_relaxed); }
  uint64_t skipped_frames() const { return skipped_.load(std::memory_order_relaxed); }
  uint64_t frames_written() const { return write_.load(std::memory_order_relaxed); }
  uint64_t frames_read() const { return read_.load(std::memory_order_relaxed); }

 private:
  // How many of `frames` fit, counting the rest as dropped.
  int Reserve(int frames) {
    if (frames <= 0)
      return 0;
    const int room = capacity() - Available();
    const int n = std::min(frames, room);
    if (n < frames)
      dropped_.fetch_add(static_cast<uint64_t>(frames - n), std::memory_order_relaxed);
    return n;
  }

  // `samples` null writes silence.
  void CopyIn(uint64_t position, const int16_t* samples, int n) {
    if (n <= 0)
      return;
    const uint32_t start = static_cast<uint32_t>(position) & mask_;
    const int first = std::min(n, static_cast<int>(mask_ + 1 - start));
    int16_t* to = &buffer_[static_cast<size_t>(start) * kChannels];
    const size_t first_bytes = static_cast<size_t>(first) * kChannels * sizeof(int16_t);
    const size_t rest_bytes = static_cast<size_t>(n - first) * kChannels * sizeof(int16_t);
    if (samples != nullptr) {
      std::memcpy(to, samples, first_bytes);
      if (n > first)
        std::memcpy(&buffer_[0], samples + static_cast<size_t>(first) * kChannels, rest_bytes);
    } else {
      std::memset(to, 0, first_bytes);
      if (n > first)
        std::memset(&buffer_[0], 0, rest_bytes);
    }
  }

  std::vector<int16_t> buffer_;
  uint32_t mask_ = 0;   // capacity in frames, less one

  // Frames ever written and ever read. Sixty-four bits never wrap - at 44,100
  // a second they would take millions of years - so "how many are waiting" is
  // a plain subtraction with no wrap case to get wrong. Each on its own cache
  // line, so the two threads writing them do not keep stealing it from each
  // other.
  alignas(64) std::atomic<uint64_t> write_{0};
  alignas(64) std::atomic<uint64_t> read_{0};
  alignas(64) std::atomic<uint64_t> dropped_{0};
  std::atomic<uint64_t> short_{0};
  std::atomic<uint64_t> skipped_{0};
  std::atomic<bool> producing_{false};
  std::atomic<int> target_{0};
};

}  // namespace host
}  // namespace emulation
