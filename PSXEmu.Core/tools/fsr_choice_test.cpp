/*****************************************************************************************************************
* Copyright (c) 2012 Khalid Ali Al-Kooheji                                                                       *
*                                                                                                                *
* Permission is hereby granted, free of charge, to any person obtaining a copy of this software and              *
* associated documentation files (the "Software"), to deal in the Software without restriction, including        *
* without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell        *
* copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the       *
* following conditions:                                                                                          *
*                                                                                                                *
* The above copyright notice and this permission notice shall be included in all copies or substantial           *
* portions of the Software.                                                                                      *
*                                                                                                                *
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT          *
* LIMITED TO THE WARRANTIES OF MERCHANTABILITY, * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.          *
* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, * DAMAGES OR OTHER LIABILITY,      *
* WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE            *
* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                                                         *
*****************************************************************************************************************/

// fsr_choice_test: the arithmetic of sizes around AMD FSR (graphics/fsr/fsr_choice.h) - the
// settings' names, the rasteriser's scale for each mode and window, the jitter's length against
// AMD's own table, and the output asked for.

#include "graphics/fsr/fsr_choice.h"

#include <cstdio>

namespace {

using psxemu::FsrMode;

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

void TestNames() {
  printf("the settings' names\n");
  Check(psxemu::ParseFsrMode("native_aa") == FsrMode::kNativeAa, "native_aa");
  Check(psxemu::ParseFsrMode("ultra_performance") == FsrMode::kUltraPerformance,
        "ultra_performance");
  Check(psxemu::ParseFsrMode("dlaa") == FsrMode::kOff, "DLSS's dlaa is not FSR's");
  Check(psxemu::ParseFsrVersion("fsr4") == psxemu::FsrVersion::kFsr4, "fsr4");
  Check(psxemu::ParseFsrVersion("fsr3") == psxemu::FsrVersion::kFsr3, "fsr3");
  Check(psxemu::ParseFsrVersion("") == psxemu::FsrVersion::kAuto, "anything else automatic");
  Check(psxemu::ParseFsrSharpness("off") == 0.0f, "sharpening off is 0");
  Check(psxemu::ParseFsrSharpness("max") == 1.0f, "maximum is 1");
  Check(psxemu::ParseFsrSharpness("medium") == 0.5f, "medium is 0.5");
  psxemu::FsrChoice choice;
  choice.frame_generation = true;
  Check(!choice.generating(), "no Frame Generation without an FSR mode");
  choice.mode = FsrMode::kQuality;
  Check(choice.generating(), "...and with one");
}

// The laptop's 1600-line screen: each mode's ratio as near as a whole scale gets it.
void TestScales1600() {
  printf("scales for a 1600-line screen\n");
  CheckInt(psxemu::FsrScale(FsrMode::kNativeAa, 1600), 6, "Native AA: the largest that fits");
  CheckInt(psxemu::FsrScale(FsrMode::kQuality, 1600), 4, "Quality: 1600 / 1.5 is 1067, 4x");
  CheckInt(psxemu::FsrScale(FsrMode::kBalanced, 1600), 4, "Balanced: 941, 4x");
  CheckInt(psxemu::FsrScale(FsrMode::kPerformance, 1600), 3, "Performance: 800, 3x");
  CheckInt(psxemu::FsrScale(FsrMode::kUltraPerformance, 1600), 2, "Ultra Performance: 533, 2x");
  CheckInt(psxemu::FsrScale(FsrMode::kOff, 1600), 1, "off");
}

void TestScales1080() {
  printf("scales for a 1080-line screen\n");
  CheckInt(psxemu::FsrScale(FsrMode::kQuality, 1080), 3, "Quality: exactly 1.5");
  CheckInt(psxemu::FsrScale(FsrMode::kPerformance, 1080), 2, "Performance: 540 lines, 2x");
  CheckInt(psxemu::FsrScale(FsrMode::kNativeAa, 1080), 4, "Native AA");
}

void TestScalesLimits() {
  printf("scales at the ends\n");
  CheckInt(psxemu::FsrScale(FsrMode::kNativeAa, 2160), 8, "Native AA on 2160 lines: 9 is past 8x");
  CheckInt(psxemu::FsrScale(FsrMode::kNativeAa, 1700), 6, "no 7x");
  CheckInt(psxemu::FsrScale(FsrMode::kQuality, 200), 2,
           "a tiny window: 2x, since 1x hands FSR no picture");
  CheckInt(psxemu::FsrScale(FsrMode::kUltraPerformance, 480), 2, "Ultra Performance at 480: 2x");
  CheckInt(psxemu::FsrScale(FsrMode::kQuality, 0), 1, "minimised: nothing");
  // From 480 lines to 2160, every scale is one the rasteriser has.
  int bad = 0;
  for (int height = 480; height <= 2160; height += 10) {
    for (FsrMode mode : { FsrMode::kNativeAa, FsrMode::kQuality, FsrMode::kBalanced,
                          FsrMode::kPerformance, FsrMode::kUltraPerformance }) {
      const int scale = psxemu::FsrScale(mode, height);
      if (scale < 2 || scale > 8 || scale == 7)
        ++bad;
    }
  }
  CheckInt(bad, 0, "screen sizes given a scale the rasteriser does not have");
}

// AMD's own table (Kits/FidelityFX/docs/techniques/super-resolution-ml.md): 18 for Quality, 23
// for Balanced, 32 for Performance, 72 for Ultra Performance - at their exact ratios.
void TestJitter() {
  printf("the jitter's length\n");
  CheckInt(psxemu::FsrJitterPhases(1440, 4), 18, "1.5: 18, AMD's Quality");
  CheckInt(psxemu::FsrJitterPhases(1920, 4), 32, "2.0: 32, AMD's Performance");
  CheckInt(psxemu::FsrJitterPhases(1440, 2), 72, "3.0: 72, AMD's Ultra Performance");
  CheckInt(psxemu::FsrJitterPhases(1440, 6), 8, "Native AA exactly: 8");
  CheckInt(psxemu::FsrJitterPhases(1600, 6), 10, "Native AA on 1600 from 1440: 8 x 1.11^2");
  CheckInt(psxemu::FsrJitterPhases(1200, 6), 8, "a picture larger than the screen: 8 at least");
  CheckInt(psxemu::FsrJitterPhases(1600, 4), 23, "1600 from 960: 8 x 1.67^2");
}

void TestOutputs() {
  printf("the output FSR is asked for\n");
  int width = 0, height = 0;
  psxemu::FsrOutput(1280, 960, 2133, 1600, &width, &height);
  Check(width == 2133 && height == 1600, "the screen's rectangle, one to one");
  psxemu::FsrOutput(1024, 960, 2133, 1600, &width, &height);
  Check(width == 2133 && height == 1600, "a 256-wide game too: FSR takes any ratio");
  psxemu::FsrOutput(3840, 1440, 2133, 1600, &width, &height);
  Check(width == 3840 && height == 1440,
        "a 640-wide game at 6x is wider than the screen: FSR at its own size");
}

}  // namespace

int main() {
  printf("fsr_choice_test - the sizes around AMD FSR\n\n");

  TestNames();
  TestScales1600();
  TestScales1080();
  TestScalesLimits();
  TestJitter();
  TestOutputs();

  printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
