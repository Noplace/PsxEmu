// The video thread: owns whatever draws the frames, and shows the newest one
// the machine has finished (Docs/Threading-Plan.md, phase 4).
//
// Waiting for vsync happens here, and only here. While presenting was on the
// machine's thread, a monitor slower than the machine - 200% on a 60 Hz
// display - paced the machine itself; now it only decides which frames are
// seen, and the mailbox counts the ones that are not.
//
// The drawing itself is the front end's: a Presenter, created on this thread
// by a factory, because a Direct3D device is best created by the thread that is
// going to use it and must only ever be used by one.
#pragma once

#include "host/doorbell.h"
#include "host/frame_mailbox.h"
#include "host/request_queue.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <thread>

namespace emulation {
namespace host {

// Implemented by the front end; every call is on the video thread.
class Presenter {
 public:
  virtual ~Presenter() = default;

  // Shows `frame`, which stays valid and unchanged until this returns.
  virtual void Present(const VideoFrame& frame) = 0;

  // The window's client area changed.
  virtual void Resize(int width, int height) = 0;
};

class VideoOutput {
 public:
  // Creates the presenter, on the video thread. Null if it could not - the
  // factory is the one that knows why, and says so itself.
  typedef std::function<std::unique_ptr<Presenter>()> PresenterFactory;

  explicit VideoOutput(PresenterFactory factory);
  ~VideoOutput();

  VideoOutput(const VideoOutput&) = delete;
  VideoOutput& operator=(const VideoOutput&) = delete;

  void Start();

  // Stops the thread, which destroys the presenter on its way out. Returns
  // once it has - so a caller can then destroy the window it drew into.
  void Stop();

  // This thread's inbox, for the machine to fill.
  FrameMailbox& frames() { return frames_; }

  // Any thread. Runs `request` on the video thread, before the next frame.
  void Post(std::function<void(VideoOutput&)> request) { requests_.Post(std::move(request)); }

  // ---- The video thread only: inside a request ---------------------------

  // Null if the factory could not create one.
  Presenter* presenter() { return presenter_.get(); }

  // Shows the current frame again - after a resize, or a filter change, when
  // nothing new has arrived to show it with.
  void PresentAgain();

  // ---- Any thread --------------------------------------------------------

  // Presents since the last call, and the milliseconds they took between
  // them - the timings readout's "present". Resets both.
  void TakeTiming(uint64_t* presents, double* total_ms);

 private:
  void Run();
  void Show(const VideoFrame& frame);

  PresenterFactory factory_;

  // Declared before what rings it.
  Doorbell doorbell_;
  FrameMailbox frames_{&doorbell_};
  RequestQueue<VideoOutput> requests_{&doorbell_};

  std::unique_ptr<Presenter> presenter_;   // the video thread's

  std::thread thread_;
  std::atomic<bool> stop_{false};
  std::atomic<uint64_t> presents_{0};
  std::atomic<uint64_t> present_ns_{0};
};

}  // namespace host
}  // namespace emulation
