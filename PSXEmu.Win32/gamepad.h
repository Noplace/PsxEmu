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

// An XInput pad feeding one PSX controller port.
//
// The polling, slot search-and-latch and deadzone handling here are the same
// mechanism GBAEmu's GamepadInputDevice uses for the GBA - none of it is
// specific to that emulator, it is just what driving XInput correctly on
// Windows looks like. What differs is entirely the mapping: PSX has four face
// buttons to GBA's two, so the Xbox pad's four map across by position rather
// than GBA's compromise of doubling two Xbox buttons onto one GBA button, and
// the analog triggers become L2/R2, which XInput's button bitmask has no
// equivalent for.

#include "psx/psx.h"

#include <windows.h>
#include <xinput.h>

#pragma comment(lib, "xinput.lib")

namespace psxemu {

class Gamepad {
 public:
  Gamepad() { ZeroMemory(&state_, sizeof(state_)); }

  bool connected() const { return connected_; }

  // Polls this pad and returns the buttons held, in the same Sio::k* bitmask
  // shape ReadKeyboardPad() already returns, so a keyboard and a pad driving
  // the same port are just ORed together with no further plumbing.
  //
  // `claimed` has one bit per XInput user index. A bit already set there
  // belongs to a different Gamepad instance's pad and is skipped over, which
  // is what stops two PSX ports from both ending up reading the one physical
  // controller. This instance keeps its own claim current in it: set for as
  // long as it stays latched to a slot, cleared the moment that pad goes
  // away, so the index is free for whichever instance finds it next.
  uint16_t Poll(uint32_t& claimed) {
    if (connected_)
      claimed &= ~(1u << player_index_);

    // XInputGetState on an empty slot is not the cheap no-op it looks like,
    // so back off to about once a second while nothing is latched rather than
    // asking every frame.
    if (!connected_) {
      if (++idle_frames_ < 60)
        return 0;
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
      return 0;
    }
    claimed |= (1u << player_index_);
    return MapButtons();
  }

 private:
  uint16_t MapButtons() const {
    using emulation::psx::Sio;
    const WORD buttons = state_.Gamepad.wButtons;
    uint16_t out = 0;

    // By position, not by Xbox letter: A sits at the bottom of the four face
    // buttons on both pads, and so on round the other three.
    if (buttons & XINPUT_GAMEPAD_A) out |= Sio::kCross;
    if (buttons & XINPUT_GAMEPAD_B) out |= Sio::kCircle;
    if (buttons & XINPUT_GAMEPAD_X) out |= Sio::kSquare;
    if (buttons & XINPUT_GAMEPAD_Y) out |= Sio::kTriangle;

    if (buttons & XINPUT_GAMEPAD_START) out |= Sio::kStart;
    if (buttons & XINPUT_GAMEPAD_BACK)  out |= Sio::kSelect;

    if (buttons & XINPUT_GAMEPAD_LEFT_SHOULDER)  out |= Sio::kL1;
    if (buttons & XINPUT_GAMEPAD_RIGHT_SHOULDER) out |= Sio::kR1;

    // The PSX digital pad's L2/R2 are on or off; XInput's own "held" cutoff
    // is where the analog triggers cross into that.
    if (state_.Gamepad.bLeftTrigger  > XINPUT_GAMEPAD_TRIGGER_THRESHOLD)
      out |= Sio::kL2;
    if (state_.Gamepad.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD)
      out |= Sio::kR2;

    // The d-pad works as itself, and the left stick doubles for it past a
    // deadzone, which is what an analog stick is expected to do on a pad this
    // digital-only emulated controller has no other use for.
    if (buttons & XINPUT_GAMEPAD_DPAD_UP)    out |= Sio::kUp;
    if (buttons & XINPUT_GAMEPAD_DPAD_DOWN)  out |= Sio::kDown;
    if (buttons & XINPUT_GAMEPAD_DPAD_LEFT)  out |= Sio::kLeft;
    if (buttons & XINPUT_GAMEPAD_DPAD_RIGHT) out |= Sio::kRight;

    constexpr SHORT kDeadzone = XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE + 2000;
    const SHORT x = state_.Gamepad.sThumbLX;
    const SHORT y = state_.Gamepad.sThumbLY;
    if (x >  kDeadzone) out |= Sio::kRight;
    if (x < -kDeadzone) out |= Sio::kLeft;
    if (y >  kDeadzone) out |= Sio::kUp;
    if (y < -kDeadzone) out |= Sio::kDown;

    return out;
  }

  XINPUT_STATE state_;
  bool connected_ = false;
  int player_index_ = 0;
  int idle_frames_ = 0;
};

}  // namespace psxemu
