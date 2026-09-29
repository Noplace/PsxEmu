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

// How much memory this process is using, for the performance panel (F9): the RAM it holds, and what it
// has on the graphics cards.
//
// The card figures are DXGI's own per-process ones (IDXGIAdapter3::QueryVideoMemoryInfo, which is what
// Windows' memory manager charges this process for on each card, whatever API asked for it - Direct3D,
// Vulkan or OpenGL alike). They are not the ones Task Manager's "GPU Process Memory" counters give: those
// grow when a second device opens shared textures and never come down, though nothing is held. Summed
// over the cards, since the hardware rasteriser may be on one and the renderer on another.
//
// The Windows headers stay in memory_usage.cpp: this is included by the overlay, whose code names
// variables `small`, which windows.h turns into `char`.

#include <cstdint>
#include <memory>

namespace psxemu {

    struct MemoryUsage {
        uint64_t working_set = 0;      // physical RAM the process holds
        uint64_t commit = 0;           // memory it has committed - what a leak grows
        bool has_gpu = false;          // whether any card answered
        uint64_t gpu_dedicated = 0;    // in the cards' own memory (on an integrated one, a slice of RAM set aside)
        uint64_t gpu_shared = 0;       // in system memory the cards use as well
    };

    // Cheap enough to sample once a second: a couple of system calls and a walk over the cards.
    class MemoryMonitor {
     public:
        MemoryMonitor();
        ~MemoryMonitor();
        MemoryMonitor(MemoryMonitor&&) noexcept;
        MemoryMonitor& operator=(MemoryMonitor&&) noexcept;

        MemoryUsage Sample();

     private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

}   // namespace psxemu
