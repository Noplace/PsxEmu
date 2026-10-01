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

// NVIDIA Reflex's markers from the machine's thread (host/latency_markers.h), passed to whichever
// renderer's Streamline runs Frame Generation (Docs/DLSS-Plan.md, phase 5).
//
// One for the process: the machine keeps the pointer across renderers made and remade, and a
// renderer attaches its Streamline here once Frame Generation is ready, and detaches it before
// Streamline stops - under the lock every marker is sent under, so none reaches one shutting down.
// Without one attached every call does nothing.

#include "host/latency_markers.h"

#include <atomic>
#include <cstdint>
#include <shared_mutex>

namespace psxemu {

    class Streamline;

    class ReflexMarkers : public emulation::host::LatencyMarkers {
     public:
        static ReflexMarkers& Get();

        // The video thread's: a Streamline with Frame Generation, or null.
        void Attach(Streamline* streamline);

        // The machine's thread's.
        void Sleep(uint32_t picture) override;
        void Mark(Marker marker, uint32_t picture) override;

        // PC Latency's ping: the Windows message it posts to the window's thread now and then,
        // 0 until there is one. On seeing it, Ping() - the marker, for the next picture.
        uint32_t ping_message() const { return ping_message_.load(std::memory_order_relaxed); }
        void set_ping_message(uint32_t message) {
            ping_message_.store(message, std::memory_order_relaxed);
        }
        void Ping();

     private:
        ReflexMarkers() = default;
        std::shared_mutex mutex_;
        Streamline* streamline_ = nullptr;
        std::atomic<uint32_t> ping_message_{ 0 };
        std::atomic<uint32_t> picture_{ 0 };   // the picture whose simulation last started
        // The machine's thread's: how long Reflex has held it, for PSXEMU_DLSS_LOG.
        double slept_total_ = 0.0, slept_most_ = 0.0;
        int slept_count_ = 0;
    };

}   // namespace psxemu
