// The machine's thread: runs the emulated PlayStation, and nothing else
// touches it while it does (Docs/Threading-Plan.md, phase 5).
//
// Each iteration: run what was asked for (boot, reset, save, settings), take
// the input, run one frame, hand the frame to the video thread and the sound
// to the audio thread, sleep out the rest of the frame. The loop App::MainLoop
// used to run between messages, minus everything that is not emulation -
// presenting, the sound device and the pads each have a thread of their own.
//
// `psx/` stays exactly as single-threaded as it was: this is the only thread
// that calls into a System, so every baseline in Docs/Test-Suite.md still
// means what it did, and host_test checks that a threaded run lands on the
// same instruction and the same checksum as boot_runner.
#pragma once

#include "host/doorbell.h"
#include "host/frame_mailbox.h"
#include "host/input_exchange.h"
#include "host/request_queue.h"
#include "host/sample_ring.h"
#include "platform/frame_limiter.h"
#include "platform/speed_resampler.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <thread>
#include <vector>

namespace emulation {
namespace psx {
class System;
struct EmuConfig;
}  // namespace psx

namespace host {

// Why the machine is not running. It runs only while none is set, so a pause
// the user asked for survives a menu opening and closing on top of it.
enum PauseReason : uint32_t {
  kPausedByUser = 1u << 0,    // Space, Emulation > Pause - and nothing booted yet
  kPausedForMenu = 1u << 1,   // a menu is open and EmuConfig::pause_in_menus asks for it
  kPausedByDebugger = 1u << 2,   // halted at a breakpoint or a step - psx/debugger.h
};

// Means per emulated frame over the last second or so - the title's readout.
struct MachineReport {
  bool paused = false;
  double fps = 0.0;          // emulated frames per wall-clock second
  double refresh_hz = 0.0;   // the emulated display's rate; fps == refresh_hz is 100%
  double emulate_ms = 0.0;   // running the frame
  double handoff_ms = 0.0;   // taking the input, handing over the frame and the sound
  double idle_ms = 0.0;      // what the frame limiter slept off - the headroom
  int audio_queued_frames = 0;         // the sample ring's level, now
  uint64_t audio_short_frames = 0;     // silence the audio thread had to make up, ever
  uint64_t audio_dropped_frames = 0;   // samples the ring had no room for, ever
  uint64_t frames_dropped = 0;         // frames the video thread never showed, ever
  uint64_t instructions = 0;           // stepped since the thread started
};

class Machine {
 public:
  struct Hooks {
    // Before each frame: this frame's reading of the host's devices, to map
    // onto the ports the way the front end's settings say.
    std::function<void(psx::System&, const HostInput&)> apply_input;

    // After each frame, with the machine itself - a harness uses it to stop at
    // an exact frame; the front end has no need of it.
    std::function<void(class Machine&)> after_frame;

    // About once a second while running, and once on pausing.
    std::function<void(const MachineReport&)> report;
    // The debugger halted the machine - mid-frame, before the instruction at its pc. The machine
    // is paused for kPausedByDebugger and keeps answering requests; the half-run frame is not
    // published. To go on, a request steps or resumes the debugger and clears that reason.
    std::function<void(class Machine&)> halted;
  };

  // `video` and `audio` are the output threads' inboxes. `system` must outlive
  // the thread, and nothing else may call into it while the thread runs.
  Machine(psx::System* system, FrameMailbox* video, SampleRing* audio, Hooks hooks);
  ~Machine();

  Machine(const Machine&) = delete;
  Machine& operator=(const Machine&) = delete;

  // Starts the thread, paused for `pause_reasons` if any are given.
  void Start(uint32_t pause_reasons);

  // Stops the thread. Returns once it has - at most a frame later.
  void Stop();

  // ---- Any thread --------------------------------------------------------

  // Runs `request` on the machine's thread, between frames - or at once, if
  // it is paused. The only way into the machine from outside.
  void Post(std::function<void(Machine&)> request) { requests_.Post(std::move(request)); }

  // The input thread's end of the exchange.
  InputExchange& input() { return input_; }

  // ---- The machine's thread only: inside a request or a hook -------------

  psx::System& system() { return *system_; }

  void SetPaused(uint32_t reason, bool on);
  uint32_t pause_reasons() const { return pause_reasons_; }

  // Replaces the machine's settings with the front end's. A change of speed
  // or frame limiter restarts the pacing, since both were pacing to the old
  // rate.
  void ApplyConfig(const psx::EmuConfig& config);

  // Forgets the frame limiter's deadline and the resampler's position - after
  // anything that moved the machine's clock without running frames, a state
  // load for one.
  void ResetPacing();

  // Video > View VRAM: ship the whole of VRAM instead of the display.
  void set_view_vram(bool on) { view_vram_ = on; }

  // Instructions stepped since the thread started. The machine's thread, or
  // anyone once Stop has returned.
  uint64_t instructions() const { return instructions_; }

  // The level the sample ring is held at: 40 ms. Enough to ride out the
  // machine's once-a-frame bursts - 17 ms of sound at a time - plus the device
  // taking its 10 ms, with room to spare.
  static const int kAudioTargetFrames = 44100 * 40 / 1000;

  // How long a frame may run before it is given up on. A machine that has
  // stopped producing frames at all - the display off - would otherwise never
  // come back to its requests, or to Stop.
  static const uint64_t kMaxInstructionsPerFrame = 8000000;

 private:
  typedef std::chrono::steady_clock Clock;

  void Run();
  void RunOneFrame();
  void PublishFrame();
  void PumpAudio();
  void Pace();
  void Resume();
  void Report(bool paused);

  psx::System* system_;
  FrameMailbox* video_;
  SampleRing* audio_;
  Hooks hooks_;

  Doorbell doorbell_;
  RequestQueue<Machine> requests_{&doorbell_};
  InputExchange input_;

  // Everything below is the machine thread's.
  uint32_t pause_reasons_ = 0;
  bool view_vram_ = false;
  utilities::FrameLimiter limiter_;
  utilities::SpeedResampler resampler_;
  // The speed the machine is actually managing, in multiples of real time,
  // smoothed over about a fifth of a second. The sound is resampled by this
  // rather than by the setting: at a speed the host cannot reach the two are
  // not the same, and resampling by the setting hands the device fewer samples
  // than it needs for every second it runs. See PumpAudio.
  double achieved_speed_ = 1.0;

  std::vector<int16_t> scratch_;     // one read of the SPU's samples
  std::vector<int16_t> resampled_;   // the same, stretched for the speed
  uint64_t frame_number_ = 0;        // frames published
  uint64_t instructions_ = 0;
  // Instructions into the frame being run. Kept across a debugger halt, which returns from
  // RunOneFrame mid-frame: restarting it would reset the per-frame guard on every step.
  uint64_t frame_instructions_ = 0;

  Clock::time_point report_since_;
  int report_frames_ = 0;
  double report_emulate_ms_ = 0.0;
  double report_handoff_ms_ = 0.0;
  double report_idle_ms_ = 0.0;

  std::thread thread_;
  std::atomic<bool> stop_{false};
};

}  // namespace host
}  // namespace emulation
