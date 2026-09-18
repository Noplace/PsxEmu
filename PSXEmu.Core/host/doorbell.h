// A thread's doorbell: everything that gives a thread work rings it, and the
// thread sleeps on it when it has nothing else to do.
//
// host/ is the one part of Core that knows about threads (Docs/Threading-
// Plan.md). Each thread in it owns one of these, and every channel that feeds
// the thread - a request queue, the frame mailbox - rings the owner's bell when
// it puts something in. One bell per thread rather than one per channel is the
// point: a thread cannot wait on two condition variables at once, and a video
// thread has to wake for a new frame *or* a resize, whichever comes first.
#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>

namespace emulation {
namespace host {

class Doorbell {
 public:
  // Any thread.
  void Ring() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      rung_ = true;
    }
    rung_changed_.notify_one();
  }

  // The owning thread. Returns as soon as the bell is rung - at once if it was
  // rung while the thread was busy elsewhere - or when `timeout` runs out, and
  // says which. Either way the bell is quiet again afterwards.
  //
  // The timeout is only as precise as the system timer, about 15 ms here; that
  // is fine for what this is for, which is idling. A ring wakes the thread at
  // once regardless.
  bool Wait(std::chrono::microseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    const bool rung = rung_changed_.wait_for(lock, timeout, [this] { return rung_; });
    rung_ = false;
    return rung;
  }

 private:
  std::mutex mutex_;
  std::condition_variable rung_changed_;
  bool rung_ = false;
};

}  // namespace host
}  // namespace emulation
