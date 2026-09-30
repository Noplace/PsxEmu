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

// sl_probe: what NVIDIA Streamline says about each graphics card here (Docs/DLSS-Plan.md, phase 0).
//
//   sl_probe [folder] [--optimal] [--jitter-test] [--cost]
//
// Loads Streamline from `folder` - by default the fetched SDK's, Temp\streamline\v2.14.1\bin\x64 -
// the way the emulator does (signature checked, manual hooking, no over-the-air updates), with DLSS
// and Frame Generation, and for each card prints its LUID, whether Windows' hardware-accelerated
// GPU scheduling is on for it, and whether DLSS Super Resolution, Frame Generation and Reflex run
// on it and if not why not. Then the driver and Windows versions Streamline saw, and those it
// needs. No device is made on any card, unless --optimal asks for the input sizes DLSS takes for
// each mode: those need a Direct3D 12 device, made once on the first card DLSS runs on.
//
// --jitter-test settles the two signs the emulator gives DLSS, on that same device: frames of a
// sharp-edged pattern are made here, each pixel showing what is at itself plus the jitter - the
// rasteriser's own rule (psx/shared_picture.h) - and put through DLSS Quality for 48 frames with
// each sign of the jitter each way, then with the pattern moving and each sign of its motion.
// Each output is scored against the pattern drawn at the output's size, 64 samples a pixel: the
// signs DLSS means score far better than the others. --cost does that too, and then times DLSS on
// the card at the sizes the emulator gives it.
//
// Exits 0 when Streamline started, 1 when it did not.

#include "graphics/adapters.h"
#include "graphics/d3dx12.h"
#include "graphics/dlss/streamline.h"

#include <d3d12.h>
#include <DirectXPackedVector.h>
#include <winternl.h>
#include <d3dkmthk.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "gdi32.lib")

namespace {

    // Windows' hardware-accelerated GPU scheduling for one card, as its kernel driver reports it:
    // "on", "off", "not supported", or "unknown" if the card will not say.
    const char* GpuScheduling(uint64_t luid) {
        D3DKMT_OPENADAPTERFROMLUID open = {};
        open.AdapterLuid = psxemu::UnpackLuid(luid);
        if (D3DKMTOpenAdapterFromLuid(&open) != 0)
            return "unknown";
        D3DKMT_WDDM_2_7_CAPS caps = {};
        D3DKMT_QUERYADAPTERINFO query = {};
        query.hAdapter = open.hAdapter;
        query.Type = KMTQAITYPE_WDDM_2_7_CAPS;
        query.pPrivateDriverData = &caps;
        query.PrivateDriverDataSize = sizeof(caps);
        const bool known = D3DKMTQueryAdapterInfo(&query) == 0;
        D3DKMT_CLOSEADAPTER close = {};
        close.hAdapter = open.hAdapter;
        D3DKMTCloseAdapter(&close);
        if (!known)
            return "unknown";
        if (!caps.HwSchSupported)
            return "not supported";
        return caps.HwSchEnabled ? "on" : "off";
    }

    void PrintSupport(const psxemu::Streamline& streamline, const char* what, sl::Feature feature,
                      uint64_t luid) {
        std::string why;
        if (streamline.Supports(feature, luid, &why))
            printf("    %-20s yes\n", what);
        else
            printf("    %-20s no: %s\n", what, why.c_str());
    }

    void PrintRequirements(const psxemu::Streamline& streamline, const char* what,
                           sl::Feature feature) {
        sl::FeatureRequirements requirements{};
        if (streamline.slGetFeatureRequirements(feature, requirements) != sl::Result::eOk) {
            printf("  %s: no requirements reported\n", what);
            return;
        }
        const auto flags = static_cast<uint32_t>(requirements.flags);
        printf("  %s: driver %s (needs %s), Windows %s (needs %s)%s%s\n", what,
               requirements.driverVersionDetected.toStr().c_str(),
               requirements.driverVersionRequired.toStr().c_str(),
               requirements.osVersionDetected.toStr().c_str(),
               requirements.osVersionRequired.toStr().c_str(),
               (flags & static_cast<uint32_t>(sl::FeatureRequirementFlags::eHardwareSchedulingRequired))
                   ? ", GPU scheduling required" : "",
               (flags & static_cast<uint32_t>(sl::FeatureRequirementFlags::eVSyncOffRequired))
                   ? ", v-sync off required" : "");
    }

    void PrintOptimalSettings(psxemu::Streamline& streamline,
                              const std::vector<psxemu::GraphicsAdapter>& adapters) {
        for (const psxemu::GraphicsAdapter& adapter : adapters) {
            std::string why;
            if (!streamline.SupportsDlss(adapter.luid, &why))
                continue;
            Microsoft::WRL::ComPtr<IDXGIAdapter1> card = psxemu::OpenAdapter(adapter.luid);
            Microsoft::WRL::ComPtr<ID3D12Device> device;
            if (FAILED(D3D12CreateDevice(card.Get(), D3D_FEATURE_LEVEL_11_0,
                                         IID_PPV_ARGS(&device)))) {
                printf("\nNo Direct3D 12 device on %s\n", adapter.name.c_str());
                return;
            }
            std::string error;
            if (!streamline.SetDevice(device.Get(), &error)) {
                printf("\n%s\n", error.c_str());
                return;
            }
            printf("\nInput sizes DLSS takes on %s (optimal, then least and most):\n",
                   adapter.name.c_str());
            const struct { sl::DLSSMode mode; const char* name; } modes[] = {
                { sl::DLSSMode::eDLAA, "DLAA" },
                { sl::DLSSMode::eMaxQuality, "Quality" },
                { sl::DLSSMode::eBalanced, "Balanced" },
                { sl::DLSSMode::eMaxPerformance, "Performance" },
                { sl::DLSSMode::eUltraPerformance, "Ultra Performance" },
            };
            // 4:3 on a 1080-line and a 1600-line screen, and an odd size.
            const struct { uint32_t width, height; } outputs[] = {
                { 1440, 1080 }, { 2133, 1600 }, { 1000, 750 },
            };
            for (const auto& output : outputs) {
                printf("  to %ux%u\n", output.width, output.height);
                for (const auto& mode : modes) {
                    sl::DLSSOptions options{};
                    options.mode = mode.mode;
                    options.outputWidth = output.width;
                    options.outputHeight = output.height;
                    sl::DLSSOptimalSettings settings{};
                    const sl::Result result = streamline.slDLSSGetOptimalSettings(options, settings);
                    if (result != sl::Result::eOk) {
                        printf("    %-18s %s\n", mode.name, psxemu::StreamlineResultText(result));
                        continue;
                    }
                    printf("    %-18s %ux%u, %ux%u - %ux%u\n", mode.name,
                           settings.optimalRenderWidth, settings.optimalRenderHeight,
                           settings.renderWidthMin, settings.renderHeightMin,
                           settings.renderWidthMax, settings.renderHeightMax);
                }
            }
            // Streamline goes before the device it was given.
            streamline.Stop();
            return;
        }
        printf("\nNo card here runs DLSS, so no input sizes.\n");
    }

    // ---- --jitter-test ----------------------------------------------------------------------

    using Microsoft::WRL::ComPtr;

    // A checkerboard of 3.3-pixel cells turned 17 degrees, in the input's pixels: edges at every
    // angle, finer than an input pixel can show and plain at the output's size.
    float Pattern(float x, float y) {
        const float c = 0.9563f, s = 0.2924f;
        const float u = (x * c - y * s) / 3.3f, v = (x * s + y * c) / 3.3f;
        return ((static_cast<int>(std::floor(u)) + static_cast<int>(std::floor(v))) & 1) ? 0.9f
                                                                                         : 0.1f;
    }

    float Halton(int index, int base) {
        float f = 1.0f, r = 0.0f;
        for (int i = index; i > 0; i /= base) {
            f /= static_cast<float>(base);
            r += f * static_cast<float>(i % base);
        }
        return r;
    }

    // One command list run to completion at a time: slow, and plain.
    struct TestGpu {
        ComPtr<ID3D12Device> device;
        ComPtr<ID3D12CommandQueue> queue;
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        ComPtr<ID3D12Fence> fence;
        UINT64 value = 0;
        HANDLE event = nullptr;

        bool Make(ID3D12Device* d) {
            device = d;
            D3D12_COMMAND_QUEUE_DESC desc = {};
            desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            return SUCCEEDED(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue))) &&
                   SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                            IID_PPV_ARGS(&allocator))) &&
                   SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                       allocator.Get(), nullptr,
                                                       IID_PPV_ARGS(&list))) &&
                   SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))) &&
                   event != nullptr;
        }
        void Run() {
            list->Close();
            ID3D12CommandList* lists[] = { list.Get() };
            queue->ExecuteCommandLists(1, lists);
            queue->Signal(fence.Get(), ++value);
            fence->SetEventOnCompletion(value, event);
            WaitForSingleObject(event, INFINITE);
            allocator->Reset();
            list->Reset(allocator.Get(), nullptr);
        }
        ~TestGpu() {
            if (event != nullptr)
                CloseHandle(event);
        }
        ComPtr<ID3D12Resource> Texture(int width, int height, DXGI_FORMAT format,
                                       D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state) {
            const CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_DEFAULT);
            const CD3DX12_RESOURCE_DESC desc =
                CD3DX12_RESOURCE_DESC::Tex2D(format, width, height, 1, 1, 1, 0, flags);
            ComPtr<ID3D12Resource> texture;
            device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                            IID_PPV_ARGS(&texture));
            return texture;
        }
        ComPtr<ID3D12Resource> Buffer(UINT64 bytes, D3D12_HEAP_TYPE type) {
            const CD3DX12_HEAP_PROPERTIES heap(type);
            const CD3DX12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(bytes);
            ComPtr<ID3D12Resource> buffer;
            device->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &desc,
                type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ
                                               : D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr, IID_PPV_ARGS(&buffer));
            return buffer;
        }
        // Rows of `row_bytes` into a texture in `state`, which it is left in, through `upload`.
        void Upload(ID3D12Resource* texture, ID3D12Resource* upload, const uint8_t* rows,
                    size_t row_bytes, D3D12_RESOURCE_STATES state) {
            const D3D12_RESOURCE_DESC desc = texture->GetDesc();
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
            device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, nullptr);
            uint8_t* mapped = nullptr;
            upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
            for (UINT y = 0; y < desc.Height; ++y)
                memcpy(mapped + footprint.Offset + y * footprint.Footprint.RowPitch,
                       rows + y * row_bytes, row_bytes);
            upload->Unmap(0, nullptr);
            const CD3DX12_RESOURCE_BARRIER to_copy = CD3DX12_RESOURCE_BARRIER::Transition(
                texture, state, D3D12_RESOURCE_STATE_COPY_DEST);
            list->ResourceBarrier(1, &to_copy);
            const CD3DX12_TEXTURE_COPY_LOCATION to(texture, 0);
            const CD3DX12_TEXTURE_COPY_LOCATION from(upload, footprint);
            list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
            const CD3DX12_RESOURCE_BARRIER back = CD3DX12_RESOURCE_BARRIER::Transition(
                texture, D3D12_RESOURCE_STATE_COPY_DEST, state);
            list->ResourceBarrier(1, &back);
        }
    };

    // 48 frames of the pattern through DLSS Quality, input 192x144 to 288x216, the jitter given
    // to DLSS times (sign_x, sign_y) and the motion given times `motion_sign`, the pattern moving
    // `vx, vy` input pixels a frame. The mean difference of the last output from the pattern
    // drawn at the output's size, 0-1; negative if anything failed.
    float JitterRun(psxemu::Streamline& streamline, TestGpu& gpu, float sign_x, float sign_y,
                    float vx, float vy, float motion_sign) {
        const int kIn = 192, kInH = 144, kOut = 288, kOutH = 216, kFrames = 48;
        ComPtr<ID3D12Resource> colour = gpu.Texture(kIn, kInH, DXGI_FORMAT_R8G8B8A8_UNORM,
                                                    D3D12_RESOURCE_FLAG_NONE,
                                                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        ComPtr<ID3D12Resource> motion = gpu.Texture(kIn, kInH, DXGI_FORMAT_R16G16_FLOAT,
                                                    D3D12_RESOURCE_FLAG_NONE,
                                                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        ComPtr<ID3D12Resource> depth = gpu.Texture(kIn, kInH, DXGI_FORMAT_R32_FLOAT,
                                                   D3D12_RESOURCE_FLAG_NONE,
                                                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        ComPtr<ID3D12Resource> output = gpu.Texture(kOut, kOutH, DXGI_FORMAT_R8G8B8A8_UNORM,
                                                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ComPtr<ID3D12Resource> colour_up = gpu.Buffer(kIn * kInH * 4 + 65536,
                                                      D3D12_HEAP_TYPE_UPLOAD);
        ComPtr<ID3D12Resource> motion_up = gpu.Buffer(kIn * kInH * 4 + 65536,
                                                      D3D12_HEAP_TYPE_UPLOAD);
        ComPtr<ID3D12Resource> depth_up = gpu.Buffer(kIn * kInH * 4 + 65536,
                                                     D3D12_HEAP_TYPE_UPLOAD);
        ComPtr<ID3D12Resource> readback = gpu.Buffer(kOut * kOutH * 4 + 65536,
                                                     D3D12_HEAP_TYPE_READBACK);
        if (!colour || !motion || !depth || !output || !colour_up || !readback)
            return -1.0f;

        // Depth everywhere the same, motion everywhere the pattern's own.
        const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        std::vector<float> depth_rows(kIn * kInH, 0.5f);
        gpu.Upload(depth.Get(), depth_up.Get(), reinterpret_cast<const uint8_t*>(depth_rows.data()),
                   kIn * 4, read);
        std::vector<uint16_t> motion_rows(kIn * kInH * 2);
        for (int i = 0; i < kIn * kInH; ++i) {
            // Where it was minus where it is: the pattern moves +v, so -v, times the sign tried.
            motion_rows[i * 2] = DirectX::PackedVector::XMConvertFloatToHalf(-vx * motion_sign);
            motion_rows[i * 2 + 1] = DirectX::PackedVector::XMConvertFloatToHalf(-vy * motion_sign);
        }
        gpu.Upload(motion.Get(), motion_up.Get(),
                   reinterpret_cast<const uint8_t*>(motion_rows.data()), kIn * 4, read);
        gpu.Run();

        const sl::ViewportHandle viewport(0u);
        sl::DLSSOptions options{};
        options.mode = sl::DLSSMode::eMaxQuality;
        options.outputWidth = kOut;
        options.outputHeight = kOutH;
        options.colorBuffersHDR = sl::Boolean::eFalse;
        if (streamline.slDLSSSetOptions(viewport, options) != sl::Result::eOk)
            return -1.0f;

        std::vector<uint8_t> colour_rows(kIn * kInH * 4);
        float shift_x = 0.0f, shift_y = 0.0f;
        for (int frame = 0; frame < kFrames; ++frame) {
            shift_x = vx * frame;
            shift_y = vy * frame;
            const float jx = Halton(frame + 1, 2) - 0.5f, jy = Halton(frame + 1, 3) - 0.5f;
            // Each pixel shows what is at itself plus the jitter - of the pattern where it is now.
            for (int y = 0; y < kInH; ++y) {
                for (int x = 0; x < kIn; ++x) {
                    const float value = Pattern(x + 0.5f + jx - shift_x, y + 0.5f + jy - shift_y);
                    const uint8_t level = static_cast<uint8_t>(value * 255.0f + 0.5f);
                    uint8_t* p = &colour_rows[(y * kIn + x) * 4];
                    p[0] = p[1] = p[2] = level;
                    p[3] = 255;
                }
            }
            gpu.Upload(colour.Get(), colour_up.Get(), colour_rows.data(), kIn * 4, read);

            sl::FrameToken* token = nullptr;
            if (streamline.slGetNewFrameToken(token, nullptr) != sl::Result::eOk)
                return -1.0f;
            sl::Constants constants{};
            sl::float4x4 identity;
            identity.row[0] = sl::float4(1, 0, 0, 0);
            identity.row[1] = sl::float4(0, 1, 0, 0);
            identity.row[2] = sl::float4(0, 0, 1, 0);
            identity.row[3] = sl::float4(0, 0, 0, 1);
            constants.cameraViewToClip = constants.clipToCameraView = identity;
            constants.clipToLensClip = constants.clipToPrevClip = identity;
            constants.prevClipToClip = identity;
            constants.jitterOffset = sl::float2(jx * sign_x, jy * sign_y);
            constants.mvecScale = sl::float2(1.0f / kIn, 1.0f / kInH);
            constants.cameraPinholeOffset = sl::float2(0, 0);
            constants.cameraPos = sl::float3(0, 0, 0);
            constants.cameraUp = sl::float3(0, 1, 0);
            constants.cameraRight = sl::float3(1, 0, 0);
            constants.cameraFwd = sl::float3(0, 0, 1);
            constants.cameraNear = 1.0f;
            constants.cameraFar = 65536.0f;
            constants.cameraFOV = 1.0f;
            constants.cameraAspectRatio = 4.0f / 3.0f;
            constants.depthInverted = sl::Boolean::eTrue;
            constants.cameraMotionIncluded = sl::Boolean::eTrue;
            constants.motionVectors3D = sl::Boolean::eFalse;
            constants.reset = frame == 0 ? sl::Boolean::eTrue : sl::Boolean::eFalse;
            if (streamline.slSetConstants(constants, *token, viewport) != sl::Result::eOk)
                return -1.0f;
            sl::Resource in(sl::ResourceType::eTex2d, colour.Get(), read);
            sl::Resource out(sl::ResourceType::eTex2d, output.Get(),
                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            sl::Resource mv(sl::ResourceType::eTex2d, motion.Get(), read);
            sl::Resource z(sl::ResourceType::eTex2d, depth.Get(), read);
            const sl::ResourceLifecycle until = sl::ResourceLifecycle::eValidUntilEvaluate;
            const sl::ResourceTag tags[] = {
                sl::ResourceTag(&in, sl::kBufferTypeScalingInputColor, until),
                sl::ResourceTag(&out, sl::kBufferTypeScalingOutputColor, until),
                sl::ResourceTag(&mv, sl::kBufferTypeMotionVectors, until),
                sl::ResourceTag(&z, sl::kBufferTypeDepth, until),
            };
            if (streamline.slSetTagForFrame(*token, viewport, tags, 4, gpu.list.Get()) !=
                sl::Result::eOk)
                return -1.0f;
            const sl::BaseStructure* inputs[] = { &viewport };
            if (streamline.slEvaluateFeature(sl::kFeatureDLSS, *token, inputs, 1,
                                             gpu.list.Get()) != sl::Result::eOk)
                return -1.0f;
            gpu.Run();
        }

        // The last output back, against the pattern drawn at the output's size.
        const D3D12_RESOURCE_DESC desc = output->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
        gpu.device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, nullptr);
        const CD3DX12_RESOURCE_BARRIER to_copy = CD3DX12_RESOURCE_BARRIER::Transition(
            output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        gpu.list->ResourceBarrier(1, &to_copy);
        const CD3DX12_TEXTURE_COPY_LOCATION to(readback.Get(), footprint);
        const CD3DX12_TEXTURE_COPY_LOCATION from(output.Get(), 0);
        gpu.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        gpu.Run();
        const uint8_t* mapped = nullptr;
        readback->Map(0, nullptr, reinterpret_cast<void**>(const_cast<uint8_t**>(&mapped)));
        const float scale = static_cast<float>(kIn) / kOut;
        double total = 0.0;
        int count = 0;
        for (int y = 6; y < kOutH - 6; ++y) {
            for (int x = 6; x < kOut - 6; ++x) {
                float truth = 0.0f;
                for (int sy = 0; sy < 8; ++sy)
                    for (int sx = 0; sx < 8; ++sx)
                        truth += Pattern((x + (sx + 0.5f) / 8.0f) * scale - shift_x,
                                         (y + (sy + 0.5f) / 8.0f) * scale - shift_y);
                truth /= 64.0f;
                const float got = mapped[footprint.Offset + y * footprint.Footprint.RowPitch +
                                         x * 4] / 255.0f;
                total += std::fabs(got - truth);
                ++count;
            }
        }
        readback->Unmap(0, nullptr);
        return static_cast<float>(total / count);
    }

    // DLSS's own time on the card for one evaluation, in milliseconds, at these sizes and mode:
    // timestamps either side of it, the median of 30 after 10 to warm up. Negative if it failed.
    double MeasureCost(psxemu::Streamline& streamline, TestGpu& gpu, sl::DLSSMode mode, int in_w,
                       int in_h, int out_w, int out_h) {
        const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        ComPtr<ID3D12Resource> colour = gpu.Texture(in_w, in_h, DXGI_FORMAT_B8G8R8A8_UNORM,
                                                    D3D12_RESOURCE_FLAG_NONE, read);
        ComPtr<ID3D12Resource> motion = gpu.Texture(in_w, in_h, DXGI_FORMAT_R16G16_FLOAT,
                                                    D3D12_RESOURCE_FLAG_NONE, read);
        ComPtr<ID3D12Resource> depth = gpu.Texture(in_w, in_h, DXGI_FORMAT_R32_FLOAT,
                                                   D3D12_RESOURCE_FLAG_NONE, read);
        ComPtr<ID3D12Resource> hint = gpu.Texture(in_w, in_h, DXGI_FORMAT_R8_UNORM,
                                                  D3D12_RESOURCE_FLAG_NONE, read);
        ComPtr<ID3D12Resource> output = gpu.Texture(out_w, out_h, DXGI_FORMAT_R8G8B8A8_UNORM,
                                                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                                    read);
        ComPtr<ID3D12Resource> times = gpu.Buffer(16, D3D12_HEAP_TYPE_READBACK);
        ComPtr<ID3D12QueryHeap> queries;
        D3D12_QUERY_HEAP_DESC query_desc = {};
        query_desc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        query_desc.Count = 2;
        if (!colour || !motion || !depth || !hint || !output || !times ||
            FAILED(gpu.device->CreateQueryHeap(&query_desc, IID_PPV_ARGS(&queries))))
            return -1.0;
        UINT64 frequency = 0;
        gpu.queue->GetTimestampFrequency(&frequency);

        const sl::ViewportHandle viewport(1u);
        sl::DLSSOptions options{};
        options.mode = mode;
        options.outputWidth = static_cast<uint32_t>(out_w);
        options.outputHeight = static_cast<uint32_t>(out_h);
        options.colorBuffersHDR = sl::Boolean::eFalse;
        if (streamline.slDLSSSetOptions(viewport, options) != sl::Result::eOk)
            return -1.0;
        std::vector<double> samples;
        for (int frame = 0; frame < 40; ++frame) {
            sl::FrameToken* token = nullptr;
            if (streamline.slGetNewFrameToken(token, nullptr) != sl::Result::eOk)
                return -1.0;
            sl::Constants constants{};
            sl::float4x4 identity;
            identity.row[0] = sl::float4(1, 0, 0, 0);
            identity.row[1] = sl::float4(0, 1, 0, 0);
            identity.row[2] = sl::float4(0, 0, 1, 0);
            identity.row[3] = sl::float4(0, 0, 0, 1);
            constants.cameraViewToClip = constants.clipToCameraView = identity;
            constants.clipToLensClip = constants.clipToPrevClip = identity;
            constants.prevClipToClip = identity;
            constants.jitterOffset = sl::float2(Halton(frame + 1, 2) - 0.5f,
                                                Halton(frame + 1, 3) - 0.5f);
            constants.mvecScale = sl::float2(1.0f / in_w, 1.0f / in_h);
            constants.cameraPinholeOffset = sl::float2(0, 0);
            constants.cameraPos = sl::float3(0, 0, 0);
            constants.cameraUp = sl::float3(0, 1, 0);
            constants.cameraRight = sl::float3(1, 0, 0);
            constants.cameraFwd = sl::float3(0, 0, 1);
            constants.cameraNear = 1.0f;
            constants.cameraFar = 65536.0f;
            constants.cameraFOV = 1.0f;
            constants.cameraAspectRatio = 4.0f / 3.0f;
            constants.depthInverted = sl::Boolean::eTrue;
            constants.cameraMotionIncluded = sl::Boolean::eTrue;
            constants.motionVectors3D = sl::Boolean::eFalse;
            constants.reset = frame == 0 ? sl::Boolean::eTrue : sl::Boolean::eFalse;
            if (streamline.slSetConstants(constants, *token, viewport) != sl::Result::eOk)
                return -1.0;
            sl::Resource in(sl::ResourceType::eTex2d, colour.Get(), read);
            sl::Resource out(sl::ResourceType::eTex2d, output.Get(), read);
            sl::Resource mv(sl::ResourceType::eTex2d, motion.Get(), read);
            sl::Resource z(sl::ResourceType::eTex2d, depth.Get(), read);
            sl::Resource bias(sl::ResourceType::eTex2d, hint.Get(), read);
            const sl::ResourceLifecycle until = sl::ResourceLifecycle::eValidUntilEvaluate;
            const sl::ResourceTag tags[] = {
                sl::ResourceTag(&in, sl::kBufferTypeScalingInputColor, until),
                sl::ResourceTag(&out, sl::kBufferTypeScalingOutputColor, until),
                sl::ResourceTag(&mv, sl::kBufferTypeMotionVectors, until),
                sl::ResourceTag(&z, sl::kBufferTypeDepth, until),
                sl::ResourceTag(&bias, sl::kBufferTypeBiasCurrentColorHint, until),
            };
            if (streamline.slSetTagForFrame(*token, viewport, tags, 5, gpu.list.Get()) !=
                sl::Result::eOk)
                return -1.0;
            gpu.list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
            const sl::BaseStructure* inputs[] = { &viewport };
            if (streamline.slEvaluateFeature(sl::kFeatureDLSS, *token, inputs, 1,
                                             gpu.list.Get()) != sl::Result::eOk)
                return -1.0;
            gpu.list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
            gpu.list->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2,
                                       times.Get(), 0);
            gpu.Run();
            UINT64* stamps = nullptr;
            times->Map(0, nullptr, reinterpret_cast<void**>(&stamps));
            if (frame >= 10)
                samples.push_back(static_cast<double>(stamps[1] - stamps[0]) * 1000.0 /
                                  static_cast<double>(frequency));
            times->Unmap(0, nullptr);
        }
        streamline.slFreeResources(sl::kFeatureDLSS, viewport);
        std::sort(samples.begin(), samples.end());
        return samples[samples.size() / 2];
    }

    void RunCostTest(psxemu::Streamline& streamline, TestGpu& gpu, const char* card) {
        printf("\nDLSS's time on %s for one picture (median of 30, ms):\n", card);
        const struct { sl::DLSSMode mode; const char* name; int in_w, in_h, out_w, out_h; } runs[] = {
            { sl::DLSSMode::eMaxQuality, "Quality, 2x to a 709-line window", 640, 480, 945, 709 },
            { sl::DLSSMode::eMaxQuality, "Quality, 3x to 1080 lines", 960, 720, 1440, 1080 },
            { sl::DLSSMode::eMaxQuality, "Quality, 4x to 1600 lines", 1280, 960, 2133, 1600 },
            { sl::DLSSMode::eMaxPerformance, "Performance, 4x to 1600 lines", 1280, 960, 2133, 1600 },
            { sl::DLSSMode::eDLAA, "DLAA at 4x", 1280, 960, 1280, 960 },
            { sl::DLSSMode::eDLAA, "DLAA at 6x", 1920, 1440, 1920, 1440 },
            { sl::DLSSMode::eUltraPerformance, "Ultra Performance, 2x to 1440", 640, 480, 1920, 1440 },
        };
        for (const auto& run : runs)
            printf("  %-34s %dx%d to %dx%d  %.2f\n", run.name, run.in_w, run.in_h, run.out_w,
                   run.out_h,
                   MeasureCost(streamline, gpu, run.mode, run.in_w, run.in_h, run.out_w, run.out_h));
    }

    void RunJitterTest(psxemu::Streamline& streamline,
                       const std::vector<psxemu::GraphicsAdapter>& adapters, bool cost) {
        for (const psxemu::GraphicsAdapter& adapter : adapters) {
            std::string why;
            if (!streamline.SupportsDlss(adapter.luid, &why))
                continue;
            ComPtr<IDXGIAdapter1> card = psxemu::OpenAdapter(adapter.luid);
            ComPtr<ID3D12Device> device;
            if (FAILED(D3D12CreateDevice(card.Get(), D3D_FEATURE_LEVEL_11_0,
                                         IID_PPV_ARGS(&device)))) {
                printf("\nNo Direct3D 12 device on %s\n", adapter.name.c_str());
                return;
            }
            std::string error;
            TestGpu gpu;
            if (!gpu.Make(device.Get()) || !streamline.SetDevice(device.Get(), &error)) {
                printf("\n%s\n", error.empty() ? "the test's device would not start" : error.c_str());
                return;
            }
            printf("\nThe signs DLSS means, on %s: the last of 48 frames against the pattern\n"
                   "(mean difference, 0-1; lower is better)\n", adapter.name.c_str());
            // The renderer gives DLSS the picture's jitter negated (graphics/d3d12_dlss.cpp), which
            // is what this settled on 2026-09-30.
            const struct { float x, y; const char* name; } signs[] = {
                { 1, 1, "jitter as the picture's (+x, +y)" },
                { -1, -1, "jitter negated (-x, -y)" },
                { 1, -1, "jitter (+x, -y)" },
                { -1, 1, "jitter (-x, +y)" },
            };
            float best = 1e9f;
            float best_x = 1, best_y = 1;
            for (const auto& sign : signs) {
                const float score = JitterRun(streamline, gpu, sign.x, sign.y, 0, 0, 1);
                printf("  still, %-36s %.4f\n", sign.name, score);
                if (score >= 0 && score < best) {
                    best = score;
                    best_x = sign.x;
                    best_y = sign.y;
                }
            }
            // Moving 0.37 and 0.23 input pixels a frame, with the best jitter.
            printf("  moving, motion as where it was minus where it is  %.4f\n",
                   JitterRun(streamline, gpu, best_x, best_y, 0.37f, 0.23f, 1));
            printf("  moving, motion negated                            %.4f\n",
                   JitterRun(streamline, gpu, best_x, best_y, 0.37f, 0.23f, -1));
            printf("  moving, no motion given                           %.4f\n",
                   JitterRun(streamline, gpu, best_x, best_y, 0.37f, 0.23f, 0));
            streamline.slFreeResources(sl::kFeatureDLSS, sl::ViewportHandle(0u));
            if (cost)
                RunCostTest(streamline, gpu, adapter.name.c_str());
            // Streamline goes before the device it was given.
            streamline.Stop();
            return;
        }
        printf("\nNo card here runs DLSS, so no test.\n");
    }

}   // namespace

int wmain(int argc, wchar_t** argv) {
    std::wstring folder = argc > 1 && argv[1][0] != L'-'
                              ? argv[1]
                              : psxemu::ExecutableFolder() + L"\\..\\streamline\\v2.14.1\\bin\\x64";
    // The signature check wants a full path, not a relative one.
    wchar_t full[MAX_PATH] = {};
    if (GetFullPathNameW(folder.c_str(), MAX_PATH, full, nullptr) != 0)
        folder = full;
    printf("Streamline from %ls\n", folder.c_str());

    const std::vector<psxemu::GraphicsAdapter> adapters = psxemu::EnumerateGraphicsAdapters();
    psxemu::Streamline streamline;
    std::string error;
    const bool started = streamline.Start(folder, true, &error);
    if (!started) {
        printf("Not started: %s\n", error.c_str());
        if (!streamline.last_message().empty())
            printf("Streamline said: %s", streamline.last_message().c_str());
    } else {
        printf("Started. DLSS %s, Frame Generation %s\n", streamline.Version(sl::kFeatureDLSS).c_str(),
               streamline.Version(sl::kFeatureDLSS_G).c_str());
    }

    for (const psxemu::GraphicsAdapter& adapter : adapters) {
        printf("\n%s\n", adapter.name.c_str());
        printf("    %-20s %s\n", "LUID", psxemu::CounterLuid(adapter.luid).c_str());
        printf("    %-20s %llu MB\n", "video memory",
               static_cast<unsigned long long>(adapter.video_memory >> 20));
        printf("    %-20s %s\n", "GPU scheduling", GpuScheduling(adapter.luid));
        if (started) {
            PrintSupport(streamline, "DLSS", sl::kFeatureDLSS, adapter.luid);
            PrintSupport(streamline, "Frame Generation", sl::kFeatureDLSS_G, adapter.luid);
            PrintSupport(streamline, "Reflex", sl::kFeatureReflex, adapter.luid);
        }
    }

    if (started) {
        printf("\n");
        PrintRequirements(streamline, "DLSS", sl::kFeatureDLSS);
        PrintRequirements(streamline, "Frame Generation", sl::kFeatureDLSS_G);
    }

    // With --optimal, a device on the first card DLSS runs on, and the input sizes DLSS takes
    // for each mode at a few screen sizes - which is what the emulator's choice of scale rests
    // on. One device, made once.
    bool optimal = false, jitter = false, cost = false;
    for (int i = 1; i < argc; ++i) {
        optimal = optimal || wcscmp(argv[i], L"--optimal") == 0;
        jitter = jitter || wcscmp(argv[i], L"--jitter-test") == 0;
        cost = cost || wcscmp(argv[i], L"--cost") == 0;
    }
    if (started && optimal)
        PrintOptimalSettings(streamline, adapters);
    if (started && (jitter || cost) && streamline.started())
        RunJitterTest(streamline, adapters, cost);

    if (started && !streamline.last_message().empty())
        printf("Streamline's last warning: %s", streamline.last_message().c_str());
    streamline.Stop();
    return started ? 0 : 1;
}
