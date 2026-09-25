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

// Emulation > BIOS Console: a window showing everything software writes through the BIOS's
// console calls - putchar, puts and printf - as it arrives.
//
// The text is recorded by the core (psx::Kernel), on the machine's thread; App collects it after
// every frame and posts it here. So this is a UI-thread object with no knowledge of the machine at
// all: it is handed text and shows it. Closing the window only hides it, and it keeps collecting
// while hidden, so opening it later shows what was already written.

#include "app/framework.h"

#include <functional>

namespace psxemu {

    class ConsoleWindow {
     public:
        ConsoleWindow() = default;
        ~ConsoleWindow();

        ConsoleWindow(const ConsoleWindow&) = delete;
        ConsoleWindow& operator=(const ConsoleWindow&) = delete;

        // Creates the window, hidden, owned by `owner` so it stays above it and goes when it
        // does. `on_closed` runs when the user closes it, so the menu tick can follow.
        bool Create(HINSTANCE instance, HWND owner, std::function<void()> on_closed);

        // BIOS text as the core recorded it: 8-bit, lines ending in \n, Shift-JIS for anything
        // above 7Fh. A lead byte at the very end is held until the next call, since a frame can
        // end halfway through a character.
        void Append(const std::string& text);

        // A line of its own between one boot's output and the next. Nothing, if there is no
        // output above it yet.
        void AppendMarker(const wchar_t* text);

        void Clear();
        void Show(bool on);

     private:
        static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam,
                                           LPARAM lparam);
        void AppendWide(const std::wstring& text);
        void TrimIfLong();

        HWND window_ = nullptr;
        HWND edit_ = nullptr;
        HFONT font_ = nullptr;
        std::function<void()> on_closed_;
        std::string held_;            // an incomplete Shift-JIS character from the last Append
        bool at_line_start_ = true;   // so a marker never lands mid-line
    };

}   // namespace psxemu
