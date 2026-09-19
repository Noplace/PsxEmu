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
// key map and no class to hold between calls. Whether the result reaches the emulated machine is
// the caller's decision - the app withholds buttons while the window is unfocused, exactly as it
// does for a gamepad.
//
// The map is the person's: one key per pad button, in kKeyBindings order, 0 for none, stored in
// psxemu.ini by name (key_cross = X) so the file stays readable, and edited in Settings > Input >
// Keyboard Bindings.

#include "framework.h"
#include "const.h"

#include <array>

namespace psxemu {

    typedef std::array<int, kPadButtons> KeyMap;

    inline KeyMap DefaultKeyMap() {
        KeyMap map = {};
        for (int i = 0; i < kPadButtons; ++i)
            map[i] = kKeyBindings[i].key;
        return map;
    }

    // The buttons held right now, as the Sio::k* bitmask.
    inline uint16_t ReadKeyboardPad(const KeyMap& map) {
        uint16_t buttons = 0;
        for (int i = 0; i < kPadButtons; ++i) {
            if (map[i] != 0 && (GetAsyncKeyState(map[i]) & 0x8000))
                buttons |= kKeyBindings[i].button;
        }
        return buttons;
    }

    // Keys the window already answers to: Space pauses, F1-F8 load and save states, Escape is
    // how the binding editor cancels. A pad button on one of them would do both.
    inline bool IsReservedKey(int key) {
        return key == VK_SPACE || key == VK_ESCAPE || (key >= VK_F1 && key <= VK_F8);
    }

    namespace detail {
        struct NamedKey {
            int key;
            const char* name;
        };
        // clang-format off
        inline constexpr NamedKey kNamedKeys[] = {
            { VK_UP, "Up" }, { VK_DOWN, "Down" }, { VK_LEFT, "Left" }, { VK_RIGHT, "Right" },
            { VK_RETURN, "Return" }, { VK_SHIFT, "Shift" }, { VK_CONTROL, "Ctrl" },
            { VK_MENU, "Alt" }, { VK_TAB, "Tab" }, { VK_BACK, "Backspace" },
            { VK_SPACE, "Space" }, { VK_ESCAPE, "Escape" }, { VK_INSERT, "Insert" },
            { VK_DELETE, "Delete" }, { VK_HOME, "Home" }, { VK_END, "End" },
            { VK_PRIOR, "PageUp" }, { VK_NEXT, "PageDown" }, { VK_CAPITAL, "CapsLock" },
            { VK_MULTIPLY, "NumMultiply" }, { VK_ADD, "NumAdd" }, { VK_SUBTRACT, "NumSubtract" },
            { VK_DECIMAL, "NumDecimal" }, { VK_DIVIDE, "NumDivide" },
            { VK_OEM_1, "Semicolon" }, { VK_OEM_PLUS, "Equals" }, { VK_OEM_COMMA, "Comma" },
            { VK_OEM_MINUS, "Minus" }, { VK_OEM_PERIOD, "Period" }, { VK_OEM_2, "Slash" },
            { VK_OEM_3, "Backquote" }, { VK_OEM_4, "LeftBracket" }, { VK_OEM_5, "Backslash" },
            { VK_OEM_6, "RightBracket" }, { VK_OEM_7, "Quote" },
        };
        // clang-format on
    }   // namespace detail

    // A key as psxemu.ini stores it: "X", "7", "Up", "F10", "Num4", or "VK_xx" for anything
    // without a name. Empty for no key.
    inline std::string KeyName(int key) {
        if (key == 0)
            return std::string();
        if ((key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9'))
            return std::string(1, static_cast<char>(key));
        if (key >= VK_F1 && key <= VK_F24)
            return "F" + std::to_string(key - VK_F1 + 1);
        if (key >= VK_NUMPAD0 && key <= VK_NUMPAD9)
            return "Num" + std::to_string(key - VK_NUMPAD0);
        for (const detail::NamedKey& named : detail::kNamedKeys) {
            if (named.key == key)
                return named.name;
        }
        char hex[8];
        sprintf_s(hex, "VK_%02X", key & 0xFF);
        return hex;
    }

    // The reverse, case-insensitive. 0 for an empty or unrecognised name, so a hand-edited typo
    // leaves the button unbound rather than bound to something surprising.
    inline int KeyFromName(const std::string& name) {
        if (name.empty())
            return 0;
        if (name.size() == 1) {
            const char c = static_cast<char>(toupper(static_cast<unsigned char>(name[0])));
            if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
                return c;
            return 0;
        }
        for (int key = VK_F1; key <= VK_F24; ++key) {
            if (_stricmp(name.c_str(), KeyName(key).c_str()) == 0)
                return key;
        }
        for (int key = VK_NUMPAD0; key <= VK_NUMPAD9; ++key) {
            if (_stricmp(name.c_str(), KeyName(key).c_str()) == 0)
                return key;
        }
        for (const detail::NamedKey& named : detail::kNamedKeys) {
            if (_stricmp(name.c_str(), named.name) == 0)
                return named.key;
        }
        if (name.size() == 5 && _strnicmp(name.c_str(), "VK_", 3) == 0) {
            const long value = strtol(name.c_str() + 3, nullptr, 16);
            return (value > 0 && value < 0xFF) ? static_cast<int>(value) : 0;
        }
        return 0;
    }

}   // namespace psxemu
