#include "host/audio_output.h"

#include <algorithm>
#include <chrono>
#include <utility>

namespace emulation {
namespace host {

namespace {

// How long to wait for the device before looking at the request queue anyway.
// WASAPI signals every period, about 10 ms, so this only matters to a device
// that has stopped.
const int kDeviceWaitMs = 20;

}  // namespace

AudioOutput::AudioOutput(EngineFactory factory, OpenedCallback on_opened)
    : factory_(std::move(factory)),
      on_opened_(std::move(on_opened)),
      requests_(&doorbell_) {}

AudioOutput::~AudioOutput() {
  Stop();
}

void AudioOutput::Start(const std::string& backend) {
  if (thread_.joinable())
    return;
  stop_.store(false, std::memory_order_release);
  thread_ = std::thread(&AudioOutput::Run, this, backend);
}

void AudioOutput::Stop() {
  if (!thread_.joinable())
    return;
  stop_.store(true, std::memory_order_release);
  doorbell_.Ring();
  thread_.join();
}

void AudioOutput::SwitchBackend(const std::string& backend) {
  requests_.Post([backend](AudioOutput& output) {
    output.Close();
    output.Open(backend);
  });
}

void AudioOutput::Open(const std::string& backend) {
  std::string opened;
  engine_ = factory_ ? factory_(backend, &opened) : nullptr;
  if (engine_ != nullptr)
    engine_->Play();
  else
    opened.clear();
  if (on_opened_)
    on_opened_(opened);
}

void AudioOutput::Close() {
  if (engine_ == nullptr)
    return;
  engine_->Shutdown();
  engine_.reset();
  device_buffered_.store(0, std::memory_order_relaxed);
}

void AudioOutput::Run(std::string backend) {
  Open(backend);

  while (!stop_.load(std::memory_order_acquire)) {
    requests_.Drain(*this);

    if (engine_ == nullptr) {
      // No device - neither backend would open. The ring fills and the machine
      // drops what does not fit, which is the right outcome for a machine
      // with nowhere to send its sound; this only has to stay responsive to a
      // switch.
      doorbell_.Wait(std::chrono::milliseconds(50));
      continue;
    }

    engine_->WaitForRoom(kDeviceWaitMs);
    wakes_.fetch_add(1, std::memory_order_relaxed);

    // Far past what the machine steers for - a device that took half a second
    // to open while the machine was already producing, or a machine run with
    // no frame limiter at all. Everything piled up would otherwise play, in
    // order and late, until the half-percent trim worked it off twenty seconds
    // later. Catching up costs one jump; not catching up costs that much
    // latency until something else resets it.
    //
    // Down to twice the target, not to the target itself: the next thing that
    // happens is the device asking for everything it has room for - 30 ms of
    // it, for DirectSound - and landing on the target exactly would leave the
    // ring too thin to cover that plus the machine's next burst. The trim
    // walks the rest off from there.
    const int target = ring_.target_frames();
    if (target > 0 && ring_.Available() > target * 4)
      ring_.Skip(ring_.Available() - target * 2);

    const int room = engine_->WritableFrames();
    if (room > 0) {
      frames_to_device_.fetch_add(static_cast<uint64_t>(room), std::memory_order_relaxed);
      scratch_.resize(static_cast<size_t>(room) * SampleRing::kChannels);
      const int got = ring_.Read(scratch_.data(), room);
      if (got < room) {
        std::fill(scratch_.begin() + static_cast<size_t>(got) * SampleRing::kChannels,
                  scratch_.end(), static_cast<int16_t>(0));
        if (ring_.producing())
          ring_.NoteShort(room - got);
      }
      engine_->WriteFrames(scratch_.data(), room);
    }
    device_buffered_.store(engine_->BufferedFrames(), std::memory_order_relaxed);
  }

  Close();
}

}  // namespace host
}  // namespace emulation
