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
#include "graphics/upscale/texture/rtx_video.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

// Defined to 1 by a build that found the SDK in Temp\rtxvideo\ (build_tools.bat).
#ifndef PSXEMU_RTX_VIDEO
#define PSXEMU_RTX_VIDEO 0
#endif

#if PSXEMU_RTX_VIDEO
// NVIDIA's own headers, from the SDK unpacked into Temp\rtxvideo\ (rtx_video.h says why they are
// not committed).
#pragma warning(push)
#pragma warning(disable : 4100)   // NVIDIA's helpers leave a parameter unused
#include "nvsdk_ngx_helpers_vsr.h"
#pragma warning(pop)
#pragma comment(lib, "nvsdk_ngx_d.lib")
#pragma comment(lib, "advapi32.lib")   // NGX reads the registry for the driver's paths
#endif

namespace psxemu {

    bool RtxVideoUpscaler::Built() { return PSXEMU_RTX_VIDEO != 0; }

#if !PSXEMU_RTX_VIDEO

    struct RtxVideoUpscaler::State {};

    std::unique_ptr<RtxVideoUpscaler> RtxVideoUpscaler::Create(ID3D12Device*, const std::wstring&,
                                                               int, int, std::string* error) {
        *error = "this build has no RTX Video: the SDK was not in Temp\\rtxvideo when it was built";
        return nullptr;
    }
    RtxVideoUpscaler::~RtxVideoUpscaler() = default;
    std::string RtxVideoUpscaler::description() const { return "RTX Video, not built"; }
    bool RtxVideoUpscaler::Run(const std::vector<float>&, int, int, std::vector<float>*, int*,
                               int*, std::string* error) {
        *error = "this build has no RTX Video";
        return false;
    }

#else

    namespace {

        using Microsoft::WRL::ComPtr;

        std::string Text(NVSDK_NGX_Result result) {
            const wchar_t* wide = GetNGXResultAsString(result);
            std::string text;
            for (const wchar_t* c = wide; c != nullptr && *c; ++c)
                text += *c < 128 ? static_cast<char>(*c) : '?';
            char code[16];
            snprintf(code, sizeof(code), " (%08x)", static_cast<unsigned>(result));
            return text + code;
        }

    }   // namespace

    // Everything on the card: NGX's parameters and feature, a direct queue and list of our own,
    // and the four resources a picture goes through, made again when its size changes.
    struct RtxVideoUpscaler::State {
        ComPtr<ID3D12Device> device;
        ComPtr<ID3D12CommandQueue> queue;
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        ComPtr<ID3D12Fence> fence;
        UINT64 fence_value = 0;
        HANDLE event = nullptr;
        NVSDK_NGX_Parameter* parameters = nullptr;
        NVSDK_NGX_Handle* feature = nullptr;
        bool initialised = false;

        int in_width = 0, in_height = 0, out_width = 0, out_height = 0;
        ComPtr<ID3D12Resource> input, output, upload, readback;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT in_layout = {}, out_layout = {};

        // The list run to completion; the allocator and list ready for the next.
        bool Execute() {
            if (FAILED(list->Close()))
                return false;
            ID3D12CommandList* lists[] = { list.Get() };
            queue->ExecuteCommandLists(1, lists);
            queue->Signal(fence.Get(), ++fence_value);
            fence->SetEventOnCompletion(fence_value, event);
            WaitForSingleObject(event, INFINITE);
            return SUCCEEDED(allocator->Reset()) && SUCCEEDED(list->Reset(allocator.Get(), nullptr));
        }

        ComPtr<ID3D12Resource> Texture(int width, int height, D3D12_RESOURCE_FLAGS flags) {
            D3D12_HEAP_PROPERTIES heap = {};
            heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC desc = {};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width = static_cast<UINT64>(width);
            desc.Height = static_cast<UINT>(height);
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.Flags = flags;
            // In the common state, as NVIDIA's own samples keep them: NGX moves them as it needs,
            // and a copy promotes one implicitly.
            ComPtr<ID3D12Resource> texture;
            device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                            D3D12_RESOURCE_STATE_COMMON, nullptr,
                                            IID_PPV_ARGS(&texture));
            return texture;
        }

        ComPtr<ID3D12Resource> Buffer(UINT64 bytes, D3D12_HEAP_TYPE type) {
            D3D12_HEAP_PROPERTIES heap = {};
            heap.Type = type;
            D3D12_RESOURCE_DESC desc = {};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Width = bytes;
            desc.Height = 1;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.SampleDesc.Count = 1;
            desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            ComPtr<ID3D12Resource> buffer;
            device->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &desc,
                type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ
                                               : D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr, IID_PPV_ARGS(&buffer));
            return buffer;
        }

        bool Resize(int w, int h, int ow, int oh) {
            if (w == in_width && h == in_height && ow == out_width && oh == out_height)
                return true;
            input = Texture(w, h, D3D12_RESOURCE_FLAG_NONE);
            output = Texture(ow, oh, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            if (!input || !output)
                return false;
            UINT64 in_bytes = 0, out_bytes = 0;
            D3D12_RESOURCE_DESC in_desc = input->GetDesc(), out_desc = output->GetDesc();
            device->GetCopyableFootprints(&in_desc, 0, 1, 0, &in_layout, nullptr, nullptr, &in_bytes);
            device->GetCopyableFootprints(&out_desc, 0, 1, 0, &out_layout, nullptr, nullptr, &out_bytes);
            upload = Buffer(in_bytes, D3D12_HEAP_TYPE_UPLOAD);
            readback = Buffer(out_bytes, D3D12_HEAP_TYPE_READBACK);
            if (!upload || !readback)
                return false;
            in_width = w;
            in_height = h;
            out_width = ow;
            out_height = oh;
            return true;
        }

        ~State() {
            if (feature != nullptr)
                NVSDK_NGX_D3D12_ReleaseFeature(feature);
            if (parameters != nullptr)
                NVSDK_NGX_D3D12_DestroyParameters(parameters);
            if (initialised)
                NVSDK_NGX_D3D12_Shutdown1(device.Get());
            if (event != nullptr)
                CloseHandle(event);
        }
    };

    std::unique_ptr<RtxVideoUpscaler> RtxVideoUpscaler::Create(ID3D12Device* device,
                                                               const std::wstring& folder,
                                                               int scale, int quality,
                                                               std::string* error) {
        if (scale < 1 || scale > 8 || quality < 0 || quality > 4) {
            *error = "RTX Video wants a scale of 1-8 and a quality of 0-4";
            return nullptr;
        }
        // NVIDIA's signature on the feature DLL, before NGX loads it from that folder by name.
        const std::wstring dll = folder + L"\\nvngx_vsr.dll";
        if (GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES) {
            *error = "nvngx_vsr.dll is not beside the emulator";
            return nullptr;
        }
        if (!SignedByOrganisation(dll, "NVIDIA Corporation")) {
            *error = "nvngx_vsr.dll is not signed by NVIDIA";
            return nullptr;
        }

        std::unique_ptr<RtxVideoUpscaler> upscaler(new RtxVideoUpscaler());
        upscaler->scale_ = scale;
        upscaler->quality_ = quality;
        upscaler->state_.reset(new State());
        State& s = *upscaler->state_;
        s.device = device;

        // NGX writes its logs under the data path; a folder of its own in %TEMP%.
        wchar_t temp[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, temp);
        const std::wstring data = std::wstring(temp) + L"psxemu-ngx";
        CreateDirectoryW(data.c_str(), nullptr);
        const wchar_t* paths[] = { folder.c_str() };
        NVSDK_NGX_FeatureCommonInfo info = {};
        info.PathListInfo.Path = paths;
        info.PathListInfo.Length = 1;
        NVSDK_NGX_Result result = NVSDK_NGX_D3D12_Init(0, data.c_str(), device, &info);
        if (NVSDK_NGX_FAILED(result)) {
            *error = "NGX would not start: " + Text(result);
            return nullptr;
        }
        s.initialised = true;
        result = NVSDK_NGX_D3D12_GetCapabilityParameters(&s.parameters);
        if (NVSDK_NGX_FAILED(result)) {
            *error = "NGX gave no parameters: " + Text(result);
            return nullptr;
        }
        int available = 0;
        s.parameters->Get(NVSDK_NGX_Parameter_VSR_Available, &available);
        if (!available) {
            int needs_driver = 0;
            unsigned int major = 0, minor = 0;
            s.parameters->Get(NVSDK_NGX_Parameter_VSR_NeedsUpdatedDriver, &needs_driver);
            s.parameters->Get(NVSDK_NGX_Parameter_VSR_MinDriverVersionMajor, &major);
            s.parameters->Get(NVSDK_NGX_Parameter_VSR_MinDriverVersionMinor, &minor);
            *error = needs_driver ? "RTX Video needs driver " + std::to_string(major) + "." +
                                        std::to_string(minor) + " or later"
                                  : "RTX Video is not available on this card";
            return nullptr;
        }

        D3D12_COMMAND_QUEUE_DESC desc = {};
        desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        s.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (FAILED(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&s.queue))) ||
            FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                  IID_PPV_ARGS(&s.allocator))) ||
            FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s.allocator.Get(),
                                             nullptr, IID_PPV_ARGS(&s.list))) ||
            FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&s.fence))) ||
            s.event == nullptr) {
            *error = "no queue for RTX Video on the card";
            return nullptr;
        }
        NVSDK_NGX_Feature_Create_Params create = {};
        result = NGX_D3D12_CREATE_VSR_EXT(s.list.Get(), 1, 1, &s.feature, s.parameters, &create);
        if (NVSDK_NGX_FAILED(result)) {
            *error = "NGX would not make VSR: " + Text(result);
            return nullptr;
        }
        if (!s.Execute()) {
            *error = "making VSR did not finish";
            return nullptr;
        }
        return upscaler;
    }

    RtxVideoUpscaler::~RtxVideoUpscaler() = default;

    std::string RtxVideoUpscaler::description() const {
        return "RTX Video Super Resolution, quality " + std::to_string(quality_) + ", " +
               std::to_string(scale_) + "x";
    }

    bool RtxVideoUpscaler::Run(const std::vector<float>& rgb, int width, int height,
                               std::vector<float>* out, int* out_width, int* out_height,
                               std::string* error) {
        State& s = *state_;
        const size_t pixels = static_cast<size_t>(width) * height;
        if (rgb.size() != pixels * 3) {
            *error = "the picture is not width x height x 3";
            return false;
        }
        const int ow = width * scale_, oh = height * scale_;
        if (!s.Resize(width, height, ow, oh)) {
            *error = "RTX Video's textures could not be made";
            return false;
        }

        // The picture into the upload buffer as RGBA8 rows, then onto the card.
        uint8_t* mapped = nullptr;
        if (FAILED(s.upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped)))) {
            *error = "the upload buffer would not map";
            return false;
        }
        for (int y = 0; y < height; ++y) {
            uint8_t* row = mapped + s.in_layout.Offset + static_cast<size_t>(y) * s.in_layout.Footprint.RowPitch;
            for (int x = 0; x < width; ++x) {
                for (int c = 0; c < 3; ++c) {
                    const float v = rgb[(static_cast<size_t>(c) * height + y) * width + x];
                    row[x * 4 + c] = static_cast<uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
                }
                row[x * 4 + 3] = 255;
            }
        }
        s.upload->Unmap(0, nullptr);
        D3D12_TEXTURE_COPY_LOCATION to = {}, from = {};
        to.pResource = s.input.Get();
        to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        from.pResource = s.upload.Get();
        from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        from.PlacedFootprint = s.in_layout;
        s.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        if (!s.Execute()) {
            *error = "the upload did not finish";
            return false;
        }

        // VSR, in a list of its own: NGX leaves the list's state as it likes.
        NVSDK_NGX_D3D12_VSR_Eval_Params eval = {};
        eval.pInput = s.input.Get();
        eval.pOutput = s.output.Get();
        eval.InputSubrectSize.Width = static_cast<unsigned>(width);
        eval.InputSubrectSize.Height = static_cast<unsigned>(height);
        eval.OutputSubrectSize.Width = static_cast<unsigned>(ow);
        eval.OutputSubrectSize.Height = static_cast<unsigned>(oh);
        eval.QualityLevel = static_cast<NVSDK_NGX_VSR_QualityLevel>(quality_);
        const NVSDK_NGX_Result result =
            NGX_D3D12_EVALUATE_VSR_EXT(s.list.Get(), s.feature, s.parameters, &eval);
        if (NVSDK_NGX_FAILED(result)) {
            *error = "VSR failed: " + Text(result);
            s.list->Close();
            s.allocator->Reset();
            s.list->Reset(s.allocator.Get(), nullptr);
            return false;
        }
        if (!s.Execute()) {
            *error = "VSR did not finish";
            return false;
        }

        // Back off the card.
        to.pResource = s.readback.Get();
        to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        to.PlacedFootprint = s.out_layout;
        from.pResource = s.output.Get();
        from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        from.SubresourceIndex = 0;
        s.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        if (!s.Execute()) {
            *error = "the read back did not finish";
            return false;
        }
        const uint8_t* back = nullptr;
        if (FAILED(s.readback->Map(0, nullptr, reinterpret_cast<void**>(const_cast<uint8_t**>(&back))))) {
            *error = "the read-back buffer would not map";
            return false;
        }
        out->resize(static_cast<size_t>(ow) * oh * 3);
        for (int y = 0; y < oh; ++y) {
            const uint8_t* row = back + s.out_layout.Offset + static_cast<size_t>(y) * s.out_layout.Footprint.RowPitch;
            for (int x = 0; x < ow; ++x)
                for (int c = 0; c < 3; ++c)
                    (*out)[(static_cast<size_t>(c) * oh + y) * ow + x] = row[x * 4 + c] / 255.0f;
        }
        s.readback->Unmap(0, nullptr);
        *out_width = ow;
        *out_height = oh;
        return true;
    }

#endif

}   // namespace psxemu
