// speed_resampler_test - the audio side of running the machine at 50%..300%.
//
// The SPU produces 44,100 samples per emulated second whatever speed the
// machine is being run at, and the sound device consumes 44,100 per real one.
// This is the arithmetic that reconciles the two, so what it has to get right
// is the count - a device handed the wrong number of samples underruns or
// backs up - and the continuity across blocks, since it is called once per
// frame for as long as a game runs.

#include "platform/speed_resampler.h"

#include <cmath>
#include <cstdio>
#include <vector>

using utilities::SpeedResampler;

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool condition, const char* what) {
  ++g_checks;
  if (!condition) {
    ++g_failures;
    printf("  FAIL  %s\n", what);
  }
}

void CheckEqual(int got, int want, const char* what) {
  ++g_checks;
  if (got != want) {
    ++g_failures;
    printf("  FAIL  %s: got %d want %d\n", what, got, want);
  }
}

// A stereo ramp, so an interpolated sample is easy to reason about: frame i
// holds (i, -i).
std::vector<int16_t> Ramp(int frames) {
  std::vector<int16_t> out;
  for (int i = 0; i < frames; ++i) {
    out.push_back(static_cast<int16_t>(i));
    out.push_back(static_cast<int16_t>(-i));
  }
  return out;
}

void TestUnitySpeedIsACopy() {
  printf("100%% is a copy, bit for bit\n");
  const std::vector<int16_t> in = Ramp(64);
  std::vector<int16_t> out;
  SpeedResampler resampler;
  resampler.Append(in.data(), 64, 1.0, &out);

  CheckEqual(static_cast<int>(out.size()), 128, "every sample came through");
  bool identical = out.size() == in.size();
  for (size_t i = 0; identical && i < in.size(); ++i)
    identical = (in[i] == out[i]);
  Check(identical, "and none of them was touched");
}

void TestDoubleSpeedHalvesTheCount() {
  printf("200%% produces half as many frames\n");
  const std::vector<int16_t> in = Ramp(1000);
  std::vector<int16_t> out;
  SpeedResampler resampler;
  resampler.Append(in.data(), 1000, 2.0, &out);
  CheckEqual(static_cast<int>(out.size()) / 2, 500,
             "1000 frames at 2x is 500 frames out");
}

void TestHalfSpeedDoublesTheCount() {
  printf("50%% produces twice as many frames\n");
  const std::vector<int16_t> in = Ramp(1000);
  std::vector<int16_t> out;
  SpeedResampler resampler;
  resampler.Append(in.data(), 1000, 0.5, &out);
  CheckEqual(static_cast<int>(out.size()) / 2, 2000,
             "1000 frames at 0.5x is 2000 frames out");
}

void TestOneAndAHalf() {
  printf("150%% produces two thirds as many\n");
  const std::vector<int16_t> in = Ramp(900);
  std::vector<int16_t> out;
  SpeedResampler resampler;
  resampler.Append(in.data(), 900, 1.5, &out);
  CheckEqual(static_cast<int>(out.size()) / 2, 600,
             "900 frames at 1.5x is 600 frames out");
}

// The two fastest the menu offers. Both step by an exact 16.16 amount, so the
// count is exact for a block that divides by the speed; a block that does not
// carries its remainder, which is what the drift check below covers.
void TestTwoAndAHalf() {
  printf("250%% produces two fifths as many\n");
  const std::vector<int16_t> in = Ramp(1000);
  std::vector<int16_t> out;
  SpeedResampler resampler;
  resampler.Append(in.data(), 1000, 2.5, &out);
  CheckEqual(static_cast<int>(out.size()) / 2, 400,
             "1000 frames at 2.5x is 400 frames out");
}

void TestTripleSpeedThirdsTheCount() {
  printf("300%% produces a third as many frames\n");
  const std::vector<int16_t> in = Ramp(900);
  std::vector<int16_t> out;
  SpeedResampler resampler;
  resampler.Append(in.data(), 900, 3.0, &out);
  CheckEqual(static_cast<int>(out.size()) / 2, 300,
             "900 frames at 3x is 300 frames out");
}

// The one that matters over a session: called once a frame for a minute, the
// count must not drift. At 60 blocks a second for 60 seconds, a half-sample
// error per block would be 1,800 samples - a fifth of a second of silence or
// backlog.
void TestTheCountDoesNotDriftOverManyBlocks() {
  printf("the count holds over thousands of blocks\n");
  const std::vector<int16_t> in = Ramp(735);   // one 60 Hz frame at 44,100 Hz
  std::vector<int16_t> out;
  SpeedResampler resampler;
  const int blocks = 3600;                     // a minute at 60 a second
  for (int i = 0; i < blocks; ++i)
    resampler.Append(in.data(), 735, 1.5, &out);

  const double expected = 735.0 * blocks / 1.5;
  const double got = static_cast<double>(out.size() / 2);
  Check(std::fabs(got - expected) <= 1.0,
        "a minute at 150% is within one frame of the exact count");
}

// A block boundary must not be a discontinuity: the resampler carries the last
// frame and its fractional position, so a continuous ramp fed in two halves
// comes out the same as one fed whole.
void TestBlocksJoinContinuously() {
  printf("a stream split into blocks resamples the same as one block\n");
  const std::vector<int16_t> in = Ramp(512);

  std::vector<int16_t> whole;
  SpeedResampler one;
  one.Append(in.data(), 512, 1.5, &whole);

  std::vector<int16_t> halves;
  SpeedResampler two;
  two.Append(in.data(), 256, 1.5, &halves);
  two.Append(in.data() + 256 * 2, 256, 1.5, &halves);

  CheckEqual(static_cast<int>(halves.size()), static_cast<int>(whole.size()),
             "the same number of samples either way");
  // The joins interpolate from the carried frame rather than from zero, so the
  // two runs agree exactly.
  bool same = halves.size() == whole.size();
  for (size_t i = 0; same && i < whole.size(); ++i)
    same = (whole[i] == halves[i]);
  Check(same, "and the same samples");
}

// Reset is what a pause, a reset or a state load calls. After it, nothing of
// the previous stream may leak into the next one.
void TestResetForgetsTheCarriedFrame() {
  printf("reset forgets the stream before it\n");
  const std::vector<int16_t> loud = Ramp(64);
  std::vector<int16_t> out;
  SpeedResampler resampler;
  resampler.Append(loud.data(), 64, 2.0, &out);
  resampler.Reset();

  std::vector<int16_t> after;
  const std::vector<int16_t> silence(64 * 2, 0);
  resampler.Append(silence.data(), 64, 2.0, &after);

  bool all_silent = true;
  for (int16_t sample : after)
    all_silent = all_silent && (sample == 0);
  Check(all_silent, "silence after a reset is silent, not a fade from the last");
}

// Nothing here may crash or emit on nonsense input - it runs once a frame for
// as long as the emulator is open.
void TestDegenerateInput() {
  printf("nothing, and nonsense, are handled\n");
  std::vector<int16_t> out;
  SpeedResampler resampler;
  const std::vector<int16_t> in = Ramp(16);
  resampler.Append(nullptr, 16, 1.5, &out);
  resampler.Append(in.data(), 0, 1.5, &out);
  resampler.Append(in.data(), -4, 1.5, &out);
  resampler.Append(in.data(), 16, 0.0, nullptr);
  CheckEqual(static_cast<int>(out.size()), 0, "none of that produced audio");

  // A speed of zero would divide by nothing; it is treated as 1.0 rather than
  // hanging or emitting an unbounded block.
  resampler.Append(in.data(), 16, 0.0, &out);
  CheckEqual(static_cast<int>(out.size()) / 2, 16, "a speed of zero copies");
}

}  // namespace

int main() {
  printf("speed_resampler_test - audio for 50%%..300%% speed\n\n");

  TestUnitySpeedIsACopy();
  TestDoubleSpeedHalvesTheCount();
  TestHalfSpeedDoublesTheCount();
  TestOneAndAHalf();
  TestTwoAndAHalf();
  TestTripleSpeedThirdsTheCount();
  TestTheCountDoesNotDriftOverManyBlocks();
  TestBlocksJoinContinuously();
  TestResetForgetsTheCarriedFrame();
  TestDegenerateInput();

  printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
