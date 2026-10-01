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

// Settings > Video: how the picture is made and shown - the renderer, the graphics card and the
// filter; the rasteriser and what it adds (resolution, true colour, PGXP); NVIDIA DLSS; and what
// the on-screen display draws over the picture. One window, where these used to be six popups of
// a menu, so how they bear on each other - DLSS needs Direct3D 12 and the hardware rasteriser,
// and takes over its resolution - is in view while choosing.
//
// Like Emulation Settings there is no OK or Cancel: each control reaches the App the moment it
// changes, and the App applies it to the running game. The window keeps no settings of its own;
// every refresh reads them from the App, and shows what is running where that differs from what
// was asked for - a hardware rasteriser that could not be made shows as software, and DLSS's own
// resolution shows while it runs.

#include "app/framework.h"
#include "ui/game_scope.h"

#include <array>
#include <functional>

namespace psxemu {

    class VideoSettingsWindow {
     public:
        // What the window shows, as the App has it now.
        struct State {
            std::string renderer;                // running: a kBackendChoices key
            std::vector<std::wstring> cards;     // each graphics card, as the list names it
            int card = -1;                       // the one chosen, or -1 for automatic
            std::string filter;                  // running: a kFilterChoices key
            bool hardware = false;               // the hardware rasteriser is drawing
            int resolution_scale = 1;            // as drawing: DLSS's own while it runs
            bool true_color = false;
            bool pgxp_vertices = false;
            bool pgxp_textures = false;
            bool pgxp_culling = false;
            bool dlss_running = false;           // and so has the resolution and the vertices
            std::string dlss_mode;               // kDlssModeChoices
            std::string dlss_preset;             // kDlssPresetChoices
            std::string dlss_generation;         // kDlssGenerationChoices
            int generation_most = 2;             // the most the card generates (DlssStatus)
            bool generation_dynamic = false;
            std::wstring dlss_status;            // whether DLSS runs, or why not
            std::wstring generation_status;      // ...and Frame Generation, when asked for
            int stats_mode = 0;                  // the performance panel: off, compact, full
            bool notifications = true;
            bool controllers_always = false;
            bool glass = false;
        };

        // Each setter is the App's own, and refreshes the window once it is done - some of them
        // later, when the video thread or the machine says what it ended up with.
        struct Host {
            std::function<State()> state;
            std::function<void(const std::string& key)> set_renderer;
            std::function<void(int card)> set_card;   // an index into State::cards, or -1
            std::function<void(const std::string& key)> set_filter;
            std::function<void(bool hardware)> set_rasteriser;
            std::function<void(int scale)> set_resolution;
            std::function<void(bool on)> set_true_color;
            // `which`: 0 precise vertices, 1 perspective-correct textures, 2 precise culling.
            std::function<void(int which, bool on)> set_pgxp;
            std::function<void(const std::string& key)> set_dlss_mode;
            std::function<void(const std::string& key)> set_dlss_preset;
            std::function<void(const std::string& key)> set_dlss_generation;
            std::function<void(int mode)> set_stats;
            std::function<void(bool on)> set_notifications;
            std::function<void(bool on)> set_controllers_always;
            std::function<void(bool glass)> set_glass;
            // Whose settings the per-game ones are, and the check box that changes it.
            std::function<GameScope()> game;
            std::function<void(bool separate)> set_separate;
        };

        VideoSettingsWindow() = default;
        ~VideoSettingsWindow();
        VideoSettingsWindow(const VideoSettingsWindow&) = delete;
        VideoSettingsWindow& operator=(const VideoSettingsWindow&) = delete;

        bool Create(HINSTANCE instance, HWND owner, Host host);
        void Show();

        // Something it shows changed, from here or anywhere else; redraw if open.
        void OnSettingsChanged();

        HWND window() const { return window_; }

     private:
        static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam,
                                           LPARAM lparam);

        void Refresh();
        void OnCommand(int id, int code);
        void MakeFonts();
        int Scale(int value) const { return MulDiv(value, dpi_, 96); }

        Host host_;
        HWND window_ = nullptr;
        HWND game_box_ = nullptr;
        HWND renderer_ = nullptr;
        HWND card_ = nullptr;
        HWND filter_ = nullptr;
        HWND software_ = nullptr;
        HWND hardware_ = nullptr;
        HWND resolution_ = nullptr;
        HWND true_color_ = nullptr;
        std::array<HWND, 3> pgxp_ = {};
        HWND raster_hint_ = nullptr;
        HWND dlss_mode_ = nullptr;
        HWND dlss_preset_ = nullptr;
        HWND dlss_generation_ = nullptr;
        HWND dlss_status_ = nullptr;
        HWND generation_status_ = nullptr;
        HWND stats_ = nullptr;
        HWND notifications_ = nullptr;
        HWND controllers_ = nullptr;
        HWND classic_ = nullptr;
        HWND glass_ = nullptr;
        HWND note_ = nullptr;
        // What the two lists that change hold now: the cards, and the Frame Generation choices
        // the card makes.
        std::vector<std::wstring> card_items_;
        std::vector<std::string> generation_items_;
        HFONT font_ = nullptr;
        HFONT bold_font_ = nullptr;
        HFONT small_font_ = nullptr;
        int dpi_ = 96;
    };

}   // namespace psxemu
