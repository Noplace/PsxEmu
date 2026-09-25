/*****************************************************************************************************************
* Copyright (c) 2012 Khalid Ali Al-Kooheji                                                                       *
*                                                                                                                 *
* Permission is hereby granted, free of charge, to any person obtaining a copy of this software and              *
* associated documentation files (the "Software"), to deal in the Software without restriction, including        *
* without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell        *
* copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the       *
* following conditions:                                                                                          *
*                                                                                                                 *
* The above copyright notice and this permission notice shall be included in all copies or substantial           *
* portions of the Software.                                                                                      *
*                                                                                                                 *
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT          *
* LIMITED TO THE WARRANTIES OF MERCHANTABILITY, * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.          *
* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, * DAMAGES OR OTHER LIABILITY,      *
* WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE            *
* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                                                         *
*****************************************************************************************************************/
#pragma once

// The real Windows mouse as a PSX mouse's two inputs - the counterpart to gamepad.h and keyboard.h
// beside it, and the only source Sio::kMouse ever reads from. Unlike a pad's input_source, which
// port a mouse's buttons and motion feed is not a player choice - it is always the real mouse,
// whichever port is set to Sio::kMouse - see PSXEmu.Win32/app.cpp's PollInput.
//
// Where the movement comes from depends on which of utilities::MouseMotion the person picked, and
// that choice is the whole of what this file is about - see the enum's own comment in
// platform/mouse_scaling.h for why there are three of them and what each is for.
//
//   kWindows and kHardware read raw input (WM_INPUT): relative counts, untouched by the pointer
//   speed slider or the acceleration curve, and not clamped at a screen edge the way the visible
//   cursor is. kWindows then puts each report through Windows' own curve (approximated), kHardware
//   scales linearly for the era's much coarser sensor.
//
//   kDesktop reads the cursor instead - GetCursorPos - and pins it back to the middle of the
//   window every poll. That gives exactly the movement Windows itself decided on, acceleration
//   and all, and recentring is what keeps a screen edge from eating any of it. It is the only
//   mode that touches the person's pointer, so it does that only while a port is set to
//   Sio::kMouse and the window has focus.
//
// Buttons are GetAsyncKeyState, the same as keyboard.h: simpler than decoding RAWMOUSE's own
// button-transition flags, and level state is all a poll ever asks for anyway.
//
// Registering without RIDEV_INPUTSINK means WM_INPUT only ever arrives while this window has focus,
// which is what makes motion stop the instant focus is lost with no extra check needed here - the
// same thing keyboard.h and gamepad.h each get from the App-level `focused` gate instead. Buttons
// still need that gate applied by the caller, since GetAsyncKeyState reads the whole system rather
// than this window.

#include "framework.h"

#include "platform/mouse_scaling.h"

#include <atomic>
#include <cmath>
#include <mutex>

namespace psxemu {

    class Mouse {
     public:
        // What one poll produced: buttons as the level they are held at, and movement as however
        // much of it this call has not already handed out - see Poll().
        struct State {
            bool left = false;
            bool right = false;
            bool middle = false;
            bool back = false;
            int32_t dx = 0;
            int32_t dy = 0;
        };

        // Registers `window` for raw mouse input. Called once, from the input thread, on the
        // message-only window of its own that it pumps - harmless to leave registered even when no
        // port is ever set to Sio::kMouse, since nothing reads accumulated_dx_/accumulated_dy_
        // unless one is.
        //
        // `background` adds RIDEV_INPUTSINK, which a message-only window needs: without it raw
        // input only reaches a window that is in the foreground, and a message-only window never
        // is. The caller gates motion on focus instead - see InputThread.
        bool Attach(HWND window, bool background) {
            RAWINPUTDEVICE device = {};
            device.usUsagePage = 0x01;   // generic desktop controls
            device.usUsage = 0x02;       // mouse
            device.dwFlags = background ? RIDEV_INPUTSINK : 0;
            device.hwndTarget = window;
            return RegisterRawInputDevices(&device, 1, sizeof(device)) != FALSE;
        }

        // Gives raw input back, before the window it was registered against goes away.
        void Detach() {
            RAWINPUTDEVICE device = {};
            device.usUsagePage = 0x01;
            device.usUsage = 0x02;
            device.dwFlags = RIDEV_REMOVE;
            device.hwndTarget = nullptr;
            RegisterRawInputDevices(&device, 1, sizeof(device));
        }

        // Which of the three scalings to apply, and what the person's mouse is set to in counts
        // per inch (kHardware's only input). Called from the UI thread whenever the setting
        // changes and once at startup; read on the input thread, hence the atomics.
        void SetMotion(utilities::MouseMotion motion, int host_dpi) {
            motion_.store(static_cast<int>(motion), std::memory_order_relaxed);
            host_dpi_.store(host_dpi, std::memory_order_relaxed);
        }

        // The window the cursor is pinned to the middle of in kDesktop. Without one, kDesktop has
        // nothing to measure against and behaves as kHardware would.
        void SetWindow(HWND window) {
            capture_window_.store(reinterpret_cast<void*>(window), std::memory_order_relaxed);
        }

        // Windows' pointer-speed slider and acceleration curve, out of HKCU\Control Panel\Mouse.
        // Read once here rather than per poll: they change when someone opens the mouse control
        // panel, which the front end handles by calling this again on WM_SETTINGCHANGE.
        void ReadWindowsPointerSettings() {
            HKEY key = nullptr;
            if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\Mouse", 0, KEY_READ, &key) !=
                ERROR_SUCCESS) {
                return;
            }
            int sensitivity = 10;
            bool enhanced = true;
            wchar_t text[32] = {};
            DWORD size = sizeof(text);
            DWORD type = 0;
            if (RegQueryValueExW(key, L"MouseSensitivity", nullptr, &type,
                                 reinterpret_cast<LPBYTE>(text), &size) == ERROR_SUCCESS) {
                sensitivity = _wtoi(text);
            }
            size = sizeof(text);
            if (RegQueryValueExW(key, L"MouseSpeed", nullptr, &type,
                                 reinterpret_cast<LPBYTE>(text), &size) == ERROR_SUCCESS) {
                // 0 is "Enhance pointer precision" off, which is the slider and nothing else.
                enhanced = (_wtoi(text) != 0);
            }

            uint8_t x_bytes[64] = {};
            uint8_t y_bytes[64] = {};
            DWORD x_size = sizeof(x_bytes);
            DWORD y_size = sizeof(y_bytes);
            const bool have_x = RegQueryValueExW(key, L"SmoothMouseXCurve", nullptr, &type, x_bytes,
                                                 &x_size) == ERROR_SUCCESS;
            const bool have_y = RegQueryValueExW(key, L"SmoothMouseYCurve", nullptr, &type, y_bytes,
                                                 &y_size) == ERROR_SUCCESS;
            RegCloseKey(key);

            utilities::WindowsCurve::Curve curve;
            if (have_x && have_y)
                curve = utilities::WindowsCurve::Parse(x_bytes, x_size, y_bytes, y_size);

            std::lock_guard<std::mutex> lock(pointer_lock_);
            slider_ = utilities::WindowsCurve::SliderMultiplier(sensitivity);
            curve_ = curve;
            enhanced_ = enhanced;
        }

        // Forward WM_INPUT here from the window procedure - `lparam` cast to HRAWINPUT the same way
        // Windows itself defines that message's payload.
        void OnRawInput(HRAWINPUT handle) {
            UINT size = 0;
            GetRawInputData(handle, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER));
            if (size == 0 || size > sizeof(raw_buffer_))
                return;
            if (GetRawInputData(handle, RID_INPUT, raw_buffer_, &size, sizeof(RAWINPUTHEADER)) !=
                size) {
                return;
            }
            const RAWINPUT* raw = reinterpret_cast<const RAWINPUT*>(raw_buffer_);
            if (raw->header.dwType != RIM_TYPEMOUSE)
                return;
            const RAWMOUSE& mouse = raw->data.mouse;
            // Absolute mode (a tablet, or a remote desktop session) has nothing this device's
            // protocol can express - a relative mouse has no notion of a position to be absolute
            // about - so it is ignored rather than translated into a meaningless delta.
            if ((mouse.usFlags & MOUSE_MOVE_ABSOLUTE) != 0)
                return;
            accumulated_dx_ += mouse.lLastX;
            accumulated_dy_ += mouse.lLastY;

            // Windows' curve is a function of how fast *this* report moved, so it has to be
            // applied per report rather than to a poll's total - by the time Poll() runs, a
            // flick and a slow drag that covered the same distance look identical. Computed
            // for every report whichever mode is live, so switching modes needs no state.
            AccumulateAccelerated(mouse.lLastX, mouse.lLastY);
        }

        // Whether kDesktop may take the cursor over. The App decides: a port set to Sio::kMouse,
        // a machine that is running rather than paused or not started yet, and no menu open -
        // see App::SendMouseSettingsToInput. Poll() adds focus to that.
        void SetCaptureEnabled(bool enabled) {
            capture_enabled_.store(enabled, std::memory_order_relaxed);
        }

        // Whether the cursor is being held in the middle of the window right now, so the window
        // procedure knows to hide it (WM_SETCURSOR) - a pointer pinned in the centre of the
        // picture is worse than no pointer at all.
        bool capturing() const { return capturing_.load(std::memory_order_relaxed); }

        // What has moved and what is held since the last call. Called from the input thread's own
        // loop, which is faster than the machine consumes it; InputExchange adds up what it
        // publishes, so nothing here is lost between the machine's frames.
        State Poll(bool focused) {
            State state;
            const utilities::MouseMotion motion =
                static_cast<utilities::MouseMotion>(motion_.load(std::memory_order_relaxed));

            switch (motion) {
                case utilities::MouseMotion::kDesktop:
                    PollDesktop(focused, &state);
                    break;

                case utilities::MouseMotion::kWindows: {
                    // Already in pointer units - the whole point of the curve - so one for one,
                    // with the fraction carried the way Windows carries its own.
                    StopCapture();
                    int32_t dx = 0, dy = 0;
                    float accelerated_dx = 0.0f, accelerated_dy = 0.0f;
                    {
                        std::lock_guard<std::mutex> lock(pointer_lock_);
                        accelerated_dx = accelerated_dx_;
                        accelerated_dy = accelerated_dy_;
                        accelerated_dx_ = 0.0f;
                        accelerated_dy_ = 0.0f;
                    }
                    accumulator_.Add(accelerated_dx, accelerated_dy, 1.0f, &dx, &dy);
                    state.dx = dx;
                    state.dy = dy;
                    accumulated_dx_ = 0;
                    accumulated_dy_ = 0;
                    break;
                }

                case utilities::MouseMotion::kHardware:
                default: {
                    StopCapture();
                    const float scale =
                        utilities::HardwareScale(host_dpi_.load(std::memory_order_relaxed));
                    int32_t dx = 0, dy = 0;
                    accumulator_.Add(static_cast<float>(accumulated_dx_),
                                     static_cast<float>(accumulated_dy_), scale, &dx, &dy);
                    state.dx = dx;
                    state.dy = dy;
                    accumulated_dx_ = 0;
                    accumulated_dy_ = 0;
                    DropAccelerated();
                    break;
                }
            }

            state.left = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
            state.right = (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
            state.middle = (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0;
            state.back = (GetAsyncKeyState(VK_XBUTTON1) & 0x8000) != 0;
            return state;
        }

     private:
        // kDesktop: read where Windows has actually put its cursor and hand the game that, then
        // put the cursor back in the middle of the window so it can never reach a screen edge,
        // walk onto another monitor, or click anything behind. The delta is what the person's own
        // pointer speed and acceleration produced, which is the one reading of "like my mouse"
        // that needs nothing modelled.
        void PollDesktop(bool focused, State* state) {
            HWND window = reinterpret_cast<HWND>(capture_window_.load(std::memory_order_relaxed));
            if (window == nullptr || !focused || !capture_enabled_.load(std::memory_order_relaxed)) {
                StopCapture();
                // Raw input kept piling up while this was not looking; none of it is this mode's.
                accumulated_dx_ = 0;
                accumulated_dy_ = 0;
                DropAccelerated();
                return;
            }

            POINT centre = {};
            if (!ClientCentre(window, &centre)) {
                StopCapture();
                return;
            }

            POINT cursor = {};
            if (!GetCursorPos(&cursor)) {
                StopCapture();
                return;
            }

            if (capturing_.load(std::memory_order_relaxed)) {
                state->dx = cursor.x - centre_.x;
                state->dy = cursor.y - centre_.y;
            } else {
                // First poll of a capture: where the cursor happens to be is not movement.
                capturing_.store(true, std::memory_order_relaxed);
                accumulator_.Reset();
            }

            SetCursorPos(centre.x, centre.y);
            centre_ = centre;

            // Raw input and the curve both ran alongside this; neither is this mode's, and
            // SetCursorPos above produces raw movement of its own that would double-count.
            accumulated_dx_ = 0;
            accumulated_dy_ = 0;
            DropAccelerated();
        }

        void StopCapture() {
            capturing_.store(false, std::memory_order_relaxed);
        }

        static bool ClientCentre(HWND window, POINT* out) {
            RECT client = {};
            if (!GetClientRect(window, &client))
                return false;
            if (client.right <= client.left || client.bottom <= client.top)
                return false;
            POINT centre = { (client.right - client.left) / 2, (client.bottom - client.top) / 2 };
            if (!ClientToScreen(window, &centre))
                return false;
            *out = centre;
            return true;
        }

        // One raw report through Windows' curve, into the accelerated accumulator.
        void AccumulateAccelerated(LONG dx, LONG dy) {
            const LARGE_INTEGER now = Now();
            float milliseconds = 8.0f;   // a first report has nothing to measure against
            if (last_report_.QuadPart != 0 && frequency_.QuadPart > 0) {
                const double ticks = static_cast<double>(now.QuadPart - last_report_.QuadPart);
                milliseconds = static_cast<float>(ticks * 1000.0 /
                                                  static_cast<double>(frequency_.QuadPart));
            }
            last_report_ = now;
            // A gap means the hand stopped, not that it crawled: a stale interval would read as
            // near-zero speed and pin the gain to the bottom of the curve for one report.
            if (milliseconds <= 0.0f || milliseconds > 100.0f)
                milliseconds = 8.0f;

            const float magnitude = std::sqrt(static_cast<float>(dx) * static_cast<float>(dx) +
                                              static_cast<float>(dy) * static_cast<float>(dy));

            std::lock_guard<std::mutex> lock(pointer_lock_);
            const float gain = enhanced_
                                   ? utilities::WindowsCurve::Gain(curve_, magnitude, milliseconds)
                                   : 1.0f;
            accelerated_dx_ += static_cast<float>(dx) * gain * slider_;
            accelerated_dy_ += static_cast<float>(dy) * gain * slider_;
        }

        void DropAccelerated() {
            std::lock_guard<std::mutex> lock(pointer_lock_);
            accelerated_dx_ = 0.0f;
            accelerated_dy_ = 0.0f;
        }

        LARGE_INTEGER Now() {
            if (frequency_.QuadPart == 0)
                QueryPerformanceFrequency(&frequency_);
            LARGE_INTEGER now = {};
            QueryPerformanceCounter(&now);
            return now;
        }

        int32_t accumulated_dx_ = 0;
        int32_t accumulated_dy_ = 0;
        BYTE raw_buffer_[64] = {};

        std::atomic<int> motion_{ static_cast<int>(utilities::MouseMotion::kDesktop) };
        std::atomic<int> host_dpi_{ 800 };
        std::atomic<void*> capture_window_{ nullptr };
        std::atomic<bool> capture_enabled_{ false };
        std::atomic<bool> capturing_{ false };
        POINT centre_ = {};
        utilities::MouseAccumulator accumulator_;

        // The curve, the slider and what the curve has produced so far. Written on the input
        // thread from OnRawInput and on the UI thread by ReadWindowsPointerSettings, so they
        // share a lock rather than being read half-updated.
        std::mutex pointer_lock_;
        utilities::WindowsCurve::Curve curve_;
        float slider_ = 1.0f;
        bool enhanced_ = true;
        float accelerated_dx_ = 0.0f;
        float accelerated_dy_ = 0.0f;
        LARGE_INTEGER last_report_ = {};
        LARGE_INTEGER frequency_ = {};
    };

}   // namespace psxemu
