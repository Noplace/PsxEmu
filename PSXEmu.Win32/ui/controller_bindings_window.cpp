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
#include "ui/controller_bindings_window.h"
#include "app/app_icon.h"

#include "app/menu.h"   // ParseControllerType, ParseInputSource

#include <commctrl.h>
#include <windowsx.h>   // GET_X_LPARAM
#pragma comment(lib, "comctl32.lib")

// GDI+ for the picture: anti-aliased curves, which plain GDI cannot draw. Its headers use min and
// max unqualified, which NOMINMAX (psx.h) takes away, and need objidl.h, which WIN32_LEAN_AND_MEAN
// leaves out.
#include <algorithm>
#include <objidl.h>
namespace Gdiplus {
    using std::max;
    using std::min;
}   // namespace Gdiplus
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")

namespace psxemu {

    using emulation::psx::Sio;

    namespace {

        const wchar_t kBindingsWindowClass[] = L"PSXEmuControllerBindings";
        const wchar_t kCanvasClass[] = L"PSXEmuControllerBindingsCanvas";

        const int kIdSlot = 100;
        const int kIdDevice = 101;
        const int kIdUseDevice = 102;
        const int kIdDefaults = 103;
        const int kIdClearAll = 104;
        const int kIdCopyAll = 105;
        const int kIdClose = 106;
        const int kIdType = 107;
        const int kIdGame = 108;
        const int kIdBoxFirst = 200;   // one per button, in kKeyBindings order
        const int kIdMenuChange = 300;
        const int kIdMenuClear = 301;

        const UINT kMessageBeginCapture = WM_APP + 1;
        const UINT_PTR kPadTimer = 1;

        // The layout, in pixels at 96 DPI. The canvas holds the picture and the boxes; the rest of
        // the window is the two lists across the top and the buttons along the bottom.
        const int kMargin = 12;
        const int kClientWidth = 804;
        const int kClientHeight = 642;
        const int kCanvasTop = 134;
        const int kCanvasWidth = 780;
        const int kCanvasHeight = 420;
        const int kBoxWidth = 160;
        const int kBoxHeight = 36;
        const int kRowPitch = 45;
        const int kLeftColumn = 12;
        const int kRightColumn = kCanvasWidth - 12 - kBoxWidth;
        const float kPadCentreX = 390.0f;
        const float kPadCentreY = 205.0f;
        const float kPadScale = 1.35f;

        int Row(int row) { return 10 + row * kRowPitch; }

        // Where each button's box sits and what its line points at, in kKeyBindings order: the
        // left-hand controls down the left, top to bottom as they sit on the pad, and the same on
        // the right. Anchors are in pad units - the picture below is drawn in the same units,
        // centred on the pad.
        struct BoxPlace {
            int column;   // 0 left, 1 right
            int row;
            float x, y;
        };
        // clang-format off
        const BoxPlace kPlaces[kPadButtons] = {
            { 0, 2, -80.0f, -27.0f },   // Up
            { 0, 5, -80.0f,  12.0f },   // Down
            { 0, 3, -99.0f,  -8.0f },   // Left
            { 0, 4, -61.0f,  -8.0f },   // Right
            { 1, 5,  80.0f,  17.0f },   // Cross
            { 1, 4,  55.0f,  -8.0f },   // Square
            { 1, 3, 105.0f,  -8.0f },   // Circle
            { 1, 2,  80.0f, -33.0f },   // Triangle
            { 0, 1, -85.0f, -64.0f },   // L1
            { 1, 1,  85.0f, -64.0f },   // R1
            { 0, 0, -85.0f, -79.0f },   // L2
            { 1, 0,  85.0f, -79.0f },   // R2
            { 1, 6,  22.0f,  -8.0f },   // Start
            { 0, 6, -22.0f,  -8.0f },   // Select
            { 1, 8,   0.0f,  22.0f },   // ANALOG
            { 0, 7, -42.0f,  32.0f },   // L3
            { 1, 7,  42.0f,  32.0f },   // R3
        };
        // clang-format on

        const int kButtonAnalog = 14;
        const int kButtonL3 = 15;
        const int kButtonR3 = 16;
        const int kButtonCross = 4;
        const int kButtonCircle = 6;

        // The palette. Light, like the rest of the front end's windows.
        const COLORREF kCanvasBack = RGB(248, 249, 251);
        const Gdiplus::Color kOutline(255, 74, 79, 87);
        const Gdiplus::Color kBodyTop(255, 230, 232, 236);
        const Gdiplus::Color kBodyBottom(255, 196, 200, 206);
        const Gdiplus::Color kShoulderFront(255, 150, 155, 163);
        const Gdiplus::Color kShoulderBack(255, 118, 123, 131);
        const Gdiplus::Color kControlDark(255, 62, 66, 72);
        const Gdiplus::Color kControlMid(255, 96, 101, 109);
        const Gdiplus::Color kLine(255, 128, 135, 146);
        const Gdiplus::Color kAccent(255, 26, 115, 232);
        const Gdiplus::Color kTriangleGreen(255, 52, 190, 150);
        const Gdiplus::Color kCircleRed(255, 236, 84, 90);
        const Gdiplus::Color kCrossBlue(255, 108, 150, 240);
        const Gdiplus::Color kSquarePink(255, 232, 140, 200);

        std::wstring StripAmpersand(const wchar_t* label) {
            std::wstring out;
            for (const wchar_t* p = label; *p != 0; ++p) {
                if (*p != L'&')
                    out += *p;
            }
            return out;
        }

        std::wstring TypeName(Sio::ControllerType type) {
            switch (type) {
                case Sio::kDigital: return L"an original digital pad";
                case Sio::kDualAnalog: return L"a Dual Analog";
                case Sio::kDualShock: return L"a DualShock";
                case Sio::kMouse: return L"a mouse";
                case Sio::kMultitap: return L"a multitap";
                case Sio::kGunCon: return L"a GunCon";
                default: return L"nothing";
            }
        }

        std::wstring DeviceName(int device) {
            return StripAmpersand(kInputSourceChoices[device].label);
        }

        // The same in the middle of a sentence: "played with the keyboard", "played with Gamepad 1".
        std::wstring DevicePhrase(int device) {
            return device == kKeyboardDevice ? std::wstring(L"the keyboard") : DeviceName(device);
        }


        // A rounded rectangle as a path, for the body and the shoulder buttons.
        void AddRoundRect(Gdiplus::GraphicsPath* path, float x, float y, float w, float h,
                          float r) {
            const float d = r * 2.0f;
            path->AddArc(x, y, d, d, 180.0f, 90.0f);
            path->AddArc(x + w - d, y, d, d, 270.0f, 90.0f);
            path->AddArc(x + w - d, y + h - d, d, d, 0.0f, 90.0f);
            path->AddArc(x, y + h - d, d, d, 90.0f, 90.0f);
            path->CloseFigure();
        }

    }   // namespace

    ControllerBindingsWindow::~ControllerBindingsWindow() {
        if (window_ != nullptr && IsWindow(window_))
            DestroyWindow(window_);
        for (HFONT font : { font_, bold_font_, small_font_ }) {
            if (font != nullptr)
                DeleteObject(font);
        }
        if (gdiplus_token_ != 0)
            Gdiplus::GdiplusShutdown(gdiplus_token_);
    }

    bool ControllerBindingsWindow::Create(HINSTANCE instance, HWND owner, Host host) {
        host_ = std::move(host);

        INITCOMMONCONTROLSEX controls = { sizeof(controls), ICC_STANDARD_CLASSES };
        InitCommonControlsEx(&controls);
        Gdiplus::GdiplusStartupInput gdiplus_input;
        if (Gdiplus::GdiplusStartup(&gdiplus_token_, &gdiplus_input, nullptr) != Gdiplus::Ok)
            gdiplus_token_ = 0;

        auto register_class = [instance](const wchar_t* name, WNDPROC proc, HBRUSH background) {
            WNDCLASSEXW window_class = {};
            window_class.cbSize = sizeof(window_class);
            if (GetClassInfoExW(instance, name, &window_class))
                return true;
            window_class = {};
            window_class.cbSize = sizeof(window_class);
            window_class.lpfnWndProc = proc;
            window_class.hInstance = instance;
            window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            window_class.hIcon = AppIcon(instance);
            window_class.hIconSm = AppIconSmall(instance);
            window_class.hbrBackground = background;
            window_class.lpszClassName = name;
            return RegisterClassExW(&window_class) != 0;
        };
        if (!register_class(kBindingsWindowClass, WindowProc,
                            reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1)) ||
            !register_class(kCanvasClass, CanvasProc, nullptr))
            return false;

        const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
        window_ = CreateWindowExW(0, kBindingsWindowClass, L"PSXEmu - Controllers", style,
                                  CW_USEDEFAULT, CW_USEDEFAULT, 100, 100, owner, nullptr, instance,
                                  this);
        if (window_ == nullptr)
            return false;

        // Everything is laid out in 96-DPI pixels and scaled by the window's own DPI, so the
        // picture and the boxes stay in proportion however the front end is scaled.
        dpi_ = static_cast<int>(GetDpiForWindow(window_));
        if (dpi_ <= 0)
            dpi_ = 96;
        RECT bounds = { 0, 0, Scale(kClientWidth), Scale(kClientHeight) };
        AdjustWindowRectExForDpi(&bounds, style, FALSE, 0, static_cast<UINT>(dpi_));
        SetWindowPos(window_, nullptr, 0, 0, bounds.right - bounds.left,
                     bounds.bottom - bounds.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

        NONCLIENTMETRICSW metrics = { sizeof(metrics) };
        if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0,
                                       static_cast<UINT>(dpi_))) {
            font_ = CreateFontIndirectW(&metrics.lfMessageFont);
            LOGFONTW bold = metrics.lfMessageFont;
            bold.lfWeight = FW_BOLD;
            bold_font_ = CreateFontIndirectW(&bold);
            LOGFONTW caption = metrics.lfMessageFont;
            caption.lfHeight = caption.lfHeight * 85 / 100;
            small_font_ = CreateFontIndirectW(&caption);
        }

        auto make = [&](HWND parent, const wchar_t* cls, const wchar_t* text, DWORD control_style,
                        int id, int x, int y, int w, int h) {
            HWND control = CreateWindowExW(
                0, cls, text, WS_CHILD | WS_VISIBLE | control_style, Scale(x), Scale(y), Scale(w),
                Scale(h), parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance,
                nullptr);
            if (control != nullptr && font_ != nullptr)
                SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font_), FALSE);
            return control;
        };

        make(window_, L"STATIC", L"Port:", SS_LEFT, 0, kMargin, 16, 66, 20);
        slot_list_ = make(window_, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
                          kIdSlot, kMargin + 70, 12, 298, 300);
        make(window_, L"STATIC", L"Device:", SS_LEFT, 0, 400, 16, 50, 20);
        device_list_ = make(window_, WC_COMBOBOXW, L"",
                            CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, kIdDevice, 452, 12, 200,
                            300);
        use_device_ = make(window_, L"BUTTON", L"Use for This Port", BS_PUSHBUTTON | WS_TABSTOP,
                           kIdUseDevice, 660, 11, 132, 26);
        make(window_, L"STATIC", L"Controller:", SS_LEFT, 0, kMargin, 48, 66, 20);
        type_list_ = make(window_, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
                          kIdType, kMargin + 70, 44, 298, 300);
        game_box_ = make(window_, L"BUTTON", L"Separate settings for this game",
                         BS_AUTOCHECKBOX | WS_TABSTOP, kIdGame, 400, 46, 392, 22);
        info_ = make(window_, L"STATIC", L"", SS_LEFT, 0, kMargin, 78, kClientWidth - kMargin * 2,
                     52);

        canvas_ = CreateWindowExW(WS_EX_CLIENTEDGE, kCanvasClass, L"",
                                  WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, Scale(kMargin),
                                  Scale(kCanvasTop), Scale(kCanvasWidth) + 4,
                                  Scale(kCanvasHeight) + 4, window_, nullptr, instance, this);
        for (int i = 0; i < kPadButtons; ++i) {
            const BoxPlace& place = kPlaces[i];
            boxes_[i] = make(canvas_, L"BUTTON", L"", BS_OWNERDRAW | BS_NOTIFY | WS_TABSTOP,
                             kIdBoxFirst + i, place.column == 0 ? kLeftColumn : kRightColumn,
                             Row(place.row), kBoxWidth, kBoxHeight);
        }

        const int status_top = kCanvasTop + kCanvasHeight + 12;
        status_ = make(window_, L"STATIC", L"", SS_LEFT, 0, kMargin, status_top,
                       kClientWidth - kMargin * 2, 36);
        const int buttons_top = kClientHeight - kMargin - 28;
        defaults_ = make(window_, L"BUTTON", L"Restore Defaults", BS_PUSHBUTTON | WS_TABSTOP,
                         kIdDefaults, kMargin, buttons_top, 130, 28);
        clear_all_ = make(window_, L"BUTTON", L"Clear All", BS_PUSHBUTTON | WS_TABSTOP,
                          kIdClearAll, kMargin + 136, buttons_top, 100, 28);
        copy_all_ = make(window_, L"BUTTON", L"Copy to All Ports", BS_PUSHBUTTON | WS_TABSTOP,
                         kIdCopyAll, kMargin + 242, buttons_top, 140, 28);
        close_ = make(window_, L"BUTTON", L"Close", BS_PUSHBUTTON | WS_TABSTOP, kIdClose,
                      kClientWidth - kMargin - 100, buttons_top, 100, 28);
        return true;
    }

    void ControllerBindingsWindow::Show(const ControllerBindings& current) {
        if (window_ == nullptr)
            return;
        bindings_ = current;
        capturing_ = -1;
        // Open on the device that plays the slot, which is almost always the one to bind.
        device_ = SlotSourceDevice(slot_);
        Refresh();
        SetStatus(L"Click a box, then press the key or the gamepad control for that button. "
                  L"Right-click a box to clear it.");
        ShowWindow(window_, SW_SHOW);
        SetForegroundWindow(window_);
    }

    void ControllerBindingsWindow::OnConfigChanged() {
        if (window_ != nullptr && IsWindowVisible(window_))
            Refresh();
    }

    // ---------------------------------------------------------------------------------------------
    // What the slot is
    // ---------------------------------------------------------------------------------------------

    Sio::ControllerType ControllerBindingsWindow::SlotType() const {
        const BindingSlot& slot = kBindingSlots[slot_];
        const emulation::psx::EmuConfig& config = Config();
        if (slot.player < 0)
            return ParseControllerType(config.controller_type[slot.port]);
        return ParseControllerType(config.multitap_player_type[slot.port][slot.player]);
    }

    int ControllerBindingsWindow::SlotSourceDevice(int slot) const {
        const BindingSlot& place = kBindingSlots[slot];
        const emulation::psx::EmuConfig& config = Config();
        const std::string& key = place.player < 0
                                     ? config.input_source[place.port]
                                     : config.multitap_player_source[place.port][place.player];
        return static_cast<int>(ParseInputSource(key));
    }

    // Through the input thread rather than XInput directly, since that is what knows about the
    // PlayStation pads too, and which Gamepad each of them is.
    emulation::host::PadReading ControllerBindingsWindow::ReadPad(int device) const {
        emulation::host::PadReading reading;
        if (device == kKeyboardDevice)
            reading.connected = true;
        else if (host_.read_pad)
            reading = host_.read_pad(device - 1);
        return reading;
    }

    bool ControllerBindingsWindow::PlayStationNames(int device) const {
        if (device == kKeyboardDevice)
            return false;
        const emulation::host::PadReading pad = ReadPad(device);
        return pad.connected && pad.kind != emulation::host::PadKind::kXInput;
    }

    bool ControllerBindingsWindow::PadShown() const {
        switch (SlotType()) {
            case Sio::kDigital:
            case Sio::kDualAnalog:
            case Sio::kDualShock:
            case Sio::kGunCon:
                return true;
            default:
                return false;
        }
    }

    bool ControllerBindingsWindow::ButtonShown(int button) const {
        if (!PadShown())
            return false;
        const Sio::ControllerType type = SlotType();
        // A GunCon's A and B are its source's Cross and Circle; nothing else reaches it.
        if (type == Sio::kGunCon)
            return button == kButtonCross || button == kButtonCircle;
        if (button == kButtonAnalog || button == kButtonL3 || button == kButtonR3)
            return type == Sio::kDualAnalog || type == Sio::kDualShock;
        return true;
    }

    // ---------------------------------------------------------------------------------------------
    // Refreshing
    // ---------------------------------------------------------------------------------------------

    void ControllerBindingsWindow::Refresh() {
        // The port list says what each slot holds, so the choice is informed before it is made.
        SendMessageW(slot_list_, CB_RESETCONTENT, 0, 0);
        const emulation::psx::EmuConfig& config = Config();
        for (int slot = 0; slot < kBindingSlotCount; ++slot) {
            const BindingSlot& place = kBindingSlots[slot];
            std::wstring label = place.label;
            const bool multitap =
                ParseControllerType(config.controller_type[place.port]) == Sio::kMultitap;
            if (place.player >= 0 && !multitap) {
                label += L"  (no multitap)";
            } else {
                const std::string& type = place.player < 0
                                              ? config.controller_type[place.port]
                                              : config.multitap_player_type[place.port][place.player];
                for (const ControllerTypeChoice& choice : kControllerTypeChoices) {
                    if (type == choice.key)
                        label += L"  -  " + StripAmpersand(choice.label);
                }
            }
            SendMessageW(slot_list_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        }
        SendMessageW(slot_list_, CB_SETCURSEL, static_cast<WPARAM>(slot_), 0);
        FillTypeList();
        FillDeviceList();
        UpdateInfo();
        LayoutBoxes();
        InvalidateRect(canvas_, nullptr, TRUE);

        // Whose the types are. The bindings and the devices are everyone's either way.
        const GameScope game = host_.game ? host_.game() : GameScope();
        SendMessageW(game_box_, BM_SETCHECK, game.separate ? BST_CHECKED : BST_UNCHECKED, 0);
        EnableWindow(game_box_, game.running);
        const std::wstring caption = game.running ? L"PSXEmu - Controllers - " + game.name
                                                  : std::wstring(L"PSXEmu - Controllers");
        SetWindowTextW(window_, caption.c_str());
    }

    // What can be plugged into the slot: a port takes anything, a multitap player only a pad.
    std::span<const ControllerTypeChoice> ControllerBindingsWindow::TypeChoices() const {
        if (kBindingSlots[slot_].player < 0)
            return kControllerTypeChoices;
        return kMultitapPlayerTypeChoices;
    }

    void ControllerBindingsWindow::FillTypeList() {
        SendMessageW(type_list_, CB_RESETCONTENT, 0, 0);
        const BindingSlot& place = kBindingSlots[slot_];
        const emulation::psx::EmuConfig& config = Config();
        const std::string& current = place.player < 0
                                         ? config.controller_type[place.port]
                                         : config.multitap_player_type[place.port][place.player];
        int selected = -1;
        int index = 0;
        for (const ControllerTypeChoice& choice : TypeChoices()) {
            const std::wstring label = StripAmpersand(choice.label);
            SendMessageW(type_list_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
            if (current == choice.key)
                selected = index;
            ++index;
        }
        SendMessageW(type_list_, CB_SETCURSEL, static_cast<WPARAM>(selected), 0);
        // A player has nothing to choose while their port has no multitap.
        const bool multitap_port =
            ParseControllerType(config.controller_type[place.port]) == Sio::kMultitap;
        EnableWindow(type_list_, place.player < 0 || multitap_port);
    }

    void ControllerBindingsWindow::FillDeviceList() {
        SendMessageW(device_list_, CB_RESETCONTENT, 0, 0);
        const int in_use = SlotSourceDevice(slot_);
        for (int device = 0; device < kBindingDevices; ++device) {
            std::wstring label = DeviceName(device);
            const emulation::host::PadReading pad = ReadPad(device);
            if (device != kKeyboardDevice && pad.connected &&
                pad.kind != emulation::host::PadKind::kXInput)
                label += pad.kind == emulation::host::PadKind::kDualSense ? L" - DualSense"
                                                                           : L" - DualShock 4";
            if (device == in_use)
                label += L"  (plays this port)";
            else if (!pad.connected)
                label += L"  (not connected)";
            SendMessageW(device_list_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        }
        SendMessageW(device_list_, CB_SETCURSEL, static_cast<WPARAM>(device_), 0);
        EnableWindow(use_device_, device_ != in_use && SlotType() != Sio::kMouse &&
                                      SlotType() != Sio::kNone);
    }

    void ControllerBindingsWindow::UpdateInfo() {
        const BindingSlot& place = kBindingSlots[slot_];
        const Sio::ControllerType type = SlotType();
        const std::wstring slot_name = place.label;
        const std::wstring port_name = L"Port " + std::to_wstring(place.port + 1);
        const bool multitap_port =
            ParseControllerType(Config().controller_type[place.port]) == Sio::kMultitap;
        const int in_use = SlotSourceDevice(slot_);
        std::wstring text;

        if (place.player < 0 && type == Sio::kMultitap) {
            text = port_name + L" has a multitap. Choose one of its players above to bind their "
                               L"buttons.";
        } else if (place.player >= 0 && !multitap_port) {
            text = port_name + L" has no multitap at the moment. These bindings take effect when "
                               L"it has one: choose " + port_name + L" above and set its "
                               L"controller to Multitap.";
        } else if (type == Sio::kNone) {
            text = L"Nothing is plugged in as " + slot_name +
                   L". Choose a controller for it above.";
        } else if (type == Sio::kMouse) {
            text = port_name + L" has a mouse, which is always the Windows mouse, so there is "
                               L"nothing to bind.";
        } else {
            text = slot_name + L" is " + TypeName(type) + L", played with " + DevicePhrase(in_use) +
                   L".";
            if (type == Sio::kGunCon)
                text += L" It aims and fires with the Windows mouse; Cross and Circle below are "
                        L"the gun's A and B buttons.";
            if (device_ != in_use) {
                text += L" These are the bindings for " + DevicePhrase(device_) +
                        L", which it is not using: Use for This Port switches to them.";
            }
            if (type == Sio::kDualAnalog || type == Sio::kDualShock) {
                text += device_ == kKeyboardDevice
                            ? L" The analog sticks cannot be played from the keyboard."
                            : L" The gamepad's sticks are the controller's sticks; the left one "
                              L"also works as the D-pad, except in a direction a button is bound "
                              L"to.";
            }
        }
        SetWindowTextW(info_, text.c_str());
    }

    void ControllerBindingsWindow::LayoutBoxes() {
        for (int i = 0; i < kPadButtons; ++i) {
            ShowWindow(boxes_[i], ButtonShown(i) ? SW_SHOW : SW_HIDE);
            InvalidateRect(boxes_[i], nullptr, TRUE);
        }
        const bool pad = PadShown();
        EnableWindow(defaults_, pad);
        EnableWindow(clear_all_, pad);
        EnableWindow(copy_all_, pad);
    }

    void ControllerBindingsWindow::SetStatus(const std::wstring& text) {
        SetWindowTextW(status_, text.c_str());
    }

    // ---------------------------------------------------------------------------------------------
    // Binding
    // ---------------------------------------------------------------------------------------------

    void ControllerBindingsWindow::BeginCapture(int button) {
        if (button < 0 || button >= kPadButtons || !ButtonShown(button))
            return;
        capturing_ = button;
        const std::wstring name = kKeyBindings[button].label;
        if (device_ == kKeyboardDevice) {
            SetStatus(L"Press the key for " + name + L". Escape cancels.");
        } else {
            const emulation::host::PadReading pad = ReadPad(device_);
            const bool connected = pad.connected;
            // Whatever is already held does not count - otherwise a button still down from the
            // click that got here, or a stick resting off centre, binds itself.
            pad_baseline_ = connected ? pad.inputs : 0;
            SetStatus(connected ? L"Press the " + DeviceName(device_) + L" control for " + name +
                                      L": a button, a trigger, or a stick pushed one way. Escape "
                                      L"cancels."
                                : DeviceName(device_) + L" is not connected. Connect it and "
                                                        L"press the control for " + name +
                                      L", or press Escape.");
            SetTimer(window_, kPadTimer, 15, nullptr);
        }
        // The window itself takes the keyboard, so a key reaches its WM_KEYDOWN rather than
        // pressing whichever box has the focus.
        SetFocus(window_);
        InvalidateRect(boxes_[button], nullptr, TRUE);
        InvalidateRect(canvas_, nullptr, FALSE);
    }

    void ControllerBindingsWindow::EndCapture(const std::wstring& status) {
        KillTimer(window_, kPadTimer);
        const int was = capturing_;
        capturing_ = -1;
        if (was >= 0)
            InvalidateRect(boxes_[was], nullptr, TRUE);
        InvalidateRect(canvas_, nullptr, FALSE);
        SetStatus(status);
    }

    void ControllerBindingsWindow::PollCapturePad() {
        if (capturing_ < 0 || device_ == kKeyboardDevice)
            return;
        const emulation::host::PadReading pad = ReadPad(device_);
        if (!pad.connected)
            return;
        const uint32_t inputs = pad.inputs;
        const uint32_t pressed = inputs & ~pad_baseline_;
        // Something let go stops being "already held", so it can be pressed again to bind it.
        pad_baseline_ &= inputs;
        if (pressed == 0)
            return;
        for (int code = 1; code <= utilities::kPadInputCount; ++code) {
            if (pressed & utilities::PadInputBit(code)) {
                Bind(capturing_, code);
                return;
            }
        }
    }

    // Another slot that is played with this same device and would read the same control - which
    // is allowed, since it is sometimes wanted, but worth a word, since it presses both.
    std::wstring ControllerBindingsWindow::ConflictNote(int button, int code) const {
        if (code == 0)
            return std::wstring();
        for (int slot = 0; slot < kBindingSlotCount; ++slot) {
            if (slot == slot_ || SlotSourceDevice(slot) != device_)
                continue;
            const KeyMap& other = bindings_.map[slot][device_];
            for (int i = 0; i < kPadButtons; ++i) {
                if (other[i] == code) {
                    return L" It is also " + std::wstring(kBindingSlots[slot].label) + L"'s " +
                           kKeyBindings[i].label + L", so it presses both.";
                }
            }
        }
        (void)button;
        return std::wstring();
    }

    void ControllerBindingsWindow::Bind(int button, int code) {
        KeyMap& map = bindings_.map[slot_][device_];
        const int taken_from =
            utilities::BindTakingFromOthers(map.data(), kPadButtons, button, code);
        std::wstring status;
        if (code == 0) {
            status = std::wstring(kKeyBindings[button].label) + L" is now unbound.";
        } else {
            status = std::wstring(kKeyBindings[button].label) + L" is now " +
                     BindingCodeLabel(device_, code, PlayStationNames(device_)) + L".";
            if (taken_from >= 0) {
                status += L" That was " + std::wstring(kKeyBindings[taken_from].label) +
                          L"'s, which now has none.";
            }
            status += ConflictNote(button, code);
        }
        if (taken_from >= 0)
            InvalidateRect(boxes_[taken_from], nullptr, TRUE);
        Changed();
        EndCapture(status);
        SetFocus(boxes_[button]);
    }

    void ControllerBindingsWindow::Changed() {
        if (host_.on_change)
            host_.on_change(bindings_);
        for (HWND box : boxes_)
            InvalidateRect(box, nullptr, TRUE);
    }

    // ---------------------------------------------------------------------------------------------
    // Drawing
    // ---------------------------------------------------------------------------------------------

    void ControllerBindingsWindow::PaintCanvas(HDC dc, const RECT& client) {
        using namespace Gdiplus;
        HBRUSH back = CreateSolidBrush(kCanvasBack);
        FillRect(dc, &client, back);
        DeleteObject(back);

        if (!PadShown()) {
            HGDIOBJ old = SelectObject(dc, font_);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(110, 116, 125));
            RECT text = client;
            const wchar_t* message = SlotType() == Sio::kMultitap
                                         ? L"Choose one of the multitap's players above."
                                         : L"There is nothing to bind here.";
            DrawTextW(dc, message, -1, &text, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(dc, old);
            return;
        }

        Graphics graphics(dc);
        graphics.SetSmoothingMode(SmoothingModeAntiAlias);
        graphics.SetPixelOffsetMode(PixelOffsetModeHalf);

        // Pad units from here on: the pad's centre is the origin, and a unit is kPadScale pixels
        // at 96 DPI.
        const float scale = kPadScale * static_cast<float>(dpi_) / 96.0f;
        const float origin_x = kPadCentreX * static_cast<float>(dpi_) / 96.0f;
        const float origin_y = kPadCentreY * static_cast<float>(dpi_) / 96.0f;

        // The leader lines, in canvas pixels. Drawn over the body but under the controls, so each
        // reaches its control and stops there rather than being drawn across it - and the
        // highlighted one last, so no other line crosses it.
        const int highlighted = capturing_ >= 0 ? capturing_ : focused_box_;
        auto draw_lines = [&]() {
            graphics.ResetTransform();
            for (int pass = 0; pass < 2; ++pass) {
                for (int i = 0; i < kPadButtons; ++i) {
                    if (!ButtonShown(i) || (pass == 1) != (i == highlighted))
                        continue;
                    const BoxPlace& place = kPlaces[i];
                    const bool left = place.column == 0;
                    const float box_x = static_cast<float>(
                        Scale(left ? kLeftColumn + kBoxWidth : kRightColumn));
                    const float box_y = static_cast<float>(Scale(Row(place.row) + kBoxHeight / 2));
                    const float stub = static_cast<float>(Scale(14)) * (left ? 1.0f : -1.0f);
                    const PointF points[3] = {
                        PointF(box_x, box_y),
                        PointF(box_x + stub, box_y),
                        PointF(origin_x + place.x * scale, origin_y + place.y * scale),
                    };
                    Pen pen(i == highlighted ? kAccent : kLine,
                            static_cast<REAL>(Scale(i == highlighted ? 2 : 1)));
                    graphics.DrawLines(&pen, points, 3);
                    SolidBrush dot(i == highlighted ? kAccent : kLine);
                    const float r = static_cast<float>(Scale(3));
                    graphics.FillEllipse(&dot, points[2].X - r, points[2].Y - r, r * 2, r * 2);
                }
            }
            graphics.TranslateTransform(origin_x, origin_y);
            graphics.ScaleTransform(scale, scale);
        };
        graphics.TranslateTransform(origin_x, origin_y);
        graphics.ScaleTransform(scale, scale);

        const Sio::ControllerType type = SlotType();
        const bool analog = type == Sio::kDualAnalog || type == Sio::kDualShock;
        const Color accent_ring = kAccent;
        auto ring = [&](int button, float x, float y, float r) {
            if (button != highlighted)
                return;
            Pen pen(accent_ring, 2.2f);
            graphics.DrawEllipse(&pen, x - r - 2.5f, y - r - 2.5f, (r + 2.5f) * 2, (r + 2.5f) * 2);
        };

        // Shoulders, behind the body so it covers their lower halves.
        {
            GraphicsPath l2, r2, l1, r1;
            AddRoundRect(&l2, -118.0f, -87.0f, 64.0f, 22.0f, 7.0f);
            AddRoundRect(&r2, 54.0f, -87.0f, 64.0f, 22.0f, 7.0f);
            AddRoundRect(&l1, -122.0f, -73.0f, 72.0f, 20.0f, 7.0f);
            AddRoundRect(&r1, 50.0f, -73.0f, 72.0f, 20.0f, 7.0f);
            SolidBrush back_brush(kShoulderBack);
            SolidBrush front_brush(kShoulderFront);
            Pen outline(kOutline, 1.4f);
            for (GraphicsPath* path : { &l2, &r2 }) {
                graphics.FillPath(&back_brush, path);
                graphics.DrawPath(&outline, path);
            }
            for (GraphicsPath* path : { &l1, &r1 }) {
                graphics.FillPath(&front_brush, path);
                graphics.DrawPath(&outline, path);
            }
            const int shoulders[4] = { 10, 11, 8, 9 };   // L2 R2 L1 R1
            const float centres[4][2] = { { -86, -80 }, { 86, -80 }, { -86, -66 }, { 86, -66 } };
            for (int s = 0; s < 4; ++s) {
                if (shoulders[s] == highlighted) {
                    Pen pen(accent_ring, 2.2f);
                    graphics.DrawRectangle(&pen, centres[s][0] - 38.0f, centres[s][1] - 9.0f,
                                           76.0f, 16.0f);
                }
            }
        }

        // The body and its two grips, outlined as one shape: every piece is stroked with a thick
        // outline first, then every piece filled on top, which covers the strokes where the pieces
        // overlap and leaves only the outer edge.
        {
            GraphicsPath body;
            AddRoundRect(&body, -130.0f, -62.0f, 260.0f, 118.0f, 40.0f);
            const float grip_h = analog ? 150.0f : 128.0f;
            const float grip_y = analog ? 62.0f : 50.0f;
            GraphicsPath left_grip, right_grip;
            left_grip.AddEllipse(-45.0f, -grip_h / 2, 90.0f, grip_h);
            right_grip.AddEllipse(-45.0f, -grip_h / 2, 90.0f, grip_h);
            Matrix left_matrix, right_matrix;
            left_matrix.Translate(-100.0f, grip_y);
            left_matrix.Rotate(24.0f);
            right_matrix.Translate(100.0f, grip_y);
            right_matrix.Rotate(-24.0f);
            left_grip.Transform(&left_matrix);
            right_grip.Transform(&right_matrix);

            Pen outline(kOutline, 3.0f);
            graphics.DrawPath(&outline, &body);
            graphics.DrawPath(&outline, &left_grip);
            graphics.DrawPath(&outline, &right_grip);
            LinearGradientBrush fill(PointF(0.0f, -62.0f), PointF(0.0f, 150.0f), kBodyTop,
                                     kBodyBottom);
            graphics.FillPath(&fill, &left_grip);
            graphics.FillPath(&fill, &right_grip);
            graphics.FillPath(&fill, &body);
        }
        draw_lines();

        // The d-pad: four arms round a centre, each its own button.
        {
            SolidBrush dark(kControlDark);
            const RectF arms[4] = {
                RectF(-89.0f, -38.0f, 18.0f, 21.0f),   // Up
                RectF(-89.0f, 1.0f, 18.0f, 21.0f),     // Down
                RectF(-110.0f, -17.0f, 21.0f, 18.0f),  // Left
                RectF(-71.0f, -17.0f, 21.0f, 18.0f),   // Right
            };
            graphics.FillRectangle(&dark, RectF(-89.0f, -17.0f, 18.0f, 18.0f));
            for (int arm = 0; arm < 4; ++arm) {
                graphics.FillRectangle(&dark, arms[arm]);
                if (arm == highlighted) {
                    Pen pen(accent_ring, 2.2f);
                    graphics.DrawRectangle(&pen, arms[arm]);
                }
            }
        }

        // The four face buttons, each with its symbol in its own colour.
        {
            struct Face {
                int button;
                float x, y;
            };
            const Face faces[4] = { { 7, 80, -33 }, { 6, 105, -8 }, { 4, 80, 17 }, { 5, 55, -8 } };
            SolidBrush base(kControlDark);
            for (const Face& face : faces) {
                graphics.FillEllipse(&base, face.x - 11.0f, face.y - 11.0f, 22.0f, 22.0f);
                ring(face.button, face.x, face.y, 11.0f);
            }
            Pen green(kTriangleGreen, 2.0f), red(kCircleRed, 2.0f), blue(kCrossBlue, 2.0f),
                pink(kSquarePink, 2.0f);
            const PointF triangle[3] = { PointF(80.0f, -39.5f), PointF(85.5f, -29.5f),
                                         PointF(74.5f, -29.5f) };
            graphics.DrawPolygon(&green, triangle, 3);
            graphics.DrawEllipse(&red, 99.5f, -13.5f, 11.0f, 11.0f);
            graphics.DrawLine(&blue, 75.0f, 12.0f, 85.0f, 22.0f);
            graphics.DrawLine(&blue, 85.0f, 12.0f, 75.0f, 22.0f);
            graphics.DrawRectangle(&pink, 50.0f, -13.0f, 10.0f, 10.0f);
        }

        // Select and Start, between the two clusters.
        {
            SolidBrush mid(kControlMid);
            GraphicsPath select, start;
            AddRoundRect(&select, -32.0f, -12.0f, 20.0f, 8.0f, 4.0f);
            AddRoundRect(&start, 12.0f, -12.0f, 20.0f, 8.0f, 4.0f);
            graphics.FillPath(&mid, &select);
            graphics.FillPath(&mid, &start);
            ring(13, -22.0f, -8.0f, 10.0f);
            ring(12, 22.0f, -8.0f, 10.0f);
        }

        // A Dual Analog or DualShock adds the sticks and the ANALOG button with its light.
        if (analog) {
            SolidBrush well(kControlDark);
            SolidBrush cap(kControlMid);
            Pen cap_edge(Color(255, 130, 136, 145), 1.2f);
            for (const float x : { -42.0f, 42.0f }) {
                graphics.FillEllipse(&well, x - 20.0f, 12.0f, 40.0f, 40.0f);
                graphics.FillEllipse(&cap, x - 14.0f, 18.0f, 28.0f, 28.0f);
                graphics.DrawEllipse(&cap_edge, x - 9.0f, 23.0f, 18.0f, 18.0f);
            }
            ring(kButtonL3, -42.0f, 32.0f, 20.0f);
            ring(kButtonR3, 42.0f, 32.0f, 20.0f);
            graphics.FillEllipse(&well, -5.0f, 17.0f, 10.0f, 10.0f);
            SolidBrush led(Color(255, 214, 48, 48));
            graphics.FillEllipse(&led, -2.5f, 6.5f, 5.0f, 5.0f);
            ring(kButtonAnalog, 0.0f, 22.0f, 5.0f);
        }
    }

    void ControllerBindingsWindow::DrawBox(const DRAWITEMSTRUCT& item) {
        const int button = static_cast<int>(item.CtlID) - kIdBoxFirst;
        if (button < 0 || button >= kPadButtons)
            return;
        HDC dc = item.hDC;
        const RECT rect = item.rcItem;
        const bool capturing = button == capturing_;
        const bool focused = (item.itemState & ODS_FOCUS) != 0;

        HBRUSH back = CreateSolidBrush(kCanvasBack);
        FillRect(dc, &rect, back);
        DeleteObject(back);

        const COLORREF border = capturing ? RGB(26, 115, 232)
                                : focused ? RGB(90, 140, 220)
                                          : RGB(190, 195, 203);
        HPEN pen = CreatePen(PS_SOLID, Scale(capturing ? 2 : 1), border);
        HBRUSH fill = CreateSolidBrush(capturing ? RGB(232, 240, 254) : RGB(255, 255, 255));
        HGDIOBJ old_pen = SelectObject(dc, pen);
        HGDIOBJ old_brush = SelectObject(dc, fill);
        RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, Scale(8), Scale(8));
        SelectObject(dc, old_pen);
        SelectObject(dc, old_brush);
        DeleteObject(pen);
        DeleteObject(fill);

        SetBkMode(dc, TRANSPARENT);
        RECT caption = rect;
        caption.left += Scale(8);
        caption.top += Scale(3);
        caption.bottom = caption.top + Scale(14);
        HGDIOBJ old_font = SelectObject(dc, small_font_ != nullptr ? small_font_ : font_);
        SetTextColor(dc, RGB(110, 116, 125));
        DrawTextW(dc, kKeyBindings[button].label, -1, &caption, DT_LEFT | DT_SINGLELINE);

        RECT value = rect;
        value.left += Scale(8);
        value.right -= Scale(6);
        value.top += Scale(16);
        std::wstring text;
        const int code = bindings_.map[slot_][device_][button];
        if (capturing) {
            text = device_ == kKeyboardDevice ? L"Press a key..." : L"Press a control...";
            SetTextColor(dc, RGB(26, 115, 232));
        } else {
            text = BindingCodeLabel(device_, code, PlayStationNames(device_));
            SetTextColor(dc, code == 0 ? RGB(150, 155, 163) : RGB(32, 35, 40));
        }
        SelectObject(dc, code == 0 && !capturing ? font_ : (bold_font_ != nullptr ? bold_font_ : font_));
        DrawTextW(dc, text.c_str(), -1, &value, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(dc, old_font);
    }

    // ---------------------------------------------------------------------------------------------
    // Messages
    // ---------------------------------------------------------------------------------------------

    LRESULT CALLBACK ControllerBindingsWindow::CanvasProc(HWND window, UINT message, WPARAM wparam,
                                                         LPARAM lparam) {
        if (message == WM_NCCREATE) {
            const CREATESTRUCTW* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            SetWindowLongPtrW(window, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        }
        ControllerBindingsWindow* self =
            reinterpret_cast<ControllerBindingsWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (self == nullptr)
            return DefWindowProcW(window, message, wparam, lparam);

        switch (message) {
            case WM_ERASEBKGND:
                return 1;

            case WM_PAINT: {
                PAINTSTRUCT paint;
                HDC dc = BeginPaint(window, &paint);
                RECT client;
                GetClientRect(window, &client);
                // Drawn off screen and copied, so a redraw never flickers through the background.
                HDC memory = CreateCompatibleDC(dc);
                HBITMAP bitmap = CreateCompatibleBitmap(dc, client.right, client.bottom);
                HGDIOBJ old = SelectObject(memory, bitmap);
                self->PaintCanvas(memory, client);
                BitBlt(dc, 0, 0, client.right, client.bottom, memory, 0, 0, SRCCOPY);
                SelectObject(memory, old);
                DeleteObject(bitmap);
                DeleteDC(memory);
                EndPaint(window, &paint);
                return 0;
            }

            case WM_DRAWITEM:
                self->DrawBox(*reinterpret_cast<const DRAWITEMSTRUCT*>(lparam));
                return TRUE;

            // The boxes are the canvas's children; what they say goes to the window.
            case WM_COMMAND:
            case WM_CONTEXTMENU:
                return SendMessageW(self->window_, message, wparam, lparam);
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    LRESULT CALLBACK ControllerBindingsWindow::WindowProc(HWND window, UINT message, WPARAM wparam,
                                                         LPARAM lparam) {
        if (message == WM_NCCREATE) {
            const CREATESTRUCTW* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            SetWindowLongPtrW(window, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        }
        ControllerBindingsWindow* self =
            reinterpret_cast<ControllerBindingsWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (self == nullptr)
            return DefWindowProcW(window, message, wparam, lparam);

        switch (message) {
            case WM_KEYDOWN:
            case WM_SYSKEYDOWN: {
                if (self->capturing_ < 0)
                    break;
                const int key = static_cast<int>(wparam);
                if (key == VK_ESCAPE) {
                    const int button = self->capturing_;
                    self->EndCapture(L"Cancelled - the binding is unchanged.");
                    SetFocus(self->boxes_[button]);
                } else if (self->device_ != kKeyboardDevice) {
                    // Waiting for the pad; the keyboard only cancels.
                } else if (IsReservedKey(key)) {
                    self->SetStatus(BindingCodeLabel(kKeyboardDevice, key) +
                                    L" is taken by the emulator itself (Space pauses, F1-F8 load "
                                    L"and save states, F11 is full screen). Press another key, "
                                    L"or Escape.");
                } else {
                    self->Bind(self->capturing_, key);
                }
                return 0;
            }

            // Clicking anywhere else while a key is awaited cancels, rather than leaving the next
            // unrelated key press to rebind a button.
            case WM_KILLFOCUS:
                if (self->capturing_ >= 0)
                    self->EndCapture(L"Cancelled - the binding is unchanged.");
                break;

            case WM_TIMER:
                if (wparam == kPadTimer)
                    self->PollCapturePad();
                return 0;

            case kMessageBeginCapture:
                self->BeginCapture(static_cast<int>(wparam));
                return 0;

            case WM_CONTEXTMENU: {
                const HWND target = reinterpret_cast<HWND>(wparam);
                for (int i = 0; i < kPadButtons; ++i) {
                    if (target != self->boxes_[i])
                        continue;
                    POINT at = { GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
                    if (at.x == -1 && at.y == -1) {   // from the keyboard: under the box
                        RECT box;
                        GetWindowRect(target, &box);
                        at = { box.left, box.bottom };
                    }
                    HMENU menu = CreatePopupMenu();
                    AppendMenuW(menu, MF_STRING, kIdMenuChange, L"&Change...");
                    AppendMenuW(menu, MF_STRING, kIdMenuClear, L"C&lear");
                    const int choice = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                                      at.x, at.y, 0, window, nullptr);
                    DestroyMenu(menu);
                    if (choice == kIdMenuChange)
                        PostMessageW(window, kMessageBeginCapture, static_cast<WPARAM>(i), 0);
                    else if (choice == kIdMenuClear)
                        self->Bind(i, 0);
                    return 0;
                }
                break;
            }

            case WM_COMMAND: {
                const int id = LOWORD(wparam);
                const int code = HIWORD(wparam);
                if (id >= kIdBoxFirst && id < kIdBoxFirst + kPadButtons) {
                    const int button = id - kIdBoxFirst;
                    if (code == BN_CLICKED) {
                        // Posted rather than started here: the box is still inside its own click
                        // handling, and taking the focus from it now can see it taken straight
                        // back - which would cancel the capture before anything was pressed.
                        PostMessageW(window, kMessageBeginCapture, static_cast<WPARAM>(button),
                                     0);
                    } else if (code == BN_SETFOCUS || code == BN_KILLFOCUS) {
                        self->focused_box_ = code == BN_SETFOCUS ? button : -1;
                        InvalidateRect(self->canvas_, nullptr, FALSE);
                    }
                    return 0;
                }
                switch (id) {
                    case kIdSlot:
                        if (code == CBN_SELCHANGE) {
                            const LRESULT slot = SendMessageW(self->slot_list_, CB_GETCURSEL, 0, 0);
                            if (slot >= 0 && slot < kBindingSlotCount) {
                                self->slot_ = static_cast<int>(slot);
                                self->device_ = self->SlotSourceDevice(self->slot_);
                                self->Refresh();
                            }
                        }
                        break;
                    case kIdGame:
                        // The App's setter calls OnConfigChanged, which refreshes the window.
                        if (code == BN_CLICKED && self->host_.set_separate)
                            self->host_.set_separate(
                                SendMessageW(self->game_box_, BM_GETCHECK, 0, 0) == BST_CHECKED);
                        break;
                    case kIdType:
                        if (code == CBN_SELCHANGE) {
                            const LRESULT index = SendMessageW(self->type_list_, CB_GETCURSEL, 0, 0);
                            const auto choices = self->TypeChoices();
                            // The App's setter calls OnConfigChanged, which refreshes the window.
                            if (index >= 0 && static_cast<size_t>(index) < choices.size() &&
                                self->host_.set_type)
                                self->host_.set_type(self->slot_, choices[index].key);
                        }
                        break;
                    case kIdDevice:
                        if (code == CBN_DROPDOWN) {
                            // Whether each pad is connected, as of now rather than as of opening.
                            self->FillDeviceList();
                        } else if (code == CBN_SELCHANGE) {
                            const LRESULT device =
                                SendMessageW(self->device_list_, CB_GETCURSEL, 0, 0);
                            if (device >= 0 && device < kBindingDevices) {
                                self->device_ = static_cast<int>(device);
                                self->FillDeviceList();
                                self->UpdateInfo();
                                self->LayoutBoxes();
                                InvalidateRect(self->canvas_, nullptr, FALSE);
                            }
                        }
                        break;
                    case kIdUseDevice:
                        if (self->host_.set_source)
                            self->host_.set_source(self->slot_,
                                                   kInputSourceChoices[self->device_].key);
                        break;
                    case kIdDefaults:
                        self->bindings_.map[self->slot_][self->device_] =
                            ControllerBindings::Default(self->device_);
                        self->Changed();
                        self->EndCapture(std::wstring(kBindingSlots[self->slot_].label) + L" on " +
                                         DeviceName(self->device_) + L" is back to the defaults.");
                        break;
                    case kIdClearAll:
                        self->bindings_.map[self->slot_][self->device_].fill(0);
                        self->Changed();
                        self->EndCapture(L"Every button is unbound. Click a box to bind one.");
                        break;
                    case kIdCopyAll: {
                        // The usual reason to rebind a pad is the pad, not the port: the same
                        // layout wherever it is plugged in.
                        const KeyMap map = self->bindings_.map[self->slot_][self->device_];
                        for (int slot = 0; slot < kBindingSlotCount; ++slot)
                            self->bindings_.map[slot][self->device_] = map;
                        self->Changed();
                        self->EndCapture(L"Every port and multitap player now has these " +
                                         DeviceName(self->device_) + L" bindings.");
                        break;
                    }
                    case kIdClose:
                        SendMessageW(window, WM_CLOSE, 0, 0);
                        break;
                }
                return 0;
            }

            case WM_CLOSE:
                if (self->capturing_ >= 0)
                    self->EndCapture(std::wstring());
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
