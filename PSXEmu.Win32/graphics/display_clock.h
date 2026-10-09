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

// The display the picture is on, as Settings > Video > Frame Pacing's "Match the display" needs
// it (platform/display_sync.h): its refresh rate, exactly, and when its vblanks fall.
//
// The rate is the display mode's own, from Windows' display configuration - 59.951 Hz where
// the monitor's settings page says 60 - since the machine is to run at it for minutes on end,
// and a rate rounded to a whole number drifts a frame every twenty seconds.
//
// The vblanks are watched by a thread of this class's that waits for each one on the monitor's
// DXGI output (IDXGIOutput::WaitForVBlank) and notes when it woke. That works whichever renderer
// draws - OpenGL and Vulkan too - and whichever card the panel hangs off, which on a laptop with
// two is not the one drawing. A wake is a little late, and by a varying amount; the clock keeps
// a smoothed anchor rather than the last wake, and the machine moves its frames towards that
// only gradually (PhaseCorrection), which averages the rest out.
//
// Everything is the UI thread's but Sample, which the machine's thread calls once a frame.

#include "app/framework.h"

#include "platform/display_sync.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

namespace psxemu {

    class DisplayClock {
     public:
        DisplayClock() = default;
        ~DisplayClock() { Stop(); }
        DisplayClock(const DisplayClock&) = delete;
        DisplayClock& operator=(const DisplayClock&) = delete;

        // Starts or stops watching: only while Frame Pacing matches the display, so nothing
        // wakes sixty times a second for a setting that is off.
        void SetRunning(bool on);

        // The monitor the window is on - after a move, or when it is made. `changed` says the
        // display's mode may have changed under the same monitor (WM_DISPLAYCHANGE), so its rate
        // is read again.
        void SetMonitor(HMONITOR monitor, bool changed = false);

        // What is known now. False if nothing is: not running, or no rate found for the monitor.
        // Any thread.
        bool Sample(utilities::DisplayTiming* timing) const;

        // The rate as last read, 0 if none - for the Video Settings window. Any thread.
        double refresh_hz() const;

        // The refresh rate of `monitor`'s current mode, exactly; 0 if Windows will not say.
        static double RefreshRateOf(HMONITOR monitor);

     private:
        typedef std::chrono::steady_clock Clock;

        void Stop();
        void Run();
        // Takes in a vblank woken for at `now`.
        void NoteVblank(Clock::time_point now);

        mutable std::mutex lock_;
        // Under lock_:
        HMONITOR monitor_ = nullptr;
        uint64_t monitor_serial_ = 0;   // bumped when the monitor or its mode changes
        double refresh_hz_ = 0.0;
        bool has_anchor_ = false;
        Clock::time_point anchor_;      // a vblank, as smoothed
        int outliers_ = 0;              // wakes in a row too far from the anchor to take

        std::thread thread_;
        std::atomic<bool> stop_{ false };
        std::atomic<bool> running_{ false };   // what Sample asks, rather than thread_ itself
    };

}   // namespace psxemu
