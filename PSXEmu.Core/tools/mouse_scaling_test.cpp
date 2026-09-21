// mouse_scaling_test - how a host mouse's movement becomes a PSX mouse's counts.
//
// The device itself is not in question here: it reports raw counts and any
// acceleration is the game's own (psx-spx), which is what makes the scaling a
// host-side choice with three answers - see utilities::MouseMotion. What can
// be got wrong, and is what this checks, is the arithmetic underneath them:
// the fraction that has to survive between polls (or slow movement vanishes),
// the linear scale for the era's coarser sensor, and the reading of Windows'
// own acceleration curve out of the registry bytes.
//
// Nothing here needs Windows: the front end reads the registry and the cursor,
// and hands the numbers to the pure code this tests.

#include "platform/mouse_scaling.h"

#include <cmath>
#include <cstdio>
#include <cstring>

using utilities::HardwareScale;
using utilities::MouseAccumulator;
using utilities::WindowsCurve;

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

void CheckEqual(int32_t got, int32_t want, const char* what) {
  ++g_checks;
  if (got != want) {
    ++g_failures;
    printf("  FAIL  %s: got %d want %d\n", what, got, want);
  }
}

void CheckNear(float got, float want, float tolerance, const char* what) {
  ++g_checks;
  if (std::fabs(got - want) > tolerance) {
    ++g_failures;
    printf("  FAIL  %s: got %.4f want %.4f\n", what, got, want);
  }
}

// The real curve, as Windows ships it and as this machine's own registry holds
// it: five 8-byte entries, an unsigned 16.16 value in the low four bytes and
// the high four always zero. Read out of HKCU\Control Panel\Mouse and pasted
// here so the parse is checked against bytes it did not produce itself.
const uint8_t kRealXCurve[40] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   // 0.00
    0x15, 0x6E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   // 0.43
    0x00, 0x40, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,   // 1.25
    0x29, 0xDC, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00,   // 3.86
    0x00, 0x00, 0x28, 0x00, 0x00, 0x00, 0x00, 0x00,   // 40.00
};
const uint8_t kRealYCurve[40] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   //   0.00
    0xFD, 0x11, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,   //   1.07
    0x00, 0x24, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00,   //   4.14
    0x00, 0xFC, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00,   //  18.98
    0x00, 0xC0, 0xBB, 0x01, 0x00, 0x00, 0x00, 0x00,   // 443.75
};

void TestHardwareScale() {
  printf("the hardware scale is the console's mouse against the host's\n");
  // 200 counts an inch against 800 is a quarter; against 1600, an eighth.
  CheckNear(HardwareScale(800), 0.25f, 0.0001f, "800 DPI is a quarter");
  CheckNear(HardwareScale(1600), 0.125f, 0.0001f, "1600 DPI is an eighth");
  CheckNear(HardwareScale(400), 0.5f, 0.0001f, "400 DPI is a half");
  // A mouse that reports the same resolution the console's did is one to one.
  CheckNear(HardwareScale(200), 1.0f, 0.0001f, "200 DPI is one to one");
  // Nonsense does not divide by zero or invert the axis.
  Check(HardwareScale(0) > 0.0f, "zero DPI falls back to something positive");
  Check(HardwareScale(-100) > 0.0f, "so does a negative");
}

void TestAccumulatorKeepsTheFraction() {
  printf("the fraction between polls is kept, not rounded away\n");
  MouseAccumulator accumulator;
  int32_t dx = 0, dy = 0;

  // A quarter scale: three counts is 0.75, which is no whole count yet.
  accumulator.Add(3.0f, 0.0f, 0.25f, &dx, &dy);
  CheckEqual(dx, 0, "three counts at a quarter is not a whole one");
  // But it is not lost: another three makes 1.5, so one comes out.
  accumulator.Add(3.0f, 0.0f, 0.25f, &dx, &dy);
  CheckEqual(dx, 1, "another three makes one");
  // And the half stays in, so two more counts finish it.
  accumulator.Add(2.0f, 0.0f, 0.25f, &dx, &dy);
  CheckEqual(dx, 1, "the half that stayed in finishes the next one");

  // The classic failure this exists to prevent: a slow drag, one count a
  // poll, must still move - 40 counts at a quarter is 10, however it arrives.
  accumulator.Reset();
  int32_t total = 0;
  for (int i = 0; i < 40; ++i) {
    accumulator.Add(1.0f, 0.0f, 0.25f, &dx, &dy);
    total += dx;
  }
  CheckEqual(total, 10, "forty single counts at a quarter is ten");

  // Both axes, and negatives, on the same terms.
  accumulator.Reset();
  total = 0;
  for (int i = 0; i < 40; ++i) {
    accumulator.Add(0.0f, -1.0f, 0.25f, &dx, &dy);
    total += dy;
  }
  CheckEqual(total, -10, "and forty the other way is minus ten");

  // Reset is what a mode switch calls; nothing may leak across it.
  accumulator.Reset();
  accumulator.Add(3.0f, 3.0f, 0.25f, &dx, &dy);
  CheckEqual(dx, 0, "after a reset the carried fraction is gone");
}

void TestCurveParsing() {
  printf("Windows' own curve, read out of the registry bytes\n");
  const WindowsCurve::Curve curve =
      WindowsCurve::Parse(kRealXCurve, sizeof(kRealXCurve), kRealYCurve,
                          sizeof(kRealYCurve));
  Check(curve.valid, "the stock curve parses");
  CheckNear(curve.x[0], 0.00f, 0.005f, "x0");
  CheckNear(curve.x[1], 0.43f, 0.005f, "x1");
  CheckNear(curve.x[2], 1.25f, 0.005f, "x2");
  CheckNear(curve.x[3], 3.86f, 0.005f, "x3");
  CheckNear(curve.x[4], 40.00f, 0.005f, "x4");
  CheckNear(curve.y[1], 1.07f, 0.005f, "y1");
  CheckNear(curve.y[2], 4.14f, 0.005f, "y2");
  CheckNear(curve.y[3], 18.98f, 0.005f, "y3");
  CheckNear(curve.y[4], 443.75f, 0.005f, "y4");

  // Anything that is not five whole entries is refused rather than read past.
  const WindowsCurve::Curve truncated =
      WindowsCurve::Parse(kRealXCurve, 24, kRealYCurve, sizeof(kRealYCurve));
  Check(!truncated.valid, "a short curve is refused");
  const WindowsCurve::Curve missing =
      WindowsCurve::Parse(nullptr, 0, nullptr, 0);
  Check(!missing.valid, "so is a missing one");

  // The input axis has to climb, or nothing can interpolate between points.
  uint8_t flat[40] = {};
  const WindowsCurve::Curve unsorted =
      WindowsCurve::Parse(flat, sizeof(flat), kRealYCurve, sizeof(kRealYCurve));
  Check(!unsorted.valid, "and so is a curve whose speeds do not increase");
}

void TestCurveGain() {
  printf("the curve's gain rises with speed, and is one when there is none\n");
  const WindowsCurve::Curve curve =
      WindowsCurve::Parse(kRealXCurve, sizeof(kRealXCurve), kRealYCurve,
                          sizeof(kRealYCurve));

  // Gain is the curve's output over its input, and the input is counts per
  // millisecond over the hard-coded 3.5. At the second point, speed 0.43,
  // that is 1.07/0.43 - about 2.5. A 15-count report over 10 ms is
  // 15/10/3.5 = 0.43 exactly, so this lands on the point rather than
  // between two.
  CheckNear(WindowsCurve::Gain(curve, 15.0f, 10.0f), 1.07f / 0.43f, 0.02f,
            "a point on the curve gives that point's gain");

  // Slow: a single count over 10 ms is well below the first segment, where
  // the curve is steepest in gain terms - but still finite and positive.
  const float slow = WindowsCurve::Gain(curve, 1.0f, 10.0f);
  Check(slow > 0.0f, "a crawl still has a gain");

  // Fast beats slow: that is the whole point of the curve.
  const float fast = WindowsCurve::Gain(curve, 60.0f, 10.0f);
  Check(fast > slow, "moving faster gains more than moving slowly");

  // Past the last point Windows extrapolates the line rather than clamping,
  // so a flick faster than the curve describes keeps gaining.
  const float faster = WindowsCurve::Gain(curve, 6000.0f, 10.0f);
  Check(faster > fast, "and past the last point it keeps going");

  // Nothing degenerate divides by zero or inverts.
  const WindowsCurve::Curve invalid;
  CheckNear(WindowsCurve::Gain(invalid, 15.0f, 10.0f), 1.0f, 0.0001f,
            "no curve means no acceleration");
  CheckNear(WindowsCurve::Gain(curve, 0.0f, 10.0f), 1.0f, 0.0001f,
            "no movement means no acceleration");
  CheckNear(WindowsCurve::Gain(curve, 15.0f, 0.0f), 1.0f, 0.0001f,
            "and neither does no time");
}

void TestSlider() {
  printf("the pointer-speed slider's eleven positions\n");
  // The control panel's eleven stops are MouseSensitivity 1, 2, 4, 6, 8, 10,
  // 12, 14, 16, 18, 20, and their multipliers are 0.1 to 2.0 in steps of
  // 0.2 - sensitivity/10 at every one. 10 is the default and is 1.0.
  CheckNear(WindowsCurve::SliderMultiplier(10), 1.0f, 0.0001f, "the default is one");
  CheckNear(WindowsCurve::SliderMultiplier(1), 0.1f, 0.0001f, "the slowest is a tenth");
  CheckNear(WindowsCurve::SliderMultiplier(20), 2.0f, 0.0001f, "the fastest is double");
  CheckNear(WindowsCurve::SliderMultiplier(4), 0.4f, 0.0001f, "and the third stop is 0.4");
  CheckNear(WindowsCurve::SliderMultiplier(0), 1.0f, 0.0001f,
            "a missing setting reads as the default");
}

}  // namespace

int main() {
  printf("mouse_scaling_test - host mouse movement into PSX mouse counts\n\n");

  TestHardwareScale();
  TestAccumulatorKeepsTheFraction();
  TestCurveParsing();
  TestCurveGain();
  TestSlider();

  printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
