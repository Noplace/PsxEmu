// The machine's thread: runs the emulated console, and nothing else
// touches it while it does (Docs/Threading-Plan.md, phase 5).
//
// Each iteration: run what was asked for (boot, reset, save, settings), take
// the input, run one frame, hand the frame to the video thread and the sound
// to the audio thread, sleep out the rest of the frame. The loop App::MainLoop
// used to run between messages, minus everything that is not emulation -
// presenting, the sound device and the pads each have a thread of their own.
//
// Which console it runs it knows only through ICore (emucore/core.h), which is the only thing
// that calls into the console for it. The console stays exactly as single-threaded as it was:
// this is the only thread that calls into it, so every baseline in Docs/Test-Suite.md still
// means what it did, and host_test checks that a threaded run lands on the same instruction and
// the same checksum as boot_runner.
#pragma once

#include "emucore/core.h"
#include "emucore/host/doorbell.h"
#include "emucore/host/frame_mailbox.h"
#include "emucore/host/input_exchange.h"
#include "emucore/host/latency_markers.h"
#include "emucore/host/request_queue.h"
#include "emucore/host/sample_ring.h"
#include "emucore/timing/display_sync.h"
#include "emucore/timing/frame_limiter.h"
#include "emucore/timing/speed_resampler.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

namespace emulation {
namespace state {
class RewindBuffer;
}  // namespace state

namespace host {

// What the machine paces and keeps by: the part of a console's settings that is the machine's
// rather than the console's (PSXEmu: psx::MachineConfigFrom, from EmuConfig).
struct MachineConfig {
  // Times the console's own rate: 2.0 runs it twice as fast. Not above 16.
  float emulation_speed = 1.0f;
  // Off runs the machine as fast as it goes (Tab held, say).
  bool frame_limiter = true;
  // Settings > Video > Frame Pacing: "display" runs in step with the display the picture is on
  // when that is near enough the console's rate (emucore/timing/display_sync.h); anything else,
  // the console's own clock.
  std::string frame_pacing = "console";
  // Frames run ahead of each one shown, 0 for none; held to kMaxRunAhead.
  int run_ahead = 0;
  static const int kMaxRunAhead = 3;
  // Rewind: a state kept every this many frames, 0 for off, in this much memory.
  int rewind_interval = 0;
  int rewind_budget_mb = 256;
};

// Why the machine is not running. It runs only while none is set, so a pause
// the user asked for survives a menu opening and closing on top of it.
enum PauseReason : uint32_t {
  kPausedByUser = 1u << 0,    // Space, Emulation > Pause - and nothing booted yet
  kPausedForMenu = 1u << 1,   // a menu is open and EmuConfig::pause_in_menus asks for it
  kPausedByDebugger = 1u << 2,   // halted at a breakpoint or a step - debug/debugger.h
  kPausedForPicker = 1u << 3,    // the save-state picker is open over the picture
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
  uint64_t instructions = 0;           // stepped since the thread started, real frames only
  // Frames run ahead of each one shown (MachineConfig::run_ahead) - 0 when it is off, or held
  // off: the core too slow for it (the PSX's interpreter), the debugger armed, a movie playing.
  int run_ahead = 0;
  // Rewind (MachineConfig::rewind_interval): whether Backspace has it stepping back now, and how
  // many seconds of play its store holds.
  bool rewinding = false;
  double rewind_seconds = 0.0;
  // The rest of this group is the core's to fill (ICore::FillReport), and stays as it is for a
  // core with none.
  // Which rasteriser is drawing, and if the hardware one was asked for and is not - it could
  // not be made, or its graphics card went - why.
  bool hardware_raster = false;
  std::string raster_error;
  // The machine's again: whether the last frame's picture was handed over on the graphics card
  // rather than in memory (emucore/image/shared_picture.h).
  bool shared_picture = false;
  // Settings > Video > Frame Pacing, "Match the display" (emucore/timing/display_sync.h): the display's
  // refresh as the front end last gave it, 0 if it gave none; and the refreshes each frame is
  // shown for while the machine runs in step with it, 0 while it does not - another mode, a
  // display too far from the game's rate, another speed. `pace_ratio` is how much faster than
  // the console that runs: 1.003 at 60 Hz.
  double display_hz = 0.0;
  int display_refreshes = 0;
  double pace_ratio = 1.0;
  // The core's: textures (Docs/Texture-Upscaling-Plan.md, phase 2), while dumping or replacing -
  // the textures named so far, written so far, the replacements the pack has, and the textured
  // jobs drawn with one since it was opened.
  bool textures = false;
  uint64_t textures_named = 0;
  uint64_t textures_dumped = 0;
  uint64_t replacements = 0;
  uint64_t jobs_replaced = 0;
  std::string replacement_error;   // why the pack could not be opened, if it could not
};

// One frame's worth of timing, as the machine thread measured it - what the front end's
// performance graphs are drawn from. Everything in wall-clock milliseconds.
struct FrameSample {
  float frame_ms = 0.0f;      // start of this frame to start of the next: 1000 / this is its fps
  float emulate_ms = 0.0f;    // running it
  float handoff_ms = 0.0f;    // input in, picture and sound out
  float idle_ms = 0.0f;       // the frame limiter's sleep - the headroom
  float refresh_hz = 0.0f;    // what a frame is meant to take: 1000 / this
  uint32_t audio_queued_frames = 0;   // the sample ring's level after this frame
  uint32_t instructions = 0;          // stepped during this frame
};

class Machine {
 public:
  struct Hooks {
    // Before each frame: this frame's reading of the host's devices, to map onto the console's
    // ports the way the front end's settings say - into the console the front end holds itself.
    std::function<void(const HostInput&)> apply_input;

    // After each frame, with the machine itself - a harness uses it to stop at
    // an exact frame; the front end has no need of it. Once a frame shown: with run-ahead
    // (MachineConfig::run_ahead), after the real frame and before any frame ahead, so what it
    // reads - the BIOS console's text, a card's writes - is the real frame's, once.
    std::function<void(class Machine&)> after_frame;

    // After every frame emulated, real or run ahead: what belongs to the game's own frames,
    // which the frames ahead must have too - cheats.
    std::function<void(class Machine&)> after_each_frame;

    // Asked once a frame on the machine's thread: false holds run-ahead off - while a movie
    // plays, whose input is by frame number. Unset, it may.
    std::function<bool()> may_run_ahead;
    // The same for rewind: false holds it off, keeping no states and stepping back through none.
    std::function<bool()> may_rewind;
    // After each step back while rewinding, the state it loaded shown. A harness counts them.
    std::function<void(class Machine&)> rewound;

    // About once a second while running, and once on pausing.
    std::function<void(const MachineReport&)> report;
    // Every frame, on the machine's thread: what it took. Kept cheap by whoever takes it -
    // the front end copies it into a ring its video thread reads (ui/overlay).
    std::function<void(const FrameSample&)> frame_done;
    // The debugger halted the machine - mid-frame, before the instruction at its pc. The machine
    // is paused for kPausedByDebugger and keeps answering requests; the half-run frame is not
    // published. To go on, a request steps or resumes the debugger and clears that reason.
    std::function<void(class Machine&)> halted;
    // Settings > Video > Frame Pacing, "Match the display": what the front end knows of the
    // display the picture is on, asked once a frame on the machine's thread. False when it knows
    // nothing, and the console's own clock paces the machine.
    std::function<bool(timing::DisplayTiming*)> display_timing;
  };

  // `video` and `audio` are the output threads' inboxes. `core` must outlive
  // the thread, and nothing else may call into its console while the thread runs. `config` is
  // what the machine paces and keeps by until ApplyConfig changes it.
  Machine(ICore* core, const MachineConfig& config, FrameMailbox* video, SampleRing* audio,
          Hooks hooks);
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

  // The console it runs: the front end's own (PSXEmu: psx::CoreOf).
  ICore& core() { return *core_; }

  void SetPaused(uint32_t reason, bool on);
  uint32_t pause_reasons() const { return pause_reasons_; }

  // Replaces the machine's settings with the front end's. A change of speed, frame limiter or
  // Frame Pacing restarts the pacing, since all three were pacing to the old rate. Before Start
  // too, from the thread that will start it.
  void ApplyConfig(const MachineConfig& config);

  // Forgets the frame limiter's deadline and the resampler's position - after
  // anything that moved the machine's clock without running frames, a state
  // load for one.
  void ResetPacing();

  // Ship the console's video memory, as it holds it, instead of its picture (PSXEmu: View > VRAM;
  // ICore::FillFrame).
  void set_raw_view(bool on) { raw_view_ = on; }

  // Rewind (MachineConfig::rewind_interval, Docs/Rewind-and-Run-Ahead-Plan.md): while on, each frame
  // shown is a step back - the state kept before the last one loaded and shown, the sound muted -
  // instead of a frame run. Off, the machine plays on from wherever that left it.
  void set_rewinding(bool on) { rewinding_ = on; }
  // The machine has jumped somewhere a step back would not lead - a reset, a boot, a state loaded
  // from a file, a movie started: the states kept so far are thrown away.
  void ForgetHistory();
  // For a harness: every state kept for rewind waits until the store has it
  // (state::RewindBuffer::WaitIdle), so none is replaced by the next before the store's worker
  // gets to it, and each step back is exactly one kept state further. Without it - the front
  // end's way - the machine never waits on the worker, and a state it falls behind on is dropped:
  // the store keeps fewer, never a broken chain. Set before Start.
  void set_rewind_keeps_every_state(bool on) { rewind_keeps_every_state_ = on; }

  // NVIDIA Reflex's markers while DLSS Frame Generation runs (emucore/host/latency_markers.h), null for
  // none. Outlives the machine, or is set to null first; the machine's thread calls it.
  void set_latency_markers(LatencyMarkers* markers) { latency_ = markers; }

  // Instructions stepped since the thread started. The machine's thread, or
  // anyone once Stop has returned.
  uint64_t instructions() const { return instructions_; }

  // The level the sample ring is held at: 40 ms of the core's sound. Enough to ride out the
  // machine's once-a-frame bursts - 17 ms of sound at a time - plus the device taking its 10 ms,
  // with room to spare.
  int audio_target_frames() const { return audio_target_frames_; }

  // How long a frame may run before it is given up on. A machine that has
  // stopped producing frames at all - the display off - would otherwise never
  // come back to its requests, or to Stop.
  static const uint64_t kMaxInstructionsPerFrame = 8000000;

 private:
  typedef std::chrono::steady_clock Clock;

  void Run();
  void RunOneFrame();
  // Run-ahead (MachineConfig::run_ahead, Docs/Rewind-and-Run-Ahead-Plan.md): how many frames to run
  // ahead of this one, 0 when it is off or held off; and running them - the state kept, the
  // frames run on the same input, the last one's picture published, the state loaded back.
  int RunAheadFrames();
  void RunAhead(int frames);
  // Rewind: whether it is on and may keep states now; keeping one at the end of a real frame,
  // every rewind_interval; and a step back while Backspace is held - false when there is nothing
  // to step back to, and the machine runs the frame instead.
  bool RewindAllowed() const;
  void KeepForRewind();
  bool StepBack();
  void PublishFrame();
  void PumpAudio();
  void Pace();
  void Resume();
  void Report(bool paused);

  ICore* core_;
  FrameMailbox* video_;
  SampleRing* audio_;
  Hooks hooks_;
  // One read of the core's sound: a frame at 30 fps, more than a frame ever makes. A frame that
  // ran long - the instruction guard - leaves the rest for the next read, which PumpAudio does
  // straight away.
  const int scratch_frames_;
  const int audio_target_frames_;

  Doorbell doorbell_;
  RequestQueue<Machine> requests_{&doorbell_};
  InputExchange input_;

  // Everything below is the machine thread's.
  MachineConfig config_;
  uint32_t pause_reasons_ = 0;
  bool raw_view_ = false;
  LatencyMarkers* latency_ = nullptr;
  // The picture whose simulation is under way, 0 before the first starts (latency_ only).
  uint32_t latency_picture_ = 0;
  timing::FrameLimiter limiter_;
  timing::SpeedResampler resampler_;
  // The speed the machine is actually managing, in multiples of real time,
  // smoothed over about a fifth of a second and never above the setting. Only
  // used for the sound while falling_behind_ is set - see PumpAudio.
  double achieved_speed_ = 1.0;
  // The display the machine was last in step with (emucore/timing/display_sync.h), and the refresh
  // it saw. Its ratio is the speed the sound is resampled for, beside the setting's.
  timing::DisplayMatch display_match_;
  double display_hz_ = 0.0;
  // Where in a refresh each frame starts once in step: just after a vblank, so the frame is
  // drawn and handed over long before the next one, whatever it costs to emulate.
  static constexpr std::chrono::microseconds kDisplayPhase{1000};
  // Frames running whose present waited behind a queued one (Pace); negative while the frame
  // left out to empty the queue takes effect.
  int queued_presents_ = 0;
  static const int kQueuedPresentsToDrain = 15;
  // The speed asked for, times the display's ratio while in step with it.
  double PacedSpeed() const;
  // Whether the machine has stopped reaching the speed it was asked for: the
  // limiter has had nothing to sleep off for kBehindFrames frames running. Not
  // one slow frame - the first frame after a resume is unpaced by design, and
  // a seek or a heavy scene can take one long frame - but a sustained shortfall.
  // Cleared the same way, by the limiter sleeping for kBehindFrames running.
  bool falling_behind_ = false;
  int behind_frames_ = 0;
  int ahead_frames_ = 0;
  static const int kBehindFrames = 30;

  std::vector<int16_t> scratch_;     // one read of the core's samples
  std::vector<int16_t> resampled_;   // the same, stretched for the speed
  uint64_t frame_number_ = 0;        // frames published
  bool published_shared_ = false;    // the last one's picture was on the card
  uint64_t instructions_ = 0;
  uint64_t frame_start_instructions_ = 0;   // instructions_ as the last FrameSample left it
  // Instructions into the frame being run. Kept across a debugger halt, which returns from
  // RunOneFrame mid-frame: restarting it would reset the per-frame guard on every step.
  uint64_t frame_instructions_ = 0;

  // Run-ahead's state, and rewind's - the one being taken for its store, or stepped back to -
  // kept and reused frame to frame. Each made by the core on first use.
  std::unique_ptr<MemoryState> ahead_state_;
  std::unique_ptr<MemoryState> rewind_state_;
  // Rewind's store, made when rewind is turned on and freed when it is turned off, and where the
  // machine is in its interval.
  std::unique_ptr<state::RewindBuffer> rewind_;
  int rewind_countdown_ = 0;
  bool rewinding_ = false;
  bool rewind_keeps_every_state_ = false;
  bool was_rewinding_ = false;
  double ahead_ms_ = 0.0;      // the last frame's frames ahead, emulating them
  int run_ahead_now_ = 0;      // frames run ahead of the last frame shown
  bool run_ahead_failed_ = false;   // a load back failed once: held off from then on

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
