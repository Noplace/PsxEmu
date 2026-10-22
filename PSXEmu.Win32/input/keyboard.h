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
// keep: GetAsyncKeyState reads the current keyboard from anywhere. The input thread reads every key
// some binding uses and publishes which are held; which pad buttons that makes is worked out on
// the machine's thread, per port (controller_bindings.h). Whether the result reaches the emulated
// machine is the caller's decision - the app withholds buttons while the window is unfocused,
// exactly as it does for a gamepad.
//
// A map is the person's: one key per pad button, in kKeyBindings order, 0 for none, stored in
// psxemu.ini by name (key_cross = X) so the file stays readable, and edited in Settings > Input >
// Controllers.

#include "app/framework.h"
#include "app/const.h"
#include "emuwin32/input/key_names.h"

#include <array>

namespace psxemu {

    typedef std::array<int, kPadButtons> KeyMap;

    inline KeyMap DefaultKeyMap() {
        KeyMap map = {};
        for (int i = 0; i < kPadButtons; ++i)
            map[i] = kKeyBindings[i].key;
        return map;
    }

    // The gamepad's own defaults, in the same button order. A pad's map is a KeyMap too: the
    // codes are emulation::input::PadInput values rather than virtual keys.
    inline KeyMap DefaultPadMap() {
        KeyMap map = {};
        for (int i = 0; i < kPadButtons; ++i)
            map[i] = kKeyBindings[i].pad;
        return map;
    }

    // Keys the window already answers to: Space pauses, Tab held fast-forwards, F1-F8 load and
    // save states, F9 steps through the performance overlay, F10 opens the save-state picker,
    // F11 and Escape go in and out of full screen, F12 takes a screenshot, and Escape is how the
    // binding editors cancel. A pad button on one of them would do both.
    inline bool IsReservedKey(int key) {
        return key == VK_SPACE || key == VK_ESCAPE || key == VK_TAB ||
               (key >= VK_F1 && key <= VK_F12);
    }

}   // namespace psxemu
