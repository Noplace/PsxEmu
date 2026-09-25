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

// Settings > Input > Keyboard Bindings: which key presses each of the pad's fourteen buttons.
//
// Pick a button - double-click it, or select it and press Set Key - and the next key pressed is
// bound to it. Escape cancels. A key can only press one button, so binding it takes it from
// wherever it was; the keys the window already answers to (Space, F1-F8) are refused. Every
// change is handed to the app at once, which saves it and passes it to the input thread - there
// is no OK button to forget.

#include "app/framework.h"
#include "input/keyboard.h"

#include <functional>

namespace psxemu {

    class KeyBindingsWindow {
     public:
        KeyBindingsWindow() = default;
        ~KeyBindingsWindow();

        KeyBindingsWindow(const KeyBindingsWindow&) = delete;
        KeyBindingsWindow& operator=(const KeyBindingsWindow&) = delete;

        bool Create(HINSTANCE instance, HWND owner, std::function<void(const KeyMap&)> on_change);
        void Show(const KeyMap& current);

     private:
        static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam,
                                           LPARAM lparam);
        void Layout(int width, int height);
        void Refresh();
        void BeginCapture();
        void EndCapture(const wchar_t* status);
        void Bind(int button, int key);
        int SelectedButton() const;

        HWND window_ = nullptr;
        HWND list_ = nullptr;
        HWND set_ = nullptr;
        HWND clear_ = nullptr;
        HWND defaults_ = nullptr;
        HWND status_ = nullptr;
        HFONT font_ = nullptr;
        KeyMap map_ = {};
        int capturing_ = -1;   // the button waiting for a key, or -1
        std::function<void(const KeyMap&)> on_change_;
    };

}   // namespace psxemu
