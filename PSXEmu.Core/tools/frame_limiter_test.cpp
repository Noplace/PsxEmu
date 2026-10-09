// frame_limiter_test - checks that platform/frame_limiter.h actually holds a
// loop to a rate, which is the thing that decides how fast the emulator runs
// for anyone using it.
//
// Worth its own harness because the front end's speed cannot be measured from
// a headless run at all: it is set by the monitor's refresh rate and the sound
// device, neither of which exists here. What *can* be checked here is the
// piece that takes that decision away from both of them.

#include "platform/display_sync.h"
#include "platform/frame_limiter.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>

namespace {

int failures = 0;
int checks = 0;

void Check(bool ok, const char* what) {
  ++checks;
  printf("  %-64s %s\n", what, ok ? "ok" : "FAILED");
  if (!ok)
    ++failures;
}

// Runs `frames` iterations through the limiter at `hz`, with `work_ms` of
// pretend emulation in each, and returns the rate actually achieved.
double MeasureRate(double hz, int frames, int work_ms) {
  utilities::FrameLimiter limiter;
  // One warm-up frame: the first Wait only starts the deadline, so counting it
  // would report a rate one frame short over a short run.
  limiter.Wait(hz);

  const auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < frames; ++i) {
    if (work_ms > 0)
      std::this_thread::sleep_for(std::chrono::milliseconds(work_ms));
    limiter.Wait(hz);
  }
  const double elapsed =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
          .count();
  return frames / elapsed;
}

// The spacing between consecutive frames, in milliseconds, sorted.
//
// The rate checks above average over many frames, and an average hides the
// thing that matters most for sound: a limiter that overshoots one frame by 13
// ms and then runs the next one immediately to catch up is exactly on rate and
// delivers audio in bursts 0 to 30 ms apart. That is what this one did, on a
// machine where Sleep(1) really takes 15 ms, and every rate check passed.
std::vector<double> MeasureSpacing(double hz, int frames) {
  utilities::FrameLimiter limiter;
  limiter.Wait(hz);
  std::vector<double> spacing;
  auto last = std::chrono::steady_clock::now();
  for (int i = 0; i < frames; ++i) {
    limiter.Wait(hz);
    const auto now = std::chrono::steady_clock::now();
    spacing.push_back(std::chrono::duration<double, std::milli>(now - last).count());
    last = now;
  }
  std::sort(spacing.begin(), spacing.end());
  return spacing;
}

}  // namespace

// Frame Pacing's "Match the display" (platform/display_sync.h): which displays a game can be run
// in step with, and at what rate.
void TestDisplayMatch() {
  printf("display match\n");
  const double ntsc = 59.8261;   // 53.693175 MHz / (3413 x 263)
  const double pal = 49.7607;
  using utilities::MatchDisplay;

  const utilities::DisplayMatch at60 = MatchDisplay(ntsc, 60.0);
  printf("    NTSC at 60 Hz: %d refresh(es), %.4f Hz, ratio %.5f\n", at60.refreshes,
         at60.frame_hz, at60.ratio);
  Check(at60.locked && at60.refreshes == 1 && std::fabs(at60.frame_hz - 60.0) < 1e-9 &&
            at60.ratio > 1.002 && at60.ratio < 1.003,
        "NTSC on a 60 Hz display runs at 60, one refresh a frame, 0.3% fast");
  const utilities::DisplayMatch at5994 = MatchDisplay(ntsc, 59.94);
  Check(at5994.locked && at5994.refreshes == 1 && at5994.ratio > 1.0,
        "and on a 59.94 Hz display, at 59.94");
  const utilities::DisplayMatch at120 = MatchDisplay(ntsc, 119.88);
  Check(at120.locked && at120.refreshes == 2 && std::fabs(at120.frame_hz - 59.94) < 1e-9,
        "at 119.88 Hz, two refreshes a frame");
  const utilities::DisplayMatch at240 = MatchDisplay(ntsc, 240.0);
  Check(at240.locked && at240.refreshes == 4, "at 240 Hz, four");
  Check(!MatchDisplay(ntsc, 144.0).locked && !MatchDisplay(ntsc, 165.0).locked,
        "144 and 165 Hz are no whole multiple: not matched");
  Check(!MatchDisplay(pal, 60.0).locked,
        "PAL on a 60 Hz display is 20% off: left to the console's clock");
  const utilities::DisplayMatch pal50 = MatchDisplay(pal, 50.0);
  Check(pal50.locked && pal50.refreshes == 1 && pal50.ratio < 1.005,
        "PAL on a 50 Hz display runs at 50, 0.5% fast");
  Check(MatchDisplay(pal, 100.0).refreshes == 2, "and on a 100 Hz display, two refreshes a frame");
  Check(!MatchDisplay(ntsc, 30.0).locked, "a display slower than the game is not matched");
  Check(!MatchDisplay(ntsc, 0.0).locked && !MatchDisplay(0.0, 60.0).locked,
        "an unknown rate matches nothing");
  Check(!MatchDisplay(ntsc, 61.5).locked && MatchDisplay(ntsc, 61.0).locked,
        "the line is 2%: 61 Hz is matched, 61.5 is not");
}

// The step that holds each frame's start at one point of the refresh.
void TestPhaseCorrection() {
  printf("phase correction\n");
  typedef std::chrono::steady_clock Clock;
  using std::chrono::microseconds;
  const Clock::duration period = std::chrono::duration_cast<Clock::duration>(
      std::chrono::duration<double>(1.0 / 60.0));
  const Clock::duration target = microseconds(1000);
  const Clock::duration most = microseconds(250);
  const Clock::time_point vblank = Clock::time_point() + std::chrono::seconds(100);
  auto step = [&](Clock::time_point deadline) {
    return utilities::PhaseCorrection(deadline, vblank, period, target, 0.1, most);
  };

  Check(step(vblank + target) == Clock::duration::zero() &&
            step(vblank + target + period * 7) == Clock::duration::zero(),
        "a start on the target, any number of refreshes on, is left alone");
  Check(step(vblank + target + microseconds(1000)) == -microseconds(100),
        "a millisecond late is moved a tenth of it earlier");
  Check(step(vblank + target - microseconds(1000)) == microseconds(100),
        "a millisecond early, a tenth later");
  Check(step(vblank + target + microseconds(6000)) == -most,
        "never more than the step's limit at once");
  Check(step(vblank + target + period - microseconds(500)) > Clock::duration::zero(),
        "just short of the next refresh's target is pulled on to it, not back a whole refresh");
  Check(utilities::PhaseCorrection(vblank, vblank, Clock::duration::zero(), target, 0.1, most) ==
            Clock::duration::zero(),
        "no refresh period, no correction");

  // The loop as the machine runs it: frames a refresh apart at the matched rate, starting at the
  // worst phase there is, and the vblank seen through a thread that wakes up to half a millisecond
  // late. It has to settle within a second and then stay put.
  unsigned seed = 12345;
  auto noise = [&seed]() {   // 0 to 500 microseconds, a wake's lateness
    seed = seed * 1103515245u + 12345u;
    return microseconds((seed >> 8) % 500);
  };
  Clock::time_point deadline = vblank + target + period / 2;
  double worst_settled = 0.0;
  double biggest_move = 0.0;
  for (int frame = 0; frame < 600; ++frame) {
    const Clock::time_point seen = vblank + period * frame + noise();
    const Clock::duration move =
        utilities::PhaseCorrection(deadline, seen, period, target, 0.1, most);
    deadline += period + move;
    if (frame >= 60) {
      long long phase = (deadline - vblank - target).count() % period.count();
      if (phase > period.count() / 2)
        phase -= period.count();
      worst_settled = std::max(worst_settled, std::fabs(phase / 1e6));
      biggest_move = std::max(biggest_move, std::fabs(move.count() / 1e6));
    }
  }
  printf("    after a second: within %.3f ms of the target, moving at most %.3f ms a frame\n",
         worst_settled, biggest_move);
  Check(worst_settled < 0.5, "from half a refresh out, the start settles within a second");
  Check(biggest_move < 0.1, "and then moves by less than a tenth of a millisecond a frame");
}

// FrameLimiter::Shift, which the correction goes through.
void TestShift() {
  printf("limiter shift\n");
  utilities::FrameLimiter limiter;
  limiter.Shift(std::chrono::milliseconds(5));
  Check(!limiter.has_deadline(), "nothing to move before the first frame");
  limiter.Wait(60.0);
  const auto before = limiter.deadline();
  limiter.Shift(std::chrono::milliseconds(5));
  Check(limiter.has_deadline() && limiter.deadline() - before == std::chrono::milliseconds(5),
        "a shift moves the next deadline by exactly that much");
  limiter.Reset();
  Check(!limiter.has_deadline(), "and Reset still drops it");
}

int main() {
  printf("frame limiter\n");

  // NTSC. This is the number that matters: 59.29, not the 165 the display is
  // running at and not the 60 that gets assumed.
  {
    const double rate = MeasureRate(59.2926, 90, 0);
    printf("    59.2926 Hz target, no work: %.2f fps\n", rate);
    Check(rate > 58.0 && rate < 60.5,
          "an idle loop is held to the NTSC frame rate, not run flat out");
  }

  // The same, with each frame doing some of its budget's worth of work - the
  // realistic case, where the limiter waits out the remainder.
  {
    const double rate = MeasureRate(59.2926, 90, 8);
    printf("    59.2926 Hz target, 8ms of work: %.2f fps\n", rate);
    Check(rate > 58.0 && rate < 60.5,
          "work inside the frame comes out of the wait, not on top of it");
  }

  // PAL runs slower, and the limiter has to follow the machine rather than
  // hold one hardcoded rate.
  {
    const double rate = MeasureRate(49.7551, 75, 0);
    printf("    49.7551 Hz target: %.2f fps\n", rate);
    Check(rate > 48.5 && rate < 51.0,
          "PAL is paced at its own rate rather than NTSC's");
  }

  // A host too slow to make the deadline must run slow, not bank the debt and
  // sprint through the next second making it up.
  {
    const auto start = std::chrono::steady_clock::now();
    const double rate = MeasureRate(59.2926, 20, 40);
    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
            .count();
    printf("    40ms of work against a 16.9ms budget: %.2f fps over %.2fs\n",
           rate, elapsed);
    Check(rate > 20.0 && rate < 26.0,
          "a host that cannot keep up runs slow rather than catching up");
  }

  // Each frame on time, not just the average. The emulator's audio is pumped
  // once a frame, so a frame 30 ms late is 30 ms with nothing fed to the sound
  // device - longer than its buffer - and a frame run straight after it to
  // catch up is a burst. Both are heard.
  {
    const double period = 1000.0 / 59.2926;
    const std::vector<double> spacing = MeasureSpacing(59.2926, 180);
    const double p5 = spacing[spacing.size() * 5 / 100];
    const double median = spacing[spacing.size() / 2];
    const double p95 = spacing[spacing.size() * 95 / 100];
    const double worst = spacing.back();
    printf("    frame spacing at 59.2926 Hz (%.2f ms): p5 %.2f  median %.2f  "
           "p95 %.2f  max %.2f ms\n", period, p5, median, p95, worst);
    Check(p5 > period - 3.0 && p95 < period + 3.0,
          "frames are evenly spaced, not bunched to make the average");
    Check(worst < period + 8.0,
          "and no frame is late by half a frame or more");
  }

  // Disabled by a non-positive rate, which is what a caller with no machine
  // to ask passes. It must return at once rather than dividing by zero.
  {
    const auto start = std::chrono::steady_clock::now();
    utilities::FrameLimiter limiter;
    for (int i = 0; i < 1000; ++i)
      limiter.Wait(0.0);
    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
            .count();
    Check(elapsed < 0.1, "a rate of zero disables the limiter instead of hanging");
  }

  // Reset must not leave the next frame trying to make up the gap.
  {
    utilities::FrameLimiter limiter;
    limiter.Wait(59.2926);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    limiter.Reset();
    const auto start = std::chrono::steady_clock::now();
    limiter.Wait(59.2926);
    limiter.Wait(59.2926);
    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
            .count();
    Check(elapsed < 0.05,
          "Reset drops the deadline rather than owing 300ms of frames");
  }

  TestDisplayMatch();
  TestPhaseCorrection();
  TestShift();

  printf("\n%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
