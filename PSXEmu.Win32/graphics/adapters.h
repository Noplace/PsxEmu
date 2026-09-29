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

// The graphics cards this machine has, and how one is picked (Settings > Video > Graphics Card).
//
// Everything that draws names a card by its LUID - the locally unique id Windows gives an adapter
// for as long as the machine is up, and that Direct3D 11 and 12, Vulkan and the hardware
// rasteriser all know it by. A LUID is not the same after a restart, so what is *saved* is the
// card's name, and looked up here each time the emulator starts.
//
// Header-only, so the command-line tools that link the hardware rasteriser can use it too.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#pragma comment(lib, "dxgi.lib")

namespace psxemu {

    struct GraphicsAdapter {
        // What DXGI calls it, as UTF-8 - and what is saved. Two identical cards get " #2" and so
        // on after the first, so a name is always a key.
        std::string name;
        uint64_t luid = 0;
        // Dedicated video memory: a laptop's integrated card has next to none, and its discrete
        // one gigabytes, which is the only way a menu can tell them apart.
        uint64_t video_memory = 0;
    };

    inline uint64_t PackLuid(const LUID& luid) {
        return (static_cast<uint64_t>(static_cast<uint32_t>(luid.HighPart)) << 32) | luid.LowPart;
    }

    inline LUID UnpackLuid(uint64_t packed) {
        LUID luid = {};
        luid.LowPart = static_cast<DWORD>(packed & 0xFFFFFFFFu);
        luid.HighPart = static_cast<LONG>(packed >> 32);
        return luid;
    }

    // How Windows' performance counters spell a LUID - the part of a `\GPU Engine` instance name
    // after "luid_" - for finding which card a process is using.
    inline std::string CounterLuid(uint64_t packed) {
        char text[40];
        snprintf(text, sizeof(text), "0x%08X_0x%08X", static_cast<unsigned>(packed >> 32),
                 static_cast<unsigned>(packed & 0xFFFFFFFFu));
        return text;
    }

    // The hardware adapters, in the order Windows lists them - the software one ("Microsoft Basic
    // Render Driver", which is WARP) left out. Empty if DXGI is not there.
    inline std::vector<GraphicsAdapter> EnumerateGraphicsAdapters() {
        std::vector<GraphicsAdapter> found;
        Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
            return found;
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 description = {};
            if (SUCCEEDED(adapter->GetDesc1(&description)) &&
                (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
                char name[256] = {};
                WideCharToMultiByte(CP_UTF8, 0, description.Description, -1, name,
                                    sizeof(name) - 1, nullptr, nullptr);
                GraphicsAdapter entry;
                entry.name = name;
                entry.luid = PackLuid(description.AdapterLuid);
                entry.video_memory = description.DedicatedVideoMemory;
                int same = 1;
                for (const GraphicsAdapter& earlier : found) {
                    if (earlier.name == entry.name ||
                        earlier.name.rfind(entry.name + " #", 0) == 0)
                        ++same;
                }
                if (same > 1)
                    entry.name += " #" + std::to_string(same);
                found.push_back(entry);
            }
            adapter.Reset();
        }
        return found;
    }

    // The LUID of the card called `name` - 0 for "", and for a card that is not here (a laptop
    // undocked from its external one), which then means the same as "automatic".
    inline uint64_t FindGraphicsAdapter(const std::vector<GraphicsAdapter>& adapters,
                                        const std::string& name) {
        if (name.empty())
            return 0;
        for (const GraphicsAdapter& adapter : adapters)
            if (adapter.name == name)
                return adapter.luid;
        return 0;
    }

    // A card by part of its name, whatever the case - for the command line's `--gpu 4060`.
    inline const GraphicsAdapter* FindGraphicsAdapterLike(
        const std::vector<GraphicsAdapter>& adapters, const std::string& part) {
        auto lower = [](std::string text) {
            std::transform(text.begin(), text.end(), text.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return text;
        };
        const std::string wanted = lower(part);
        for (const GraphicsAdapter& adapter : adapters)
            if (!wanted.empty() && lower(adapter.name).find(wanted) != std::string::npos)
                return &adapter;
        return nullptr;
    }

    // The adapter with that LUID, or null: Direct3D's way of being told which card to make a
    // device on.
    inline Microsoft::WRL::ComPtr<IDXGIAdapter1> OpenAdapter(uint64_t luid) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        if (luid == 0)
            return adapter;
        Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) ||
            FAILED(factory->EnumAdapterByLuid(UnpackLuid(luid), IID_PPV_ARGS(&adapter))))
            adapter.Reset();
        return adapter;
    }

}   // namespace psxemu
