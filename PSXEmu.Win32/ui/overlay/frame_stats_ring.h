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

// The machine thread's per-frame timings on their way to the overlay on the video thread: one
// writer, one reader, no lock. A frame the reader is too slow to take is dropped rather than
// waited for - the machine must never stall on a graph.

#include "host/machine.h"

#include <array>
#include <atomic>
#include <cstddef>

namespace psxemu {

    class FrameStatsRing {
     public:
        // The machine's thread.
        void Push(const emulation::host::FrameSample& sample) {
            const size_t head = head_.load(std::memory_order_relaxed);
            const size_t next = (head + 1) % kCapacity;
            if (next == tail_.load(std::memory_order_acquire))
                return;   // full: the video thread has not looked for a while
            slots_[head] = sample;
            head_.store(next, std::memory_order_release);
        }

        // The video thread. Calls `take` with each sample waiting, oldest first.
        template <typename Take>
        void Drain(Take take) {
            size_t tail = tail_.load(std::memory_order_relaxed);
            const size_t head = head_.load(std::memory_order_acquire);
            while (tail != head) {
                take(slots_[tail]);
                tail = (tail + 1) % kCapacity;
            }
            tail_.store(tail, std::memory_order_release);
        }

     private:
        static const size_t kCapacity = 1024;   // seventeen seconds of frames
        std::array<emulation::host::FrameSample, kCapacity> slots_{};
        std::atomic<size_t> head_{ 0 };
        std::atomic<size_t> tail_{ 0 };
    };

}   // namespace psxemu
