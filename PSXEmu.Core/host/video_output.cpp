#include "host/video_output.h"

#include <chrono>
#include <utility>

namespace emulation {
namespace host {

VideoOutput::VideoOutput(PresenterFactory factory) : factory_(std::move(factory)) {}

VideoOutput::~VideoOutput() {
  Stop();
}

void VideoOutput::Start() {
  if (thread_.joinable())
    return;
  stop_.store(false, std::memory_order_release);
  thread_ = std::thread(&VideoOutput::Run, this);
}

void VideoOutput::Stop() {
  if (!thread_.joinable())
    return;
  stop_.store(true, std::memory_order_release);
  doorbell_.Ring();
  thread_.join();
}

void VideoOutput::PresentAgain() {
  const VideoFrame* frame = frames_.current();
  if (frame != nullptr)
    Show(*frame);
}

void VideoOutput::TakeTiming(uint64_t* presents, double* total_ms) {
  *presents = presents_.exchange(0, std::memory_order_relaxed);
  *total_ms = static_cast<double>(present_ns_.exchange(0, std::memory_order_relaxed)) / 1e6;
}

void VideoOutput::Show(const VideoFrame& frame) {
  if (presenter_ == nullptr)
    return;
  const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
  presenter_->Present(frame);
  const std::chrono::steady_clock::duration took = std::chrono::steady_clock::now() - start;
  present_ns_.fetch_add(
      static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(took).count()),
      std::memory_order_relaxed);
  presents_.fetch_add(1, std::memory_order_relaxed);
}

void VideoOutput::Run() {
  if (factory_)
    presenter_ = factory_();

  while (!stop_.load(std::memory_order_acquire)) {
    requests_.Drain(*this);

    const VideoFrame* frame = frames_.TakeNew();
    if (frame == nullptr) {
      // A tenth of a second is only the longest this goes without looking at
      // the request queue on its own; a frame or a request rings the bell.
      doorbell_.Wait(std::chrono::milliseconds(100));
      continue;
    }
    Show(*frame);
  }

  // Destroyed here, on the thread that made it - and before Stop returns, so
  // the window it drew into can safely go next.
  presenter_.reset();
}

}  // namespace host
}  // namespace emulation
