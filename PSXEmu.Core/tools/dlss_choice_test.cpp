// dlss_choice_test - the arithmetic of sizes around DLSS (graphics/dlss/dlss_choice.h), checked
// without a graphics card: the rasteriser's scale the front end picks for each mode, the length of
// the jitter, and the output DLSS is asked for. The ranges are the ones DLSS 310.9.1 reported on
// the RTX 4060 (sl_probe --optimal), so a scale chosen here is one DLSS takes there.

#include "graphics/dlss/dlss_choice.h"

#include <cstdio>

namespace {

using psxemu::DlssMode;

int g_checks = 0;
int g_failures = 0;

void Check(bool ok, const char* what) {
  ++g_checks;
  if (!ok) {
    ++g_failures;
    printf("  FAIL  %s\n", what);
  }
}

void CheckInt(int got, int want, const char* what) {
  ++g_checks;
  if (got != want) {
    ++g_failures;
    printf("  FAIL  %s: got %d want %d\n", what, got, want);
  }
}

// The range DLSS reported for an output, by mode: half to all of it for Quality to
// Performance, within 1% for DLAA, a third for Ultra Performance.
psxemu::DlssRange RangeFor(DlssMode mode, int width, int height) {
  psxemu::DlssRange range;
  if (mode == DlssMode::kDlaa) {
    range = { width * 99 / 100, height * 99 / 100, width, height };
  } else if (mode == DlssMode::kUltraPerformance) {
    range = { (width + 1) / 3, (height + 1) / 3, (width + 1) / 3, (height + 1) / 3 };
  } else {
    range = { (width + 1) / 2, (height + 1) / 2, width, height };
  }
  return range;
}

void TestNames() {
  printf("the settings' names\n");
  Check(psxemu::ParseDlssMode("quality") == DlssMode::kQuality, "quality");
  Check(psxemu::ParseDlssMode("ultra_performance") == DlssMode::kUltraPerformance,
        "ultra_performance");
  Check(psxemu::ParseDlssMode("dlaa") == DlssMode::kDlaa, "dlaa");
  Check(psxemu::ParseDlssMode("nonsense") == DlssMode::kOff, "anything else is off");
  Check(psxemu::ParseDlssPreset("m") == psxemu::DlssPreset::kM, "preset m");
  Check(psxemu::ParseDlssPreset("") == psxemu::DlssPreset::kAuto, "preset auto");
}

void TestShownHeight() {
  printf("the picture's height on the window, 4:3\n");
  CheckInt(psxemu::DlssShownHeight(2560, 1600), 1600, "a wide window: all its height");
  CheckInt(psxemu::DlssShownHeight(1000, 1600), 750, "a tall one: its width's 3/4");
  CheckInt(psxemu::DlssShownHeight(0, 1600), 0, "a minimised one: nothing");
}

// The laptop's 1600-line screen: Quality to Performance all land on 4x, the only whole scale
// near their ratios that DLSS takes there; DLAA on the largest that fits, Ultra Performance on
// a third.
void TestScales1600() {
  printf("scales for a 1600-line screen\n");
  CheckInt(psxemu::DlssScale(DlssMode::kQuality, 1600), 4, "Quality");
  CheckInt(psxemu::DlssScale(DlssMode::kBalanced, 1600), 4, "Balanced");
  CheckInt(psxemu::DlssScale(DlssMode::kPerformance, 1600), 4,
           "Performance: 3x would be under half the output");
  CheckInt(psxemu::DlssScale(DlssMode::kDlaa, 1600), 6, "DLAA");
  CheckInt(psxemu::DlssScale(DlssMode::kUltraPerformance, 1600), 2, "Ultra Performance");
  CheckInt(psxemu::DlssScale(DlssMode::kOff, 1600), 1, "off");
}

void TestScales1080() {
  printf("scales for a 1080-line screen\n");
  CheckInt(psxemu::DlssScale(DlssMode::kQuality, 1080), 3, "Quality: exactly 1.5");
  CheckInt(psxemu::DlssScale(DlssMode::kPerformance, 1080), 3, "Performance");
  CheckInt(psxemu::DlssScale(DlssMode::kDlaa, 1080), 4, "DLAA");
}

void TestScalesLimits() {
  printf("scales at the ends\n");
  CheckInt(psxemu::DlssScale(DlssMode::kDlaa, 2160), 8, "DLAA on 2160 lines: 9 is past 8x");
  CheckInt(psxemu::DlssScale(DlssMode::kDlaa, 1700), 6, "no 7x");
  CheckInt(psxemu::DlssScale(DlssMode::kQuality, 200), 2,
           "a tiny window: 2x, since 1x hands DLSS no picture");
  CheckInt(psxemu::DlssScale(DlssMode::kQuality, 480), 2, "480 lines: 2x, not 1x");
  CheckInt(psxemu::DlssScale(DlssMode::kUltraPerformance, 720), 2, "Ultra Performance too");
}

// Every scale chosen for Quality to Performance is one DLSS takes for the screen: from 480
// lines to 2160, a 320x240 game's input is in the range for its letterbox.
void TestScalesAreTaken() {
  printf("the scale chosen is in DLSS's range for the screen\n");
  int bad = 0;
  for (int height = 480; height <= 2160; height += 10) {
    const int width = height * 4 / 3;
    for (DlssMode mode : { DlssMode::kQuality, DlssMode::kBalanced, DlssMode::kPerformance }) {
      const int scale = psxemu::DlssScale(mode, height);
      if (!RangeFor(mode, width, height).Takes(320 * scale, 240 * scale))
        ++bad;
    }
  }
  CheckInt(bad, 0, "screen sizes where it is not");
}

void TestJitter() {
  printf("the jitter's length\n");
  CheckInt(psxemu::DlssJitterPhases(DlssMode::kQuality, 1600, 4), 23,
           "Quality at 1600 lines from 960: 8 x 1.67^2");
  CheckInt(psxemu::DlssJitterPhases(DlssMode::kDlaa, 1600, 6), 8, "DLAA: 8");
  CheckInt(psxemu::DlssJitterPhases(DlssMode::kUltraPerformance, 1600, 2), 72,
           "Ultra Performance: 8 x 9");
}

void TestOutputs() {
  printf("the output DLSS is asked for\n");
  Check(psxemu::DlssTriesScreen(DlssMode::kBalanced), "Balanced tries the screen");
  Check(!psxemu::DlssTriesScreen(DlssMode::kDlaa), "DLAA does not");
  Check(!psxemu::DlssTriesScreen(DlssMode::kUltraPerformance), "nor Ultra Performance");
  int width = 0, height = 0;
  psxemu::DlssOwnOutput(DlssMode::kUltraPerformance, 640, 480, &width, &height);
  Check(width == 1920 && height == 1440, "Ultra Performance: three times each way");
  Check(RangeFor(DlssMode::kUltraPerformance, width, height).Takes(640, 480),
        "...which DLSS takes");
  psxemu::DlssOwnOutput(DlssMode::kDlaa, 1536, 1440, &width, &height);
  Check(width == 1536 && height == 1440, "DLAA: the input's own size");
  // A 256-wide game at 4x on the 1600-line screen is too narrow for the screen's range...
  Check(!RangeFor(DlssMode::kQuality, 2133, 1600).Takes(1024, 960),
        "256 wide at 4x is under half of 2133");
  // ...and goes through the mode at its own ratio instead.
  psxemu::DlssOwnOutput(DlssMode::kQuality, 1024, 960, &width, &height);
  Check(RangeFor(DlssMode::kQuality, width, height).Takes(1024, 960),
        "Quality's own output takes it");
}

}  // namespace

int main() {
  printf("dlss_choice_test - the sizes around DLSS\n\n");

  TestNames();
  TestShownHeight();
  TestScales1600();
  TestScales1080();
  TestScalesLimits();
  TestScalesAreTaken();
  TestJitter();
  TestOutputs();

  printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
