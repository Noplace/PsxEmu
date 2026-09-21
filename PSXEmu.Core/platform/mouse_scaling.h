// Standards section 1.
#pragma once

#include <cstdint>
#include <cstddef>

namespace utilities {

// How much a host mouse's movement moves a PlayStation mouse.
//
// The device itself does nothing to its numbers: psx-spx says it returns "raw
// mickeys, so effects like double speed threshold must (should) be implemented
// by software" - a count from the ball is a count on the wire, and any
// acceleration is the game's own. So the only question here is *how many*
// counts a given hand movement should produce, and there are three honest
// answers, none of which is a fact about the console:
//
//   kDesktop   Hand over what the OS already decided. Windows applies the
//              pointer-speed slider and, with "Enhance pointer precision" on,
//              its acceleration curve before it moves the visible cursor; feed
//              the game those same deltas and the in-game pointer tracks the
//              hand exactly as the desktop one does. Nothing is modelled or
//              guessed - the front end reads the cursor rather than the mouse,
//              so this needs no arithmetic here at all (see PSXEmu.Win32's
//              mouse.h for the capture it does need).
//
//   kWindows   Windows' own curve, reapplied to raw counts. Same feel as
//              kDesktop in principle, without capturing the cursor, but it has
//              to reproduce the ballistics rather than read their result - and
//              those are not documented. WindowsCurve below is as close as
//              community reverse-engineering gets; it is an approximation and
//              says so.
//
//   kHardware  What the console did: nothing. Linear counts, scaled only for
//              the gulf between a 1994 ball mouse and a modern sensor, so an
//              inch of desk produces about as many counts as it would have
//              then. A game that applies its own acceleration then behaves as
//              it would on hardware, because the acceleration it sees is its
//              own and not the host's on top of it.
enum class MouseMotion { kDesktop, kWindows, kHardware };

// A 1994 ball mouse's resolution, in counts per inch. Sony published no figure
// for the SCPH-1030 and psx-spx has none; opto-mechanical mice of that decade
// were 100-400 CPI and 200 was the common one, so this is an era estimate
// standing in for a measurement, and the only number in kHardware that is not
// the host's own. Wrong by a factor of two here means the whole mode is out by
// a factor of two, which is why the front end also offers kDesktop.
inline constexpr float kPlayStationMouseCpi = 200.0f;

// Carries the fraction of a count that a scale left over, so slow movement
// adds up across polls instead of being rounded away at every one. Whole
// counts come out; the remainder stays in.
class MouseAccumulator {
 public:
  void Reset() {
    remainder_x_ = 0.0f;
    remainder_y_ = 0.0f;
  }

  // Adds `dx`/`dy` scaled by `scale`, and returns whole counts through
  // `out_dx`/`out_dy`.
  void Add(float dx, float dy, float scale, int32_t* out_dx, int32_t* out_dy) {
    remainder_x_ += dx * scale;
    remainder_y_ += dy * scale;
    // Toward zero, and the fraction is kept rather than dropped - what
    // Windows 7 and later do with their own sub-pixel remainder.
    const int32_t whole_x = static_cast<int32_t>(remainder_x_);
    const int32_t whole_y = static_cast<int32_t>(remainder_y_);
    remainder_x_ -= static_cast<float>(whole_x);
    remainder_y_ -= static_cast<float>(whole_y);
    *out_dx = whole_x;
    *out_dy = whole_y;
  }

 private:
  float remainder_x_ = 0.0f;
  float remainder_y_ = 0.0f;
};

// kHardware's scale: how many PlayStation counts one host count is worth.
// `host_dpi` is what the host mouse reports per inch - not discoverable from
// Windows (HID does not carry it), so it is a setting the person fills in from
// what their mouse is set to.
inline float HardwareScale(int host_dpi) {
  const float dpi = (host_dpi > 0) ? static_cast<float>(host_dpi) : 800.0f;
  return kPlayStationMouseCpi / dpi;
}

// Windows' pointer acceleration, as far as it is known.
//
// **This is an approximation of undocumented behaviour.** What is on the record
// is the shape: five inflection points in HKCU\Control Panel\Mouse's
// SmoothMouseXCurve and SmoothMouseYCurve, 16.16 fixed point, linearly
// interpolated and linearly extrapolated past the last point; an input axis in
// mouse speed and an output axis in pointer gain; and the pointer-speed slider
// applied last. The constants tying those axes to real units - a hard-coded
// 3.5 on the input, a hard-coded 150 Hz mouse rate and the 96-DPI screen
// assumption on the output - come from people reading the binaries, not from
// Microsoft. Treat a close match as luck and an exact one as unavailable: that
// is what MouseMotion::kDesktop is for.
//
// Sources: Microsoft's "Pointer Ballistics for Windows XP" as quoted around
// the web, esreality's curve tutorial for the formulas and the slider table,
// and donewmouseaccel.blogspot.com for what changed in Windows 7.
class WindowsCurve {
 public:
  static const int kPoints = 5;

  // The curve as the registry holds it, already converted from 16.16 to
  // floats: `x` in the input units above, `y` in output gain.
  struct Curve {
    float x[kPoints] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
    float y[kPoints] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
    bool valid = false;
  };

  // Reads one curve out of the raw registry bytes: five entries of eight,
  // each an unsigned 16.16 value in the low four with the high four zero
  // (they never reach 65,536, so the top half is always empty). Anything
  // that is not exactly that shape leaves the curve invalid, and Gain()
  // falls back to no acceleration rather than to a made-up table - there is
  // no default worth hardcoding here, since a real Windows install always
  // has both curves to read.
  static Curve Parse(const uint8_t* x_bytes, size_t x_size,
                     const uint8_t* y_bytes, size_t y_size) {
    Curve curve;
    if (x_bytes == nullptr || y_bytes == nullptr)
      return curve;
    if (x_size < kPoints * 8 || y_size < kPoints * 8)
      return curve;
    for (int i = 0; i < kPoints; ++i) {
      curve.x[i] = Fixed16(x_bytes + i * 8);
      curve.y[i] = Fixed16(y_bytes + i * 8);
    }
    // The first point is the origin and the input axis has to climb, or
    // nothing below can interpolate.
    for (int i = 1; i < kPoints; ++i) {
      if (curve.x[i] <= curve.x[i - 1])
        return curve;
    }
    curve.valid = true;
    return curve;
  }

  // The pointer-speed slider: the eleven positions the control panel offers
  // are MouseSensitivity 1, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, and their
  // multipliers are 0.1 to 2.0 in steps of 0.2 - which is MouseSensitivity/10
  // at every one of them. 10, the default, is 1.0.
  static float SliderMultiplier(int mouse_sensitivity) {
    if (mouse_sensitivity <= 0)
      return 1.0f;
    return static_cast<float>(mouse_sensitivity) / 10.0f;
  }

  // Gain for a movement of `magnitude` counts over `milliseconds`, before the
  // slider. Below the first point and above the last the curve is a straight
  // line, as Windows extrapolates.
  static float Gain(const Curve& curve, float magnitude, float milliseconds) {
    if (!curve.valid || milliseconds <= 0.0f || magnitude <= 0.0f)
      return 1.0f;

    // The input axis: counts per millisecond, less the hard-coded 3.5 that
    // Windows divides by before it looks anything up.
    const float speed = (magnitude / milliseconds) / 3.5f;

    // Which pair of points it falls between, then the line through them. The
    // gain is the curve's output per unit of input, so the last step divides.
    int upper = 1;
    while (upper < kPoints - 1 && speed > curve.x[upper])
      ++upper;
    const int lower = upper - 1;
    const float run = curve.x[upper] - curve.x[lower];
    const float t = (run > 0.0f) ? ((speed - curve.x[lower]) / run) : 0.0f;
    const float out = curve.y[lower] + t * (curve.y[upper] - curve.y[lower]);
    if (speed <= 0.0f || out <= 0.0f)
      return 1.0f;
    return out / speed;
  }

 private:
  static float Fixed16(const uint8_t* bytes) {
    const uint32_t raw = static_cast<uint32_t>(bytes[0]) |
                         (static_cast<uint32_t>(bytes[1]) << 8) |
                         (static_cast<uint32_t>(bytes[2]) << 16) |
                         (static_cast<uint32_t>(bytes[3]) << 24);
    return static_cast<float>(raw) / 65536.0f;
  }

  WindowsCurve() = delete;
};

}  // namespace utilities
