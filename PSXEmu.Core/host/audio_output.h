// The audio thread: owns the sound device and keeps it fed from a SampleRing,
// at the device's own pace (Docs/Threading-Plan.md, phase 3).
//
// The device asks, this answers: WaitForRoom, WritableFrames, read that many
// from the ring, write them. Whatever the ring cannot supply is written as
// silence, so the device is never starved into playing whatever it held last -
// and a shortfall is counted, on the ring, while the machine is running, which
// is when a shortfall is a fault rather than the design.
//
// The device is opened on this thread, not handed in: WASAPI's COM objects, the
// "Pro Audio" scheduling registration and DirectSound's buffers all belong to
// the thread that created them.
#pragma once

#include "audio/iaudioengine.h"
#include "host/doorbell.h"
#include "host/request_queue.h"
#include "host/sample_ring.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace emulation {
namespace host {

class AudioOutput {
 public:
  // Opens a device on the audio thread, by settings key - "wasapi" or
  // "dsound" - falling back however it likes. Null if nothing opened;
  // `*opened` is set to what did, or cleared.
  typedef std::function<std::unique_ptr<IAudioEngine>(const std::string& backend,
                                                      std::string* opened)>
      EngineFactory;

  // Told, on the audio thread, what is open after every open or switch: the
  // settings key, or the empty string for nothing at all.
  typedef std::function<void(const std::string& opened)> OpenedCallback;

  AudioOutput(EngineFactory factory, OpenedCallback on_opened);
  ~AudioOutput();

  // The ring this thread drains - the machine's to fill. Owned here because it
  // is this thread's inbox, the way the frame mailbox is the video thread's.
  SampleRing& samples() { return ring_; }

  AudioOutput(const AudioOutput&) = delete;
  AudioOutput& operator=(const AudioOutput&) = delete;

  // Starts the thread, which opens `backend` first thing.
  void Start(const std::string& backend);

  // Stops the thread and closes the device. Returns once it has.
  void Stop();

  // Any thread. Closes the open device and opens `backend` instead; the
  // OpenedCallback says how that went.
  void SwitchBackend(const std::string& backend);

  // Any thread: frames the device holds that it has not played, as of the last
  // time it was fed. For the timings readout.
  int device_buffered_frames() const {
    return device_buffered_.load(std::memory_order_relaxed);
  }

  // Any thread: how many times the device has asked for data, and how many
  // frames it has been given. A device that has stopped asking shows up here
  // and nowhere else - the ring simply backs up.
  uint64_t wakes() const { return wakes_.load(std::memory_order_relaxed); }
  uint64_t frames_to_device() const {
    return frames_to_device_.load(std::memory_order_relaxed);
  }

 private:
  void Run(std::string backend);
  void Open(const std::string& backend);
  void Close();

  // 8192 frames, 186 ms: several frames' worth of bursts on top of the target
  // the machine holds it at, so it only ever fills when nothing is draining it.
  SampleRing ring_{8192};
  EngineFactory factory_;
  OpenedCallback on_opened_;

  Doorbell doorbell_;
  RequestQueue<AudioOutput> requests_;

  std::unique_ptr<IAudioEngine> engine_;   // the audio thread's
  std::vector<int16_t> scratch_;           // the audio thread's

  std::thread thread_;
  std::atomic<bool> stop_{false};
  std::atomic<int> device_buffered_{0};
  std::atomic<uint64_t> wakes_{0};
  std::atomic<uint64_t> frames_to_device_{0};
};

}  // namespace host
}  // namespace emulation
