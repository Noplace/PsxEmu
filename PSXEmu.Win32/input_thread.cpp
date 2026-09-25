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
#include "input_thread.h"

#include "keyboard.h"
#include "tools/letterbox.h"

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace psxemu {

    namespace {

        const wchar_t kRawInputWindowClass[] = L"PSXEmuRawInput";

        // A thousand a second: fresher than any frame needs, and cheap - a poll is a handful of
        // microseconds. What it buys is that the reading the machine takes is never more than a
        // millisecond old, and that a slow device cannot make a frame late.
        const int kPollIntervalMs = 1;

        // Where the cursor is over the picture, as fractions of it. The presenters both draw the
        // frame into a 4:3 letterbox of the client area (d3d11_presenter.cpp,
        // d3d12_graphics_engine.cpp), so this works it out the same way rather than asking the
        // video thread. Nothing here sends a message, so it is safe from this thread.
        void PointerOverPicture(HWND window, float* x, float* y) {
            *x = -1.0f;
            *y = -1.0f;
            POINT cursor = {};
            RECT client = {};
            if (!GetCursorPos(&cursor) || !ScreenToClient(window, &cursor) ||
                !GetClientRect(window, &client) || client.right <= 0 || client.bottom <= 0)
                return;
            const LetterboxRect picture = ComputeLetterboxRect(client.right, client.bottom,
                                                               4.0f / 3.0f);
            if (picture.width <= 0.0f || picture.height <= 0.0f)
                return;
            *x = (static_cast<float>(cursor.x) + 0.5f - picture.x) / picture.width;
            *y = (static_cast<float>(cursor.y) + 0.5f - picture.y) / picture.height;
        }

    }   // namespace

    InputThread::InputThread(emulation::host::InputExchange* exchange, HWND main_window)
        : exchange_(exchange), main_window_(main_window) {
        SetKeyMap(DefaultKeyMap());
        // The window the cursor is recentred in, and Windows' own pointer settings, both of which
        // MouseMotion's modes need before the first poll.
        mouse_.SetWindow(main_window);
        mouse_.ReadWindowsPointerSettings();
    }

    InputThread::~InputThread() {
        Stop();
    }

    void InputThread::Start() {
        if (thread_.joinable())
            return;
        stop_.store(false, std::memory_order_release);
        thread_ = std::thread(&InputThread::Run, this);
    }

    void InputThread::Stop() {
        if (!thread_.joinable())
            return;
        stop_.store(true, std::memory_order_release);
        thread_.join();
    }

    LRESULT CALLBACK InputThread::RawInputWindowProc(HWND window, UINT message, WPARAM wparam,
                                                    LPARAM lparam) {
        if (message == WM_NCCREATE) {
            const CREATESTRUCTW* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
            SetWindowLongPtrW(window, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(create->lpCreateParams));
            return DefWindowProcW(window, message, wparam, lparam);
        }
        if (message == WM_INPUT) {
            InputThread* input =
                reinterpret_cast<InputThread*>(GetWindowLongPtrW(window, GWLP_USERDATA));
            if (input != nullptr)
                input->mouse_.OnRawInput(reinterpret_cast<HRAWINPUT>(lparam));
            // Let DefWindowProcW do raw input's own cleanup.
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    void InputThread::Run() {
        const HINSTANCE instance = GetModuleHandleW(nullptr);

        WNDCLASSEXW window_class = {};
        window_class.cbSize = sizeof(window_class);
        window_class.lpfnWndProc = RawInputWindowProc;
        window_class.hInstance = instance;
        window_class.lpszClassName = kRawInputWindowClass;
        RegisterClassExW(&window_class);   // already registered is fine

        // Message-only: it is never shown, never activated, and belongs to this thread, so raw
        // input arrives here instead of on the UI thread.
        HWND window = CreateWindowExW(0, kRawInputWindowClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE,
                                      nullptr, instance, this);
        if (window != nullptr)
            mouse_.Attach(window, /*background=*/true);

        HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr,
                                              CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                              TIMER_ALL_ACCESS);

        while (!stop_.load(std::memory_order_acquire)) {
            // Raw mouse messages queue up between polls; take them all.
            MSG message;
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }

            emulation::host::HostInput reading;
            reading.focused = (GetForegroundWindow() == main_window_);
            KeyMap keys;
            for (int i = 0; i < kPadButtons; ++i)
                keys[i] = keys_[i].load(std::memory_order_relaxed);
            reading.keyboard = ReadKeyboardPad(keys);
            for (int i = 0; i < emulation::host::HostInput::kPads; ++i) {
                const Gamepad::State state = gamepads_[i].Poll();
                reading.pads[i].connected = gamepads_[i].connected();
                reading.pads[i].buttons = state.buttons;
                reading.pads[i].left_x = state.left_x;
                reading.pads[i].left_y = state.left_y;
                reading.pads[i].right_x = state.right_x;
                reading.pads[i].right_y = state.right_y;
            }

            // Polled either way, so the accumulated motion cannot pile up while the window is
            // someone else's; only what is published is gated on focus - which is what registering
            // without RIDEV_INPUTSINK used to do for free.
            const Mouse::State mouse = mouse_.Poll(reading.focused);
            reading.mouse_left = mouse.left;
            reading.mouse_right = mouse.right;
            reading.mouse_middle = mouse.middle;
            reading.mouse_back = mouse.back;
            PointerOverPicture(main_window_, &reading.pointer_x, &reading.pointer_y);
            if (reading.focused) {
                reading.mouse_dx = mouse.dx;
                reading.mouse_dy = mouse.dy;
            }

            exchange_->Publish(reading);
            polls_.fetch_add(1, std::memory_order_relaxed);

            // What the machine asked the motors for last frame. SetRumble is a no-op unless it
            // changed, so this costs nothing while nothing is shaking.
            for (int i = 0; i < emulation::host::HostInput::kPads; ++i) {
                uint8_t small_motor = 0;
                uint8_t large_motor = 0;
                exchange_->GetRumble(i, &small_motor, &large_motor);
                gamepads_[i].SetRumble(small_motor, large_motor);
            }

            if (timer != nullptr) {
                LARGE_INTEGER due;
                due.QuadPart = -static_cast<LONGLONG>(kPollIntervalMs) * 10000;   // 100 ns units
                if (SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0)) {
                    WaitForSingleObject(timer, INFINITE);
                    continue;
                }
            }
            Sleep(static_cast<DWORD>(kPollIntervalMs));
        }

        // Raw input goes back before the window it was registered against does.
        mouse_.Detach();
        if (timer != nullptr)
            CloseHandle(timer);
        if (window != nullptr)
            DestroyWindow(window);
    }

}   // namespace psxemu
