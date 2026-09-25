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

// Settings > Input > Controller Bindings: pick a port (or a multitap player) and a device (the
// keyboard or a gamepad), and the window draws that port's controller with a line from each
// button to a box holding what presses it. Click a box, press the key or the pad button, done.
//
// It edits a copy of the App's ControllerBindings and hands every change straight back, the way
// the older Keyboard Bindings list does - there is no OK or Cancel, and a change applies to the
// running game at once. It reads which controller each port has and which device plays it from
// the App's settings, and can change the device ("Use for This Port"), since choosing what to
// bind and choosing what to play with are usually the same decision.
//
// The picture is drawn here with GDI+ rather than loaded: no artwork to ship or license, and it
// follows the port's type - a DualShock or Dual Analog gets sticks, L3/R3 and ANALOG; the original
// digital pad does not.

#include "framework.h"
#include "controller_bindings.h"

#include <functional>

namespace psxemu {

    class ControllerBindingsWindow {
     public:
        struct Host {
            // The settings the menus tick against: each port's type and source.
            std::function<const emulation::psx::EmuConfig&()> config;
            // Every change, whole.
            std::function<void(const ControllerBindings&)> on_change;
            // "Use for This Port": `slot` is a kBindingSlots index, `source` a
            // kInputSourceChoices key.
            std::function<void(int slot, const std::string& source)> set_source;
        };

        ControllerBindingsWindow() = default;
        ~ControllerBindingsWindow();
        ControllerBindingsWindow(const ControllerBindingsWindow&) = delete;
        ControllerBindingsWindow& operator=(const ControllerBindingsWindow&) = delete;

        bool Create(HINSTANCE instance, HWND owner, Host host);

        // Shows the window over `current`, opened on the slot and device last looked at.
        void Show(const ControllerBindings& current);

        // A port's type or source changed in the menus; redraw if open.
        void OnConfigChanged();

        HWND window() const { return window_; }

     private:
        static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam,
                                           LPARAM lparam);
        static LRESULT CALLBACK CanvasProc(HWND window, UINT message, WPARAM wparam,
                                           LPARAM lparam);

        const emulation::psx::EmuConfig& Config() const { return host_.config(); }
        int Slot() const { return slot_; }
        int Device() const { return device_; }
        // The slot's controller, as far as binding goes: the port's own type, or a multitap
        // player's type; kMultitap for a port-level slot whose port holds a multitap.
        emulation::psx::Sio::ControllerType SlotType() const;
        int SlotSourceDevice(int slot) const;   // which device plays it now
        bool ButtonShown(int button) const;
        bool PadShown() const;

        void Refresh();              // everything, from the bindings and the settings
        void FillDeviceList();
        void UpdateInfo();
        void LayoutBoxes();
        void SetStatus(const std::wstring& text);

        void BeginCapture(int button);
        void EndCapture(const std::wstring& status);
        void Bind(int button, int code);
        void PollCapturePad();
        std::wstring ConflictNote(int button, int code) const;
        void Changed();

        void PaintCanvas(HDC dc, const RECT& client);
        void DrawBox(const DRAWITEMSTRUCT& item);
        int Scale(int value) const { return MulDiv(value, dpi_, 96); }

        Host host_;
        HWND window_ = nullptr;
        HWND canvas_ = nullptr;
        HWND slot_list_ = nullptr;
        HWND device_list_ = nullptr;
        HWND use_device_ = nullptr;
        HWND info_ = nullptr;
        HWND status_ = nullptr;
        HWND defaults_ = nullptr;
        HWND clear_all_ = nullptr;
        HWND copy_all_ = nullptr;
        HWND close_ = nullptr;
        std::array<HWND, kPadButtons> boxes_ = {};
        HFONT font_ = nullptr;
        HFONT bold_font_ = nullptr;
        HFONT small_font_ = nullptr;
        ULONG_PTR gdiplus_token_ = 0;
        int dpi_ = 96;

        ControllerBindings bindings_;
        int slot_ = 0;
        int device_ = kKeyboardDevice;
        int capturing_ = -1;          // the button awaiting a key or a pad control
        uint32_t pad_baseline_ = 0;   // what the pad already held when the capture began
        int focused_box_ = -1;
    };

}   // namespace psxemu
