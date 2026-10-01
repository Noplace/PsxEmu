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
#include "ui/video_settings_window.h"
#include "app/app_icon.h"

#include "app/const.h"   // the choice tables, RendererHasFilters
#include "ui/dpi.h"

#include <algorithm>
#include <commctrl.h>
#pragma comment(lib, "comctl32.lib")

namespace psxemu {

    namespace {

        const wchar_t kVideoSettingsClass[] = L"PSXEmuVideoSettings";

        const int kIdGame = 100;
        const int kIdRenderer = 101;
        const int kIdCard = 102;
        const int kIdFilter = 103;
        const int kIdSoftware = 104;
        const int kIdHardware = 105;
        const int kIdHardware12 = 121;
        const int kIdResolution = 106;
        const int kIdTrueColor = 107;
        const int kIdPgxpFirst = 108;   // three
        const int kIdDlssMode = 111;
        const int kIdDlssPreset = 112;
        const int kIdDlssGeneration = 113;
        const int kIdStats = 114;
        const int kIdNotifications = 115;
        const int kIdControllers = 116;
        const int kIdClassic = 117;
        const int kIdGlass = 118;
        const int kIdClose = 119;
        const int kIdDlssFiles = 120;
        const int kIdHintFirst = 300;   // the grey lines, which WM_CTLCOLORSTATIC finds by id
        const int kIdHintLast = 399;

        // The layout, in pixels at 96 DPI: the game's check box across the top; what shows the
        // picture across the window; what draws it and NVIDIA DLSS side by side, the same height;
        // what is drawn over it across the bottom; then Close. A list has its label in front of
        // it, in a column of their own so the lists in a group line up.
        const int kMargin = 12;
        const int kClientWidth = 760;
        const int kFullWidth = kClientWidth - kMargin * 2;
        const int kColumnWidth = (kClientWidth - kMargin * 3) / 2;
        const int kGroupsTop = 44;
        const int kGroupHeader = 22;   // from a group box's top to its first row
        const int kGroupFooter = 10;
        const int kGroupGap = 10;
        const int kInset = 12;         // a group's rows from its sides
        const int kListRow = 30;
        const int kSwitchRow = 24;
        const int kHintLine = 15;      // a line of the small font
        // A group across the window is two halves: the left for the short lists, the right wide
        // enough for a graphics card's whole name.
        const int kLeftHalf = 300;
        const int kHalfGap = 24;
        const int kRightHalf = kFullWidth - kInset * 2 - kLeftHalf - kHalfGap;
        const int kHalfLabel = 96;
        const int kColumnLabel = 118;   // in the two side by side: "Internal resolution:"

        void SetItems(HWND list, const std::vector<std::wstring>& items) {
            SendMessageW(list, CB_RESETCONTENT, 0, 0);
            for (const std::wstring& item : items)
                SendMessageW(list, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item.c_str()));
        }

        void Select(HWND list, int index) {
            SendMessageW(list, CB_SETCURSEL, static_cast<WPARAM>(index), 0);
        }

        int Selected(HWND list) {
            return static_cast<int>(SendMessageW(list, CB_GETCURSEL, 0, 0));
        }

        void Check(HWND button, bool on) {
            SendMessageW(button, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
        }

        bool Checked(HWND button) {
            return SendMessageW(button, BM_GETCHECK, 0, 0) == BST_CHECKED;
        }

        // Where a key sits in one of the tables, 0 - the first choice - if nowhere.
        template <typename Table>
        int IndexOf(const Table& table, const std::string& key) {
            for (size_t i = 0; i < std::size(table); ++i) {
                if (key == table[i].key)
                    return static_cast<int>(i);
            }
            return 0;
        }

    }   // namespace

    VideoSettingsWindow::~VideoSettingsWindow() {
        if (window_ != nullptr && IsWindow(window_))
            DestroyWindow(window_);
        for (HFONT font : { font_, bold_font_, small_font_ }) {
            if (font != nullptr)
                DeleteObject(font);
        }
    }

    bool VideoSettingsWindow::Create(HINSTANCE instance, HWND owner, Host host) {
        host_ = std::move(host);

        // The link to NVIDIA's files is a SysLink, Common Controls 6's.
        INITCOMMONCONTROLSEX controls = { sizeof(controls), ICC_STANDARD_CLASSES | ICC_LINK_CLASS };
        InitCommonControlsEx(&controls);

        WNDCLASSEXW window_class = {};
        window_class.cbSize = sizeof(window_class);
        if (!GetClassInfoExW(instance, kVideoSettingsClass, &window_class)) {
            window_class = {};
            window_class.cbSize = sizeof(window_class);
            window_class.lpfnWndProc = WindowProc;
            window_class.hInstance = instance;
            window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            window_class.hIcon = AppIcon(instance);
            window_class.hIconSm = AppIconSmall(instance);
            window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
            window_class.lpszClassName = kVideoSettingsClass;
            if (RegisterClassExW(&window_class) == 0)
                return false;
        }

        // Not WS_CLIPCHILDREN: a group box paints only its frame, and relies on the window
        // painting what is inside it.
        const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
        window_ = CreateWindowExW(0, kVideoSettingsClass, L"PSXEmu - Video Settings", style,
                                  CW_USEDEFAULT, CW_USEDEFAULT, 100, 100, owner, nullptr, instance,
                                  this);
        if (window_ == nullptr)
            return false;
        dpi_ = WindowDpi(window_);
        MakeFonts();

        auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD control_style, int id,
                        int x, int y, int w, int h, HFONT font) {
            HWND control = CreateWindowExW(
                0, cls, text, WS_CHILD | WS_VISIBLE | control_style, Scale(x), Scale(y),
                Scale(x + w) - Scale(x), Scale(y + h) - Scale(y), window_,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
            if (control != nullptr && font != nullptr)
                SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
            return control;
        };

        // A group's box is made first - so what is in it sits above it and paints over its frame
        // - and sized once its rows are in. `y` is where the next row goes.
        struct Group {
            HWND box;
            int x;
            int width;
            int top;
            int y;
        };
        int hint_id = kIdHintFirst;
        auto begin_group = [&](const wchar_t* title, int x, int width, int top) {
            HWND box = make(L"BUTTON", title, BS_GROUPBOX, 0, x, top, width, 0, font_);
            return Group{ box, x, width, top, top + kGroupHeader };
        };
        // To fit its rows, or `height` if that is more, so two side by side can match.
        auto end_group = [&](const Group& group, int height) {
            height = std::max(height, group.y - group.top + kGroupFooter);
            SetWindowPos(group.box, nullptr, 0, 0, Scale(group.x + group.width) - Scale(group.x),
                         Scale(group.top + height) - Scale(group.top),
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            return group.top + height;
        };
        // A list `width` wide with its label, the first `label_width` of it.
        // Its label kept in `label_out` too, for a list greyed with its label.
        auto list_at = [&](int x, int y, int label_width, int width, const wchar_t* label, int id,
                           HWND* label_out = nullptr) {
            HWND text = make(L"STATIC", label, SS_LEFT, 0, x, y + 4, label_width - 4, 20, font_);
            if (label_out != nullptr)
                *label_out = text;
            return make(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, id,
                        x + label_width, y, width - label_width, 320, font_);
        };
        auto switch_at = [&](int x, int y, int width, const wchar_t* label, DWORD kind, int id) {
            return make(L"BUTTON", label, kind | WS_TABSTOP, id, x, y, width, 20, font_);
        };
        // A line or two of the small font: grey with a hint id, saying what the settings need or
        // do; in the ordinary colour without, saying what is happening.
        auto text_at = [&](int x, int y, int width, int lines, const wchar_t* text, int id) {
            return make(L"STATIC", text, SS_LEFT, id, x, y, width, kHintLine * lines, small_font_);
        };
        // Rows down one of the two groups side by side.
        auto list_row = [&](Group& group, const wchar_t* label, int id,
                            HWND* label_out = nullptr) {
            HWND list = list_at(group.x + kInset, group.y, kColumnLabel,
                                group.width - kInset * 2, label, id, label_out);
            group.y += kListRow;
            return list;
        };
        auto switch_row = [&](Group& group, const wchar_t* label, DWORD kind, int id) {
            HWND button = switch_at(group.x + kInset, group.y, group.width - kInset * 2, label,
                                    kind, id);
            group.y += kSwitchRow;
            return button;
        };
        auto text_row = [&](Group& group, int lines, const wchar_t* text, int id) {
            HWND line = text_at(group.x + kInset, group.y + 2, group.width - kInset * 2, lines,
                                text, id);
            group.y += 2 + kHintLine * lines;
            return line;
        };

        // Whose settings these are: everyone's, or the running game's alone.
        game_box_ = make(L"BUTTON", L"", BS_AUTOCHECKBOX | WS_TABSTOP, kIdGame, kMargin, 12,
                         kFullWidth, 22, bold_font_);

        // What shows the picture, across the window.
        const int left = kMargin + kInset;
        const int right = left + kLeftHalf + kHalfGap;
        Group display = begin_group(L"Renderer", kMargin, kFullWidth, kGroupsTop);
        renderer_ = list_at(left, display.y, kHalfLabel, kLeftHalf, L"&Renderer:", kIdRenderer);
        card_ = list_at(right, display.y, kHalfLabel, kRightHalf, L"Graphics &card:", kIdCard);
        display.y += kListRow;
        filter_ = list_at(left, display.y, kHalfLabel, kLeftHalf, L"&Filter:", kIdFilter);
        text_at(right, display.y + 1, kRightHalf, 2,
                L"NVIDIA DLSS needs Direct3D 12. The filters need Direct3D 12, OpenGL or Vulkan.",
                hint_id++);
        display.y += kListRow;
        const int display_bottom = end_group(display, 0);

        // What draws it, and NVIDIA DLSS, which works on what it draws: side by side.
        const int middle = display_bottom + kGroupGap;
        Group raster = begin_group(L"Rasteriser", kMargin, kColumnWidth, middle);
        software_ = switch_row(raster, L"&Software: draws as the console does", BS_RADIOBUTTON,
                               kIdSoftware);
        hardware_ = switch_row(raster, L"&Hardware: on the graphics card, Direct3D 11",
                               BS_RADIOBUTTON, kIdHardware);
        hardware12_ = switch_row(raster, L"Hardware: on the graphics card, Direct3D 1&2",
                                 BS_RADIOBUTTON, kIdHardware12);
        raster.y += 4;
        resolution_ = list_row(raster, L"Internal r&esolution:", kIdResolution);
        true_color_ = switch_row(raster, L"&True colour (2x and above)", BS_AUTOCHECKBOX,
                                 kIdTrueColor);
        pgxp_[0] = switch_row(raster, L"PGXP: precise &vertices (no wobble)", BS_AUTOCHECKBOX,
                              kIdPgxpFirst);
        pgxp_[1] = switch_row(raster, L"PGXP: perspective-correct te&xtures", BS_AUTOCHECKBOX,
                              kIdPgxpFirst + 1);
        pgxp_[2] = switch_row(raster, L"PGXP: precise c&ulling (may break some games)",
                              BS_AUTOCHECKBOX, kIdPgxpFirst + 2);
        raster_hint_ = text_row(raster, 2, L"", hint_id++);

        Group dlss = begin_group(L"NVIDIA DLSS", kMargin * 2 + kColumnWidth, kColumnWidth, middle);
        dlss_labels_[0] = dlss.box;
        dlss_mode_ = list_row(dlss, L"&Mode:", kIdDlssMode, &dlss_labels_[1]);
        dlss_preset_ = list_row(dlss, L"&Preset:", kIdDlssPreset, &dlss_labels_[2]);
        dlss_generation_ = list_row(dlss, L"Frame &generation:", kIdDlssGeneration,
                                    &dlss_labels_[3]);
        dlss_status_ = text_row(dlss, 2, L"", 0);
        generation_status_ = text_row(dlss, 2, L"", 0);
        // Shown only while NVIDIA's files are not all here (State::dlss_files_missing).
        dlss_files_ = make(WC_LINK, L"<a>Get NVIDIA's DLSS files...</a>", WS_TABSTOP,
                           kIdDlssFiles, dlss.x + kInset, dlss.y + 4, dlss.width - kInset * 2,
                           20, font_);
        dlss.y += 4 + 20;
        dlss.y += 4;
        text_row(dlss, 3,
                 L"Needs an NVIDIA RTX graphics card, Direct3D 12 and the hardware rasteriser. "
                 L"Frame generation runs at 100% speed only, with NVIDIA Reflex on.",
                 hint_id++);

        const int middle_height = std::max(raster.y, dlss.y) - middle + kGroupFooter;
        end_group(raster, middle_height);
        const int middle_bottom = end_group(dlss, middle_height);

        // What is drawn over the picture, across the window.
        Group osd = begin_group(L"On-screen display", kMargin, kFullWidth,
                                middle_bottom + kGroupGap);
        stats_ = list_at(left, osd.y, kHalfLabel, kLeftHalf, L"Perf&ormance:", kIdStats);
        notifications_ = switch_at(right, osd.y + 3, kRightHalf, L"&Notifications",
                                   BS_AUTOCHECKBOX, kIdNotifications);
        osd.y += kListRow;
        make(L"STATIC", L"Theme:", SS_LEFT, 0, left, osd.y + 4, kHalfLabel - 4, 20, font_);
        classic_ = switch_at(left + kHalfLabel, osd.y + 3, 90, L"Classic", BS_RADIOBUTTON,
                             kIdClassic);
        glass_ = switch_at(left + kHalfLabel + 96, osd.y + 3, 90, L"Glass", BS_RADIOBUTTON,
                           kIdGlass);
        controllers_ = switch_at(right, osd.y + 3, kRightHalf, L"Always show the contro&llers",
                                 BS_AUTOCHECKBOX, kIdControllers);
        osd.y += kListRow;
        text_at(left, osd.y, kFullWidth - kInset * 2, 1,
                L"F9 steps through the performance panels.", hint_id++);
        osd.y += kHintLine + 2;
        const int osd_bottom = end_group(osd, 0);

        const int buttons_top = osd_bottom + 8;
        note_ = make(L"STATIC", L"", SS_LEFT, 0, kMargin, buttons_top,
                     kClientWidth - kMargin * 3 - 100, 30, small_font_);
        make(L"BUTTON", L"Close", BS_PUSHBUTTON | WS_TABSTOP, kIdClose,
             kClientWidth - kMargin - 100, buttons_top, 100, 28, font_);
        const int client_height = buttons_top + 28 + kMargin;

        // The lists that never change.
        {
            std::vector<std::wstring> items;
            for (const BackendChoice& choice : kBackendChoices)
                items.push_back(choice.label);
            SetItems(renderer_, items);
            items.clear();
            for (const FilterChoice& choice : kFilterChoices)
                items.push_back(choice.label);
            SetItems(filter_, items);
            items.clear();
            for (const ResolutionChoice& choice : kResolutionChoices)
                items.push_back(choice.label);
            SetItems(resolution_, items);
            items.clear();
            for (const DlssModeChoice& choice : kDlssModeChoices)
                items.push_back(choice.label);
            SetItems(dlss_mode_, items);
            items.clear();
            for (const DlssModeChoice& choice : kDlssPresetChoices)
                items.push_back(choice.label);
            SetItems(dlss_preset_, items);
            items.clear();
            for (const wchar_t* choice : kStatsChoices)
                items.push_back(choice);
            SetItems(stats_, items);
        }

        RECT bounds = { 0, 0, Scale(kClientWidth), Scale(client_height) };
        AdjustWindowRectExForDpi(&bounds, style, FALSE, 0, static_cast<UINT>(dpi_));
        SetWindowPos(window_, nullptr, 0, 0, bounds.right - bounds.left,
                     bounds.bottom - bounds.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        return true;
    }

    void VideoSettingsWindow::MakeFonts() {
        font_ = CreateMessageFont(dpi_);
        bold_font_ = CreateMessageFont(dpi_, true);
        small_font_ = CreateMessageFont(dpi_, false, 90);
    }

    void VideoSettingsWindow::Show() {
        if (window_ == nullptr)
            return;
        Refresh();
        ShowWindow(window_, SW_SHOW);
        SetForegroundWindow(window_);
    }

    void VideoSettingsWindow::OnSettingsChanged() {
        if (window_ != nullptr && IsWindowVisible(window_))
            Refresh();
    }

    void VideoSettingsWindow::Refresh() {
        if (!host_.state)
            return;
        const State state = host_.state();

        // Renderer.
        Select(renderer_, IndexOf(kBackendChoices, state.renderer));
        // The cards: Automatic, then each - or, with one card, nothing to choose and its name.
        std::vector<std::wstring> cards;
        if (state.cards.size() < 2) {
            cards.push_back(state.cards.empty() ? std::wstring(L"(none found)")
                                                : state.cards[0] + L" (the only one)");
        } else {
            cards.push_back(L"Automatic: each renderer's own choice");
            cards.insert(cards.end(), state.cards.begin(), state.cards.end());
        }
        if (cards != card_items_) {
            card_items_ = cards;
            SetItems(card_, cards);
        }
        const bool choosing_card = state.cards.size() >= 2;
        Select(card_, choosing_card ? state.card + 1 : 0);
        EnableWindow(card_, choosing_card);
        const bool filters = RendererHasFilters(state.renderer);
        Select(filter_, filters ? IndexOf(kFilterChoices, state.filter) : 0);
        EnableWindow(filter_, filters);

        // Rasteriser: what is drawing, which a hardware one that could not be made leaves as
        // software. Its resolution, true colour and PGXP are the hardware one's alone - greyed
        // while software draws, and still showing what switching would give. While DLSS runs it
        // has the resolution and precise vertices: shown, and greyed.
        Check(software_, !state.hardware);
        Check(hardware_, state.hardware && !state.hardware_d3d12);
        Check(hardware12_, state.hardware && state.hardware_d3d12);
        int resolution = 0;
        for (size_t i = 0; i < std::size(kResolutionChoices); ++i) {
            if (kResolutionChoices[i].scale == state.resolution_scale)
                resolution = static_cast<int>(i);
        }
        Select(resolution_, resolution);
        EnableWindow(resolution_, state.hardware && !state.dlss_running);
        Check(true_color_, state.true_color);
        EnableWindow(true_color_, state.hardware && state.resolution_scale > 1);
        const bool vertices = state.pgxp_vertices || state.dlss_running;
        Check(pgxp_[0], vertices);
        Check(pgxp_[1], state.pgxp_textures);
        Check(pgxp_[2], state.pgxp_culling);
        EnableWindow(pgxp_[0], state.hardware && !state.dlss_running);
        EnableWindow(pgxp_[1], state.hardware && vertices);
        EnableWindow(pgxp_[2], state.hardware && vertices);
        SetWindowTextW(raster_hint_,
                       !state.hardware
                           ? L"The resolution, true colour and PGXP are the hardware rasteriser's."
                       : state.dlss_running
                           ? L"NVIDIA DLSS sets the internal resolution, and keeps precise "
                             L"vertices on, while it runs."
                           : L"The hardware rasteriser draws at up to 8x the console's "
                             L"resolution. Experimental.");

        // NVIDIA DLSS. Frame generation past 2x, and Dynamic, only once the card says it makes
        // them - and whatever is chosen, so the list never shows something else.
        // The whole group greyed unless what DLSS draws with is in place; the status line, still
        // in black, says what is missing.
        const bool available = state.dlss_available;
        const bool dlss = available && state.dlss_mode != "off";
        for (HWND part : dlss_labels_)
            EnableWindow(part, available);
        Select(dlss_mode_, IndexOf(kDlssModeChoices, state.dlss_mode));
        EnableWindow(dlss_mode_, available);
        Select(dlss_preset_, IndexOf(kDlssPresetChoices, state.dlss_preset));
        EnableWindow(dlss_preset_, dlss);
        std::vector<std::string> generation;
        std::vector<std::wstring> generation_labels;
        for (size_t i = 0; i < std::size(kDlssGenerationChoices); ++i) {
            const DlssModeChoice& choice = kDlssGenerationChoices[i];
            const bool dynamic = std::string(choice.key) == "dynamic";
            const bool offered = i <= 1 || (dynamic ? state.generation_dynamic
                                                    : static_cast<int>(i) <= state.generation_most);
            if (offered || state.dlss_generation == choice.key) {
                generation.push_back(choice.key);
                generation_labels.push_back(choice.label);
            }
        }
        if (generation != generation_items_) {
            generation_items_ = generation;
            SetItems(dlss_generation_, generation_labels);
        }
        const auto chosen = std::find(generation.begin(), generation.end(), state.dlss_generation);
        Select(dlss_generation_, chosen == generation.end()
                                     ? 0
                                     : static_cast<int>(chosen - generation.begin()));
        EnableWindow(dlss_generation_, dlss);
        SetWindowTextW(dlss_status_, state.dlss_status.c_str());
        SetWindowTextW(generation_status_, state.generation_status.c_str());
        ShowWindow(dlss_files_, state.dlss_files_missing ? SW_SHOWNA : SW_HIDE);

        // On-screen display.
        Select(stats_, std::clamp(state.stats_mode, 0, 2));
        Check(notifications_, state.notifications);
        Check(controllers_, state.controllers_always);
        Check(classic_, !state.glass);
        Check(glass_, state.glass);

        // Whose settings.
        const GameScope game = host_.game ? host_.game() : GameScope();
        const std::wstring label = game.running
                                       ? L"Separate settings for " + game.name
                                       : std::wstring(L"Separate settings for a game (start one "
                                                      L"first)");
        SetWindowTextW(game_box_, label.c_str());
        Check(game_box_, game.separate);
        EnableWindow(game_box_, game.running);
        SetWindowTextW(note_, game.separate
                                  ? L"The rasteriser and what it adds, and the DLSS mode and frame "
                                    L"generation, are kept for this game alone. The rest is "
                                    L"shared."
                                  : L"Every change applies at once, to a running game, and to every "
                                    L"game.");
        const std::wstring caption = game.running
                                         ? L"PSXEmu - Video Settings - " + game.name
                                         : std::wstring(L"PSXEmu - Video Settings");
        SetWindowTextW(window_, caption.c_str());
    }

    // Each change goes to the App, whose setter refreshes the window - which is what puts a
    // list back where it was if the change was refused.
    void VideoSettingsWindow::OnCommand(int id, int code) {
        if (code == CBN_SELCHANGE) {
            const int index = Selected(GetDlgItem(window_, id));
            if (index < 0)
                return;
            switch (id) {
                case kIdRenderer:
                    if (index < static_cast<int>(std::size(kBackendChoices)) && host_.set_renderer)
                        host_.set_renderer(kBackendChoices[index].key);
                    break;
                case kIdCard:
                    if (host_.set_card)
                        host_.set_card(index - 1);
                    break;
                case kIdFilter:
                    if (index < static_cast<int>(std::size(kFilterChoices)) && host_.set_filter)
                        host_.set_filter(kFilterChoices[index].key);
                    break;
                case kIdResolution:
                    if (index < static_cast<int>(std::size(kResolutionChoices)) &&
                        host_.set_resolution)
                        host_.set_resolution(kResolutionChoices[index].scale);
                    break;
                case kIdDlssMode:
                    if (index < static_cast<int>(std::size(kDlssModeChoices)) &&
                        host_.set_dlss_mode)
                        host_.set_dlss_mode(kDlssModeChoices[index].key);
                    break;
                case kIdDlssPreset:
                    if (index < static_cast<int>(std::size(kDlssPresetChoices)) &&
                        host_.set_dlss_preset)
                        host_.set_dlss_preset(kDlssPresetChoices[index].key);
                    break;
                case kIdDlssGeneration:
                    if (index < static_cast<int>(generation_items_.size()) &&
                        host_.set_dlss_generation)
                        host_.set_dlss_generation(generation_items_[index]);
                    break;
                case kIdStats:
                    if (host_.set_stats)
                        host_.set_stats(index);
                    break;
            }
            return;
        }
        if (code != BN_CLICKED)
            return;
        switch (id) {
            case kIdGame:
                if (host_.set_separate)
                    host_.set_separate(Checked(game_box_));
                break;
            case kIdSoftware:
            case kIdHardware:
            case kIdHardware12:
                if (host_.set_rasteriser)
                    host_.set_rasteriser(id == kIdSoftware   ? "software"
                                         : id == kIdHardware ? "hardware"
                                                             : "hardware_d3d12");
                // The machine says what it ended up with later; until then, what was clicked.
                Check(software_, id == kIdSoftware);
                Check(hardware_, id == kIdHardware);
                Check(hardware12_, id == kIdHardware12);
                break;
            case kIdTrueColor:
                if (host_.set_true_color)
                    host_.set_true_color(Checked(true_color_));
                break;
            case kIdPgxpFirst:
            case kIdPgxpFirst + 1:
            case kIdPgxpFirst + 2:
                if (host_.set_pgxp)
                    host_.set_pgxp(id - kIdPgxpFirst, Checked(pgxp_[id - kIdPgxpFirst]));
                break;
            case kIdNotifications:
                if (host_.set_notifications)
                    host_.set_notifications(Checked(notifications_));
                break;
            case kIdControllers:
                if (host_.set_controllers_always)
                    host_.set_controllers_always(Checked(controllers_));
                break;
            case kIdClassic:
            case kIdGlass:
                if (host_.set_glass)
                    host_.set_glass(id == kIdGlass);
                break;
            case kIdClose:
                SendMessageW(window_, WM_CLOSE, 0, 0);
                break;
        }
    }

    LRESULT CALLBACK VideoSettingsWindow::WindowProc(HWND window, UINT message, WPARAM wparam,
                                                     LPARAM lparam) {
        if (message == WM_NCCREATE) {
            const CREATESTRUCTW* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            SetWindowLongPtrW(window, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        }
        VideoSettingsWindow* self =
            reinterpret_cast<VideoSettingsWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (self == nullptr)
            return DefWindowProcW(window, message, wparam, lparam);

        switch (message) {
            case WM_CTLCOLORSTATIC: {
                // The hints in grey, so the settings read first - and anything greyed, the
                // DLSS group's box among them, which would otherwise take the black here.
                HDC dc = reinterpret_cast<HDC>(wparam);
                const HWND control = reinterpret_cast<HWND>(lparam);
                const int id = GetDlgCtrlID(control);
                SetTextColor(dc, GetSysColor((id >= kIdHintFirst && id <= kIdHintLast) ||
                                                     !IsWindowEnabled(control)
                                                 ? COLOR_GRAYTEXT
                                                 : COLOR_BTNTEXT));
                SetBkColor(dc, GetSysColor(COLOR_BTNFACE));
                return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));
            }

            case WM_COMMAND:
                self->OnCommand(LOWORD(wparam), HIWORD(wparam));
                return 0;

            case WM_NOTIFY: {
                // The link to NVIDIA's files, clicked or chosen with Enter.
                const NMHDR* header = reinterpret_cast<const NMHDR*>(lparam);
                if (header->idFrom == static_cast<UINT_PTR>(kIdDlssFiles) &&
                    (header->code == NM_CLICK || header->code == NM_RETURN) &&
                    self->host_.get_dlss_files)
                    self->host_.get_dlss_files();
                return 0;
            }

            case WM_DPICHANGED: {
                // Onto a monitor with another scaling: the same layout, at its DPI.
                const int from = self->dpi_;
                const HFONT old[] = { self->font_, self->bold_font_, self->small_font_ };
                self->dpi_ = HIWORD(wparam);
                self->MakeFonts();
                SwapFonts(window, { { old[0], self->font_ },
                                    { old[1], self->bold_font_ },
                                    { old[2], self->small_font_ } });
                for (HFONT font : old) {
                    if (font != nullptr)
                        DeleteObject(font);
                }
                RescaleForDpi(window, from, self->dpi_, *reinterpret_cast<const RECT*>(lparam));
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
