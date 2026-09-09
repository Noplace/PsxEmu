// frame_limiter_test - checks that platform/frame_limiter.h actually holds a
// loop to a rate, which is the thing that decides how fast the emulator runs
// for anyone using it.
//
// Worth its own harness because the front end's speed cannot be measured from
// a headless run at all: it is set by the monitor's refresh rate and the sound
// device, neither of which exists here. What *can* be checked here is the
// piece that takes that decision away from both of them.

#include "platform/frame_limiter.h"

#include <chrono>
#include <cstdio>
#include <thread>

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

}  // namespace

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

  printf("\n%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
