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

// Emulation > Cheats: the running game's GameShark codes. A list on the left - each cheat with a
// check box that turns it on or off at once - and on the right the chosen one's name and code,
// to change or to type a new one in. Import reads a DuckStation or RetroArch cheat file.
//
// Like the other settings windows it keeps nothing of its own: the App holds the cheats, saves
// them, and hands the enabled ones to the machine; this shows them and says what changed.

#include "app/framework.h"
#include "psx/cheats.h"
#include "ui/game_scope.h"

#include <functional>
#include <string>
#include <vector>

namespace psxemu {

    class CheatsWindow {
     public:
        struct Host {
            std::function<GameScope()> game;
            std::function<const std::vector<emulation::psx::Cheat>&()> cheats;
            std::function<void(int index, bool enabled)> set_enabled;
            // `index` -1 adds one. What was wrong, or empty if it took.
            std::function<std::string(int index, const std::string& name,
                                      const std::string& code)> save;
            std::function<void(int index)> remove;
            std::function<void()> import;
        };

        CheatsWindow() = default;
        ~CheatsWindow();
        CheatsWindow(const CheatsWindow&) = delete;
        CheatsWindow& operator=(const CheatsWindow&) = delete;

        bool Create(HINSTANCE instance, HWND owner, Host host);
        void Show();
        // The cheats or the game changed - redraw if open. `select` picks a cheat to show.
        void OnCheatsChanged(int select = -2);

        HWND window() const { return window_; }

     private:
        static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam,
                                           LPARAM lparam);
        void Refresh(int select);
        void ShowCheat(int index);   // into the editor; -1 for a new one
        void SetStatus(const std::wstring& text, bool error);
        void MakeFonts();
        int Scale(int value) const { return MulDiv(value, dpi_, 96); }

        Host host_;
        HWND window_ = nullptr;
        HWND heading_ = nullptr;
        HWND list_ = nullptr;
        HWND name_ = nullptr;
        HWND code_ = nullptr;
        HWND status_ = nullptr;
        HWND new_ = nullptr;
        HWND delete_ = nullptr;
        HWND import_ = nullptr;
        HWND save_ = nullptr;
        HFONT font_ = nullptr;
        HFONT bold_font_ = nullptr;
        HFONT code_font_ = nullptr;
        int dpi_ = 96;
        int editing_ = -1;       // the cheat in the editor, -1 for a new one
        bool filling_ = false;   // the list is being filled: its notifications are this window's own
        bool status_error_ = false;
    };

}   // namespace psxemu
