/*****************************************************************************************************************
* Copyright (c) 2014 Khalid Ali Al-Kooheji                                                                       *
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

// The renderer's Direct3D 12 device, for the hardware rasteriser to draw on as well.
//
// With the renderer and the rasteriser both on Direct3D 12 and on one card, one device does
// for both (Docs/Hardware-Renderer-Plan.md): the rasteriser's pictures go to the renderer as
// they are - left compressed, opened by no handle, waited for on the card rather than by the
// video thread, and drawn from without being copied first. D3D12GraphicsEngine says here which
// device it has while it runs; the App's factory hands it to the rasteriser it makes, and makes
// the rasteriser again whenever the count here moves - another renderer, another card, the
// renderer made again for DLSS.
//
// The native device always, never Streamline's proxy of it: nothing the rasteriser makes passes
// through NVIDIA's code. Header-only, and safe from any thread.

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <mutex>

namespace psxemu {

    class SharedD3D12Device {
     public:
        static SharedD3D12Device& Get() {
            static SharedD3D12Device instance;
            return instance;
        }

        // The video thread's: this device, on the card with this LUID, is the renderer's now.
        void Publish(ID3D12Device* device, uint64_t luid) {
            std::lock_guard<std::mutex> lock(mutex_);
            device_ = device;
            luid_ = luid;
            ++generation_;
        }
        // ...and no longer: the renderer is going. Another's published since is left alone.
        void Withdraw(ID3D12Device* device) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (device_.Get() != device)
                return;
            device_.Reset();
            luid_ = 0;
            ++generation_;
        }

        // The renderer's device if it is on the card with `luid`, or null; and in `generation`,
        // the count of devices published and withdrawn when it was asked.
        Microsoft::WRL::ComPtr<ID3D12Device> On(uint64_t luid, uint64_t* generation) const {
            std::lock_guard<std::mutex> lock(mutex_);
            if (generation != nullptr)
                *generation = generation_;
            return luid != 0 && luid == luid_ ? device_ : nullptr;
        }
        uint64_t generation() const {
            std::lock_guard<std::mutex> lock(mutex_);
            return generation_;
        }

     private:
        SharedD3D12Device() = default;

        mutable std::mutex mutex_;
        Microsoft::WRL::ComPtr<ID3D12Device> device_;
        uint64_t luid_ = 0;
        uint64_t generation_ = 0;
    };

}   // namespace psxemu
