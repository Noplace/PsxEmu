/*****************************************************************************************************************
* Copyright (c) 2014 Khalid Ali Al-Kooheji                                                                       *
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
#pragma once

#include <array>
#include <string>

// The knobs that change how the machine behaves for the person using it, as
// opposed to emulated hardware state.
//
// Nothing outside this file should declare its own copy of a setting. A
// component that needs one reads it from the config rather than caching it -
// `system().config()` from anything deriving from Component.
//
// Measured hardware characteristics do not belong here. The CD-ROM's sector
// timing, the GPU's dot clock and the load stalls in `Cpu::Load` are all
// constants describing a PlayStation, not choices, and they stay next to the
// code that uses them.

namespace emulation {
namespace psx {

struct EmuConfig {
  // --- Audio ----------------------------------------------------------
  // A gain applied after the SPU's own main volume, so it covers the voices,
  // the reverb and CD audio alike.
  //
  // A PlayStation is quiet by modern standards: games set the main volume
  // conservatively and the mix peaks well below full scale - measured at about
  // a fifth of it on the discs tested here. That is faithful, and it is also
  // not what anyone wants out of their speakers, so this exists to make up the
  // difference without pretending the hardware did it.
  //
  // 1.0 is the hardware level. The mix is clamped afterwards, so a high value
  // distorts loud passages rather than wrapping them.
  float audio_volume = 2.0f;

  static const float kMinAudioVolume;
  static const float kMaxAudioVolume;

  // --- Video ------------------------------------------------------------
  // Which presenter draws the framebuffer. "d3d11" is the default so an
  // existing settings file (or none at all) behaves exactly as it did before
  // the D3D12 path existed - nobody's picture changes on upgrade unless they
  // opt in.
  std::string graphics_backend = "d3d11";
  static const std::array<const char*, 2> kValidGraphicsBackends;

  // A pixel-shader filter, by the key it was loaded under - see
  // PSXEmu.Win32/shaders/. Empty means the engine's own built-in
  // pass-through. Only the D3D12 backend honours this; the D3D11 path has no
  // filter support (see D3D11Presenter's class comment).
  std::string video_filter = "";
  static const std::array<const char*, 9> kValidVideoFilters;

  // --- Input --------------------------------------------------------------
  // What is plugged into each SIO0 port - one of the three real PS1
  // controllers, a mouse, or nothing at all. See Sio::ControllerType.
  // "dualshock" for both, so an existing game that already negotiates
  // analog input keeps working exactly as it did before this was choosable.
  std::array<std::string, 2> controller_type = { "dualshock", "dualshock" };
  static const std::array<const char*, 5> kValidControllerTypes;

  // Which physical source drives each PSX port - the keyboard, or one of the
  // two XInput slots the Input menu labels "Gamepad 1"/"Gamepad 2" (XInput
  // user index 0 and 1 respectively). Both default to a gamepad: two people
  // already play this way today, one pad per port with no keyboard fallback
  // to reason about.
  std::array<std::string, 2> input_source = { "gamepad1", "gamepad2" };
  static const std::array<const char*, 3> kValidInputSources;

  // --- Timing -------------------------------------------------------------
  // Whether the front end holds the machine to the emulated display's own
  // frame rate - 59.29 Hz in NTSC, 49.76 in PAL - against the wall clock.
  //
  // On is a console, and is the default because it is the only setting that
  // runs at a defined speed. Off does not mean "uncapped" so much as "paced by
  // whatever happens to block first": the monitor's refresh rate through
  // Present with vsync on, or the sound device's buffer filling. On a 165 Hz
  // display with audio not gating, that is 2.8x - which is what made the BIOS
  // intro flash past and is the whole reason this exists (bug 49).
  //
  // Worth turning off to get through a long load or an unskippable intro, or
  // to see how much headroom the host has - the window title's percentage is
  // only interesting when something is allowed to exceed 100.
  bool frame_limiter = true;

  // --- CD-ROM -------------------------------------------------------------
  // Whether the drive is charged for the mechanical work a real one does:
  // spinning up from a standstill, moving the head a distance rather than
  // teleporting it to the target, and waiting for the disc to come round.
  //
  // The costs themselves are hardware characteristics and stay in cdrom.cpp
  // beside the code that uses them, per the note above; what lives here is
  // only the choice of whether to charge them, because it is a genuine
  // trade-off rather than a fact about a PlayStation.
  //
  // Off - the default, and the timing this emulator has always had - makes
  // every seek flat and nearly free. Loading is faster than hardware
  // everywhere, and it shows most on the BIOS's "Licensed by SCEA" logo
  // screen, which is on screen for exactly as long as the drive takes and so
  // lasts about 2.5 seconds instead of the several a console spends there.
  //
  // On is closer to a console at the cost of waiting for it, and of moving
  // every disc-dependent timing baseline in Docs/Test-Suite.md - which is why
  // it is opt-in rather than the default.
  bool cdrom_mechanical_timing = false;
};

// Out of line so there is one definition; these are bounds a UI can offer
// rather than anything the emulation depends on.
inline const float EmuConfig::kMinAudioVolume = 0.0f;
inline const float EmuConfig::kMaxAudioVolume = 8.0f;

inline const std::array<const char*, 2>
    EmuConfig::kValidGraphicsBackends = { "d3d11", "d3d12" };

// Empty string ("None") first, then the eight loaded filters in the same
// order PSXEmu.Win32's Video > Filter menu offers them.
inline const std::array<const char*, 9> EmuConfig::kValidVideoFilters = {
    "",         "nearest",    "bilinear", "crt",   "eagle",
    "hq2x",     "xbrz_legacy", "scanline", "xbrz",
};

// Order matches PSXEmu.Win32's Input > Controller Port menus and
// Sio::ControllerType (kDigital, kDualAnalog, kDualShock, kMouse, kNone).
inline const std::array<const char*, 5> EmuConfig::kValidControllerTypes = {
    "digital", "dual_analog", "dualshock", "mouse", "none" };

// Order matches PSXEmu.Win32's Input > Port Source menus.
inline const std::array<const char*, 3> EmuConfig::kValidInputSources = {
    "keyboard", "gamepad1", "gamepad2" };

}
}
