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

// ffx_probe: what AMD's FidelityFX runtime says about the graphics cards here (Docs/FSR-Plan.md,
// phase 0).
//
//   ffx_probe [folder] [--card n] [--all] [--jitter-test] [--cost] [--version name]
//
// Loads AMD's DLLs from `folder` - by default its own, where build_tools.bat copies the fetched
// SDK's - the way the emulator does (each signature checked), and for each card makes one
// Direct3D 12 device and
// prints the versions of FSR Upscaling and FSR Frame Generation AMD offers on it, best first. The
// NVIDIA card only with --all or --card: a device made on it is one more on a card that has not
// liked being cycled (the 0x9F of 2026-09-29), so it is asked about only when wanted.
//
// --jitter-test settles the two signs the emulator gives FSR, on the first card asked about (or
// --card n): frames of a sharp-edged pattern, each pixel showing what is at itself plus the jitter
// - the rasteriser's own rule (psx/shared_picture.h) - go through FSR Quality for 48 frames with
// each sign of the jitter each way, then with the pattern moving and each sign of its motion, each
// output scored against the pattern drawn at the output's size. --cost does that too, then times
// FSR on the card at the sizes the emulator gives it. --version picks the upscaler by AMD's name
// for it ("3.1.5", "4.1.1"); the default is AMD's own choice for the card.
//
// Exits 0 when AMD's DLLs loaded, 1 when they did not.

#include "graphics/adapters.h"
#include "graphics/d3dx12.h"
#include "graphics/fsr/fidelityfx.h"
#include "graphics/dlss/streamline.h"   // ExecutableFolder

#include <d3d12.h>
#include <DirectXPackedVector.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

namespace {

    using Microsoft::WRL::ComPtr;
    using psxemu::FidelityFx;

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

    // The emulator's context flags (graphics/d3d12_fsr.cpp): depth is 1/z, nearer larger, with
    // no far plane, and the picture is the console's own colours, gamma and all.
    const uint32_t kContextFlags = FFX_UPSCALE_ENABLE_DEPTH_INVERTED |
                                   FFX_UPSCALE_ENABLE_DEPTH_INFINITE |
                                   FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE;

    // An upscaler context for these sizes, of version `version_id` (0: AMD's own choice).
    bool MakeUpscaler(ID3D12Device* device, uint64_t version_id, int in_w, int in_h, int out_w,
                      int out_h, ffxContext* context) {
        ffxCreateBackendDX12Desc backend = {};
        backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
        backend.device = device;
        ffxOverrideVersion override_version = {};
        override_version.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
        override_version.versionId = version_id;
        ffxCreateContextDescUpscaleVersion api = {};
        api.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
        api.version = FFX_UPSCALER_VERSION;
        ffxCreateContextDescUpscale create = {};
        create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        create.flags = kContextFlags;
        create.maxRenderSize = { static_cast<uint32_t>(in_w), static_cast<uint32_t>(in_h) };
        create.maxUpscaleSize = { static_cast<uint32_t>(out_w), static_cast<uint32_t>(out_h) };
        create.header.pNext = &api.header;
        api.header.pNext = &backend.header;
        if (version_id != 0)
            backend.header.pNext = &override_version.header;
        *context = nullptr;
        const ffxReturnCode_t made = FidelityFx::Get().CreateContext(context, &create.header);
        if (made != FFX_API_RETURN_OK) {
            printf("  the upscaler would not start: %s\n", psxemu::FfxResultText(made));
            return false;
        }
        // Which one AMD made, said once.
        static bool said = false;
        if (!said) {
            said = true;
            ffxQueryGetProviderVersion provider = {};
            provider.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
            if (FidelityFx::Get().Query(context, &provider.header) == FFX_API_RETURN_OK)
                printf("  running upscaler %s\n",
                       provider.versionName != nullptr ? provider.versionName : "(unnamed)");
        }
        return true;
    }

    // One dispatch of the upscaler, as the emulator gives it.
    ffxReturnCode_t Upscale(ffxContext* context, ID3D12GraphicsCommandList* list,
                            ID3D12Resource* colour, ID3D12Resource* depth, ID3D12Resource* motion,
                            ID3D12Resource* reactive, ID3D12Resource* output, int in_w, int in_h,
                            int out_w, int out_h, float jx, float jy, bool reset) {
        ffxDispatchDescUpscale dispatch = {};
        dispatch.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
        dispatch.commandList = list;
        dispatch.color = psxemu::FfxResource(colour, FFX_API_RESOURCE_STATE_PIXEL_READ);
        dispatch.depth = psxemu::FfxResource(depth, FFX_API_RESOURCE_STATE_PIXEL_READ);
        dispatch.motionVectors = psxemu::FfxResource(motion, FFX_API_RESOURCE_STATE_PIXEL_READ);
        if (reactive != nullptr)
            dispatch.reactive = psxemu::FfxResource(reactive, FFX_API_RESOURCE_STATE_PIXEL_READ);
        dispatch.output = psxemu::FfxResource(output, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
        dispatch.jitterOffset = { jx, jy };
        dispatch.motionVectorScale = { 1.0f, 1.0f };
        dispatch.renderSize = { static_cast<uint32_t>(in_w), static_cast<uint32_t>(in_h) };
        dispatch.upscaleSize = { static_cast<uint32_t>(out_w), static_cast<uint32_t>(out_h) };
        dispatch.enableSharpening = false;
        dispatch.frameTimeDelta = 16.7f;
        dispatch.preExposure = 1.0f;
        dispatch.reset = reset;
        dispatch.cameraNear = 1.0f;
        dispatch.cameraFar = 65536.0f;
        dispatch.cameraFovAngleVertical = 1.0f;
        dispatch.viewSpaceToMetersFactor = 0.01f;
        dispatch.flags = FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB;
        return FidelityFx::Get().Dispatch(context, &dispatch.header);
    }

    // 48 frames of the pattern through FSR Quality, input 192x144 to 288x216, the jitter given to
    // FSR times (sign_x, sign_y) and the motion given times `motion_sign`, the pattern moving
    // `vx, vy` input pixels a frame. The mean difference of the last output from the pattern
    // drawn at the output's size, 0-1; negative if anything failed.
    float JitterRun(TestGpu& gpu, uint64_t version_id, float sign_x, float sign_y, float vx,
                    float vy, float motion_sign) {
        const int kIn = 192, kInH = 144, kOut = 288, kOutH = 216, kFrames = 48;
        const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        ComPtr<ID3D12Resource> colour = gpu.Texture(kIn, kInH, DXGI_FORMAT_R8G8B8A8_UNORM,
                                                    D3D12_RESOURCE_FLAG_NONE, read);
        ComPtr<ID3D12Resource> motion = gpu.Texture(kIn, kInH, DXGI_FORMAT_R16G16_FLOAT,
                                                    D3D12_RESOURCE_FLAG_NONE, read);
        ComPtr<ID3D12Resource> depth = gpu.Texture(kIn, kInH, DXGI_FORMAT_R32_FLOAT,
                                                   D3D12_RESOURCE_FLAG_NONE, read);
        ComPtr<ID3D12Resource> output = gpu.Texture(kOut, kOutH, DXGI_FORMAT_R8G8B8A8_UNORM,
                                                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        ComPtr<ID3D12Resource> colour_up = gpu.Buffer(kIn * kInH * 4 + 65536, D3D12_HEAP_TYPE_UPLOAD);
        ComPtr<ID3D12Resource> motion_up = gpu.Buffer(kIn * kInH * 4 + 65536, D3D12_HEAP_TYPE_UPLOAD);
        ComPtr<ID3D12Resource> depth_up = gpu.Buffer(kIn * kInH * 4 + 65536, D3D12_HEAP_TYPE_UPLOAD);
        ComPtr<ID3D12Resource> readback = gpu.Buffer(kOut * kOutH * 4 + 65536,
                                                     D3D12_HEAP_TYPE_READBACK);
        if (!colour || !motion || !depth || !output || !colour_up || !readback)
            return -1.0f;

        std::vector<float> depth_rows(kIn * kInH, 1.0f / 500.0f);   // 1/z, all at z 500
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

        ffxContext context = nullptr;
        if (!MakeUpscaler(gpu.device.Get(), version_id, kIn, kInH, kOut, kOutH, &context))
            return -1.0f;
        std::vector<uint8_t> colour_rows(kIn * kInH * 4);
        float shift_x = 0.0f, shift_y = 0.0f;
        bool ok = true;
        for (int frame = 0; frame < kFrames && ok; ++frame) {
            shift_x = vx * frame;
            shift_y = vy * frame;
            const float jx = Halton(frame + 1, 2) - 0.5f, jy = Halton(frame + 1, 3) - 0.5f;
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
            const ffxReturnCode_t done =
                Upscale(&context, gpu.list.Get(), colour.Get(), depth.Get(), motion.Get(), nullptr,
                        output.Get(), kIn, kInH, kOut, kOutH, jx * sign_x, jy * sign_y, frame == 0);
            if (done != FFX_API_RETURN_OK) {
                printf("  dispatch failed: %s\n", psxemu::FfxResultText(done));
                ok = false;
            }
            gpu.Run();
        }
        FidelityFx::Get().DestroyContext(&context);
        if (!ok)
            return -1.0f;

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
                const float got =
                    mapped[footprint.Offset + y * footprint.Footprint.RowPitch + x * 4] / 255.0f;
                total += std::fabs(got - truth);
                ++count;
            }
        }
        readback->Unmap(0, nullptr);
        return static_cast<float>(total / count);
    }

    // FSR's own time on the card for one picture, in milliseconds, at these sizes: timestamps
    // either side of it, the median of 30 after 10 to warm up. Negative if it failed.
    double MeasureCost(TestGpu& gpu, uint64_t version_id, int in_w, int in_h, int out_w,
                       int out_h) {
        const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        ComPtr<ID3D12Resource> colour = gpu.Texture(in_w, in_h, DXGI_FORMAT_B8G8R8A8_UNORM,
                                                    D3D12_RESOURCE_FLAG_NONE, read);
        ComPtr<ID3D12Resource> motion = gpu.Texture(in_w, in_h, DXGI_FORMAT_R16G16_FLOAT,
                                                    D3D12_RESOURCE_FLAG_NONE, read);
        ComPtr<ID3D12Resource> depth = gpu.Texture(in_w, in_h, DXGI_FORMAT_R32_FLOAT,
                                                   D3D12_RESOURCE_FLAG_NONE, read);
        ComPtr<ID3D12Resource> reactive = gpu.Texture(in_w, in_h, DXGI_FORMAT_R8_UNORM,
                                                      D3D12_RESOURCE_FLAG_NONE, read);
        ComPtr<ID3D12Resource> output = gpu.Texture(out_w, out_h, DXGI_FORMAT_R8G8B8A8_UNORM,
                                                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                                    read);
        ComPtr<ID3D12Resource> times = gpu.Buffer(16, D3D12_HEAP_TYPE_READBACK);
        ComPtr<ID3D12QueryHeap> queries;
        D3D12_QUERY_HEAP_DESC query_desc = {};
        query_desc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        query_desc.Count = 2;
        if (!colour || !motion || !depth || !reactive || !output || !times ||
            FAILED(gpu.device->CreateQueryHeap(&query_desc, IID_PPV_ARGS(&queries))))
            return -1.0;
        UINT64 frequency = 0;
        gpu.queue->GetTimestampFrequency(&frequency);
        ffxContext context = nullptr;
        if (!MakeUpscaler(gpu.device.Get(), version_id, in_w, in_h, out_w, out_h, &context))
            return -1.0;
        std::vector<double> samples;
        for (int frame = 0; frame < 40; ++frame) {
            gpu.list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
            ffxReturnCode_t done = Upscale(&context, gpu.list.Get(), colour.Get(), depth.Get(),
                                           motion.Get(), reactive.Get(), output.Get(), in_w, in_h,
                                           out_w, out_h, Halton(frame + 1, 2) - 0.5f,
                                           Halton(frame + 1, 3) - 0.5f, frame == 0);
            gpu.list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
            gpu.list->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2,
                                       times.Get(), 0);
            gpu.Run();
            if (done != FFX_API_RETURN_OK) {
                FidelityFx::Get().DestroyContext(&context);
                return -1.0;
            }
            UINT64* stamps = nullptr;
            times->Map(0, nullptr, reinterpret_cast<void**>(&stamps));
            if (frame >= 10)
                samples.push_back(static_cast<double>(stamps[1] - stamps[0]) * 1000.0 /
                                  static_cast<double>(frequency));
            times->Unmap(0, nullptr);
        }
        FidelityFx::Get().DestroyContext(&context);
        std::sort(samples.begin(), samples.end());
        return samples[samples.size() / 2];
    }

    void RunTests(ID3D12Device* device, const char* card, uint64_t version_id, bool cost) {
        TestGpu gpu;
        if (!gpu.Make(device)) {
            printf("\nthe test's device would not start\n");
            return;
        }
        printf("\nThe signs FSR means, on %s: the last of 48 frames against the pattern\n"
               "(mean difference, 0-1; lower is better)\n", card);
        const struct { float x, y; const char* name; } signs[] = {
            { 1, 1, "jitter as the picture's (+x, +y)" },
            { -1, -1, "jitter negated (-x, -y)" },
            { 1, -1, "jitter (+x, -y)" },
            { -1, 1, "jitter (-x, +y)" },
        };
        float best = 1e9f;
        float best_x = 1, best_y = 1;
        for (const auto& sign : signs) {
            const float score = JitterRun(gpu, version_id, sign.x, sign.y, 0, 0, 1);
            printf("  still, %-36s %.4f\n", sign.name, score);
            if (score >= 0 && score < best) {
                best = score;
                best_x = sign.x;
                best_y = sign.y;
            }
        }
        printf("  still, no jitter at all (the input unjittered)    %.4f\n",
               JitterRun(gpu, version_id, 0, 0, 0, 0, 1));
        printf("  moving, motion as where it was minus where it is  %.4f\n",
               JitterRun(gpu, version_id, best_x, best_y, 0.37f, 0.23f, 1));
        printf("  moving, motion negated                            %.4f\n",
               JitterRun(gpu, version_id, best_x, best_y, 0.37f, 0.23f, -1));
        printf("  moving, no motion given                           %.4f\n",
               JitterRun(gpu, version_id, best_x, best_y, 0.37f, 0.23f, 0));
        if (!cost)
            return;
        printf("\nFSR's time on %s for one picture (median of 30, ms):\n", card);
        const struct { const char* name; int in_w, in_h, out_w, out_h; } runs[] = {
            { "Quality, 2x to a 709-line window", 640, 480, 945, 709 },
            { "Quality, 3x to 1080 lines", 960, 720, 1440, 1080 },
            { "Quality, 4x to 1600 lines", 1280, 960, 2133, 1600 },
            { "Performance, 3x to 1600 lines", 960, 720, 2133, 1600 },
            { "Native AA at 4x", 1280, 960, 1280, 960 },
            { "Native AA at 6x", 1920, 1440, 1920, 1440 },
            { "Ultra Performance, 2x to 1440", 640, 480, 1920, 1440 },
            // 480 lines at a scale chosen for 240: larger than the screen.
            { "480 lines at 3x, onto 960 lines", 1920, 1440, 1280, 960 },
            { "480 lines at 3x, at its own size", 1920, 1440, 1920, 1440 },
        };
        for (const auto& run : runs)
            printf("  %-34s %dx%d to %dx%d  %.2f\n", run.name, run.in_w, run.in_h, run.out_w,
                   run.out_h, MeasureCost(gpu, version_id, run.in_w, run.in_h, run.out_w, run.out_h));
    }

    void PrintVersions(const char* what, const std::vector<FidelityFx::Version>& versions) {
        printf("    %-20s ", what);
        if (versions.empty())
            printf("none");
        for (size_t i = 0; i < versions.size(); ++i)
            printf("%s%s", i == 0 ? "" : ", ", versions[i].name.c_str());
        printf("\n");
    }

}   // namespace

int wmain(int argc, wchar_t** argv) {
    // AMD's loader finds the upscaler and Frame Generation only beside the executable, not beside
    // itself: build_tools.bat copies them into Temp\tools.
    std::wstring folder = argc > 1 && argv[1][0] != L'-' ? argv[1] : psxemu::ExecutableFolder();
    wchar_t full[MAX_PATH] = {};
    if (GetFullPathNameW(folder.c_str(), MAX_PATH, full, nullptr) != 0)
        folder = full;
    printf("AMD FidelityFX from %ls\n", folder.c_str());

    int card = -1;
    bool all = false, jitter = false, cost = false;
    std::string version_name;
    for (int i = 1; i < argc; ++i) {
        if (wcscmp(argv[i], L"--card") == 0 && i + 1 < argc)
            card = _wtoi(argv[++i]);
        else if (wcscmp(argv[i], L"--version") == 0 && i + 1 < argc) {
            for (const wchar_t* c = argv[++i]; *c; ++c)
                version_name += static_cast<char>(*c);
        }
        all = all || wcscmp(argv[i], L"--all") == 0;
        jitter = jitter || wcscmp(argv[i], L"--jitter-test") == 0;
        cost = cost || wcscmp(argv[i], L"--cost") == 0;
    }

    std::string error;
    if (!FidelityFx::Get().Load(folder, &error)) {
        printf("Not loaded: %s\n", error.c_str());
        return 1;
    }
    printf("Loaded, every DLL signed by AMD. Frame Generation's DLL %s.\n",
           FidelityFx::Get().has_frame_generation() ? "there" : "missing");

    const std::vector<psxemu::GraphicsAdapter> adapters = psxemu::EnumerateGraphicsAdapters();
    bool tested = false;
    for (size_t i = 0; i < adapters.size(); ++i) {
        const psxemu::GraphicsAdapter& adapter = adapters[i];
        printf("\n[%zu] %s\n", i, adapter.name.c_str());
        const bool nvidia = adapter.name.find("NVIDIA") != std::string::npos;
        const bool asked = card >= 0 ? static_cast<int>(i) == card : (all || !nvidia);
        if (!asked) {
            printf("    not asked about (--all, or --card %zu)\n", i);
            continue;
        }
        ComPtr<IDXGIAdapter1> dxgi = psxemu::OpenAdapter(adapter.luid);
        ComPtr<ID3D12Device> device;
        if (!dxgi || FAILED(D3D12CreateDevice(dxgi.Get(), D3D_FEATURE_LEVEL_11_0,
                                              IID_PPV_ARGS(&device)))) {
            printf("    no Direct3D 12 device\n");
            continue;
        }
        const auto upscalers =
            FidelityFx::Get().Versions(FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE, device.Get());
        PrintVersions("FSR Upscaling", upscalers);
        PrintVersions("FSR Frame Generation",
                      FidelityFx::Get().Versions(FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION,
                                                 device.Get()));
        PrintVersions("Frame Gen swap chain",
                      FidelityFx::Get().Versions(
                          FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12,
                          device.Get()));
        if ((jitter || cost) && !tested) {
            tested = true;
            uint64_t version_id = 0;
            for (const FidelityFx::Version& version : upscalers)
                if (!version_name.empty() && version.name.rfind(version_name, 0) == 0)
                    version_id = version.id;
            if (!version_name.empty() && version_id == 0)
                printf("    no version %s here; AMD's own choice instead\n", version_name.c_str());
            RunTests(device.Get(), adapter.name.c_str(), version_id, cost);
        }
    }
    return 0;
}
