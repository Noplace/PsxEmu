#include "host/machine.h"

#include "psx/psx.h"

#include <utility>

namespace emulation {
namespace host {

namespace {

// One read of the SPU's buffer: a frame at 30 fps, more than a frame ever
// makes. A frame that ran long - the instruction guard - leaves the rest for
// the next read, which PumpAudio does straight away.
const int kScratchFrames = psx::Spu::kSampleRate / 30;

double Ms(std::chrono::steady_clock::duration d) {
  return std::chrono::duration<double, std::milli>(d).count();
}

}  // namespace

Machine::Machine(psx::System* system, FrameMailbox* video, SampleRing* audio, Hooks hooks)
    : system_(system), video_(video), audio_(audio), hooks_(std::move(hooks)) {
  scratch_.resize(static_cast<size_t>(kScratchFrames) * SampleRing::kChannels);
  // What the trim steers for, and what the audio thread catches up to if the
  // ring ever runs far past it.
  audio_->set_target_frames(kAudioTargetFrames);
}

Machine::~Machine() {
  Stop();
}

void Machine::Start(uint32_t pause_reasons) {
  if (thread_.joinable())
    return;
  pause_reasons_ = pause_reasons;
  stop_.store(false, std::memory_order_release);
  thread_ = std::thread(&Machine::Run, this);
}

void Machine::Stop() {
  if (!thread_.joinable())
    return;
  stop_.store(true, std::memory_order_release);
  doorbell_.Ring();
  thread_.join();
}

void Machine::SetPaused(uint32_t reason, bool on) {
  if (on)
    pause_reasons_ |= reason;
  else
    pause_reasons_ &= ~reason;
}

void Machine::ApplyConfig(const psx::EmuConfig& config) {
  psx::EmuConfig& current = system_->config();
  const bool pacing_changed = current.frame_limiter != config.frame_limiter ||
                              current.emulation_speed != config.emulation_speed;
  current = config;
  if (pacing_changed)
    ResetPacing();
}

void Machine::ResetPacing() {
  limiter_.Reset();
  resampler_.Reset();
}

void Machine::Run() {
  bool idle = true;   // nothing has run yet, so the first frame is a resume
  while (!stop_.load(std::memory_order_acquire)) {
    requests_.Drain(*this);
    if (stop_.load(std::memory_order_acquire))
      break;
    // The debugger's pause follows the debugger: a request that stepped, resumed or reset the
    // machine has un-halted it, and that is all it needs to do to set it running again.
    if ((pause_reasons_ & kPausedByDebugger) != 0 && !system_->debugger().halted())
      pause_reasons_ &= ~kPausedByDebugger;

    if (pause_reasons_ != 0) {
      if (!idle) {
        idle = true;
        audio_->set_producing(false);
        // A save made just before pausing is on disk now, not a second of running later.
        system_->mc(0).Flush();
        system_->mc(1).Flush();
        Report(true);
      }
      // Requests ring the bell, so this is only how long an idle machine goes
      // without looking round - it answers a request the moment it arrives.
      doorbell_.Wait(std::chrono::milliseconds(100));
      continue;
    }
    if (idle) {
      idle = false;
      Resume();
    }

    const Clock::time_point start = Clock::now();
    if (hooks_.apply_input)
      hooks_.apply_input(*system_, input_.Take());
    const Clock::time_point input_taken = Clock::now();
    RunOneFrame();
    const Clock::time_point emulated = Clock::now();
    if (system_->debugger().halted()) {
      // Mid-frame: nothing to publish, no sound to pump, no pace to keep.
      SetPaused(kPausedByDebugger, true);
      if (hooks_.halted)
        hooks_.halted(*this);
      continue;
    }
    PublishFrame();
    PumpAudio();
    // Memory cards go to disk a second after the game stops writing them (psx/mc.h).
    system_->mc(0).OnFrame();
    system_->mc(1).OnFrame();
    if (hooks_.after_frame)
      hooks_.after_frame(*this);
    const Clock::time_point handed_over = Clock::now();
    Pace();
    const Clock::time_point paced = Clock::now();

    report_emulate_ms_ += Ms(emulated - input_taken);
    report_handoff_ms_ += Ms(input_taken - start) + Ms(handed_over - emulated);
    report_idle_ms_ += Ms(paced - handed_over);
    ++report_frames_;
    if (paced - report_since_ >= std::chrono::seconds(1))
      Report(false);
  }
  audio_->set_producing(false);
}

// Runs the machine until the GPU says a frame is finished - the same loop
// boot_runner runs, which is what makes the two comparable.
void Machine::RunOneFrame() {
  const uint64_t target = system_->gpu().frame_count() + 1;
  // Nothing arms the debugger in the middle of a frame - the window's requests are drained
  // between frames - so an unarmed one is asked once here, not once an instruction.
  if (!system_->debugger().armed()) {
    while (system_->gpu().frame_count() < target &&
           frame_instructions_ < kMaxInstructionsPerFrame) {
      system_->StepInstructionUnarmed();
      ++frame_instructions_;
      ++instructions_;
    }
    frame_instructions_ = 0;
    return;
  }
  while (system_->gpu().frame_count() < target &&
         frame_instructions_ < kMaxInstructionsPerFrame) {
    // A halt ran nothing, so it is not an instruction; the frame carries on from here when the
    // debugger lets it, towards the same frame boundary.
    if (!system_->StepInstruction())
      return;
    ++frame_instructions_;
    ++instructions_;
  }
  frame_instructions_ = 0;
}

// The frame is resolved at the start of vblank, which is exactly when
// RunOneFrame returns - so this copies a finished picture, never a half-drawn
// one. About a tenth of a millisecond for 640x480.
void Machine::PublishFrame() {
  VideoFrame& frame = video_->back();
  frame.number = ++frame_number_;
  if (view_vram_) {
    const int width = psx::GpuCore::kVramWidth;
    const int height = psx::GpuCore::kVramHeight;
    const uint16_t* vram = system_->gpu().vram();
    frame.vram.assign(vram, vram + static_cast<size_t>(width) * height);
    frame.is_vram = true;
    frame.width = width;
    frame.height = height;
  } else {
    int width = 0;
    int height = 0;
    const uint32_t* pixels = system_->gpu().framebuffer(width, height);
    frame.pixels.assign(pixels, pixels + static_cast<size_t>(width) * height);
    frame.is_vram = false;
    frame.width = width;
    frame.height = height;
  }
  video_->Publish();
}

// The SPU makes 44,100 samples per *emulated* second and the device drains
// 44,100 per real one, so anything but 100% is resampled on the way out. On
// top of the speed, a trim of at most half a percent holds the ring at its
// target: the frame limiter paces the machine off the host's clock, the sound
// card consumes off its own, and left alone the difference would empty or fill
// the ring every few minutes, forever. Half a percent is about eight cents -
// inaudible - and clamped, so a host that cannot keep up shows up as a
// shortfall rather than as pitch.
void Machine::PumpAudio() {
  for (;;) {
    const int frames = system_->spu().ReadSamples(scratch_.data(), kScratchFrames);
    if (frames <= 0)
      break;
    const double error =
        static_cast<double>(audio_->Available() - kAudioTargetFrames) / kAudioTargetFrames;
    double trim = 1.0 + 0.005 * error;
    if (trim < 0.995) trim = 0.995;
    if (trim > 1.005) trim = 1.005;
    resampled_.clear();
    resampler_.Append(scratch_.data(), frames, system_->config().emulation_speed * trim,
                      &resampled_);
    audio_->Write(resampled_.data(),
                  static_cast<int>(resampled_.size() / SampleRing::kChannels));
  }
}

// Times the speed the machine is run at: 2.0 waits half as long. With the
// limiter off it is paced by nothing, and the deadline is dropped so turning it
// back on starts a fresh one rather than owing however long it ran unpaced.
void Machine::Pace() {
  const psx::EmuConfig& config = system_->config();
  if (config.frame_limiter)
    limiter_.Wait(system_->gpu().refresh_hz() * config.emulation_speed);
  else
    limiter_.Reset();
}

// Back from a pause - or starting. Nothing to catch up on, and the ring is put
// back at its target with silence, so the rate trim - half a percent at most -
// does not spend seconds climbing back up from empty.
void Machine::Resume() {
  ResetPacing();
  const int missing = kAudioTargetFrames - audio_->Available();
  if (missing > 0)
    audio_->WriteSilence(missing);
  audio_->set_producing(true);
  report_since_ = Clock::now();
  report_frames_ = 0;
  report_emulate_ms_ = report_handoff_ms_ = report_idle_ms_ = 0.0;
}

void Machine::Report(bool paused) {
  const Clock::time_point now = Clock::now();
  MachineReport report;
  report.paused = paused;
  report.refresh_hz = system_->gpu().refresh_hz();
  if (!paused && report_frames_ > 0) {
    const double seconds = std::chrono::duration<double>(now - report_since_).count();
    const double n = static_cast<double>(report_frames_);
    report.fps = seconds > 0.0 ? n / seconds : 0.0;
    report.emulate_ms = report_emulate_ms_ / n;
    report.handoff_ms = report_handoff_ms_ / n;
    report.idle_ms = report_idle_ms_ / n;
  }
  report.audio_queued_frames = audio_->Available();
  report.audio_short_frames = audio_->short_frames();
  report.audio_dropped_frames = audio_->dropped_frames();
  report.frames_dropped = video_->dropped();
  report.instructions = instructions_;
  if (hooks_.report)
    hooks_.report(report);

  report_since_ = now;
  report_frames_ = 0;
  report_emulate_ms_ = report_handoff_ms_ = report_idle_ms_ = 0.0;
}

}  // namespace host
}  // namespace emulation
