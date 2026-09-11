/*****************************************************************************************************************
* Copyright (c) 2012 Khalid Ali Al-Kooheji                                                                       *
*                                                                                                                 *
* Permission is hereby granted, free of charge, to any person obtaining a copy of this software and              *
* associated documentation files (the "Software"), to deal in the Software without restriction, including        *
* without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell        *
* copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the       *
* following conditions:                                                                                          *
*                                                                                                                 *
* The above copyright notice and this permission notice shall be included in all copies or substantial           *
* portions of the Software.                                                                                      *
*                                                                                                                 *
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT          *
* LIMITED TO THE WARRANTIES OF MERCHANTABILITY, * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.          *
* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, * DAMAGES OR OTHER LIABILITY,      *
* WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE            *
* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                                                         *
*****************************************************************************************************************/
#pragma once

// The real Windows mouse as a PSX mouse's two inputs - the counterpart to gamepad.h and keyboard.h
// beside it, and the only source Sio::kMouse ever reads from. Unlike a pad's input_source, which
// port a mouse's buttons and motion feed is not a player choice - it is always the real mouse,
// whichever port is set to Sio::kMouse - see PSXEmu.Win32/app.cpp's PollInput.
//
// Motion comes from raw input (WM_INPUT) rather than GetCursorPos, because this device reports
// relative movement with no concept of a screen position to clamp against, and raw input's deltas
// are not clamped at a screen edge the way the visible OS cursor is - tracking GetCursorPos instead
// would lose movement the instant the real cursor hit one. Buttons are GetAsyncKeyState, the same as
// keyboard.h: simpler than decoding RAWMOUSE's own button-transition flags, and level state, sampled
// once a frame the same as every other button on this bus, is all a poll ever asks for anyway.
//
// Registering without RIDEV_INPUTSINK means WM_INPUT only ever arrives while this window has focus,
// which is what makes motion stop the instant focus is lost with no extra check needed here - the
// same thing keyboard.h and gamepad.h each get from the App-level `focused` gate instead. Buttons
// still need that gate applied by the caller, since GetAsyncKeyState reads the whole system rather
// than this window.

#include "framework.h"

namespace psxemu {

    class Mouse {
     public:
        // What one poll produced: buttons as the level they are held at, and movement as however
        // much of it this call has not already handed out - see Poll().
        struct State {
            bool left = false;
            bool right = false;
            int32_t dx = 0;
            int32_t dy = 0;
        };

        // Registers `window` for raw mouse input. Called once, from App::CreateAppWindow right after
        // the window exists - harmless to leave registered even when no port is ever set to
        // Sio::kMouse, since nothing reads accumulated_dx_/accumulated_dy_ unless one is.
        bool Attach(HWND window) {
            RAWINPUTDEVICE device = {};
            device.usUsagePage = 0x01;   // generic desktop controls
            device.usUsage = 0x02;       // mouse
            device.hwndTarget = window;
            return RegisterRawInputDevices(&device, 1, sizeof(device)) != FALSE;
        }

        // Forward WM_INPUT here from the window procedure - `lparam` cast to HRAWINPUT the same way
        // Windows itself defines that message's payload.
        void OnRawInput(HRAWINPUT handle) {
            UINT size = 0;
            GetRawInputData(handle, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER));
            if (size == 0 || size > sizeof(raw_buffer_))
                return;
            if (GetRawInputData(handle, RID_INPUT, raw_buffer_, &size, sizeof(RAWINPUTHEADER)) !=
                size) {
                return;
            }
            const RAWINPUT* raw = reinterpret_cast<const RAWINPUT*>(raw_buffer_);
            if (raw->header.dwType != RIM_TYPEMOUSE)
                return;
            const RAWMOUSE& mouse = raw->data.mouse;
            // Absolute mode (a tablet, or a remote desktop session) has nothing this device's
            // protocol can express - a relative mouse has no notion of a position to be absolute
            // about - so it is ignored rather than translated into a meaningless delta.
            if ((mouse.usFlags & MOUSE_MOVE_ABSOLUTE) != 0)
                return;
            accumulated_dx_ += mouse.lLastX;
            accumulated_dy_ += mouse.lLastY;
        }

        // What has moved and what is held since the last call. Called once a frame from PollInput,
        // the same cadence every other input on this bus is already sampled at.
        State Poll() {
            State state;

            // A real PS1 mouse's own sensor resolution is far below a modern USB mouse's, and raw
            // input's deltas are not scaled by Windows' own pointer-speed setting the way a normal
            // cursor move is - so without some correction here, a modern mouse would read as many
            // times more sensitive than the hardware this is emulating. Divide-by-4 has not been
            // checked against a real one; it is a starting point, not a measurement. The remainder
            // is kept rather than discarded, so a slow, precise movement below the divisor still
            // adds up over several frames instead of being rounded to nothing every single time.
            constexpr int32_t kSensitivityDivisor = 4;
            state.dx = accumulated_dx_ / kSensitivityDivisor;
            state.dy = accumulated_dy_ / kSensitivityDivisor;
            accumulated_dx_ -= state.dx * kSensitivityDivisor;
            accumulated_dy_ -= state.dy * kSensitivityDivisor;

            state.left = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
            state.right = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
            return state;
        }

     private:
        int32_t accumulated_dx_ = 0;
        int32_t accumulated_dy_ = 0;
        BYTE raw_buffer_[64] = {};
    };

}   // namespace psxemu
