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

// A key as a settings file stores it - "X", "7", "Up", "F10", "Num4", or "VK_xx" for anything
// without a name - and back. Moved out of PSXEmu.Win32's input/keyboard.h, which keeps what is
// the PlayStation's: the default maps, and the keys its window already answers to.

#include "emuwin32/framework.h"

#include <string>

namespace emulation::win32 {

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

}   // namespace emulation::win32
