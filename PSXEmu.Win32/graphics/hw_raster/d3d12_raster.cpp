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
#include "graphics/hw_raster/d3d12_raster.h"

#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <mutex>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxgi.lib")

using emulation::psx::DrawJob;
using emulation::psx::RasterState;
using emulation::psx::RasterVertex;
using Microsoft::WRL::ComPtr;

namespace psxemu {

    // What both hardware rasterisers draw with (raster_common.h).
    using namespace raster;

    namespace {

        // Upload memory comes in pages this big, or as big as one thing needs.
        constexpr size_t kUploadPage = 8u << 20;

        inline size_t Align(size_t value, size_t alignment) {
            return (value + alignment - 1) & ~(alignment - 1);
        }

        // Pixel rows in a read-back buffer start 256 bytes apart at least.
        inline UINT RowPitch(UINT width, UINT bytes_per_pixel) {
            return static_cast<UINT>(Align(static_cast<size_t>(width) * bytes_per_pixel,
                                           D3D12_TEXTURE_DATA_PITCH_ALIGNMENT));
        }

        uint64_t LuidBits(const LUID& luid) {
            return (static_cast<uint64_t>(static_cast<uint32_t>(luid.HighPart)) << 32) |
                   luid.LowPart;
        }

        LUID LuidFrom(uint64_t packed) {
            LUID luid = {};
            luid.LowPart = static_cast<DWORD>(packed & 0xFFFFFFFFu);
            luid.HighPart = static_cast<LONG>(packed >> 32);
            return luid;
        }

    }   // namespace

    // Shared pictures, the part any thread may touch (psx/shared_picture.h) - D3D11's
    // CardPictures, with a Direct3D 12 fence: each texture's NT handle and bookkeeping, and the
    // fence the rasteriser's queue signals once it has drawn a picture. Frames carry a reference
    // to this, so it outlives the rasteriser while any are in flight - and with it the handles,
    // which keep the textures themselves alive after the rasteriser has let go of them. On the
    // renderer's own device there are no handles: the textures themselves are kept here instead.
    class D3D12Pictures : public emulation::psx::SharedPictureSource {
     public:
        static constexpr int kSlots = HardwareRaster::kSharedPictures;

        struct Slot {
            HANDLE handle = nullptr;
            ComPtr<ID3D12Resource> resource;   // on the renderer's device, in place of `handle`
            UINT width = 0, height = 0;
            uint64_t bytes = 0;   // the texture's allocation, for OpenGL
            uint64_t id = 0;
            HANDLE planes = nullptr;
            ComPtr<ID3D12Resource> planes_resource;
            uint64_t planes_id = 0;
            std::atomic<uint64_t> serial{ 0 };
            std::atomic<uint64_t> dropped{ 0 };

            bool empty() const { return handle == nullptr && resource == nullptr; }
            // What a picture carries: the handle, or the resource.
            void* texture() const { return resource ? static_cast<void*>(resource.Get()) : handle; }
            void* planes_texture() const {
                return planes_resource ? static_cast<void*>(planes_resource.Get()) : planes;
            }
        };

        // `device`: the renderer's, when the textures are made on it and handed over as they
        // are; null when they go by handle.
        D3D12Pictures(ComPtr<ID3D12Fence> fence, uint64_t adapter, ID3D12Device* device)
            : fence_(std::move(fence)), adapter_(adapter), device_(device),
              event_(CreateEventW(nullptr, FALSE, FALSE, nullptr)) {}

        ~D3D12Pictures() override {
            for (Slot& slot : slots_) {
                if (slot.handle != nullptr)
                    CloseHandle(slot.handle);
                if (slot.planes != nullptr)
                    CloseHandle(slot.planes);
            }
            if (event_ != nullptr)
                CloseHandle(event_);
        }

        bool WaitReady(uint64_t serial, uint32_t timeout_ms) override {
            std::lock_guard<std::mutex> lock(wait_mutex_);
            const ULONGLONG deadline = GetTickCount64() + timeout_ms;
            for (;;) {
                // A card that has gone reads as every value reached: the picture is then
                // garbage for a frame, and the rasteriser is replaced by the next.
                if (fence_->GetCompletedValue() >= serial)
                    return true;
                const ULONGLONG now = GetTickCount64();
                if (now >= deadline || event_ == nullptr ||
                    FAILED(fence_->SetEventOnCompletion(serial, event_)))
                    return false;
                WaitForSingleObject(event_, static_cast<DWORD>(deadline - now));
            }
        }

        void Dropped(uint64_t serial) override {
            for (Slot& slot : slots_)
                if (slot.serial.load(std::memory_order_acquire) == serial)
                    slot.dropped.store(serial, std::memory_order_release);
        }

        uint64_t adapter() const override { return adapter_; }
        bool d3d12() const override { return true; }
        void* device() const override { return device_.Get(); }
        void* device_fence() const override { return fence_.Get(); }

        bool Free(int index) const {
            const uint64_t serial = slots_[index].serial.load(std::memory_order_acquire);
            return serial == 0 || serial < released() ||
                   slots_[index].dropped.load(std::memory_order_acquire) == serial;
        }
        int Pick(UINT width, UINT height) const {
            int any = -1;
            for (int i = 0; i < kSlots; ++i) {
                if (!Free(i))
                    continue;
                if (slots_[i].width == width && slots_[i].height == height)
                    return i;
                if (any < 0 || slots_[i].empty())
                    any = i;
            }
            return any;
        }
        // A slot's texture made again: by its handle, or - on the renderer's device - itself.
        void Replace(int index, HANDLE handle, ID3D12Resource* resource, UINT width, UINT height,
                     uint64_t bytes) {
            Slot& slot = slots_[index];
            if (slot.handle != nullptr)
                CloseHandle(slot.handle);
            slot.handle = handle;
            slot.resource = resource;
            slot.width = width;
            slot.height = height;
            slot.bytes = bytes;
            slot.id = next_texture_id.fetch_add(1, std::memory_order_relaxed);
            ReplacePlanes(index, nullptr, nullptr);
        }
        void ReplacePlanes(int index, HANDLE handle, ID3D12Resource* resource) {
            Slot& slot = slots_[index];
            if (slot.planes != nullptr)
                CloseHandle(slot.planes);
            slot.planes = handle;
            slot.planes_resource = resource;
            slot.planes_id = handle != nullptr || resource != nullptr
                                 ? next_texture_id.fetch_add(1, std::memory_order_relaxed)
                                 : 0;
        }
        void Drawn(int index, uint64_t serial) {
            slots_[index].dropped.store(0, std::memory_order_relaxed);
            slots_[index].serial.store(serial, std::memory_order_release);
        }
        const Slot& slot(int index) const { return slots_[index]; }
        ID3D12Fence* fence() const { return fence_.Get(); }

     private:
        Slot slots_[kSlots];
        ComPtr<ID3D12Fence> fence_;
        const uint64_t adapter_;
        const ComPtr<ID3D12Device> device_;
        HANDLE event_;
        std::mutex wait_mutex_;
    };

    D3D12Raster::~D3D12Raster() {
        // Nothing may be released while the card still reads it.
        if (queue_ && fence_) {
            Submit();
            WaitFor(fence_value_);
        }
        if (fence_event_ != nullptr)
            CloseHandle(fence_event_);
    }

    // ------------------------------------------------------------------------------------------
    // Reading a shared picture back, for a screenshot
    // ------------------------------------------------------------------------------------------

    bool D3D12Raster::ReadSharedPicture(const emulation::psx::SharedPicture& picture,
                                        std::vector<uint32_t>* pixels) {
        if (!picture || picture.width <= 0 || picture.height <= 0 ||
            !picture.source->WaitReady(picture.serial, 1000))
            return false;
        ComPtr<ID3D12Device> device;
        ComPtr<ID3D12Resource> shared, readback;
        // On the renderer's own device the texture is there to be read, in the state it was
        // handed over in. Nothing else may be reading it meanwhile: the front end has the
        // renderer finish its frames first (IGraphicsEngine::Idle).
        D3D12_RESOURCE_STATES before = D3D12_RESOURCE_STATE_COMMON;
        if (picture.source->device() != nullptr) {
            device = static_cast<ID3D12Device*>(picture.source->device());
            shared = static_cast<ID3D12Resource*>(picture.texture);
            before = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
        } else {
            ComPtr<IDXGIFactory4> factory;
            ComPtr<IDXGIAdapter1> adapter;
            if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) ||
                FAILED(factory->EnumAdapterByLuid(LuidFrom(picture.source->adapter()),
                                                  IID_PPV_ARGS(&adapter))) ||
                FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                         IID_PPV_ARGS(&device))) ||
                FAILED(device->OpenSharedHandle(static_cast<HANDLE>(picture.texture),
                                                IID_PPV_ARGS(&shared))))
                return false;
        }
        const D3D12_RESOURCE_DESC texture = shared->GetDesc();
        if (texture.Width != static_cast<UINT64>(picture.width) ||
            texture.Height != static_cast<UINT>(picture.height))
            return false;
        const UINT width = static_cast<UINT>(picture.width);
        const UINT height = static_cast<UINT>(picture.height);
        const UINT pitch = RowPitch(width, 4);
        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC buffer = {};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = static_cast<UINT64>(pitch) * height;
        buffer.Height = 1;
        buffer.DepthOrArraySize = 1;
        buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_COMMAND_QUEUE_DESC queue_desc = {};
        queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ComPtr<ID3D12CommandQueue> queue;
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        ComPtr<ID3D12Fence> fence;
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                   D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                   IID_PPV_ARGS(&readback))) ||
            FAILED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue))) ||
            FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                  IID_PPV_ARGS(&allocator))) ||
            FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(),
                                             nullptr, IID_PPV_ARGS(&list))) ||
            FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
            return false;
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = shared.Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(1, &barrier);
        D3D12_TEXTURE_COPY_LOCATION to = {};
        to.pResource = readback.Get();
        to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        to.PlacedFootprint.Footprint = { texture.Format, width, height, 1, pitch };
        D3D12_TEXTURE_COPY_LOCATION from = {};
        from.pResource = shared.Get();
        from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        list->ResourceBarrier(1, &barrier);
        if (FAILED(list->Close()))
            return false;
        ID3D12CommandList* lists[] = { list.Get() };
        queue->ExecuteCommandLists(1, lists);
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (event == nullptr)
            return false;
        bool done = SUCCEEDED(queue->Signal(fence.Get(), 1)) &&
                    SUCCEEDED(fence->SetEventOnCompletion(1, event)) &&
                    WaitForSingleObject(event, 5000) == WAIT_OBJECT_0;
        CloseHandle(event);
        void* mapped = nullptr;
        const D3D12_RANGE read = { 0, static_cast<SIZE_T>(buffer.Width) };
        if (!done || FAILED(readback->Map(0, &read, &mapped)))
            return false;
        pixels->resize(static_cast<size_t>(width) * height);
        for (UINT row = 0; row < height; ++row)
            memcpy(pixels->data() + static_cast<size_t>(row) * width,
                   static_cast<const uint8_t*>(mapped) + static_cast<size_t>(row) * pitch,
                   static_cast<size_t>(width) * sizeof(uint32_t));
        const D3D12_RANGE written = { 0, 0 };
        readback->Unmap(0, &written);
        return true;
    }

    // ------------------------------------------------------------------------------------------
    // Making it
    // ------------------------------------------------------------------------------------------

    std::unique_ptr<D3D12Raster> D3D12Raster::Create(uint16_t* vram,
                                                     const emulation::psx::RasterOptions& options,
                                                     bool warp, std::string* error,
                                                     ID3D12Device* device) {
        emulation::psx::RasterOptions checked = options;
        checked.scale = std::min(std::max(options.scale, 1), 8);
        std::unique_ptr<D3D12Raster> raster(new D3D12Raster(vram, checked, device));
        if (!raster->Initialize(warp, error))
            return nullptr;
        return raster;
    }

    bool D3D12Raster::Initialize(bool warp, std::string* error) {
        // The presenter's card when the front end names it, so the picture can stay there, and
        // otherwise Windows' default - as D3D11Raster::Initialize, which says why.
        ComPtr<IDXGIFactory4> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
            *error = "DXGI could not be started";
            return false;
        }
        // Given the renderer's device, that is the card.
        if (!one_device_) {
            ComPtr<IDXGIAdapter1> chosen;
            if (warp) {
                if (FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&chosen)))) {
                    *error = "Direct3D 12's WARP device could not be found";
                    return false;
                }
            } else if (adapter_ != 0 && FAILED(factory->EnumAdapterByLuid(
                                            LuidFrom(adapter_), IID_PPV_ARGS(&chosen)))) {
                chosen.Reset();
            }
            HRESULT result = D3D12CreateDevice(chosen.Get(), D3D_FEATURE_LEVEL_11_0,
                                               IID_PPV_ARGS(&device_));
            if (FAILED(result) && chosen && !warp) {
                // The card asked for cannot make a Direct3D 12 device: Windows' default one,
                // whose pictures StartSharing then sees cannot be shared, and reads back.
                chosen.Reset();
                result = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_));
            }
            if (FAILED(result)) {
                *error = warp ? "Direct3D 12's WARP device could not be made"
                              : "no graphics card here offers Direct3D 12 at feature level 11.0";
                return false;
            }
        }
        adapter_luid_ = LuidBits(device_->GetAdapterLuid());
        {
            ComPtr<IDXGIAdapter1> used;
            DXGI_ADAPTER_DESC1 description = {};
            if (SUCCEEDED(factory->EnumAdapterByLuid(device_->GetAdapterLuid(), IID_PPV_ARGS(&used))) &&
                SUCCEEDED(used->GetDesc1(&description))) {
                char name[128] = {};
                WideCharToMultiByte(CP_UTF8, 0, description.Description, -1, name, sizeof(name) - 1,
                                    nullptr, nullptr);
                adapter_name_ = name;
            }
        }

        D3D12_COMMAND_QUEUE_DESC queue = {};
        queue.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(device_->CreateCommandQueue(&queue, IID_PPV_ARGS(&queue_))) ||
            FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)))) {
            *error = "the hardware rasteriser could not make its command queue";
            return false;
        }
        for (Frame& frame : frames_) {
            if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                       IID_PPV_ARGS(&frame.allocator)))) {
                *error = "the hardware rasteriser could not make its command allocators";
                return false;
            }
        }
        if (FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              frames_[0].allocator.Get(), nullptr,
                                              IID_PPV_ARGS(&list_))) ||
            FAILED(list_->Close())) {
            *error = "the hardware rasteriser could not make its command list";
            return false;
        }
        fence_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (fence_event_ == nullptr) {
            *error = "the hardware rasteriser could not make an event";
            return false;
        }

        D3D12_DESCRIPTOR_HEAP_DESC heap = {};
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heap.NumDescriptors = kRtvCount;
        if (FAILED(device_->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&rtv_heap_)))) {
            *error = "the hardware rasteriser could not make its descriptor heaps";
            return false;
        }
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap.NumDescriptors = 2 * kTableCount;
        heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(device_->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&srv_heap_)))) {
            *error = "the hardware rasteriser could not make its descriptor heaps";
            return false;
        }
        rtv_size_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        srv_size_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        // VRAM at scale_, and its read copy: 128 MB each at 8x.
        const UINT width = kWidth * scale_, height = kHeight * scale_;
        if (!MakeTexture(&target_, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, true,
                         D3D12_RESOURCE_STATE_RENDER_TARGET)) {
            *error = "the graphics card could not hold VRAM at " + std::to_string(scale_) +
                     "x its size";
            return false;
        }
        if (!MakeTexture(&read_copy_, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, false,
                         D3D12_RESOURCE_STATE_COPY_DEST)) {
            *error = "the graphics card could not hold a second copy of VRAM at " +
                     std::to_string(scale_) + "x its size";
            return false;
        }
        if (scale_ > 1 &&
            (!MakeTexture(&native_target_, kWidth, kHeight, DXGI_FORMAT_R8G8B8A8_UNORM, true,
                          D3D12_RESOURCE_STATE_RENDER_TARGET) ||
             !MakeTexture(&native_source_, kWidth, kHeight, DXGI_FORMAT_R8G8B8A8_UNORM, false,
                          D3D12_RESOURCE_STATE_COPY_DEST))) {
            *error = "the hardware rasteriser could not make its download and upload textures";
            return false;
        }
        if (!MakeReadback(&vram_readback_, static_cast<size_t>(kWidth) * 4 * kHeight)) {
            *error = "the hardware rasteriser could not make its read-back buffer";
            return false;
        }
        D3D12_RENDER_TARGET_VIEW_DESC rtv = {};
        rtv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        device_->CreateRenderTargetView(target_.resource.Get(), &rtv, RtvHandle(kRtvTarget));
        if (scale_ > 1)
            device_->CreateRenderTargetView(native_target_.resource.Get(), &rtv,
                                            RtvHandle(kRtvNative));
        WriteTable(kTableDraw, read_copy_.resource.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, nullptr,
                   DXGI_FORMAT_R16G16B16A16_FLOAT);
        WriteTable(kTableTarget, target_.resource.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, nullptr,
                   DXGI_FORMAT_R8G8B8A8_UNORM);
        WriteTable(kTableNative, native_source_.resource.Get(), DXGI_FORMAT_R8G8B8A8_UNORM,
                   nullptr, DXGI_FORMAT_R8G8B8A8_UNORM);
        WriteTable(kTablePlane, nullptr, DXGI_FORMAT_R16G16B16A16_FLOAT, nullptr,
                   DXGI_FORMAT_R16G16B16A16_FLOAT);

        if (!CreatePipelines(error))
            return false;

        batch_.reserve(kBatchVertices);
        Reloaded();
        // Only above 1x is there a sharper picture to hand over at all.
        if (share_ && scale_ > 1)
            StartSharing();
        return true;
    }

    bool D3D12Raster::CreatePipelines(std::string* error) {
        // Root constants are the cbuffer (b0); one table of two textures is t0 and t1.
        D3D12_DESCRIPTOR_RANGE range = {};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = 2;
        range.BaseShaderRegister = 0;
        range.OffsetInDescriptorsFromTableStart = 0;
        D3D12_ROOT_PARAMETER parameters[2] = {};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[0].Constants.ShaderRegister = 0;
        parameters[0].Constants.Num32BitValues = sizeof(Constants) / 4;
        parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[1].DescriptorTable.NumDescriptorRanges = 1;
        parameters[1].DescriptorTable.pDescriptorRanges = &range;
        parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC description = {};
        description.NumParameters = 2;
        description.pParameters = parameters;
        description.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ComPtr<ID3DBlob> serialized, errors;
        if (FAILED(D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1,
                                               &serialized, &errors)) ||
            FAILED(device_->CreateRootSignature(0, serialized->GetBufferPointer(),
                                                serialized->GetBufferSize(), IID_PPV_ARGS(&root_)))) {
            *error = "the hardware rasteriser could not make its root signature";
            return false;
        }

        auto compile = [&](const char* entry, const char* target, bool planes, ComPtr<ID3DBlob>* blob) {
            return CompileShader("d3d12_raster", entry, target, scale_, true_color_, planes, blob,
                                 error);
        };
        ComPtr<ID3DBlob> vertex, pixels[raster::kShaderCount], plane_draw, plane_copy;
        if (!compile("VsMain", "vs_5_0", false, &vertex))
            return false;
        for (int i = 0; i < raster::kShaderCount; ++i)
            if (!compile(kShaderEntries[i], "ps_5_0", false, &pixels[i]))
                return false;
        if (!compile("PsDraw", "ps_5_0", true, &plane_draw) ||
            !compile("PsCopy", "ps_5_0", true, &plane_copy))
            return false;

        const D3D12_INPUT_ELEMENT_DESC elements[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(Vertex, x),
              D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "P", 0, DXGI_FORMAT_R32G32B32A32_UINT, 0, offsetof(Vertex, p),
              D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "P", 1, DXGI_FORMAT_R32G32B32A32_UINT, 0, offsetof(Vertex, p) + 16,
              D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "P", 2, DXGI_FORMAT_R32G32B32A32_UINT, 0, offsetof(Vertex, p) + 32,
              D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "P", 3, DXGI_FORMAT_R32G32B32A32_UINT, 0, offsetof(Vertex, p) + 48,
              D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "P", 4, DXGI_FORMAT_R32G32B32A32_UINT, 0, offsetof(Vertex, p) + 64,
              D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        };

        // Nothing culled, nothing depth-tested, and the output merger only writes - the shader
        // does its own blending - but for the plane while it is kept: its second target kept as
        // it was wherever the shader's plane alpha is 0, which is where it drew something
        // translucent, with that alpha stored, saying so.
        auto make = [&](ID3DBlob* shader, std::initializer_list<DXGI_FORMAT> formats,
                        bool plane_blend, bool points, ComPtr<ID3D12PipelineState>* pipeline) {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC state = {};
            state.pRootSignature = root_.Get();
            state.VS = { vertex->GetBufferPointer(), vertex->GetBufferSize() };
            state.PS = { shader->GetBufferPointer(), shader->GetBufferSize() };
            state.InputLayout = { elements, static_cast<UINT>(std::size(elements)) };
            state.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            state.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            state.RasterizerState.DepthClipEnable = TRUE;
            state.BlendState.IndependentBlendEnable = plane_blend ? TRUE : FALSE;
            for (D3D12_RENDER_TARGET_BLEND_DESC& target : state.BlendState.RenderTarget)
                target.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            if (plane_blend) {
                D3D12_RENDER_TARGET_BLEND_DESC& plane = state.BlendState.RenderTarget[1];
                plane.BlendEnable = TRUE;
                plane.SrcBlend = D3D12_BLEND_SRC_ALPHA;
                plane.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
                plane.BlendOp = D3D12_BLEND_OP_ADD;
                plane.SrcBlendAlpha = D3D12_BLEND_ONE;
                plane.DestBlendAlpha = D3D12_BLEND_ZERO;
                plane.BlendOpAlpha = D3D12_BLEND_OP_ADD;
                plane.LogicOp = D3D12_LOGIC_OP_NOOP;
            }
            state.DepthStencilState.DepthEnable = FALSE;
            state.DepthStencilState.StencilEnable = FALSE;
            state.SampleMask = UINT_MAX;
            state.PrimitiveTopologyType = points ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT
                                                 : D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            state.NumRenderTargets = static_cast<UINT>(formats.size());
            UINT i = 0;
            for (DXGI_FORMAT format : formats)
                state.RTVFormats[i++] = format;
            state.SampleDesc.Count = 1;
            return SUCCEEDED(device_->CreateGraphicsPipelineState(
                &state, IID_PPV_ARGS(pipeline->ReleaseAndGetAddressOf())));
        };
        const DXGI_FORMAT vram = DXGI_FORMAT_R8G8B8A8_UNORM;
        const DXGI_FORMAT plane = DXGI_FORMAT_R16G16B16A16_FLOAT;
        const DXGI_FORMAT shown = DXGI_FORMAT_B8G8R8A8_UNORM;
        const bool made =
            make(pixels[raster::kShaderDraw].Get(), { vram }, false, false, &pipelines_[kPipeDraw]) &&
            make(pixels[raster::kShaderDraw].Get(), { vram }, false, true, &pipelines_[kPipeDrawPoints]) &&
            make(plane_draw.Get(), { vram, plane }, true, false, &pipelines_[kPipeDrawPlanes]) &&
            make(plane_draw.Get(), { vram, plane }, true, true, &pipelines_[kPipeDrawPlanesPoints]) &&
            make(pixels[raster::kShaderCopy].Get(), { vram }, false, false, &pipelines_[kPipeCopy]) &&
            make(plane_copy.Get(), { vram, plane }, false, false, &pipelines_[kPipeCopyPlanes]) &&
            make(pixels[raster::kShaderDownsample].Get(), { vram }, false, false,
                 &pipelines_[kPipeDownsample]) &&
            make(pixels[raster::kShaderExpand].Get(), { vram }, false, false,
                 &pipelines_[kPipeExpand]) &&
            make(pixels[raster::kShaderDisplay].Get(), { shown }, false, false,
                 &pipelines_[kPipeDisplay]) &&
            make(pixels[raster::kShaderDisplayDepth].Get(), { shown }, false, false,
                 &pipelines_[kPipeDisplayDepth]) &&
            make(pixels[raster::kShaderDisplayMotion].Get(), { shown }, false, false,
                 &pipelines_[kPipeDisplayMotion]);
        if (!made) {
            *error = "the hardware rasteriser could not make its pipelines";
            return false;
        }
        return true;
    }

    void D3D12Raster::StartSharing() {
        // Only on the card the presenter draws on, which is the one asked for: a device that
        // ended up elsewhere has nothing a presenter could open, and its pictures are read back.
        if (adapter_ != 0 && adapter_luid_ != adapter_)
            return;
        ComPtr<ID3D12Fence> fence;
        if (adapter_luid_ == 0 ||
            FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
            return;
        pictures_ = std::make_shared<D3D12Pictures>(std::move(fence), adapter_luid_,
                                                    one_device_ ? device_.Get() : nullptr);
    }

    // ------------------------------------------------------------------------------------------
    // Command lists, memory and states
    // ------------------------------------------------------------------------------------------

    ID3D12GraphicsCommandList* D3D12Raster::List() {
        if (!recording_) {
            Frame& frame = frames_[frame_];
            // The allocator's last list, and whatever it read from upload memory, are done.
            WaitFor(frame.fence);
            for (UploadPage& page : frame.pages)
                page.used = 0;
            if (FAILED(frame.allocator->Reset()) ||
                FAILED(list_->Reset(frame.allocator.Get(), nullptr)))
                CheckDevice();
            ID3D12DescriptorHeap* heaps[] = { srv_heap_.Get() };
            list_->SetDescriptorHeaps(1, heaps);
            list_->SetGraphicsRootSignature(root_.Get());
            recording_ = true;
            recorded_draws_ = 0;
        }
        return list_.Get();
    }

    uint64_t D3D12Raster::Submit() {
        if (!recording_)
            return fence_value_;
        recording_ = false;
        if (FAILED(list_->Close())) {
            CheckDevice();
            return fence_value_;
        }
        ID3D12CommandList* lists[] = { list_.Get() };
        queue_->ExecuteCommandLists(1, lists);
        if (FAILED(queue_->Signal(fence_.Get(), fence_value_ + 1))) {
            CheckDevice();
            return fence_value_;
        }
        ++fence_value_;
        frames_[frame_].fence = fence_value_;
        frame_ = (frame_ + 1) % kFrames;
        return fence_value_;
    }

    bool D3D12Raster::WaitFor(uint64_t value) {
        if (fence_->GetCompletedValue() >= value)
            return true;
        if (FAILED(fence_->SetEventOnCompletion(value, fence_event_))) {
            CheckDevice();
            return false;
        }
        WaitForSingleObject(fence_event_, INFINITE);
        // A device that has gone completes everything, at once.
        if (fence_->GetCompletedValue() == UINT64_MAX) {
            CheckDevice();
            return false;
        }
        return true;
    }

    D3D12Raster::UploadSpace D3D12Raster::Allocate(size_t size, size_t alignment) {
        List();
        Frame& frame = frames_[frame_];
        for (UploadPage& page : frame.pages) {
            const size_t offset = Align(page.used, alignment);
            if (offset + size <= page.size) {
                page.used = offset + size;
                return { page.cpu + offset, page.gpu + offset, page.buffer.Get(), offset };
            }
        }
        UploadPage page;
        page.size = std::max(kUploadPage, Align(size, 65536));
        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC buffer = {};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = page.size;
        buffer.Height = 1;
        buffer.DepthOrArraySize = 1;
        buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        void* mapped = nullptr;
        const D3D12_RANGE none = { 0, 0 };
        if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                    IID_PPV_ARGS(&page.buffer))) ||
            FAILED(page.buffer->Map(0, &none, &mapped))) {
            CheckDevice();
            return { nullptr, 0, nullptr, 0 };
        }
        page.cpu = static_cast<uint8_t*>(mapped);
        page.gpu = page.buffer->GetGPUVirtualAddress();
        page.used = size;
        frame.pages.push_back(page);
        const UploadPage& added = frame.pages.back();
        return { added.cpu, added.gpu, added.buffer.Get(), 0 };
    }

    void D3D12Raster::Use(Texture& texture, D3D12_RESOURCE_STATES state) {
        if (!texture.resource || texture.state == state)
            return;
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = texture.resource.Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = texture.state;
        barrier.Transition.StateAfter = state;
        List()->ResourceBarrier(1, &barrier);
        texture.state = state;
    }

    void D3D12Raster::SetDrawState(Pipeline pipeline, Table table,
                                   const D3D12_CPU_DESCRIPTOR_HANDLE* targets, UINT target_count,
                                   UINT width, UINT height, const D3D12_RECT& scissor) {
        ID3D12GraphicsCommandList* list = List();
        list->SetPipelineState(pipelines_[pipeline].Get());
        list->SetGraphicsRootDescriptorTable(1, TableHandle(table));
        list->OMSetRenderTargets(target_count, targets, FALSE, nullptr);
        D3D12_VIEWPORT viewport = {};
        viewport.Width = static_cast<float>(width);
        viewport.Height = static_cast<float>(height);
        viewport.MaxDepth = 1.0f;
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
    }

    D3D12_CPU_DESCRIPTOR_HANDLE D3D12Raster::RtvHandle(int index) const {
        D3D12_CPU_DESCRIPTOR_HANDLE handle = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(index) * rtv_size_;
        return handle;
    }

    D3D12_GPU_DESCRIPTOR_HANDLE D3D12Raster::TableHandle(Table table) const {
        D3D12_GPU_DESCRIPTOR_HANDLE handle = srv_heap_->GetGPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<UINT64>(table) * 2 * srv_size_;
        return handle;
    }

    void D3D12Raster::WriteTable(Table table, ID3D12Resource* first, DXGI_FORMAT first_format,
                                 ID3D12Resource* second, DXGI_FORMAT second_format) {
        D3D12_CPU_DESCRIPTOR_HANDLE handle = srv_heap_->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(table) * 2 * srv_size_;
        const struct { ID3D12Resource* resource; DXGI_FORMAT format; } views[2] = {
            { first, first_format }, { second, second_format },
        };
        for (const auto& view : views) {
            // A null view where there is nothing: valid to bind, and read as zero.
            D3D12_SHADER_RESOURCE_VIEW_DESC description = {};
            description.Format = view.format;
            description.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            description.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            description.Texture2D.MipLevels = 1;
            device_->CreateShaderResourceView(view.resource, &description, handle);
            handle.ptr += srv_size_;
        }
    }

    bool D3D12Raster::MakeTexture(Texture* texture, UINT width, UINT height, DXGI_FORMAT format,
                                  bool render_target, D3D12_RESOURCE_STATES initial,
                                  D3D12_HEAP_FLAGS heap_flags) {
        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC description = {};
        description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        description.Width = width;
        description.Height = height;
        description.DepthOrArraySize = 1;
        description.MipLevels = 1;
        description.Format = format;
        description.SampleDesc.Count = 1;
        description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        description.Flags = render_target ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET
                                          : D3D12_RESOURCE_FLAG_NONE;
        // A texture another device opens is made for that: uncompressed, so handing it over in
        // the common state costs nothing. Compressed, the card unpacked it at every hand-off -
        // Ace Combat 3 at 8x on the Radeon 780M took 15.4 ms of the card's time a frame with
        // that, 10.7 without.
        if (heap_flags & D3D12_HEAP_FLAG_SHARED)
            description.Flags |= D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
        texture->resource.Reset();
        if (FAILED(device_->CreateCommittedResource(&heap, heap_flags, &description, initial,
                                                    nullptr, IID_PPV_ARGS(&texture->resource)))) {
            texture->resource.Reset();
            CheckDevice();
            return false;
        }
        texture->state = initial;
        return true;
    }

    bool D3D12Raster::MakeReadback(ComPtr<ID3D12Resource>* buffer, size_t size) {
        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC description = {};
        description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        description.Width = size;
        description.Height = 1;
        description.DepthOrArraySize = 1;
        description.MipLevels = 1;
        description.SampleDesc.Count = 1;
        description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        buffer->Reset();
        if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
                                                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                    IID_PPV_ARGS(buffer->GetAddressOf())))) {
            CheckDevice();
            return false;
        }
        return true;
    }

    void D3D12Raster::CopyToBuffer(ID3D12Resource* buffer, UINT width, UINT height, UINT pitch,
                                   DXGI_FORMAT format, UINT dest_x, UINT dest_y,
                                   ID3D12Resource* from, const D3D12_BOX& box) {
        D3D12_TEXTURE_COPY_LOCATION to = {};
        to.pResource = buffer;
        to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        to.PlacedFootprint.Offset = 0;
        to.PlacedFootprint.Footprint = { format, width, height, 1, pitch };
        D3D12_TEXTURE_COPY_LOCATION source = {};
        source.pResource = from;
        source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        List()->CopyTextureRegion(&to, dest_x, dest_y, 0, &source, &box);
    }

    // ------------------------------------------------------------------------------------------
    // The plane beside VRAM (psx/shared_picture.h, Docs/DLSS-Plan.md)
    // ------------------------------------------------------------------------------------------

    void D3D12Raster::SetPlanes(bool keep, emulation::psx::PlaneView view) {
        const bool was = planes();
        Flush();   // what is batched is drawn the way it was batched
        keep_planes_ = keep;
        shown_plane_ = view;
        if ((keep || view != emulation::psx::PlaneView::kPicture) && !plane_target_.resource &&
            !MakePlanes()) {
            // The card cannot hold it: nothing is kept, and the picture is shown as ever.
            keep_planes_ = false;
            shown_plane_ = emulation::psx::PlaneView::kPicture;
        }
        // Started afresh: nothing drawn while it was not kept is known - and its read copy,
        // which has not been kept either, is stale everywhere.
        if (planes() && !was) {
            ForgetPlanes(0, 0, kWidth, kHeight);
            std::fill(stale_.begin(), stale_.end(), 1);
        }
    }

    bool D3D12Raster::MakePlanes() {
        // The descriptor tables change: nothing in flight may still be reading them.
        Finish();
        // VRAM's size at scale_, eight bytes a sub-pixel: 256 MB at 8x, and its read copy as much.
        const UINT width = kWidth * scale_, height = kHeight * scale_;
        if (!MakeTexture(&plane_target_, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, true,
                         D3D12_RESOURCE_STATE_RENDER_TARGET) ||
            !MakeTexture(&plane_read_copy_, width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, false,
                         D3D12_RESOURCE_STATE_COPY_DEST)) {
            plane_target_ = Texture();
            plane_read_copy_ = Texture();
            return false;
        }
        D3D12_RENDER_TARGET_VIEW_DESC rtv = {};
        rtv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        device_->CreateRenderTargetView(plane_target_.resource.Get(), &rtv, RtvHandle(kRtvPlane));
        WriteTable(kTableDraw, read_copy_.resource.Get(), DXGI_FORMAT_R8G8B8A8_UNORM,
                   plane_read_copy_.resource.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT);
        WriteTable(kTablePlane, plane_target_.resource.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
                   nullptr, DXGI_FORMAT_R16G16B16A16_FLOAT);
        return true;
    }

    void D3D12Raster::ForgetPlanes(int32_t x, int32_t y, int32_t w, int32_t h) {
        const float unknown[4] = { emulation::psx::kUnknownMotion, emulation::psx::kUnknownMotion,
                                   0.0f, 1.0f };
        const D3D12_RECT rect = { x * scale_, y * scale_, (x + w) * scale_, (y + h) * scale_ };
        Use(plane_target_, D3D12_RESOURCE_STATE_RENDER_TARGET);
        List()->ClearRenderTargetView(RtvHandle(kRtvPlane), unknown, 1, &rect);
    }

    void D3D12Raster::NewPicture(bool reset) {
        picture_new_ = true;
        picture_reset_ = reset;
        // The picture about to be shown was drawn since the last new picture, with the jitter
        // set then; what is drawn from here, the next, with the next in the sequence.
        if (jitter_phases_ > 0) {
            shown_jitter_x_ = jitter_x_;
            shown_jitter_y_ = jitter_y_;
            jitter_index_ = jitter_index_ % static_cast<uint32_t>(jitter_phases_) + 1;
            jitter_x_ = Halton(jitter_index_, 2) - 0.5f;
            jitter_y_ = Halton(jitter_index_, 3) - 0.5f;
        }
    }

    void D3D12Raster::SetJitter(int phases) {
        jitter_phases_ = std::max(phases, 0);
        jitter_index_ = 1;
        jitter_x_ = jitter_phases_ > 0 ? Halton(1, 2) - 0.5f : 0.0f;
        jitter_y_ = jitter_phases_ > 0 ? Halton(1, 3) - 0.5f : 0.0f;
        shown_jitter_x_ = shown_jitter_y_ = 0.0f;
    }

    void D3D12Raster::WarpCheck(uint32_t x, uint32_t y, UINT width, UINT height) {
        const UINT colour_pitch = RowPitch(width, 4), plane_pitch = RowPitch(width, 8);
        if (!warp_colour_ || warp_width_ != width || warp_height_ != height) {
            Finish();
            warp_colour_.Reset();
            warp_plane_.Reset();
            warp_last_.clear();
            if (!MakeReadback(&warp_colour_, static_cast<size_t>(colour_pitch) * height) ||
                !MakeReadback(&warp_plane_, static_cast<size_t>(plane_pitch) * height)) {
                warp_colour_.Reset();
                warp_plane_.Reset();
                return;
            }
            warp_width_ = width;
            warp_height_ = height;
        }
        const D3D12_BOX box = { x * scale_, y * scale_, 0, x * scale_ + width, y * scale_ + height, 1 };
        Use(target_, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Use(plane_target_, D3D12_RESOURCE_STATE_COPY_SOURCE);
        CopyToBuffer(warp_colour_.Get(), width, height, colour_pitch, DXGI_FORMAT_R8G8B8A8_UNORM, 0,
                     0, target_.resource.Get(), box);
        CopyToBuffer(warp_plane_.Get(), width, height, plane_pitch,
                     DXGI_FORMAT_R16G16B16A16_FLOAT, 0, 0, plane_target_.resource.Get(), box);
        if (!Finish())
            return;
        void* colour = nullptr;
        void* plane = nullptr;
        const D3D12_RANGE colour_range = { 0, static_cast<SIZE_T>(colour_pitch) * height };
        const D3D12_RANGE plane_range = { 0, static_cast<SIZE_T>(plane_pitch) * height };
        if (FAILED(warp_colour_->Map(0, &colour_range, &colour))) {
            CheckDevice();
            return;
        }
        if (FAILED(warp_plane_->Map(0, &plane_range, &plane))) {
            const D3D12_RANGE none = { 0, 0 };
            warp_colour_->Unmap(0, &none);
            CheckDevice();
            return;
        }
        std::vector<uint8_t> now(static_cast<size_t>(width) * height * 4);
        for (UINT row = 0; row < height; ++row)
            memcpy(now.data() + static_cast<size_t>(row) * width * 4,
                   static_cast<const uint8_t*>(colour) + static_cast<size_t>(row) * colour_pitch,
                   width * 4);
        WarpSums(now, warp_last_, static_cast<const uint8_t*>(plane), plane_pitch, width, height,
                 picture_reset_, &counters_);
        const D3D12_RANGE none = { 0, 0 };
        warp_plane_->Unmap(0, &none);
        warp_colour_->Unmap(0, &none);
        warp_last_.swap(now);
    }

    bool D3D12Raster::ReadPlanes(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                                 std::vector<float>* rgba) {
        if (!planes() || w == 0 || h == 0 || x + w > kWidth || y + h > kHeight)
            return false;
        Flush();
        const UINT width = w * scale_, height = h * scale_;
        const UINT pitch = RowPitch(width, 8);
        ComPtr<ID3D12Resource> readback;
        if (!MakeReadback(&readback, static_cast<size_t>(pitch) * height))
            return false;
        const D3D12_BOX box = Scaled(static_cast<int32_t>(x), static_cast<int32_t>(y),
                                     static_cast<int32_t>(x + w), static_cast<int32_t>(y + h));
        Use(plane_target_, D3D12_RESOURCE_STATE_COPY_SOURCE);
        CopyToBuffer(readback.Get(), width, height, pitch, DXGI_FORMAT_R16G16B16A16_FLOAT, 0, 0,
                     plane_target_.resource.Get(), box);
        if (!Finish())
            return false;
        void* mapped = nullptr;
        const D3D12_RANGE range = { 0, static_cast<SIZE_T>(pitch) * height };
        if (FAILED(readback->Map(0, &range, &mapped))) {
            CheckDevice();
            return false;
        }
        const size_t values = static_cast<size_t>(width) * 4;
        rgba->resize(values * height);
        for (UINT row = 0; row < height; ++row) {
            const uint16_t* source = reinterpret_cast<const uint16_t*>(
                static_cast<const uint8_t*>(mapped) + static_cast<size_t>(row) * pitch);
            for (size_t i = 0; i < values; ++i)
                (*rgba)[row * values + i] = HalfToFloat(source[i]);
        }
        const D3D12_RANGE none = { 0, 0 };
        readback->Unmap(0, &none);
        return true;
    }

    void D3D12Raster::Apply(const DrawJob& job) {
        switch (job.kind) {
        case DrawJob::kTriangle:
            AddTriangle(job);
            break;
        case DrawJob::kLine:
            AddLine(job);
            break;
        case DrawJob::kRectangle:
            AddRectangle(job);
            break;
        case DrawJob::kFill:
            Fill(job);
            break;
        case DrawJob::kVramCopy:
            Copy(job);
            break;
        }
    }

    // ------------------------------------------------------------------------------------------
    // Batching
    // ------------------------------------------------------------------------------------------

    void D3D12Raster::Flush() {
        if (batch_.empty())
            return;
        const size_t bytes = batch_.size() * sizeof(Vertex);
        const UploadSpace vertices = Allocate(bytes, 16);
        if (vertices.cpu != nullptr) {
            memcpy(vertices.cpu, batch_.data(), bytes);
            // While the plane is kept, draws and copies write it too: a draw blending it, a copy
            // carrying it across whole - and both reading its read copy, as they read VRAM's.
            const bool planes_kept = planes();
            const bool copy = batch_key_.shader == raster::kShaderCopy;
            const bool points = batch_key_.topology == D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
            const Pipeline pipeline = copy ? (planes_kept ? kPipeCopyPlanes : kPipeCopy)
                                      : planes_kept ? (points ? kPipeDrawPlanesPoints : kPipeDrawPlanes)
                                                    : (points ? kPipeDrawPoints : kPipeDraw);
            Use(target_, D3D12_RESOURCE_STATE_RENDER_TARGET);
            Use(read_copy_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            if (planes_kept) {
                Use(plane_target_, D3D12_RESOURCE_STATE_RENDER_TARGET);
                Use(plane_read_copy_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            }
            const D3D12_CPU_DESCRIPTOR_HANDLE targets[2] = { RtvHandle(kRtvTarget),
                                                             RtvHandle(kRtvPlane) };
            const D3D12_RECT scissor = { batch_key_.scissor.left * scale_,
                                         batch_key_.scissor.top * scale_,
                                         batch_key_.scissor.right * scale_,
                                         batch_key_.scissor.bottom * scale_ };
            SetDrawState(pipeline, kTableDraw, targets, planes_kept ? 2 : 1, kWidth * scale_,
                         kHeight * scale_, scissor);
            ID3D12GraphicsCommandList* list = List();
            list->SetGraphicsRoot32BitConstants(0, sizeof(Constants) / 4, &batch_key_.constants, 0);
            list->IASetPrimitiveTopology(batch_key_.topology);
            const D3D12_VERTEX_BUFFER_VIEW view = { vertices.gpu, static_cast<UINT>(bytes),
                                                    sizeof(Vertex) };
            list->IASetVertexBuffers(0, 1, &view);
            list->DrawInstanced(static_cast<UINT>(batch_.size()), 1, 0, 0);
        }
        batch_.clear();
        ++batch_serial_;
        // A long run of drawing with nothing read back is sent on before it grows large.
        if (++recorded_draws_ >= 2048)
            Submit();
    }

    void D3D12Raster::Begin(const BatchKey& key, size_t count) {
        // Every batch carries the jitter, which only triangles use: so a batch mixing them with
        // rectangles is not broken up, and one drawn before a new picture's jitter keeps its own.
        BatchKey keyed = key;
        keyed.constants.jitter_x = jitter_x_;
        keyed.constants.jitter_y = jitter_y_;
        if (!batch_.empty() &&
            (memcmp(&keyed, &batch_key_, sizeof(BatchKey)) != 0 ||
             batch_.size() + count > kBatchVertices))
            Flush();
        batch_key_ = keyed;
    }

    bool D3D12Raster::KeyFor(const DrawJob& job, D3D_PRIMITIVE_TOPOLOGY topology, BatchKey* key) {
        memset(key, 0, sizeof(BatchKey));
        key->topology = topology;
        // The drawing area's right and bottom are inclusive; a scissor's are not.
        key->scissor.left = std::max(job.env.area_left, 0);
        key->scissor.top = std::max(job.env.area_top, 0);
        key->scissor.right = std::min(job.env.area_right + 1, kWidth);
        key->scissor.bottom = std::min(job.env.area_bottom + 1, kHeight);
        if (key->scissor.left >= key->scissor.right || key->scissor.top >= key->scissor.bottom)
            return false;
        key->shader = raster::kShaderDraw;
        Constants& constants = key->constants;
        constants.skip_field = job.env.skip_field ? 1 : 0;
        constants.active_line_lsb = static_cast<int32_t>(job.env.active_line_lsb & 1);
        constants.force_mask = job.env.force_set_mask ? 1 : 0;
        constants.check_mask = job.env.check_mask ? 1 : 0;
        constants.tw_mask_x = static_cast<int32_t>(job.env.tw_mask_x);
        constants.tw_mask_y = static_cast<int32_t>(job.env.tw_mask_y);
        constants.tw_offset_x = static_cast<int32_t>(job.env.tw_offset_x);
        constants.tw_offset_y = static_cast<int32_t>(job.env.tw_offset_y);
        return true;
    }

    void D3D12Raster::AddBox(int32_t left, int32_t top, int32_t right, int32_t bottom,
                             const uint32_t (&payload)[kPayload]) {
        const float l = static_cast<float>(left), t = static_cast<float>(top);
        const float r = static_cast<float>(right), b = static_cast<float>(bottom);
        const float corners[6][2] = { { l, t }, { r, t }, { l, b }, { r, t }, { r, b }, { l, b } };
        for (const auto& corner : corners) {
            Vertex vertex;
            vertex.x = corner[0];
            vertex.y = corner[1];
            memcpy(vertex.p, payload, sizeof(vertex.p));
            batch_.push_back(vertex);
        }
        MarkDrawn(left, top, right, bottom);
    }

    // ------------------------------------------------------------------------------------------
    // Drawing - D3D11Raster's, line for line
    // ------------------------------------------------------------------------------------------

    void D3D12Raster::AddTriangle(const DrawJob& job) {
        const RasterVertex& v0 = job.v[0];
        const RasterVertex& v1 = job.v[1];
        const RasterVertex& v2 = job.v[2];
        const int32_t min_x = std::min(v0.x, std::min(v1.x, v2.x));
        const int32_t max_x = std::max(v0.x, std::max(v1.x, v2.x));
        const int32_t min_y = std::min(v0.y, std::min(v1.y, v2.y));
        const int32_t max_y = std::max(v0.y, std::max(v1.y, v2.y));
        if (max_x - min_x >= 1024 || max_y - min_y >= 512)
            return;
        const int32_t area = (v1.x - v0.x) * (v2.y - v0.y) - (v2.x - v0.x) * (v1.y - v0.y);
        const bool precise = v0.precise || v1.precise || v2.precise;
        if (area == 0 && !precise)
            return;

        BatchKey key;
        if (!KeyFor(job, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, &key))
            return;
        if (precise || jitter_phases_ > 0) {
            AddPreciseTriangle(job, key);
            return;
        }
        const int32_t left = std::max<int32_t>(min_x, key.scissor.left);
        const int32_t right = std::min<int32_t>(max_x, key.scissor.right);
        const int32_t top = std::max<int32_t>(min_y, key.scissor.top);
        const int32_t bottom = std::min<int32_t>(max_y, key.scissor.bottom);
        if (left >= right || top >= bottom)
            return;

        const RasterState& state = job.state;
        ReadyToRead(job, left, top, right, bottom,
                    std::min<int32_t>(v0.u, std::min(v1.u, v2.u)),
                    std::max<int32_t>(v0.u, std::max(v1.u, v2.u)),
                    std::min<int32_t>(v0.v, std::min(v1.v, v2.v)),
                    std::max<int32_t>(v0.v, std::max(v1.v, v2.v)));

        const RasterVertex& a = v0;
        const RasterVertex& b = (area > 0) ? v1 : v2;
        const RasterVertex& c = (area > 0) ? v2 : v1;
        const uint32_t rule = (EdgeLeftOut(b.x - a.x, b.y - a.y) ? 1u : 0u) |
                              (EdgeLeftOut(c.x - b.x, c.y - b.y) ? 2u : 0u) |
                              (EdgeLeftOut(a.x - c.x, a.y - c.y) ? 4u : 0u);
        uint32_t payload[kPayload] = {};
        payload[0] = PackXy(a.x, a.y);
        payload[1] = PackXy(b.x, b.y);
        payload[2] = PackXy(c.x, c.y);
        payload[3] = PackColor(a.r, a.g, a.b) | (static_cast<uint32_t>(a.u) << 24);
        payload[4] = PackColor(b.r, b.g, b.b) | (static_cast<uint32_t>(b.u) << 24);
        payload[5] = PackColor(c.r, c.g, c.b) | (static_cast<uint32_t>(c.u) << 24);
        payload[6] = a.v | (static_cast<uint32_t>(b.v) << 8) | (static_cast<uint32_t>(c.v) << 16) |
                     (rule << 24);
        payload[7] = static_cast<uint32_t>(area > 0 ? area : -area);
        payload[8] = Attributes(state, kKindTriangle, state.textured, state.dither);
        payload[9] = (state.clut_x & 1023) | ((state.clut_y & 511) << 10);
        PutMotion(payload, a, b, c);
        Begin(key, 6);
        AddBox(left, top, right, bottom, payload);
    }

    void D3D12Raster::AddPreciseTriangle(const DrawJob& job, const BatchKey& key) {
        struct Corner {
            float x, y, w;
            const RasterVertex* vertex;
        };
        Corner corners[3];
        for (int i = 0; i < 3; ++i) {
            const RasterVertex& v = job.v[i];
            corners[i] = { v.precise ? v.fx : static_cast<float>(v.x),
                           v.precise ? v.fy : static_cast<float>(v.y), v.precise ? v.w : 0.0f,
                           &v };
        }
        const float area = (corners[1].x - corners[0].x) * (corners[2].y - corners[0].y) -
                           (corners[2].x - corners[0].x) * (corners[1].y - corners[0].y);
        if (!(area != 0.0f))
            return;
        if (area < 0.0f)
            std::swap(corners[1], corners[2]);
        const Corner& a = corners[0];
        const Corner& b = corners[1];
        const Corner& c = corners[2];
        auto left_out = [](float dx, float dy) { return !((dy < 0.0f) || (dy == 0.0f && dx > 0.0f)); };
        const uint32_t rule = (left_out(b.x - a.x, b.y - a.y) ? 1u : 0u) |
                              (left_out(c.x - b.x, c.y - b.y) ? 2u : 0u) |
                              (left_out(a.x - c.x, a.y - c.y) ? 4u : 0u);

        const int32_t margin = jitter_phases_ > 0 ? 1 : 0;
        const int32_t left = std::max<int32_t>(
            static_cast<int32_t>(std::floor(std::min(a.x, std::min(b.x, c.x)))) - margin,
            key.scissor.left);
        const int32_t right = std::min<int32_t>(
            static_cast<int32_t>(std::floor(std::max(a.x, std::max(b.x, c.x)))) + 1 + margin,
            key.scissor.right);
        const int32_t top = std::max<int32_t>(
            static_cast<int32_t>(std::floor(std::min(a.y, std::min(b.y, c.y)))) - margin,
            key.scissor.top);
        const int32_t bottom = std::min<int32_t>(
            static_cast<int32_t>(std::floor(std::max(a.y, std::max(b.y, c.y)))) + 1 + margin,
            key.scissor.bottom);
        if (left >= right || top >= bottom)
            return;

        const RasterState& state = job.state;
        const RasterVertex& v0 = job.v[0];
        const RasterVertex& v1 = job.v[1];
        const RasterVertex& v2 = job.v[2];
        ReadyToRead(job, left, top, right, bottom,
                    std::min<int32_t>(v0.u, std::min(v1.u, v2.u)),
                    std::max<int32_t>(v0.u, std::max(v1.u, v2.u)),
                    std::min<int32_t>(v0.v, std::min(v1.v, v2.v)),
                    std::max<int32_t>(v0.v, std::max(v1.v, v2.v)));

        auto bits = [](float f) {
            uint32_t word;
            memcpy(&word, &f, sizeof(word));
            return word;
        };
        const bool perspective = a.w > 0.0f && b.w > 0.0f && c.w > 0.0f;
        uint32_t payload[kPayload] = {};
        payload[0] = bits(a.x);
        payload[1] = bits(a.y);
        payload[2] = bits(b.x);
        payload[7] = bits(b.y);
        payload[10] = bits(c.x);
        payload[11] = bits(c.y);
        const RasterVertex& va = *a.vertex;
        const RasterVertex& vb = *b.vertex;
        const RasterVertex& vc = *c.vertex;
        payload[3] = PackColor(va.r, va.g, va.b) | (static_cast<uint32_t>(va.u) << 24);
        payload[4] = PackColor(vb.r, vb.g, vb.b) | (static_cast<uint32_t>(vb.u) << 24);
        payload[5] = PackColor(vc.r, vc.g, vc.b) | (static_cast<uint32_t>(vc.u) << 24);
        payload[6] = va.v | (static_cast<uint32_t>(vb.v) << 8) | (static_cast<uint32_t>(vc.v) << 16);
        payload[8] = Attributes(state, kKindPrecise, state.textured, state.dither);
        payload[9] = (state.clut_x & 1023) | ((state.clut_y & 511) << 10);
        payload[12] = perspective ? bits(a.w) : 0;
        payload[13] = perspective ? bits(b.w) : 0;
        payload[14] = perspective ? bits(c.w) : 0;
        payload[15] = rule;
        PutMotion(payload, va, vb, vc);
        Begin(key, 6);
        AddBox(left, top, right, bottom, payload);
    }

    void D3D12Raster::AddRectangle(const DrawJob& job) {
        if (job.w <= 0 || job.h <= 0)
            return;
        BatchKey key;
        if (!KeyFor(job, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, &key))
            return;
        const int32_t left = std::max<int32_t>(job.x, key.scissor.left);
        const int32_t right = std::min<int32_t>(job.x + job.w, key.scissor.right);
        const int32_t top = std::max<int32_t>(job.y, key.scissor.top);
        const int32_t bottom = std::min<int32_t>(job.y + job.h, key.scissor.bottom);
        if (left >= right || top >= bottom)
            return;

        const RasterState& state = job.state;
        const int32_t base_u = job.base_u, base_v = job.base_v;
        const int32_t first_col = left - job.x, last_col = right - 1 - job.x;
        const int32_t first_row = top - job.y, last_row = bottom - 1 - job.y;
        ReadyToRead(job, left, top, right, bottom,
                    state.flip_x ? base_u - last_col : base_u + first_col,
                    state.flip_x ? base_u - first_col : base_u + last_col,
                    state.flip_y ? base_v - last_row : base_v + first_row,
                    state.flip_y ? base_v - first_row : base_v + last_row);

        uint32_t payload[kPayload] = {};
        payload[0] = PackXy(job.x, job.y);
        payload[3] = PackColor(job.r, job.g, job.b) | (static_cast<uint32_t>(base_u) << 24);
        payload[6] = static_cast<uint32_t>(base_v) | (state.flip_x ? 0x100u : 0) |
                     (state.flip_y ? 0x200u : 0);
        payload[8] = Attributes(state, kKindRectangle, state.textured, false);   // never dithered
        payload[9] = (state.clut_x & 1023) | ((state.clut_y & 511) << 10);
        if (job.moved) {
            payload[16] = PackMotion(job.mx, job.my);
            payload[19] = 7;
        }
        Begin(key, 6);
        AddBox(left, top, right, bottom, payload);
    }

    void D3D12Raster::AddLine(const DrawJob& job) {
        const RasterVertex& v0 = job.v[0];
        const RasterVertex& v1 = job.v[1];
        const bool points = scale_ == 1;
        BatchKey key;
        if (!KeyFor(job, points ? D3D_PRIMITIVE_TOPOLOGY_POINTLIST
                                : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST, &key))
            return;
        const int32_t left = std::max<int32_t>(std::min(v0.x, v1.x), key.scissor.left);
        const int32_t right = std::min<int32_t>(std::max(v0.x, v1.x) + 1, key.scissor.right);
        const int32_t top = std::max<int32_t>(std::min(v0.y, v1.y), key.scissor.top);
        const int32_t bottom = std::min<int32_t>(std::max(v0.y, v1.y) + 1, key.scissor.bottom);
        if (left >= right || top >= bottom)
            return;
        ReadyToRead(job, left, top, right, bottom, 0, 0, 0, 0);

        const int32_t dx = std::abs(v1.x - v0.x);
        const int32_t dy = -std::abs(v1.y - v0.y);
        const int32_t step_x = (v0.x < v1.x) ? 1 : -1;
        const int32_t step_y = (v0.y < v1.y) ? 1 : -1;
        const int32_t steps = std::max(dx, -dy);
        Begin(key, (static_cast<size_t>(steps) + 1) * (points ? 1 : 6));

        Vertex point = {};
        point.p[8] = Attributes(job.state, kKindFlat, false, job.state.dither);
        int32_t x = v0.x;
        int32_t y = v0.y;
        int32_t error = dx + dy;
        for (int32_t i = 0; ; ++i) {
            uint8_t r = v0.r, g = v0.g, b = v0.b;
            if (job.state.gouraud && steps > 0) {
                r = Clamp8(v0.r + ((v1.r - v0.r) * i) / steps);
                g = Clamp8(v0.g + ((v1.g - v0.g) * i) / steps);
                b = Clamp8(v0.b + ((v1.b - v0.b) * i) / steps);
            }
            point.p[3] = PackColor(r, g, b);
            if (points) {
                point.x = x + 0.5f;
                point.y = y + 0.5f;
                batch_.push_back(point);
            } else {
                const float corners[6][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 },
                                              { 1, 0 }, { 1, 1 }, { 0, 1 } };
                for (const auto& corner : corners) {
                    point.x = x + corner[0];
                    point.y = y + corner[1];
                    batch_.push_back(point);
                }
            }
            if (x == v1.x && y == v1.y)
                break;
            const int32_t error2 = 2 * error;
            if (error2 >= dy) { error += dy; x += step_x; }
            if (error2 <= dx) { error += dx; y += step_y; }
        }
        MarkDrawn(left, top, right, bottom);
    }

    void D3D12Raster::Fill(const DrawJob& job) {
        const int32_t right = std::min(job.x + job.w, kWidth);
        const int32_t bottom = std::min(job.y + job.h, kHeight);
        if (job.x >= right || job.y >= bottom)
            return;
        BatchKey key;
        memset(&key, 0, sizeof(key));
        key.topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        key.scissor.right = kWidth;
        key.scissor.bottom = kHeight;
        key.shader = raster::kShaderDraw;
        key.constants.skip_field = job.env.skip_field ? 1 : 0;
        key.constants.active_line_lsb = static_cast<int32_t>(job.env.active_line_lsb & 1);
        uint32_t payload[kPayload] = {};
        payload[3] = ToCard(job.fill_colour) & 0x00FFFFFF;
        payload[8] = kKindFlat << 27;
        payload[15] = 1;   // a fill, not a line's pixel: to the plane, a background standing still
        Begin(key, 6);
        AddBox(job.x, job.y, right, bottom, payload);
    }

    void D3D12Raster::Copy(const DrawJob& job) {
        const int32_t across = (job.x - job.src_x) & (kWidth - 1);
        const int32_t down = (job.y - job.src_y) & (kHeight - 1);
        if ((across < job.w || kWidth - across < job.w) &&
            (down < job.h || kHeight - down < job.h)) {
            CopyInPlace(job);
            return;
        }
        Fresh(job.src_x, job.src_y, job.w, job.h);
        if (job.env.check_mask)
            Fresh(job.x, job.y, job.w, job.h);

        BatchKey key;
        memset(&key, 0, sizeof(key));
        key.topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        key.scissor.right = kWidth;
        key.scissor.bottom = kHeight;
        key.shader = raster::kShaderCopy;
        key.constants.force_mask = job.env.force_set_mask ? 1 : 0;
        key.constants.check_mask = job.env.check_mask ? 1 : 0;
        Begin(key, 24);
        ForEachPiece(job.x, job.y, job.w, job.h,
                     [&](int32_t x, int32_t y, int32_t w, int32_t h, int32_t col, int32_t row) {
            uint32_t payload[kPayload] = {};
            payload[0] = PackXy(x, y);
            payload[1] = PackXy(job.src_x + col, job.src_y + row);
            AddBox(x, y, x + w, y + h, payload);
        });
    }

    void D3D12Raster::CopyInPlace(const DrawJob& job) {
        PrepareRead(static_cast<uint32_t>(job.src_x), static_cast<uint32_t>(job.src_y),
                    static_cast<uint32_t>(job.w), static_cast<uint32_t>(job.h));
        PrepareRead(static_cast<uint32_t>(job.x), static_cast<uint32_t>(job.y),
                    static_cast<uint32_t>(job.w), static_cast<uint32_t>(job.h));
        auto at = [this](uint32_t x, uint32_t y) -> uint16_t& {
            return vram_[(y & (kHeight - 1)) * kWidth + (x & (kWidth - 1))];
        };
        for (int32_t row = 0; row < job.h; ++row) {
            for (int32_t col = 0; col < job.w; ++col) {
                const uint16_t pixel = at(job.src_x + col, job.src_y + row);
                uint16_t& target = at(job.x + col, job.y + row);
                if (job.env.check_mask && (target & 0x8000))
                    continue;
                target = job.env.force_set_mask ? static_cast<uint16_t>(pixel | 0x8000) : pixel;
            }
        }
        ForEachPiece(job.x, job.y, job.w, job.h,
                     [&](int32_t x, int32_t y, int32_t w, int32_t h, int32_t, int32_t) {
            Upload(x, y, w, h);
            for (int32_t ty = y / kTile; ty <= (y + h - 1) / kTile; ++ty)
                for (int32_t tx = x / kTile; tx <= (x + w - 1) / kTile; ++tx)
                    stale_[ty * kTilesX + tx] = 1;
        });
    }

    // ------------------------------------------------------------------------------------------
    // The read copy
    // ------------------------------------------------------------------------------------------

    void D3D12Raster::ReadyToRead(const DrawJob& job, int32_t left, int32_t top, int32_t right,
                                  int32_t bottom, int32_t umin, int32_t umax, int32_t vmin,
                                  int32_t vmax) {
        const RasterState& state = job.state;
        if (job.kind != DrawJob::kLine && state.textured) {
            if (job.env.tw_mask_x != 0 || umin < 0 || umax > 255) {
                umin = 0;
                umax = 255;
            }
            if (job.env.tw_mask_y != 0 || vmin < 0 || vmax > 255) {
                vmin = 0;
                vmax = 255;
            }
            int32_t first, last;
            switch (state.texpage_colors) {
            case 0: first = umin / 4; last = umax / 4; break;
            case 1: first = umin / 2; last = umax / 2; break;
            default: first = umin; last = umax; break;
            }
            Fresh(static_cast<int32_t>(state.texpage_x) + first,
                  static_cast<int32_t>(state.texpage_y) + vmin, last - first + 1, vmax - vmin + 1);
            if (state.texpage_colors == 0)
                Fresh(static_cast<int32_t>(state.clut_x), static_cast<int32_t>(state.clut_y), 16, 1);
            else if (state.texpage_colors == 1)
                Fresh(static_cast<int32_t>(state.clut_x), static_cast<int32_t>(state.clut_y), 256, 1);
        }
        if (state.semi_transparent || job.env.check_mask)
            Fresh(left, top, right - left, bottom - top);
    }

    void D3D12Raster::Fresh(int32_t x, int32_t y, int32_t w, int32_t h) {
        bool any = false, flush = false;
        ForEachPiece(x, y, w, h, [&](int32_t px, int32_t py, int32_t pw, int32_t ph, int32_t,
                                     int32_t) {
            for (int32_t ty = py / kTile; ty <= (py + ph - 1) / kTile; ++ty)
                for (int32_t tx = px / kTile; tx <= (px + pw - 1) / kTile; ++tx)
                    if (stale_[ty * kTilesX + tx]) {
                        any = true;
                        flush = flush || batch_tile_[ty * kTilesX + tx] == batch_serial_;
                    }
        });
        if (!any)
            return;
        if (flush)
            Flush();
        const bool with_planes = planes();
        Use(target_, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Use(read_copy_, D3D12_RESOURCE_STATE_COPY_DEST);
        if (with_planes) {
            Use(plane_target_, D3D12_RESOURCE_STATE_COPY_SOURCE);
            Use(plane_read_copy_, D3D12_RESOURCE_STATE_COPY_DEST);
        }
        ID3D12GraphicsCommandList* list = List();
        auto copy = [list](ID3D12Resource* to, ID3D12Resource* from, const D3D12_BOX& box) {
            D3D12_TEXTURE_COPY_LOCATION destination = {};
            destination.pResource = to;
            destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            D3D12_TEXTURE_COPY_LOCATION source = {};
            source.pResource = from;
            source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            list->CopyTextureRegion(&destination, box.left, box.top, 0, &source, &box);
        };
        ForEachPiece(x, y, w, h, [&](int32_t px, int32_t py, int32_t pw, int32_t ph, int32_t,
                                     int32_t) {
            const int32_t last_tx = (px + pw - 1) / kTile;
            for (int32_t ty = py / kTile; ty <= (py + ph - 1) / kTile; ++ty) {
                for (int32_t tx = px / kTile; tx <= last_tx; ) {
                    if (!stale_[ty * kTilesX + tx]) {
                        ++tx;
                        continue;
                    }
                    int32_t end = tx;
                    while (end <= last_tx && stale_[ty * kTilesX + end])
                        stale_[ty * kTilesX + end++] = 0;
                    const D3D12_BOX box = Scaled(tx * kTile, ty * kTile, end * kTile,
                                                 (ty + 1) * kTile);
                    copy(read_copy_.resource.Get(), target_.resource.Get(), box);
                    // The plane's read copy goes with it: whatever draws into a tile draws
                    // into both, so they are stale together.
                    if (with_planes)
                        copy(plane_read_copy_.resource.Get(), plane_target_.resource.Get(), box);
                    tx = end;
                }
            }
        });
    }

    // ------------------------------------------------------------------------------------------
    // Keeping native VRAM in step
    // ------------------------------------------------------------------------------------------

    void D3D12Raster::MarkDrawn(int32_t left, int32_t top, int32_t right, int32_t bottom) {
        left = std::max(left, 0);
        top = std::max(top, 0);
        right = std::min(right, kWidth);
        bottom = std::min(bottom, kHeight);
        if (left >= right || top >= bottom)
            return;
        ++drawn_serial_;
        for (int32_t ty = top / kTile; ty <= (bottom - 1) / kTile; ++ty) {
            for (int32_t tx = left / kTile; tx <= (right - 1) / kTile; ++tx) {
                dirty_[ty * kTilesX + tx] = 1;
                stale_[ty * kTilesX + tx] = 1;
                batch_tile_[ty * kTilesX + tx] = batch_serial_;
            }
        }
    }

    void D3D12Raster::PrepareRead(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
        if (last_read_valid_ && drawn_serial_ == last_read_serial_ && x == last_read_[0] &&
            y == last_read_[1] && w == last_read_[2] && h == last_read_[3])
            return;
        last_read_[0] = x;
        last_read_[1] = y;
        last_read_[2] = w;
        last_read_[3] = h;
        last_read_serial_ = drawn_serial_;
        last_read_valid_ = true;
        Flush();

        std::vector<uint8_t>& wanted = wanted_;
        std::fill(wanted.begin(), wanted.end(), 0);
        bool any = false;
        ForEachPiece(static_cast<int32_t>(x), static_cast<int32_t>(y), static_cast<int32_t>(w),
                     static_cast<int32_t>(h),
                     [&](int32_t px, int32_t py, int32_t pw, int32_t ph, int32_t, int32_t) {
            for (int32_t ty = py / kTile; ty <= (py + ph - 1) / kTile; ++ty)
                for (int32_t tx = px / kTile; tx <= (px + pw - 1) / kTile; ++tx)
                    if (dirty_[ty * kTilesX + tx]) {
                        wanted[ty * kTilesX + tx] = 1;
                        any = true;
                    }
        });
        if (!any)
            return;

        // Copied to the read-back buffer a run of tiles at a time, in VRAM's own layout, then
        // read in one go. Above 1x each run is first brought down to native size, a console
        // pixel's own sub-pixel apiece.
        std::vector<D3D12_RECT> runs;
        for (int32_t ty = 0; ty < kTilesY; ++ty) {
            for (int32_t tx = 0; tx < kTilesX; ) {
                if (!wanted[ty * kTilesX + tx]) {
                    ++tx;
                    continue;
                }
                int32_t end = tx;
                while (end < kTilesX && wanted[ty * kTilesX + end])
                    ++end;
                runs.push_back({ tx * kTile, ty * kTile, end * kTile, (ty + 1) * kTile });
                tx = end;
            }
        }
        if (scale_ > 1)
            Pass(raster::kShaderDownsample, true, runs);
        Texture& from = scale_ > 1 ? native_target_ : target_;
        Use(from, D3D12_RESOURCE_STATE_COPY_SOURCE);
        const UINT pitch = kWidth * 4;
        for (const D3D12_RECT& run : runs) {
            const D3D12_BOX box = { static_cast<UINT>(run.left), static_cast<UINT>(run.top), 0,
                                    static_cast<UINT>(run.right), static_cast<UINT>(run.bottom), 1 };
            CopyToBuffer(vram_readback_.Get(), kWidth, kHeight, pitch, DXGI_FORMAT_R8G8B8A8_UNORM,
                         box.left, box.top, from.resource.Get(), box);
        }
        if (!Finish())
            return;
        void* mapped = nullptr;
        const D3D12_RANGE range = { 0, static_cast<SIZE_T>(pitch) * kHeight };
        if (FAILED(vram_readback_->Map(0, &range, &mapped))) {
            CheckDevice();
            return;
        }
        for (int32_t tile = 0; tile < kTilesX * kTilesY; ++tile) {
            if (!wanted[tile])
                continue;
            const int32_t left = (tile % kTilesX) * kTile;
            const int32_t top = (tile / kTilesX) * kTile;
            for (int32_t row = top; row < top + kTile; ++row) {
                const uint32_t* source = reinterpret_cast<const uint32_t*>(
                    static_cast<const uint8_t*>(mapped) + static_cast<size_t>(row) * pitch) + left;
                uint16_t* destination = vram_ + row * kWidth + left;
                for (int32_t col = 0; col < kTile; ++col)
                    destination[col] = FromCard(source[col]);
            }
            dirty_[tile] = 0;
        }
        const D3D12_RANGE none = { 0, 0 };
        vram_readback_->Unmap(0, &none);
    }

    void D3D12Raster::Written(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
        Flush();
        ForEachPiece(static_cast<int32_t>(x), static_cast<int32_t>(y), static_cast<int32_t>(w),
                     static_cast<int32_t>(h),
                     [&](int32_t px, int32_t py, int32_t pw, int32_t ph, int32_t, int32_t) {
            Upload(px, py, pw, ph);
            for (int32_t ty = py / kTile; ty <= (py + ph - 1) / kTile; ++ty)
                for (int32_t tx = px / kTile; tx <= (px + pw - 1) / kTile; ++tx)
                    stale_[ty * kTilesX + tx] = 1;
            for (int32_t ty = (py + kTile - 1) / kTile; ty < (py + ph) / kTile; ++ty)
                for (int32_t tx = (px + kTile - 1) / kTile; tx < (px + pw) / kTile; ++tx)
                    dirty_[ty * kTilesX + tx] = 0;
        });
    }

    void D3D12Raster::Reloaded() {
        Flush();
        Upload(0, 0, kWidth, kHeight);
        std::fill(dirty_.begin(), dirty_.end(), 0);
        std::fill(stale_.begin(), stale_.end(), 1);
    }

    void D3D12Raster::CheckDevice() {
        if (!lost_.empty() || !device_)
            return;
        const HRESULT reason = device_->GetDeviceRemovedReason();
        if (reason == S_OK)
            return;
        char text[96];
        snprintf(text, sizeof(text), "the graphics card stopped answering (0x%08lX)",
                 static_cast<unsigned long>(reason));
        lost_ = text;
    }

    void D3D12Raster::Upload(int32_t x, int32_t y, int32_t w, int32_t h) {
        const UINT pitch = RowPitch(static_cast<UINT>(w), 4);
        const UploadSpace memory =
            Allocate(static_cast<size_t>(pitch) * h, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
        if (memory.cpu == nullptr)
            return;
        for (int32_t row = 0; row < h; ++row) {
            const uint16_t* source = vram_ + (y + row) * kWidth + x;
            uint32_t* destination = reinterpret_cast<uint32_t*>(memory.cpu + static_cast<size_t>(row) * pitch);
            for (int32_t col = 0; col < w; ++col)
                destination[col] = ToCard(source[col]);
        }
        // Above 1x, to native VRAM's copy on the card first, then each pixel over its
        // sub-pixels: an uploaded picture looks just as it would at native size.
        Texture& into = scale_ > 1 ? native_source_ : target_;
        Use(into, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION destination = {};
        destination.pResource = into.resource.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION source = {};
        source.pResource = memory.buffer;
        source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source.PlacedFootprint.Offset = memory.offset;
        source.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, static_cast<UINT>(w),
                                             static_cast<UINT>(h), 1, pitch };
        List()->CopyTextureRegion(&destination, static_cast<UINT>(x), static_cast<UINT>(y), 0,
                                  &source, nullptr);
        if (scale_ > 1)
            Pass(raster::kShaderExpand, false, { { x, y, x + w, y + h } });
        if (planes())
            ForgetPlanes(x, y, w, h);
    }

    D3D12_BOX D3D12Raster::Scaled(int32_t left, int32_t top, int32_t right, int32_t bottom) const {
        return { static_cast<UINT>(left * scale_), static_cast<UINT>(top * scale_), 0,
                 static_cast<UINT>(right * scale_), static_cast<UINT>(bottom * scale_), 1 };
    }

    // Above 1x: a download's runs brought down to native size from the target, or an upload's
    // rectangle spread over its sub-pixels from native VRAM's copy on the card.
    void D3D12Raster::Pass(Shader shader, bool into_native, const std::vector<D3D12_RECT>& boxes) {
        if (boxes.empty())
            return;
        Flush();
        std::vector<Vertex> corners;
        corners.reserve(boxes.size() * 6);
        for (const D3D12_RECT& box : boxes) {
            const float l = static_cast<float>(box.left), t = static_cast<float>(box.top);
            const float r = static_cast<float>(box.right), b = static_cast<float>(box.bottom);
            const float points[6][2] = { { l, t }, { r, t }, { l, b }, { r, t }, { r, b }, { l, b } };
            for (const auto& point : points) {
                Vertex vertex = {};
                vertex.x = point[0];
                vertex.y = point[1];
                corners.push_back(vertex);
            }
        }
        const size_t bytes = corners.size() * sizeof(Vertex);
        const UploadSpace vertices = Allocate(bytes, 16);
        if (vertices.cpu == nullptr)
            return;
        memcpy(vertices.cpu, corners.data(), bytes);
        Texture& into = into_native ? native_target_ : target_;
        Texture& from = into_native ? target_ : native_source_;
        Use(from, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Use(into, D3D12_RESOURCE_STATE_RENDER_TARGET);
        // Positions are console pixels; the viewport says how big one is.
        const int size = into_native ? 1 : scale_;
        const D3D12_CPU_DESCRIPTOR_HANDLE target = RtvHandle(into_native ? kRtvNative : kRtvTarget);
        const D3D12_RECT scissor = { 0, 0, kWidth * size, kHeight * size };
        SetDrawState(shader == raster::kShaderDownsample ? kPipeDownsample : kPipeExpand,
                     into_native ? kTableTarget : kTableNative, &target, 1, kWidth * size,
                     kHeight * size, scissor);
        ID3D12GraphicsCommandList* list = List();
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const D3D12_VERTEX_BUFFER_VIEW view = { vertices.gpu, static_cast<UINT>(bytes),
                                                sizeof(Vertex) };
        list->IASetVertexBuffers(0, 1, &view);
        list->DrawInstanced(static_cast<UINT>(corners.size()), 1, 0, 0);
    }

    // ------------------------------------------------------------------------------------------
    // Showing
    // ------------------------------------------------------------------------------------------

    void D3D12Raster::DrawDisplay(D3D12_CPU_DESCRIPTOR_HANDLE into, Texture& texture, uint32_t x,
                                  uint32_t y, UINT width, UINT height) {
        // One box over the whole viewport - which is the picture's size - carrying where in
        // the target the picture starts.
        Vertex corners[6] = {};
        const float points[6][2] = { { 0, 0 }, { kWidth, 0 }, { 0, kHeight },
                                     { kWidth, 0 }, { kWidth, kHeight }, { 0, kHeight } };
        for (int i = 0; i < 6; ++i) {
            corners[i].x = points[i][0];
            corners[i].y = points[i][1];
            corners[i].p[0] = PackXy(static_cast<int32_t>(x) * scale_,
                                     static_cast<int32_t>(y) * scale_);
        }
        const UploadSpace vertices = Allocate(sizeof(corners), 16);
        if (vertices.cpu == nullptr)
            return;
        memcpy(vertices.cpu, corners, sizeof(corners));
        // The picture - or, for View > Depth and Motion, the plane in its place.
        Pipeline pipeline = kPipeDisplay;
        Table table = kTableTarget;
        Texture* source = &target_;
        if (planes() && shown_plane_ != emulation::psx::PlaneView::kPicture) {
            pipeline = shown_plane_ == emulation::psx::PlaneView::kDepth ? kPipeDisplayDepth
                                                                         : kPipeDisplayMotion;
            table = kTablePlane;
            source = &plane_target_;
        }
        Use(*source, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Use(texture, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const D3D12_RECT scissor = { 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
        SetDrawState(pipeline, table, &into, 1, width, height, scissor);
        ID3D12GraphicsCommandList* list = List();
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const D3D12_VERTEX_BUFFER_VIEW view = { vertices.gpu, static_cast<UINT>(sizeof(corners)),
                                                sizeof(Vertex) };
        list->IASetVertexBuffers(0, 1, &view);
        list->DrawInstanced(6, 1, 0, 0);
    }

    // The picture left on the card for the presenter's device (psx/shared_picture.h). Nothing
    // here waits: the picture's commands are sent and the fence signalled behind them, and the
    // presenter waits for the fence - on its own thread, or on the renderer's own device on the
    // card - before it draws from the texture.
    //
    // On the renderer's device the textures are ordinary ones: compressed as the card likes, and
    // left where any shader may read them (kHandedOver) rather than in the common state, which
    // would have the card unpack them at every hand-off. Otherwise each is made to be opened by
    // another device, and handed over in the common state, the one that device may open it in.
    bool D3D12Raster::ShareDisplay(uint32_t x, uint32_t y, UINT width, UINT height,
                                   emulation::psx::SharedPicture* shared) {
        static_assert(kPictureSlots == D3D12Pictures::kSlots, "one texture per shared slot");
        constexpr D3D12_RESOURCE_STATES kHandedOver = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;
        const D3D12_HEAP_FLAGS heap_flags = one_device_ ? D3D12_HEAP_FLAG_NONE
                                                        : D3D12_HEAP_FLAG_SHARED;
        const D3D12_RESOURCE_STATES handed_over = one_device_ ? kHandedOver
                                                              : D3D12_RESOURCE_STATE_COMMON;
        // A new texture's handle for another device; none on the renderer's own.
        auto share = [this](ID3D12Resource* resource, HANDLE* handle) {
            *handle = nullptr;
            return one_device_ || SUCCEEDED(device_->CreateSharedHandle(resource, nullptr,
                                                                        GENERIC_ALL, nullptr,
                                                                        handle));
        };

        D3D12Pictures& pictures = *pictures_;
        const int slot = pictures.Pick(width, height);
        if (slot < 0)
            return false;
        if (pictures.slot(slot).width != width || pictures.slot(slot).height != height ||
            !picture_textures_[slot].resource) {
            // The old texture may still be read by a list this rasteriser sent: let it finish.
            Finish();
            picture_textures_[slot] = Texture();
            picture_planes_[slot] = Texture();
            HANDLE handle = nullptr;
            if (!MakeTexture(&picture_textures_[slot], width, height, DXGI_FORMAT_B8G8R8A8_UNORM,
                             true, handed_over, heap_flags) ||
                !share(picture_textures_[slot].resource.Get(), &handle)) {
                // A card that will not share a texture will not share the next one either:
                // the picture is read back from here on.
                picture_textures_[slot] = Texture();
                pictures_.reset();
                CheckDevice();
                return false;
            }
            const D3D12_RESOURCE_DESC description = picture_textures_[slot].resource->GetDesc();
            const D3D12_RESOURCE_ALLOCATION_INFO allocation =
                device_->GetResourceAllocationInfo(0, 1, &description);
            pictures.Replace(slot, handle,
                             one_device_ ? picture_textures_[slot].resource.Get() : nullptr,
                             width, height, allocation.SizeInBytes);
            D3D12_RENDER_TARGET_VIEW_DESC rtv = {};
            rtv.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            device_->CreateRenderTargetView(picture_textures_[slot].resource.Get(), &rtv,
                                            RtvHandle(kRtvPicture + slot));
        }

        Texture& texture = picture_textures_[slot];
        DrawDisplay(RtvHandle(kRtvPicture + slot), texture, x, y, width, height);
        // The plane's same area beside it, while the plane is kept for DLSS - drawn before the
        // fence, so the presenter's one wait covers both. A card that will not share this one
        // still hands the picture over, without it.
        bool with_planes = false;
        if (keep_planes_ && planes()) {
            if (!picture_planes_[slot].resource || pictures.slot(slot).planes_texture() == nullptr) {
                picture_planes_[slot] = Texture();
                HANDLE handle = nullptr;
                if (MakeTexture(&picture_planes_[slot], width, height,
                                DXGI_FORMAT_R16G16B16A16_FLOAT, false, handed_over, heap_flags) &&
                    share(picture_planes_[slot].resource.Get(), &handle))
                    pictures.ReplacePlanes(
                        slot, handle, one_device_ ? picture_planes_[slot].resource.Get() : nullptr);
                else
                    picture_planes_[slot] = Texture();
            }
            if (picture_planes_[slot].resource) {
                const D3D12_BOX box = { x * scale_, y * scale_, 0, x * scale_ + width,
                                        y * scale_ + height, 1 };
                Use(plane_target_, D3D12_RESOURCE_STATE_COPY_SOURCE);
                Use(picture_planes_[slot], D3D12_RESOURCE_STATE_COPY_DEST);
                D3D12_TEXTURE_COPY_LOCATION destination = {};
                destination.pResource = picture_planes_[slot].resource.Get();
                destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                D3D12_TEXTURE_COPY_LOCATION source = {};
                source.pResource = plane_target_.resource.Get();
                source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                List()->CopyTextureRegion(&destination, 0, 0, 0, &source, &box);
                Use(picture_planes_[slot], handed_over);
                with_planes = true;
            }
        }
        Use(texture, handed_over);
        const uint64_t serial = ++picture_serial_;
        pictures.Drawn(slot, serial);
        Submit();
        if (FAILED(queue_->Signal(pictures.fence(), serial))) {
            CheckDevice();
            return false;
        }

        const D3D12Pictures::Slot& chosen = pictures.slot(slot);
        shared->source = pictures_;
        shared->texture = chosen.texture();
        shared->texture_id = chosen.id;
        shared->texture_bytes = chosen.bytes;
        shared->serial = serial;
        shared->width = static_cast<int>(width);
        shared->height = static_cast<int>(height);
        shared->planes = with_planes ? chosen.planes_texture() : nullptr;
        shared->planes_id = with_planes ? chosen.planes_id : 0;
        shared->jitter_x = jitter_phases_ > 0 ? shown_jitter_x_ : 0.0f;
        shared->jitter_y = jitter_phases_ > 0 ? shown_jitter_y_ : 0.0f;
        return true;
    }

    bool D3D12Raster::ResolveDisplay(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                                     std::vector<uint32_t>* picture,
                                     emulation::psx::SharedPicture* shared, int* scale) {
        if (scale_ == 1 || !lost_.empty() || w == 0 || h == 0 || x + w > kWidth ||
            y + h > kHeight)
            return false;
        Flush();
        const UINT width = w * scale_, height = h * scale_;
        if (picture_new_) {
            if (motion_check_ && planes())
                WarpCheck(x, y, width, height);
            picture_new_ = false;
        }

        if (pictures_ && ShareDisplay(x, y, width, height, shared)) {
            display_pending_ = false;
            *scale = scale_;
            return true;
        }
        if (!lost_.empty())
            return false;

        // Read back: drawn into a B8G8R8A8 texture, alpha opaque, whose rows are then the
        // presenters' own 0xFFRRGGBB words.
        if (!display_texture_.resource || display_width_ < width || display_height_ < height) {
            Finish();
            display_pending_ = false;
            const UINT texture_width = std::max(width, display_width_);
            const UINT texture_height = std::max(height, display_height_);
            display_pitch_ = RowPitch(texture_width, 4);
            if (!MakeTexture(&display_texture_, texture_width, texture_height,
                             DXGI_FORMAT_B8G8R8A8_UNORM, true, D3D12_RESOURCE_STATE_RENDER_TARGET) ||
                !MakeReadback(&display_readback_[0],
                              static_cast<size_t>(display_pitch_) * texture_height) ||
                !MakeReadback(&display_readback_[1],
                              static_cast<size_t>(display_pitch_) * texture_height)) {
                display_texture_ = Texture();
                return false;
            }
            D3D12_RENDER_TARGET_VIEW_DESC rtv = {};
            rtv.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            device_->CreateRenderTargetView(display_texture_.resource.Get(), &rtv,
                                            RtvHandle(kRtvDisplay));
            display_width_ = texture_width;
            display_height_ = texture_height;
            display_fence_[0] = display_fence_[1] = 0;
        }
        DrawDisplay(RtvHandle(kRtvDisplay), display_texture_, x, y, width, height);

        // Read back a frame behind, as D3D11Raster does: this frame's picture copied out now,
        // last frame's - which the card finished long ago - handed over. The first frame, or the
        // first at a new size, has nothing behind it and waits.
        const int copied = display_next_;
        display_next_ ^= 1;
        Use(display_texture_, D3D12_RESOURCE_STATE_COPY_SOURCE);
        const D3D12_BOX box = { 0, 0, 0, width, height, 1 };
        CopyToBuffer(display_readback_[copied].Get(), width, height, display_pitch_,
                     DXGI_FORMAT_B8G8R8A8_UNORM, 0, 0, display_texture_.resource.Get(), box);
        display_fence_[copied] = Submit();
        const bool behind = display_pending_ && pending_width_ == width &&
                            pending_height_ == height;
        const int read = behind ? copied ^ 1 : copied;
        display_pending_ = true;
        pending_width_ = width;
        pending_height_ = height;
        if (!WaitFor(display_fence_[read]))
            return false;
        void* mapped = nullptr;
        const D3D12_RANGE range = { 0, static_cast<SIZE_T>(display_pitch_) * height };
        if (FAILED(display_readback_[read]->Map(0, &range, &mapped))) {
            CheckDevice();
            return false;
        }
        picture->resize(static_cast<size_t>(width) * height);
        for (UINT row = 0; row < height; ++row)
            memcpy(picture->data() + static_cast<size_t>(row) * width,
                   static_cast<const uint8_t*>(mapped) + static_cast<size_t>(row) * display_pitch_,
                   width * sizeof(uint32_t));
        const D3D12_RANGE none = { 0, 0 };
        display_readback_[read]->Unmap(0, &none);
        *scale = scale_;
        return true;
    }

}   // namespace psxemu
