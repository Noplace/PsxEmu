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
#include "ui/emulation_settings_window.h"
#include "app/app_icon.h"

#include <algorithm>
#include <commctrl.h>
#pragma comment(lib, "comctl32.lib")

namespace psxemu {

    using emulation::psx::EmuConfig;
    using emulation::psx::EmulationPreset;

    namespace {

        const wchar_t kWindowClass[] = L"PSXEmuEmulationSettings";

        const int kIdSwitchFirst = 100;   // one per kSwitches entry
        const int kIdAccuracy = 200;
        const int kIdPerformance = 201;
        const int kIdDefaults = 202;
        const int kIdClose = 203;
        const int kIdGame = 204;
        const int kIdHintFirst = 300;     // the grey line under each switch

        // The layout, in pixels at 96 DPI: the presets across the top, then two columns of
        // groups, then Close.
        const int kMargin = 12;
        const int kClientWidth = 760;
        const int kColumnWidth = (kClientWidth - kMargin * 3) / 2;
        const int kPresetTop = 44;
        const int kGroupsTop = 128;
        const int kGroupHeader = 22;      // from a group box's top to its first switch
        const int kGroupFooter = 8;
        const int kGroupGap = 10;
        const int kSwitchHeight = 20;
        const int kHintHeight = 30;       // two lines of the small font
        const int kItemGap = 6;

        // Every switch, in the order the window shows them: the groups of the left column, then
        // those of the right. `group` starts a new group box when it differs from the entry
        // before.
        struct Switch {
            bool EmuConfig::*setting;
            const wchar_t* group;
            int column;
            const wchar_t* label;
            const wchar_t* hint;
        };

        const Switch kSwitches[EmulationSettingsWindow::kSwitchCount] = {
            { &EmuConfig::recompiler, L"CPU", 0, L"&Recompiler",
              L"About twice as fast. A game's timing differs slightly from the interpreter, "
              L"which every test here is measured with." },
            { &EmuConfig::icache_timing, L"CPU", 0, L"&Instruction cache timing",
              L"Instruction fetches pay for cache misses, and uncached code such as the BIOS "
              L"for every fetch. Interpreter only." },
            { &EmuConfig::exact_event_timing, L"Timing accuracy", 0, L"&Exact event timing",
              L"Interrupts on the cycle they happen, rather than up to 31 cycles late. Costs "
              L"about 5% speed." },
            { &EmuConfig::dma_stops_cpu, L"Timing accuracy", 0, L"&DMA stops the CPU",
              L"The CPU waits while a DMA transfer holds the bus, as it does on a console." },
            { &EmuConfig::measured_bus_timing, L"Timing accuracy", 0, L"&Measured bus timing",
              L"The BIOS ROM, CD-ROM, SPU and expansion ports timed by a rule fitted to a "
              L"console's measured access times." },
            { &EmuConfig::write_queue_timing, L"Timing accuracy", 0, L"&Write queue timing",
              L"Stores go through the CPU's write queue. Estimated rather than measured, so no "
              L"preset turns it on. Interpreter only." },
            { &EmuConfig::gpu_thread, L"GPU", 1, L"Rasterise on a &GPU thread",
              L"Draws on a thread of its own. The picture is identical either way; only the "
              L"work moves." },
            { &EmuConfig::gpu_transfer_timing, L"GPU", 1, L"Charge GPU time for VRAM &transfers",
              L"Uploads to and from VRAM take time, so commands after a large one wait for it. "
              L"Derived, not measured." },
            { &EmuConfig::cdrom_mechanical_timing, L"CD-ROM and BIOS", 1,
              L"CD-ROM me&chanical timing",
              L"The drive pays for spin-up, seek distance and rotation, so loading takes about "
              L"as long as on a console." },
            { &EmuConfig::skip_bios_intro, L"CD-ROM and BIOS", 1, L"S&kip BIOS intro",
              L"A disc boots straight into the game, past the logo and the disc check. Not "
              L"changed by the presets." },
            { &EmuConfig::pause_in_menus, L"Front end", 1, L"&Pause while in menus",
              L"The game stops while one of the main window's menus is open. Not changed by the "
              L"presets." },
        };

        const wchar_t* PresetName(EmulationPreset preset) {
            switch (preset) {
                case EmulationPreset::kAccuracy: return L"Accuracy";
                case EmulationPreset::kPerformance: return L"Performance";
                case EmulationPreset::kDefault: return L"Defaults";
                default: return L"Custom";
            }
        }

        const wchar_t* PresetText(EmulationPreset preset) {
            switch (preset) {
                case EmulationPreset::kAccuracy:
                    return L"Every timing model built on a console's behaviour, on the "
                           L"interpreter. The closest to hardware, and the slowest: for a game "
                           L"that misbehaves.";
                case EmulationPreset::kPerformance:
                    return L"The recompiler, with every timing model off. The fastest, for a "
                           L"slower computer or running a game faster than a console.";
                case EmulationPreset::kDefault:
                    return L"The interpreter, with every timing model off: the timing every test "
                           L"here is measured with, and what a new install starts with.";
                default:
                    return L"Your own mix. A preset sets every switch here except Skip BIOS Intro "
                           L"and Pause While in Menus.";
            }
        }

    }   // namespace

    EmulationSettingsWindow::~EmulationSettingsWindow() {
        if (window_ != nullptr && IsWindow(window_))
            DestroyWindow(window_);
        for (HFONT font : { font_, bold_font_, small_font_ }) {
            if (font != nullptr)
                DeleteObject(font);
        }
    }

    bool EmulationSettingsWindow::Create(HINSTANCE instance, HWND owner, Host host) {
        host_ = std::move(host);

        INITCOMMONCONTROLSEX controls = { sizeof(controls), ICC_STANDARD_CLASSES };
        InitCommonControlsEx(&controls);

        WNDCLASSEXW window_class = {};
        window_class.cbSize = sizeof(window_class);
        if (!GetClassInfoExW(instance, kWindowClass, &window_class)) {
            window_class = {};
            window_class.cbSize = sizeof(window_class);
            window_class.lpfnWndProc = WindowProc;
            window_class.hInstance = instance;
            window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            window_class.hIcon = AppIcon(instance);
            window_class.hIconSm = AppIconSmall(instance);
            window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
            window_class.lpszClassName = kWindowClass;
            if (RegisterClassExW(&window_class) == 0)
                return false;
        }

        // Not WS_CLIPCHILDREN: a group box paints only its frame, and relies on the window
        // painting what is inside it.
        const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
        window_ = CreateWindowExW(0, kWindowClass, L"PSXEmu - Emulation Settings", style,
                                  CW_USEDEFAULT, CW_USEDEFAULT, 100, 100, owner, nullptr, instance,
                                  this);
        if (window_ == nullptr)
            return false;

        dpi_ = static_cast<int>(GetDpiForWindow(window_));
        if (dpi_ <= 0)
            dpi_ = 96;

        NONCLIENTMETRICSW metrics = { sizeof(metrics) };
        if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0,
                                       static_cast<UINT>(dpi_))) {
            font_ = CreateFontIndirectW(&metrics.lfMessageFont);
            LOGFONTW bold = metrics.lfMessageFont;
            bold.lfWeight = FW_BOLD;
            bold_font_ = CreateFontIndirectW(&bold);
            LOGFONTW caption = metrics.lfMessageFont;
            caption.lfHeight = caption.lfHeight * 90 / 100;
            small_font_ = CreateFontIndirectW(&caption);
        }

        auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD control_style, int id,
                        int x, int y, int w, int h, HFONT font) {
            HWND control = CreateWindowExW(
                0, cls, text, WS_CHILD | WS_VISIBLE | control_style, Scale(x), Scale(y), Scale(w),
                Scale(h), window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance,
                nullptr);
            if (control != nullptr && font != nullptr)
                SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
            return control;
        };

        // Whose settings these are: everyone's, or the running game's alone.
        game_box_ = make(L"BUTTON", L"", BS_AUTOCHECKBOX | WS_TABSTOP, kIdGame, kMargin, 12,
                         kClientWidth - kMargin * 2, 22, bold_font_);

        // The presets, and which one the settings are now.
        make(L"STATIC", L"Preset:", SS_LEFT, 0, kMargin, kPresetTop + 6, 50, 20, font_);
        make(L"BUTTON", L"&Accuracy", BS_PUSHBUTTON | WS_TABSTOP, kIdAccuracy, kMargin + 52,
             kPresetTop, 110, 28, font_);
        make(L"BUTTON", L"Per&formance", BS_PUSHBUTTON | WS_TABSTOP, kIdPerformance,
             kMargin + 168, kPresetTop, 110, 28, font_);
        make(L"BUTTON", L"Defa&ults", BS_PUSHBUTTON | WS_TABSTOP, kIdDefaults, kMargin + 284,
             kPresetTop, 110, 28, font_);
        make(L"STATIC", L"Now:", SS_LEFT, 0, kMargin + 420, kPresetTop + 6, 36, 20, font_);
        preset_name_ = make(L"STATIC", L"", SS_LEFT, 0, kMargin + 458, kPresetTop + 6, 200, 20,
                            bold_font_);
        preset_text_ = make(L"STATIC", L"", SS_LEFT, 0, kMargin, kPresetTop + 38,
                            kClientWidth - kMargin * 2, 36, font_);

        // The two columns of groups. Each group box is made before its switches, so the switches
        // sit above it and paint over its frame rather than under it.
        int bottom = kGroupsTop;
        for (int column = 0; column < 2; ++column) {
            const int x = kMargin + column * (kColumnWidth + kMargin);
            int y = kGroupsTop;
            for (int first = 0; first < kSwitchCount;) {
                if (kSwitches[first].column != column) {
                    ++first;
                    continue;
                }
                int end = first;
                while (end < kSwitchCount && kSwitches[end].column == column &&
                       wcscmp(kSwitches[end].group, kSwitches[first].group) == 0)
                    ++end;
                const int count = end - first;
                const int height = kGroupHeader +
                                   count * (kSwitchHeight + kHintHeight) +
                                   (count - 1) * kItemGap + kGroupFooter;
                make(L"BUTTON", kSwitches[first].group, BS_GROUPBOX, 0, x, y, kColumnWidth,
                     height, font_);
                int item_y = y + kGroupHeader;
                for (int i = first; i < end; ++i) {
                    switches_[i] = make(L"BUTTON", kSwitches[i].label,
                                        BS_AUTOCHECKBOX | WS_TABSTOP, kIdSwitchFirst + i, x + 12,
                                        item_y, kColumnWidth - 24, kSwitchHeight, font_);
                    make(L"STATIC", kSwitches[i].hint, SS_LEFT, kIdHintFirst + i, x + 30,
                         item_y + kSwitchHeight, kColumnWidth - 42, kHintHeight, small_font_);
                    item_y += kSwitchHeight + kHintHeight + kItemGap;
                }
                y += height + kGroupGap;
                first = end;
            }
            bottom = std::max(bottom, y);
        }

        const int buttons_top = bottom + 4;
        note_ = make(L"STATIC", L"", SS_LEFT, 0, kMargin, buttons_top + 6, 620, 20, small_font_);
        make(L"BUTTON", L"Close", BS_PUSHBUTTON | WS_TABSTOP, kIdClose,
             kClientWidth - kMargin - 100, buttons_top, 100, 28, font_);
        const int client_height = buttons_top + 28 + kMargin;

        RECT bounds = { 0, 0, Scale(kClientWidth), Scale(client_height) };
        AdjustWindowRectExForDpi(&bounds, style, FALSE, 0, static_cast<UINT>(dpi_));
        SetWindowPos(window_, nullptr, 0, 0, bounds.right - bounds.left,
                     bounds.bottom - bounds.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        return true;
    }

    void EmulationSettingsWindow::Show() {
        if (window_ == nullptr)
            return;
        Refresh();
        ShowWindow(window_, SW_SHOW);
        SetForegroundWindow(window_);
    }

    void EmulationSettingsWindow::OnConfigChanged() {
        if (window_ != nullptr && IsWindowVisible(window_))
            Refresh();
    }

    void EmulationSettingsWindow::Refresh() {
        const EmuConfig& config = host_.config();
        for (int i = 0; i < kSwitchCount; ++i) {
            SendMessageW(switches_[i], BM_SETCHECK,
                         config.*(kSwitches[i].setting) ? BST_CHECKED : BST_UNCHECKED, 0);
        }
        const EmulationPreset preset = emulation::psx::MatchingEmulationPreset(config);
        SetWindowTextW(preset_name_, PresetName(preset));
        SetWindowTextW(preset_text_, PresetText(preset));

        const GameScope game = host_.game ? host_.game() : GameScope();
        const std::wstring label = game.running
                                       ? L"Separate settings for " + game.name
                                       : std::wstring(L"Separate settings for a game (start one "
                                                      L"first)");
        SetWindowTextW(game_box_, label.c_str());
        SendMessageW(game_box_, BM_SETCHECK, game.separate ? BST_CHECKED : BST_UNCHECKED, 0);
        EnableWindow(game_box_, game.running);
        SetWindowTextW(note_, game.separate
                                  ? L"Changes apply at once and are kept for this game alone, except "
                                    L"Pause while in menus."
                                  : L"Every change applies at once, to a running game, and to every "
                                    L"game.");
        const std::wstring caption = game.running
                                         ? L"PSXEmu - Emulation Settings - " + game.name
                                         : std::wstring(L"PSXEmu - Emulation Settings");
        SetWindowTextW(window_, caption.c_str());
    }

    LRESULT CALLBACK EmulationSettingsWindow::WindowProc(HWND window, UINT message, WPARAM wparam,
                                                         LPARAM lparam) {
        if (message == WM_NCCREATE) {
            const CREATESTRUCTW* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            SetWindowLongPtrW(window, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        }
        EmulationSettingsWindow* self =
            reinterpret_cast<EmulationSettingsWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (self == nullptr)
            return DefWindowProcW(window, message, wparam, lparam);

        switch (message) {
            case WM_CTLCOLORSTATIC: {
                // The hints in grey, so the switches read first.
                HDC dc = reinterpret_cast<HDC>(wparam);
                const int id = GetDlgCtrlID(reinterpret_cast<HWND>(lparam));
                if (id >= kIdHintFirst && id < kIdHintFirst + kSwitchCount)
                    SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
                else
                    SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
                SetBkColor(dc, GetSysColor(COLOR_BTNFACE));
                return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));
            }

            case WM_COMMAND: {
                const int id = LOWORD(wparam);
                const int code = HIWORD(wparam);
                if (id >= kIdSwitchFirst && id < kIdSwitchFirst + kSwitchCount) {
                    if (code == BN_CLICKED && self->host_.set) {
                        const int i = id - kIdSwitchFirst;
                        const bool on = SendMessageW(self->switches_[i], BM_GETCHECK, 0, 0) ==
                                        BST_CHECKED;
                        // The App's setter calls OnConfigChanged, which refreshes the preset.
                        self->host_.set(kSwitches[i].setting, on);
                    }
                    return 0;
                }
                if (code != BN_CLICKED)
                    return 0;
                switch (id) {
                    case kIdGame:
                        // The App's setter calls OnConfigChanged, which refreshes the window.
                        if (self->host_.set_separate)
                            self->host_.set_separate(
                                SendMessageW(self->game_box_, BM_GETCHECK, 0, 0) == BST_CHECKED);
                        break;
                    case kIdAccuracy:
                    case kIdPerformance:
                    case kIdDefaults:
                        if (self->host_.apply_preset)
                            self->host_.apply_preset(id == kIdAccuracy ? EmulationPreset::kAccuracy
                                                     : id == kIdPerformance
                                                         ? EmulationPreset::kPerformance
                                                         : EmulationPreset::kDefault);
                        break;
                    case kIdClose:
                        SendMessageW(window, WM_CLOSE, 0, 0);
                        break;
                }
                return 0;
            }

            case WM_CLOSE:
                ShowWindow(window, SW_HIDE);
                return 0;

            case WM_NCDESTROY:
                SetWindowLongPtrW(window, GWLP_USERDATA, 0);
                self->window_ = nullptr;
                break;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

}   // namespace psxemu
