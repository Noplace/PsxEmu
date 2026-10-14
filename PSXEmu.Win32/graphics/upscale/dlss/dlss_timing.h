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

// What DLSS cost and what Frame Generation made, for Emulation > Show Timings: added up on the
// video thread as pictures are drawn and presented, and taken by the UI thread with the rest of
// the readout, about once a second. The App owns one; the presenter hands it to each engine it
// makes (IGraphicsEngine::SetDlssTiming), and only the Direct3D 12 one adds to it.

#include <atomic>
#include <cstdint>

namespace psxemu {

    class DlssTiming {
     public:
        // The video thread's. One picture DLSS made, and the card's time for it: its inputs made
        // from the plane, and DLSS itself.
        void AddPicture(double gpu_ms) {
            pictures_.fetch_add(1, std::memory_order_relaxed);
            gpu_ns_.fetch_add(static_cast<uint64_t>(gpu_ms * 1e6), std::memory_order_relaxed);
        }
        // ...and one present of ours with Frame Generation on, and how many frames went to the
        // screen since the last - its own and those Frame Generation made.
        void AddGenerated(uint32_t shown) {
            generating_presents_.fetch_add(1, std::memory_order_relaxed);
            shown_.fetch_add(shown, std::memory_order_relaxed);
        }

        // Any thread: the totals since the last call, which starts them again.
        struct Totals {
            uint64_t pictures = 0;
            double gpu_ms = 0.0;
            uint64_t generating_presents = 0;
            uint64_t shown = 0;
        };
        Totals Take() {
            Totals totals;
            totals.pictures = pictures_.exchange(0, std::memory_order_relaxed);
            totals.gpu_ms =
                static_cast<double>(gpu_ns_.exchange(0, std::memory_order_relaxed)) / 1e6;
            totals.generating_presents =
                generating_presents_.exchange(0, std::memory_order_relaxed);
            totals.shown = shown_.exchange(0, std::memory_order_relaxed);
            return totals;
        }

     private:
        std::atomic<uint64_t> pictures_{0};
        std::atomic<uint64_t> gpu_ns_{0};
        std::atomic<uint64_t> generating_presents_{0};
        std::atomic<uint64_t> shown_{0};
    };

}   // namespace psxemu
