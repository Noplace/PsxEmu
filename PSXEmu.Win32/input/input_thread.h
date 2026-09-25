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

// The input thread: reads the host's pads, keyboard and mouse a thousand times a second and
// publishes what it read; drives the pads' motors from what the machine last asked for.
//
// It exists to keep the devices off the machine's thread (Docs/Threading-Plan.md, phase 6). The
// expensive one is XInput: a slot with nothing plugged into it costs real time to ask, which is
// why Gamepad backs off to once a second - a hitch here delays nothing but the next reading, where
// on the machine's thread it was a late frame.
//
// It owns raw mouse input too, on a message-only window of its own, so WM_INPUT no longer arrives
// on the UI thread at all. Nothing here touches the machine: it only fills a host::InputExchange,
// which the machine reads once a frame.

#include "framework.h"

#include "gamepad.h"
#include "controller_bindings.h"
#include "host/input_exchange.h"
#include "mouse.h"

#include <array>
#include <atomic>
#include <thread>

namespace psxemu {

    class InputThread {
     public:
        // `main_window` is only read, to ask whether the emulator has focus.
        InputThread(emulation::host::InputExchange* exchange, HWND main_window);
        ~InputThread();

        InputThread(const InputThread&) = delete;
        InputThread& operator=(const InputThread&) = delete;

        void Start();
        void Stop();

        // Any thread: how many readings have been published. For the timings readout, and for
        // "is the input thread alive at all".
        uint64_t polls() const { return polls_.load(std::memory_order_relaxed); }

        // Any thread: which keys to read from the next poll on - every key some binding uses
        // (KeysInUse). Which pad buttons they press is the machine thread's business. One atomic
        // per key, so a poll racing a change reads each key's old answer or its new one, and
        // nothing waits.
        void SetKeysInUse(const std::array<bool, 256>& used) {
            for (int i = 0; i < 256; ++i)
                keys_in_use_[i].store(used[i], std::memory_order_relaxed);
        }

        // Any thread: how a host mouse's movement is turned into a PSX mouse's counts, and
        // whether the mode that captures the cursor is allowed to right now - which is the
        // App's call, not this thread's (App::SendMouseSettingsToInput). See mouse.h.
        void SetMouseMotion(utilities::MouseMotion motion, int host_dpi, bool may_capture) {
            mouse_.SetMotion(motion, host_dpi);
            mouse_.SetCaptureEnabled(may_capture);
        }

        // Any thread: re-reads Windows' own pointer speed and acceleration curve, for
        // MouseMotion::kWindows. Called on WM_SETTINGCHANGE, when someone has been in the mouse
        // control panel.
        void RefreshWindowsPointerSettings() { mouse_.ReadWindowsPointerSettings(); }

        // Whether the cursor is currently pinned to the window's middle, so the window procedure
        // can hide it while it is.
        bool capturing_mouse() const { return mouse_.capturing(); }

     private:
        void Run();
        static LRESULT CALLBACK RawInputWindowProc(HWND window, UINT message, WPARAM wparam,
                                                   LPARAM lparam);

        emulation::host::InputExchange* exchange_;
        HWND main_window_;

        // The input thread's own, from here down.
        Mouse mouse_;
        std::array<Gamepad, 4> gamepads_{ Gamepad(0), Gamepad(1), Gamepad(2), Gamepad(3) };

        std::thread thread_;
        std::atomic<bool> stop_{ false };
        std::atomic<uint64_t> polls_{ 0 };
        std::array<std::atomic<bool>, 256> keys_in_use_{};
    };

}   // namespace psxemu
