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
#include "graphics/dlss/reflex_markers.h"

#include "graphics/dlss/streamline.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <mutex>

namespace psxemu {

    ReflexMarkers& ReflexMarkers::Get() {
        static ReflexMarkers markers;
        return markers;
    }

    void ReflexMarkers::Attach(Streamline* streamline) {
        std::unique_lock lock(mutex_);
        streamline_ = streamline;
    }

    // Streamline's frame tokens are its frame numbers: asked for by number, from any thread, and
    // used at once, before its ring of six could come round to the same slot.
    //
    // At the simulation's start, on the machine's thread, as NVIDIA has it: called from the
    // renderer instead, before drawing, Frame Generation stopped making pictures. How long it
    // holds the machine goes to PSXEMU_DLSS_LOG, every 256 pictures.
    void ReflexMarkers::Sleep(uint32_t picture) {
        picture_.store(picture, std::memory_order_relaxed);
        std::shared_lock lock(mutex_);
        if (streamline_ == nullptr)
            return;
        sl::FrameToken* token = nullptr;
        if (streamline_->slGetNewFrameToken(token, &picture) != sl::Result::eOk)
            return;
        const auto start = std::chrono::steady_clock::now();
        streamline_->slReflexSleep(*token);
        const double slept = std::chrono::duration<double, std::milli>(
                                 std::chrono::steady_clock::now() - start).count();
        slept_total_ += slept;
        slept_most_ = (std::max)(slept_most_, slept);
        if (++slept_count_ == 256) {
            char line[120];
            snprintf(line, sizeof(line), "Reflex held the machine %.2f ms a picture, at most %.2f ms",
                     slept_total_ / slept_count_, slept_most_);
            DlssNote(line);
            slept_total_ = slept_most_ = 0.0;
            slept_count_ = 0;
        }
    }

    void ReflexMarkers::Mark(Marker marker, uint32_t picture) {
        std::shared_lock lock(mutex_);
        if (streamline_ == nullptr)
            return;
        const sl::PCLMarker pcl = marker == Marker::kSimulationStart ? sl::PCLMarker::eSimulationStart
                                  : marker == Marker::kSimulationEnd ? sl::PCLMarker::eSimulationEnd
                                                                     : sl::PCLMarker::eControllerInputSample;
        sl::FrameToken* token = nullptr;
        if (streamline_->slGetNewFrameToken(token, &picture) == sl::Result::eOk)
            streamline_->slPCLSetMarker(pcl, *token);
    }

    // The ping is read by the machine at the next picture's input, so it is that picture's.
    void ReflexMarkers::Ping() {
        std::shared_lock lock(mutex_);
        if (streamline_ == nullptr)
            return;
        const uint32_t next = picture_.load(std::memory_order_relaxed) + 1;
        sl::FrameToken* token = nullptr;
        if (streamline_->slGetNewFrameToken(token, &next) == sl::Result::eOk)
            streamline_->slPCLSetMarker(sl::PCLMarker::ePCLatencyPing, *token);
    }

}   // namespace psxemu
