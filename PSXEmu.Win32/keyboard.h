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

// The keyboard as a digital PSX pad - the counterpart to gamepad.h beside it, and the source a
// port uses when its EmuConfig::input_source says "keyboard".
//
// Far smaller than the gamepad because there is nothing to open, nothing to lose, and no state to
// keep: GetAsyncKeyState reads the current keyboard from anywhere, so this is one pass over the
// map in const.h and no class to hold between calls. Whether the result reaches the emulated
// machine is the caller's decision - the app withholds buttons while the window is unfocused,
// exactly as it does for a gamepad.

#include "framework.h"
#include "const.h"

namespace psxemu {

    // The buttons held right now, as the Sio::k* bitmask.
    inline uint16_t ReadKeyboardPad() {
        uint16_t buttons = 0;
        for (const KeyBinding& binding : kKeyBindings) {
            if (GetAsyncKeyState(binding.key) & 0x8000)
                buttons |= binding.button;
        }
        return buttons;
    }

}   // namespace psxemu
