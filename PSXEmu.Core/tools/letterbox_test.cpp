// letterbox_test - the presenter's fit-to-window math, checked without a
// graphics device. bug 47: deriving the target aspect from the framebuffer's
// own width:height instead of a fixed 4:3 broke any screen that pairs a
// lower horizontal sample rate with the full interlaced vertical range (a
// common combination for 2D menus), letterboxing it far narrower than every
// other screen even though real hardware shows it at the same width.

#include "tools/letterbox.h"

#include <cmath>
#include <cstdio>

namespace {

int g_checks = 0;
int g_failures = 0;

void CheckNear(float got, float want, const char* what) {
  ++g_checks;
  if (std::fabs(got - want) > 0.01f) {
    ++g_failures;
    printf("  FAIL  %s: got %f want %f\n", what, got, want);
  }
}

// A 640x480 frame (or any frame whose own ratio already happens to be 4:3,
// which is every "normal" PS1 resolution pairing - 256x192, 320x240,
// 512x384, 640x480) into a 4:3 window fills it exactly, no bars either way.
void TestFourByThreeWindowExactFit() {
  printf("a 4:3 window is filled exactly\n");
  const LetterboxRect rect = ComputeLetterboxRect(800, 600, 4.0f / 3.0f);
  CheckNear(rect.x, 0.0f, "no horizontal bar");
  CheckNear(rect.y, 0.0f, "no vertical bar");
  CheckNear(rect.width, 800.0f, "full width");
  CheckNear(rect.height, 600.0f, "full height");
}

// bug 47's actual case: Ace Combat 3's menu is a 320x480 framebuffer - 2:3
// as a raw pixel ratio - but real hardware still shows it at 4:3, the same
// width as the title screen either side of it. A wide window pillarboxes
// (bars left/right), it does not come out portrait-shaped.
void TestWideWindowPillarboxesAtFixedAspect() {
  printf("a wide window pillarboxes rather than following the frame's own ratio\n");
  const LetterboxRect rect = ComputeLetterboxRect(1600, 900, 4.0f / 3.0f);
  CheckNear(rect.height, 900.0f, "the full height is used");
  CheckNear(rect.width, 1200.0f, "width is height * 4/3, not the window's own width");
  CheckNear(rect.x, 200.0f, "centred horizontally - (1600-1200)/2");
  CheckNear(rect.y, 0.0f, "no vertical bar");
}

// A tall, narrow window (portrait monitor, or a resized-thin main window)
// letterboxes instead - bars top and bottom, full width used.
void TestTallWindowLetterboxes() {
  printf("a tall window letterboxes instead\n");
  const LetterboxRect rect = ComputeLetterboxRect(600, 1000, 4.0f / 3.0f);
  CheckNear(rect.width, 600.0f, "the full width is used");
  CheckNear(rect.height, 450.0f, "height is width / 4/3, not the window's own height");
  CheckNear(rect.x, 0.0f, "no horizontal bar");
  CheckNear(rect.y, 275.0f, "centred vertically - (1000-450)/2");
}

}  // namespace

int main() {
  printf("letterbox_test - fit-to-window math for the presenter\n\n");

  TestFourByThreeWindowExactFit();
  TestWideWindowPillarboxesAtFixedAspect();
  TestTallWindowLetterboxes();

  printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
