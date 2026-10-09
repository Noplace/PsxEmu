// Running the machine in step with the display it is shown on - Settings > Video > Frame Pacing,
// "Match the display".
//
// The console's frame rate is never quite the monitor's: 59.83 Hz progressive NTSC, 59.94
// interlaced, 49.76 PAL, against a 60.00 or 59.95 Hz panel. Paced by its own clock the machine
// drifts against the refresh, and every few seconds a frame lands on two refreshes or none - a
// hitch in any scroll or pan, however even the limiter's spacing is (bug 62 made it even). The
// cure is the one every console-on-a-PC has to make: run the machine at the display's rate, or a
// whole fraction of it, when that is close enough to the console's that the difference cannot be
// heard or seen, and keep each frame's start at the same point in the refresh so it is always
// ready for the same vblank.
//
// Only the arithmetic is here, so it can be tested without a monitor; the front end measures the
// display (PSXEmu.Win32/graphics/display_clock.h) and host::Machine applies the result.
#pragma once

#include <chrono>
#include <cmath>

namespace utilities {

// What the front end knows of the display the picture is on.
struct DisplayTiming {
  typedef std::chrono::steady_clock Clock;
  // The refresh rate, exactly as the display mode states it (59.951 Hz rather than 60). Zero if
  // it is not known, and then nothing is matched.
  double refresh_hz = 0.0;
  // A recent vblank on steady_clock, when the display's refresh can be watched - and then the
  // machine's frames are held at one point in the refresh, not only at its rate.
  bool has_vblank = false;
  Clock::time_point last_vblank;
};

// Whether the machine can run in step with a display, and at what rate.
struct DisplayMatch {
  bool locked = false;
  // Display refreshes per emulated frame: 1 at 60 Hz, 2 at 120, 4 at 240.
  int refreshes = 0;
  // The rate to run the machine at: the display's divided by `refreshes`.
  double frame_hz = 0.0;
  // frame_hz over the console's own rate - the speed it runs at, and the pitch the sound is
  // played at. 1.003 is 0.3% fast, five cents sharp.
  double ratio = 1.0;
};

// How far from the console's own rate the machine may be moved to match. Two percent is about a
// third of a semitone - more than any real pairing needs (60 Hz against NTSC's 59.83 is 0.3%,
// 50 against PAL's 49.76 is 0.5%), and it still refuses a PAL game on a 60 Hz display (20%),
// which cannot be matched by changing speed and is left to the console's clock.
inline constexpr double kDisplayMatchTolerance = 0.02;

// The nearest whole number of refreshes per frame, and whether that rate is within `tolerance`
// of the console's.
inline DisplayMatch MatchDisplay(double console_hz, double display_hz,
                                 double tolerance = kDisplayMatchTolerance) {
  DisplayMatch match;
  if (console_hz <= 0.0 || display_hz <= 0.0)
    return match;
  const int refreshes = static_cast<int>(std::lround(display_hz / console_hz));
  if (refreshes < 1)
    return match;   // a display slower than the game: nothing to step with
  const double frame_hz = display_hz / refreshes;
  const double ratio = frame_hz / console_hz;
  if (std::fabs(ratio - 1.0) > tolerance)
    return match;
  match.locked = true;
  match.refreshes = refreshes;
  match.frame_hz = frame_hz;
  match.ratio = ratio;
  return match;
}

// How far to move the next frame's start so that, refresh after refresh, it falls `target`
// after a vblank. `deadline` is when the next frame starts, `vblank` any recent vblank, `period`
// one refresh.
//
// Only a fraction of the error, `gain`, is taken each frame, and never more than `max_step`: a
// vblank is timestamped by a thread that wakes for it, a few hundred microseconds late and not
// always by the same amount, and correcting each sample in full would move every frame by that
// noise. Taken slowly, the noise averages out and what is left is the true phase. A frame
// moving by a fraction of a millisecond is nothing the sound or the picture can show.
//
// The error is wrapped to half a refresh either way, so the start is pulled to the nearer of
// the two vblanks around it rather than the long way round.
inline std::chrono::steady_clock::duration PhaseCorrection(
    std::chrono::steady_clock::time_point deadline, std::chrono::steady_clock::time_point vblank,
    std::chrono::steady_clock::duration period, std::chrono::steady_clock::duration target,
    double gain, std::chrono::steady_clock::duration max_step) {
  typedef std::chrono::steady_clock::duration Duration;
  if (period <= Duration::zero())
    return Duration::zero();
  const long long p = period.count();
  long long phase = (deadline - vblank - target).count() % p;
  if (phase < 0)
    phase += p;
  if (phase >= p / 2)
    phase -= p;   // now in [-p/2, p/2): how far past the target the start is
  long long step = static_cast<long long>(-static_cast<double>(phase) * gain);
  const long long most = max_step.count();
  if (step > most)
    step = most;
  if (step < -most)
    step = -most;
  return Duration(step);
}

}  // namespace utilities
