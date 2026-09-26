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

// Settings > Emulation: how the machine is emulated - the CPU, the timing models, the GPU and
// the CD-ROM - each a switch with a line saying what it costs, and the Accuracy and Performance
// presets over them (EmulationPreset, psx/emuconfig.h).
//
// Like the Controllers window there is no OK or Cancel: every switch reaches the App the moment
// it is clicked, and the App applies it to the running game. The window keeps no settings of
// its own - it reads them from the App each time it refreshes - so it cannot drift from what
// the machine is actually running with.

#include "app/framework.h"

#include <array>
#include <functional>

namespace psxemu {

    class EmulationSettingsWindow {
     public:
        struct Host {
            std::function<const emulation::psx::EmuConfig&()> config;
            // One switch, clicked.
            std::function<void(bool emulation::psx::EmuConfig::*setting, bool on)> set;
            // A preset button.
            std::function<void(emulation::psx::EmulationPreset preset)> apply_preset;
        };

        EmulationSettingsWindow() = default;
        ~EmulationSettingsWindow();
        EmulationSettingsWindow(const EmulationSettingsWindow&) = delete;
        EmulationSettingsWindow& operator=(const EmulationSettingsWindow&) = delete;

        bool Create(HINSTANCE instance, HWND owner, Host host);
        void Show();

        // The settings changed, from here or anywhere else; redraw if open.
        void OnConfigChanged();

        HWND window() const { return window_; }

        // How many switches the window has - one per entry in its table.
        static constexpr int kSwitchCount = 11;

     private:
        static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam,
                                           LPARAM lparam);

        void Refresh();
        int Scale(int value) const { return MulDiv(value, dpi_, 96); }

        Host host_;
        HWND window_ = nullptr;
        std::array<HWND, kSwitchCount> switches_ = {};
        HWND preset_name_ = nullptr;
        HWND preset_text_ = nullptr;
        HFONT font_ = nullptr;
        HFONT bold_font_ = nullptr;
        HFONT small_font_ = nullptr;
        int dpi_ = 96;
    };

}   // namespace psxemu
