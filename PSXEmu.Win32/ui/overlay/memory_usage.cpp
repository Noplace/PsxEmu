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
#include "ui/overlay/memory_usage.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dxgi1_4.h>
#include <psapi.h>
#include <wrl/client.h>

#include <vector>

#pragma comment(lib, "dxgi.lib")

namespace psxemu {

    struct MemoryMonitor::Impl {
        Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
        // The hardware cards, listed once and kept until DXGI says the list has changed: a sample
        // is then only a question to each, not a walk over the machine's adapters every second.
        std::vector<Microsoft::WRL::ComPtr<IDXGIAdapter3>> adapters;
    };

    MemoryMonitor::MemoryMonitor() : impl_(new Impl) {}
    MemoryMonitor::~MemoryMonitor() = default;
    MemoryMonitor::MemoryMonitor(MemoryMonitor&&) noexcept = default;
    MemoryMonitor& MemoryMonitor::operator=(MemoryMonitor&&) noexcept = default;

    MemoryUsage MemoryMonitor::Sample() {
        MemoryUsage usage;
        PROCESS_MEMORY_COUNTERS_EX counters = {};
        counters.cb = sizeof(counters);
        if (GetProcessMemoryInfo(GetCurrentProcess(),
                                 reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                                 sizeof(counters))) {
            usage.working_set = counters.WorkingSetSize;
            usage.commit = counters.PrivateUsage;
        }
        if (impl_ == nullptr)
            return usage;

        // A card added or removed leaves the factory out of date; a new one lists the new set.
        Microsoft::WRL::ComPtr<IDXGIFactory1>& factory = impl_->factory;
        if (factory == nullptr || !factory->IsCurrent()) {
            factory.Reset();
            impl_->adapters.clear();
            if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
                return usage;
        }
        if (impl_->adapters.empty()) {
            Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
            for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
                DXGI_ADAPTER_DESC1 description = {};
                Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter3;
                // Not WARP: it is the CPU drawing, and its "video memory" is ordinary RAM.
                if (SUCCEEDED(adapter->GetDesc1(&description)) &&
                    (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 &&
                    SUCCEEDED(adapter.As(&adapter3)))
                    impl_->adapters.push_back(adapter3);
                adapter.Reset();
            }
        }
        for (const Microsoft::WRL::ComPtr<IDXGIAdapter3>& adapter : impl_->adapters) {
            DXGI_QUERY_VIDEO_MEMORY_INFO local = {}, shared = {};
            if (SUCCEEDED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local))) {
                usage.has_gpu = true;
                usage.gpu_dedicated += local.CurrentUsage;
                if (SUCCEEDED(adapter->QueryVideoMemoryInfo(
                        0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &shared)))
                    usage.gpu_shared += shared.CurrentUsage;
            }
        }
        return usage;
    }

}   // namespace psxemu
