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
    };

}   // namespace psxemu
