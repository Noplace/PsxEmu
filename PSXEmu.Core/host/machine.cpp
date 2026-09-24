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
  if (pacing_changed) {
    ResetPacing();
    // Start the sound at the new setting rather than letting it slide there over
    // a fifth of a second from the old one.
    achieved_speed_ = config.emulation_speed > 0.0 ? config.emulation_speed : 1.0;
    falling_behind_ = false;
    behind_frames_ = ahead_frames_ = 0;
  }
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

    // What that frame actually cost in real time, against the emulated time it
    // covered: the speed the machine is managing, whatever it was asked for.
    // Measured after Pace so the limiter's sleep counts - a machine with
    // headroom is running at exactly the speed it was told to.
    const double refresh = system_->gpu().refresh_hz();
    const double real_seconds = Ms(paced - start) / 1000.0;
    const double asked =
        system_->config().emulation_speed > 0.0 ? system_->config().emulation_speed : 1.0;
    if (refresh > 0.0 && real_seconds > 0.0) {
      double measured = (1.0 / refresh) / real_seconds;
      // Never above what was asked. A frame only looks faster than that when the
      // limiter skipped its sleep for a reason of its own: the first frame after
      // a resume, which it lets through unpaced to set a fresh deadline, or one
      // catching up behind a late one. Feeding those in is what bent the pitch
      // up on every unpause (bug 92) - that first frame read as twice real time.
      if (measured > asked)
        measured = asked;
      const double kSmoothing = 0.05;
      achieved_speed_ += kSmoothing * (measured - achieved_speed_);
      if (achieved_speed_ < 0.05) achieved_speed_ = 0.05;
      if (achieved_speed_ > 16.0) achieved_speed_ = 16.0;
    }
    // Is the machine keeping up? The limiter sleeping is the evidence: a machine
    // with headroom always has something to sleep off. Half a second of frames
    // without is a real shortfall; one is a seek, or the frame after a resume.
    if (Ms(paced - handed_over) >= 0.5) {
      ++ahead_frames_;
      behind_frames_ = 0;
    } else {
      ++behind_frames_;
      ahead_frames_ = 0;
    }
    if (!falling_behind_ && behind_frames_ >= kBehindFrames)
      falling_behind_ = true;
    else if (falling_behind_ && ahead_frames_ >= kBehindFrames)
      falling_behind_ = false;

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
// 44,100 per real one, so anything but 100% is resampled on the way out.
//
// The ratio is the speed the machine is *managing*, not the one it was asked
// for. Where the host keeps up those are the same number and nothing changes.
// Where it does not they are not, and resampling by the setting starves the
// device: at 300% on a host good for 165% it compressed by three while only
// 1.65 seconds of sound arrived per second, so the device was handed about 55%
// of what it needed and made up the rest with silence - tens of thousands of
// short frames a second. Feeding forward from the measured rate fills it
// exactly, and pitches the sound to the speed the game is really running at.
//
// On top of that, a trim of at most half a percent holds the ring at its
// target: the frame limiter paces the machine off the host's clock, the sound
// card consumes off its own, and left alone the difference would empty or fill
// the ring every few minutes, forever. Half a percent is about eight cents -
// inaudible. It is the feedback on a loop the line above feeds forward, which
// is why it can stay that small.
void Machine::PumpAudio() {
  for (;;) {
    const int frames = system_->spu().ReadSamples(scratch_.data(), kScratchFrames);
    if (frames <= 0)
      break;
    const double error =
        static_cast<double>(audio_->Available() - kAudioTargetFrames) / kAudioTargetFrames;
    // Two regimes, and the line between them is whether the host keeps up.
    //
    // Keeping up - nearly always: resample by exactly the speed asked for, and
    // hold the ring with a trim of at most half a percent, about eight cents.
    // That is what this did before bug 90 and it is inaudible. Bug 90 raised
    // the gain to 0.03 everywhere, and at that gain the trim follows the ring's
    // own fill-and-drain - up by a frame's sound, down by the device's pull,
    // three frames round - so the pitch warbled by about a percent, twenty times
    // a second, in every game (bug 92).
    //
    // Falling behind: resample by the speed actually achieved, which is the only
    // way to hand the device as much sound as it plays, and give the trim the
    // authority to refill a ring the shortfall emptied. The pitch is already off
    // nominal by then, by design - it follows the speed the game really runs at.
    const double asked =
        system_->config().emulation_speed > 0.0 ? system_->config().emulation_speed : 1.0;
    const double base = falling_behind_ ? achieved_speed_ : asked;
    const double gain = falling_behind_ ? 0.03 : 0.005;
    double trim = 1.0 + gain * error;
    if (trim < 1.0 - gain) trim = 1.0 - gain;
    if (trim > 1.0 + gain) trim = 1.0 + gain;

    resampled_.clear();
    resampler_.Append(scratch_.data(), frames, base * trim, &resampled_);
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
  // The frame after this one is unpaced by design, so it proves nothing about
  // whether the machine keeps up. What was learned before the pause stands:
  // pausing does not change what the host can manage.
  behind_frames_ = ahead_frames_ = 0;
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
