// Requests for the thread that owns something, from any other thread.
//
// The one door into an object another thread owns (Docs/Threading-Plan.md,
// phase 2). The UI never touches the machine; it posts "boot this disc" here,
// and the machine's own thread runs it between frames, where nothing about the
// frame is half done. Save states were already done this way - the pending-slot
// fields the front end used to have - and this is that pattern made general.
//
// A request is a function of the owned object, so the type says whose thread
// it runs on: a RequestQueue<Machine> can only ever be drained by the thread
// that has the Machine.
#pragma once

#include "host/doorbell.h"

#include <functional>
#include <mutex>
#include <utility>
#include <vector>

namespace emulation {
namespace host {

template <typename Target>
class RequestQueue {
 public:
  typedef std::function<void(Target&)> Request;

  // `doorbell` is the owning thread's, and is rung on every post so a thread
  // idling on it hears about the request at once. Null is allowed, for an owner
  // that polls.
  explicit RequestQueue(Doorbell* doorbell = nullptr) : doorbell_(doorbell) {}

  RequestQueue(const RequestQueue&) = delete;
  RequestQueue& operator=(const RequestQueue&) = delete;

  // Any thread.
  void Post(Request request) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      pending_.push_back(std::move(request));
    }
    if (doorbell_ != nullptr)
      doorbell_->Ring();
  }

  // The owning thread only. Runs everything posted before the call, in the
  // order it was posted, and returns how many ran.
  //
  // What a request posts while it runs waits for the next Drain, so a request
  // that posts itself again cannot keep the owner here forever. The lock is not
  // held while requests run - a request is allowed to post.
  int Drain(Target& target) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      running_.swap(pending_);
    }
    const int count = static_cast<int>(running_.size());
    for (Request& request : running_)
      request(target);
    running_.clear();
    return count;
  }

  // Any thread; stale the moment it returns, so only for "is there anything to
  // do" rather than for a decision another thread's post could change.
  bool empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pending_.empty();
  }

 private:
  Doorbell* doorbell_;
  mutable std::mutex mutex_;
  std::vector<Request> pending_;
  std::vector<Request> running_;   // the owning thread's; only touched in Drain
};

}  // namespace host
}  // namespace emulation
