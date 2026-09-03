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
#pragma once

// An XInput pad feeding one PSX controller port - buttons, both analog
// sticks, and the two vibration motors.
//
// The polling, slot search-and-latch and deadzone handling here are the same
// mechanism GBAEmu's GamepadInputDevice uses for the GBA - none of it is
// specific to that emulator, it is just what driving XInput correctly on
// Windows looks like. What differs is entirely the mapping: PSX has four face
// buttons to GBA's two, so the Xbox pad's four map across by position rather
// than GBA's compromise of doubling two Xbox buttons onto one GBA button, the
// analog triggers become L2/R2 (XInput's button bitmask has no equivalent for
// them), and there are two sticks feeding the pad's analog axes rather than
// one standing in for a cartridge's tilt sensor.
//
// Whether any of this actually reaches the emulated game is not this class's
// decision - it reports what the physical pad is doing and nothing more. A
// PSX pad only goes into analog mode, and only reports axes or drives a
// motor, once the game itself asks it to via the DualShock command sequence
// in Sio; see psx/sio.cpp. A game that only ever polls the plain digital pad
// never looks at the axes this reports or turns a motor on, exactly as a
// real console would not.

#include "psx/psx.h"

#include <windows.h>
#include <xinput.h>

#pragma comment(lib, "xinput.lib")

namespace psxemu {

class Gamepad {
 public:
  // What one poll produced, in the pad's own byte conventions - buttons as
  // the Sio::k* bitmask, axes as 0x00=left/up, 0xFF=right/down, 0x80=centred.
  struct State {
    uint16_t buttons = 0;
    uint8_t left_x = 0x80, left_y = 0x80, right_x = 0x80, right_y = 0x80;
  };

  Gamepad() { ZeroMemory(&state_, sizeof(state_)); }

  bool connected() const { return connected_; }

  // Polls this pad. `claimed` has one bit per XInput user index; a bit
  // already set there belongs to a different Gamepad instance's pad and is
  // skipped over, which is what stops two PSX ports from both ending up
  // reading the one physical controller. This instance keeps its own claim
  // current in it: set for as long as it stays latched to a slot, cleared
  // the moment that pad goes away, so the index is free for whichever
  // instance finds it next.
  State Poll(uint32_t& claimed) {
    if (connected_)
      claimed &= ~(1u << player_index_);

    // XInputGetState on an empty slot is not the cheap no-op it looks like,
    // so back off to about once a second while nothing is latched rather than
    // asking every frame.
    if (!connected_) {
      if (++idle_frames_ < 60)
        return State();
      idle_frames_ = 0;
    }

    DWORD result = ERROR_DEVICE_NOT_CONNECTED;
    if (connected_) {
      result = XInputGetState(static_cast<DWORD>(player_index_), &state_);
    } else {
      for (int i = 0; i < XUSER_MAX_COUNT; ++i) {
        if (claimed & (1u << i))
          continue;
        result = XInputGetState(static_cast<DWORD>(i), &state_);
        if (result == ERROR_SUCCESS) {
          player_index_ = i;
          break;
        }
      }
    }

    connected_ = (result == ERROR_SUCCESS);
    if (!connected_) {
      ZeroMemory(&state_, sizeof(state_));
      return State();
    }
    claimed |= (1u << player_index_);
    return ReadState();
  }

  // What the pad's two motors should be doing right now, read back from
  // Sio's per-port motor state once a frame. 0 or 255 for the small one, 0
  // to 255 for the large one - matches what XInputSetState itself wants.
  void SetRumble(uint8_t small_motor, uint8_t large_motor) {
    if (!connected_)
      return;
    if (small_motor == small_ && large_motor == large_)
      return;
    small_ = small_motor;
    large_ = large_motor;
    XINPUT_VIBRATION vibration = {};
    // XInput's motors are both 16-bit; the small one only ever gets asked to
    // be fully on or off, so stretching its 8-bit range across the top of
    // XInput's is as good as any other choice.
    vibration.wLeftMotorSpeed = static_cast<WORD>(large_) * 257;
    vibration.wRightMotorSpeed = static_cast<WORD>(small_) * 257;
    XInputSetState(static_cast<DWORD>(player_index_), &vibration);
  }

 private:
  State ReadState() const {
    using emulation::psx::Sio;
    State out;
    const WORD buttons = state_.Gamepad.wButtons;

    // By position, not by Xbox letter: A sits at the bottom of the four face
    // buttons on both pads, and so on round the other three.
    if (buttons & XINPUT_GAMEPAD_A) out.buttons |= Sio::kCross;
    if (buttons & XINPUT_GAMEPAD_B) out.buttons |= Sio::kCircle;
    if (buttons & XINPUT_GAMEPAD_X) out.buttons |= Sio::kSquare;
    if (buttons & XINPUT_GAMEPAD_Y) out.buttons |= Sio::kTriangle;

    if (buttons & XINPUT_GAMEPAD_START) out.buttons |= Sio::kStart;
    if (buttons & XINPUT_GAMEPAD_BACK)  out.buttons |= Sio::kSelect;

    if (buttons & XINPUT_GAMEPAD_LEFT_SHOULDER)  out.buttons |= Sio::kL1;
    if (buttons & XINPUT_GAMEPAD_RIGHT_SHOULDER) out.buttons |= Sio::kR1;
    if (buttons & XINPUT_GAMEPAD_LEFT_THUMB)     out.buttons |= Sio::kL3;
    if (buttons & XINPUT_GAMEPAD_RIGHT_THUMB)    out.buttons |= Sio::kR3;

    // The PSX digital pad's L2/R2 are on or off; XInput's own "held" cutoff
    // is where the analog triggers cross into that.
    if (state_.Gamepad.bLeftTrigger  > XINPUT_GAMEPAD_TRIGGER_THRESHOLD)
      out.buttons |= Sio::kL2;
    if (state_.Gamepad.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD)
      out.buttons |= Sio::kR2;

    // The d-pad works as itself, and the left stick doubles for it past a
    // deadzone, which is what an analog stick is expected to do on a pad
    // whose digital buttons are all this maps to when the game never asks
    // for anything else.
    if (buttons & XINPUT_GAMEPAD_DPAD_UP)    out.buttons |= Sio::kUp;
    if (buttons & XINPUT_GAMEPAD_DPAD_DOWN)  out.buttons |= Sio::kDown;
    if (buttons & XINPUT_GAMEPAD_DPAD_LEFT)  out.buttons |= Sio::kLeft;
    if (buttons & XINPUT_GAMEPAD_DPAD_RIGHT) out.buttons |= Sio::kRight;

    constexpr SHORT kStickDeadzone = XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE + 2000;
    const SHORT lx = state_.Gamepad.sThumbLX;
    const SHORT ly = state_.Gamepad.sThumbLY;
    if (lx >  kStickDeadzone) out.buttons |= Sio::kRight;
    if (lx < -kStickDeadzone) out.buttons |= Sio::kLeft;
    if (ly >  kStickDeadzone) out.buttons |= Sio::kUp;
    if (ly < -kStickDeadzone) out.buttons |= Sio::kDown;

    out.left_x = ToPsxAxis(lx, /*invert=*/false);
    out.left_y = ToPsxAxis(ly, /*invert=*/true);
    out.right_x = ToPsxAxis(state_.Gamepad.sThumbRX, /*invert=*/false);
    out.right_y = ToPsxAxis(state_.Gamepad.sThumbRY, /*invert=*/true);
    return out;
  }

  // XInput's Y axes read positive going up and PSX's read 0x00 at the top,
  // so Y needs its sign flipped before centring; X does not, since positive
  // is right on both. A small deadzone is applied first, so a stick that
  // rests a little off true does not stop a game's menu from ever settling
  // on dead centre.
  static uint8_t ToPsxAxis(SHORT raw, bool invert) {
    constexpr int32_t kDeadzone = 3000;
    int32_t v = raw;
    if (v > -kDeadzone && v < kDeadzone)
      v = 0;
    if (invert)
      v = -v;
    int32_t byte = 128 + (v / 256);
    if (byte < 0) byte = 0;
    if (byte > 255) byte = 255;
    return static_cast<uint8_t>(byte);
  }

  XINPUT_STATE state_;
  bool connected_ = false;
  int player_index_ = 0;
  int idle_frames_ = 0;
  uint8_t small_ = 0;
  uint8_t large_ = 0;
};

}  // namespace psxemu
