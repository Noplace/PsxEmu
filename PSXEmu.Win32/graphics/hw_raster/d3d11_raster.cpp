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
#include "graphics/hw_raster/d3d11_raster.h"

#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxgi.lib")

using emulation::psx::DrawJob;
using emulation::psx::RasterState;
using emulation::psx::RasterVertex;

namespace psxemu {

    // What both hardware rasterisers draw with: the shaders, the words each vertex carries for
    // them, and the arithmetic around both (raster_common.h).
    using namespace raster;

    // Shared pictures, the part any thread may touch (psx/shared_picture.h): each texture's NT
    // handle and bookkeeping, and the fence the rasteriser signals once it has drawn a picture.
    // Frames carry a reference to this, so it outlives the rasteriser while any are in flight -
    // and with it the handles, which keep the textures themselves alive after the rasteriser has
    // let go of them.
    class CardPictures : public emulation::psx::SharedPictureSource {
     public:
        // See D3D11Raster::kSharedPictures. A picture with none free is read back instead, so
        // this is a number for speed, not for being right.
        static constexpr int kSlots = D3D11Raster::kSharedPictures;

        struct Slot {
            // The rasteriser's: the handle, and what the texture behind it is - and the plane's
            // beside it, the same size, while the plane is kept.
            HANDLE handle = nullptr;
            UINT width = 0, height = 0;
            uint64_t id = 0;
            HANDLE planes = nullptr;
            uint64_t planes_id = 0;
            // Any thread's: the picture last drawn into it (0: none yet), and that same number
            // once the frame carrying it was dropped.
            std::atomic<uint64_t> serial{ 0 };
            std::atomic<uint64_t> dropped{ 0 };
        };

        CardPictures(Microsoft::WRL::ComPtr<ID3D11Fence> fence, uint64_t adapter)
            : fence_(std::move(fence)), adapter_(adapter),
              event_(CreateEventW(nullptr, FALSE, FALSE, nullptr)) {}

        ~CardPictures() override {
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
                // Looked at again after the wait: an event left set by an earlier wait that timed
                // out would otherwise end this one early.
                WaitForSingleObject(event_, static_cast<DWORD>(deadline - now));
            }
        }

        void Dropped(uint64_t serial) override {
            for (Slot& slot : slots_)
                if (slot.serial.load(std::memory_order_acquire) == serial)
                    slot.dropped.store(serial, std::memory_order_release);
        }

        uint64_t adapter() const override { return adapter_; }

        // The rasteriser's side. A slot nobody will read again: never used, older than the
        // picture the presenter's card has moved on to, or its frame dropped.
        bool Free(int index) const {
            const uint64_t serial = slots_[index].serial.load(std::memory_order_acquire);
            return serial == 0 || serial < released() ||
                   slots_[index].dropped.load(std::memory_order_acquire) == serial;
        }
        // A free slot for a picture this size - one already that size if there is one - or -1.
        int Pick(UINT width, UINT height) const {
            int any = -1;
            for (int i = 0; i < kSlots; ++i) {
                if (!Free(i))
                    continue;
                if (slots_[i].width == width && slots_[i].height == height)
                    return i;
                if (any < 0 || slots_[i].handle == nullptr)
                    any = i;
            }
            return any;
        }
        // A new texture behind `index`, whose old one nobody will read again - nor its plane,
        // which goes with it.
        void Replace(int index, HANDLE handle, UINT width, UINT height) {
            Slot& slot = slots_[index];
            if (slot.handle != nullptr)
                CloseHandle(slot.handle);
            slot.handle = handle;
            slot.width = width;
            slot.height = height;
            slot.id = next_texture_id.fetch_add(1, std::memory_order_relaxed);
            ReplacePlanes(index, nullptr);
        }
        // A plane texture beside `index`'s picture, the same size.
        void ReplacePlanes(int index, HANDLE handle) {
            Slot& slot = slots_[index];
            if (slot.planes != nullptr)
                CloseHandle(slot.planes);
            slot.planes = handle;
            slot.planes_id = handle != nullptr ? next_texture_id.fetch_add(1, std::memory_order_relaxed)
                                               : 0;
        }
        // Picture `serial` is being drawn into `index`.
        void Drawn(int index, uint64_t serial) {
            slots_[index].dropped.store(0, std::memory_order_relaxed);
            slots_[index].serial.store(serial, std::memory_order_release);
        }
        const Slot& slot(int index) const { return slots_[index]; }
        ID3D11Fence* fence() const { return fence_.Get(); }

     private:
        Slot slots_[kSlots];
        Microsoft::WRL::ComPtr<ID3D11Fence> fence_;
        const uint64_t adapter_;
        HANDLE event_;
        std::mutex wait_mutex_;
    };

    D3D11Raster::~D3D11Raster() = default;

    bool D3D11Raster::ReadSharedPicture(const emulation::psx::SharedPicture& picture,
                                        std::vector<uint32_t>* pixels) {
        if (!picture || picture.width <= 0 || picture.height <= 0 ||
            !picture.source->WaitReady(picture.serial, 1000))
            return false;
        const uint64_t adapter_luid = picture.source->adapter();
        LUID luid = {};
        luid.LowPart = static_cast<DWORD>(adapter_luid & 0xFFFFFFFFu);
        luid.HighPart = static_cast<LONG>(adapter_luid >> 32);
        Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        Microsoft::WRL::ComPtr<ID3D11Device> device;
        Microsoft::WRL::ComPtr<ID3D11Device1> device1;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> shared, staging;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) ||
            FAILED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter))) ||
            FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr,
                                     0, D3D11_SDK_VERSION, &device, nullptr, &context)) ||
            FAILED(device.As(&device1)) ||
            FAILED(device1->OpenSharedResource1(static_cast<HANDLE>(picture.texture),
                                                IID_PPV_ARGS(&shared))))
            return false;
        D3D11_TEXTURE2D_DESC texture = {};
        shared->GetDesc(&texture);
        if (texture.Width != static_cast<UINT>(picture.width) ||
            texture.Height != static_cast<UINT>(picture.height))
            return false;
        texture.Usage = D3D11_USAGE_STAGING;
        texture.BindFlags = 0;
        texture.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        texture.MiscFlags = 0;
        if (FAILED(device->CreateTexture2D(&texture, nullptr, &staging)))
            return false;
        context->CopyResource(staging.Get(), shared.Get());
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
            return false;
        pixels->resize(static_cast<size_t>(picture.width) * picture.height);
        for (int row = 0; row < picture.height; ++row)
            memcpy(pixels->data() + static_cast<size_t>(row) * picture.width,
                   static_cast<const uint8_t*>(mapped.pData) + row * mapped.RowPitch,
                   static_cast<size_t>(picture.width) * sizeof(uint32_t));
        context->Unmap(staging.Get(), 0);
        return true;
    }

    std::unique_ptr<D3D11Raster> D3D11Raster::Create(uint16_t* vram,
                                                     const emulation::psx::RasterOptions& options,
                                                     bool warp, std::string* error) {
        emulation::psx::RasterOptions checked = options;
        checked.scale = std::min(std::max(options.scale, 1), 8);
        std::unique_ptr<D3D11Raster> raster(new D3D11Raster(vram, checked));
        if (!raster->Initialize(warp, error))
            return nullptr;
        return raster;
    }

    bool D3D11Raster::Initialize(bool warp, std::string* error) {
        const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                             D3D_FEATURE_LEVEL_10_0 };
        // The presenter's card when the front end names it, so the picture can stay there: a
        // texture can only be shared on the card it was made on. Otherwise Windows' default,
        // the one driving the screen - on a laptop with two, the integrated one - and not the
        // faster one on purpose: a picture read back comes through system memory, which an
        // integrated card shares and a separate one has to cross PCIe for. Measured on a Radeon
        // 780M beside an RTX 4060 Laptop GPU, Ridge Racer at 4x reading its picture back ran at
        // 1.29x real time on the 780M and 0.57x on the 4060.
        Microsoft::WRL::ComPtr<IDXGIAdapter1> chosen;
        if (!warp && adapter_ != 0) {
            Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
            LUID luid = {};
            luid.LowPart = static_cast<DWORD>(adapter_ & 0xFFFFFFFFu);
            luid.HighPart = static_cast<LONG>(adapter_ >> 32);
            if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) ||
                FAILED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&chosen))))
                chosen.Reset();
        }
        HRESULT result = D3D11CreateDevice(
            chosen.Get(),
            chosen ? D3D_DRIVER_TYPE_UNKNOWN : warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE,
            nullptr, 0, levels, ARRAYSIZE(levels), D3D11_SDK_VERSION, &device_, nullptr, &context_);
        if (FAILED(result) && chosen) {
            // The card asked for cannot make a Direct3D 11 device: better the picture from
            // Windows' default one than none at all. It is not the card the renderer is on, so
            // StartSharing sees the mismatch and the pictures are read back.
            chosen.Reset();
            result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels,
                                       ARRAYSIZE(levels), D3D11_SDK_VERSION, &device_, nullptr,
                                       &context_);
        }
        if (SUCCEEDED(result)) {
            // Which card it is, for saying so, and for a presenter to check it is its own.
            Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_device;
            Microsoft::WRL::ComPtr<IDXGIAdapter> used;
            DXGI_ADAPTER_DESC description = {};
            if (SUCCEEDED(device_.As(&dxgi_device)) && SUCCEEDED(dxgi_device->GetAdapter(&used)) &&
                SUCCEEDED(used->GetDesc(&description))) {
                char name[128] = {};
                WideCharToMultiByte(CP_UTF8, 0, description.Description, -1, name, sizeof(name) - 1,
                                    nullptr, nullptr);
                adapter_name_ = name;
                adapter_luid_ = (static_cast<uint64_t>(static_cast<uint32_t>(
                                     description.AdapterLuid.HighPart)) << 32) |
                                description.AdapterLuid.LowPart;
            }
        }
        if (FAILED(result)) {
            *error = warp ? "Direct3D 11's WARP device could not be made"
                          : "no graphics card here offers Direct3D 11 at feature level 10.0 or above";
            return false;
        }

        // The rasteriser's thread draws and the machine's reads, one after the other and never
        // together. That is all an immediate context asks, but the lock costs nothing that
        // matters and turns a mistake into a stall rather than a corrupt context.
        Microsoft::WRL::ComPtr<ID3D11Multithread> multithread;
        if (SUCCEEDED(context_.As(&multithread)))
            multithread->SetMultithreadProtected(TRUE);

        // VRAM at scale_: 8192x4096 at 8x, which feature level 10.0's limit still holds, and
        // 128 MB twice over - the target and its read copy.
        D3D11_TEXTURE2D_DESC texture = {};
        texture.Width = kWidth * scale_;
        texture.Height = kHeight * scale_;
        texture.MipLevels = 1;
        texture.ArraySize = 1;
        texture.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        texture.SampleDesc.Count = 1;
        texture.Usage = D3D11_USAGE_DEFAULT;
        texture.BindFlags = D3D11_BIND_RENDER_TARGET |
                            (scale_ > 1 ? D3D11_BIND_SHADER_RESOURCE : 0);
        if (FAILED(device_->CreateTexture2D(&texture, nullptr, &target_)) ||
            FAILED(device_->CreateRenderTargetView(target_.Get(), nullptr, &target_view_)) ||
            (scale_ > 1 && FAILED(device_->CreateShaderResourceView(target_.Get(), nullptr,
                                                                      &target_source_)))) {
            *error = "the graphics card could not hold VRAM at " + std::to_string(scale_) +
                     "x its size";
            return false;
        }
        texture.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device_->CreateTexture2D(&texture, nullptr, &read_copy_)) ||
            FAILED(device_->CreateShaderResourceView(read_copy_.Get(), nullptr,
                                                     &read_copy_view_))) {
            *error = "the graphics card could not hold a second copy of VRAM at " +
                     std::to_string(scale_) + "x its size";
            return false;
        }
        texture.Width = kWidth;
        texture.Height = kHeight;
        if (scale_ > 1) {
            texture.BindFlags = D3D11_BIND_RENDER_TARGET;
            if (FAILED(device_->CreateTexture2D(&texture, nullptr, &native_target_)) ||
                FAILED(device_->CreateRenderTargetView(native_target_.Get(), nullptr,
                                                       &native_target_view_))) {
                *error = "the hardware rasteriser could not make its download target";
                return false;
            }
            texture.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(device_->CreateTexture2D(&texture, nullptr, &native_source_)) ||
                FAILED(device_->CreateShaderResourceView(native_source_.Get(), nullptr,
                                                         &native_source_view_))) {
                *error = "the hardware rasteriser could not make its upload texture";
                return false;
            }
        }
        texture.Usage = D3D11_USAGE_STAGING;
        texture.BindFlags = 0;
        texture.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(device_->CreateTexture2D(&texture, nullptr, &staging_))) {
            *error = "the hardware rasteriser could not make its read-back texture";
            return false;
        }

        Microsoft::WRL::ComPtr<ID3DBlob> vertex_blob, pixel_blobs[kShaderCount];
        Microsoft::WRL::ComPtr<ID3DBlob> plane_draw_blob, plane_copy_blob;
        auto compile = [&](const char* entry, const char* target, bool planes,
                           Microsoft::WRL::ComPtr<ID3DBlob>* blob) {
            return CompileShader("d3d11_raster", entry, target, scale_, true_color_, planes, blob,
                                 error);
        };
        if (!compile("VsMain", "vs_4_0", false, &vertex_blob))
            return false;
        for (int i = 0; i < kShaderCount; ++i)
            if (!compile(kShaderEntries[i], "ps_4_0", false, &pixel_blobs[i]))
                return false;
        if (!compile("PsDraw", "ps_4_0", true, &plane_draw_blob) ||
            !compile("PsCopy", "ps_4_0", true, &plane_copy_blob))
            return false;
        const D3D11_INPUT_ELEMENT_DESC elements[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(Vertex, x),
              D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "P", 0, DXGI_FORMAT_R32G32B32A32_UINT, 0, offsetof(Vertex, p),
              D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "P", 1, DXGI_FORMAT_R32G32B32A32_UINT, 0, offsetof(Vertex, p) + 16,
              D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "P", 2, DXGI_FORMAT_R32G32B32A32_UINT, 0, offsetof(Vertex, p) + 32,
              D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "P", 3, DXGI_FORMAT_R32G32B32A32_UINT, 0, offsetof(Vertex, p) + 48,
              D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "P", 4, DXGI_FORMAT_R32G32B32A32_UINT, 0, offsetof(Vertex, p) + 64,
              D3D11_INPUT_PER_VERTEX_DATA, 0 },
        };
        bool made = SUCCEEDED(device_->CreateVertexShader(vertex_blob->GetBufferPointer(),
                                                          vertex_blob->GetBufferSize(), nullptr,
                                                          &vertex_shader_)) &&
                    SUCCEEDED(device_->CreateInputLayout(elements, ARRAYSIZE(elements),
                                                         vertex_blob->GetBufferPointer(),
                                                         vertex_blob->GetBufferSize(), &layout_));
        for (int i = 0; i < kShaderCount && made; ++i)
            made = SUCCEEDED(device_->CreatePixelShader(pixel_blobs[i]->GetBufferPointer(),
                                                        pixel_blobs[i]->GetBufferSize(), nullptr,
                                                        &pixel_shaders_[i]));
        made = made &&
               SUCCEEDED(device_->CreatePixelShader(plane_draw_blob->GetBufferPointer(),
                                                    plane_draw_blob->GetBufferSize(), nullptr,
                                                    &plane_draw_shader_)) &&
               SUCCEEDED(device_->CreatePixelShader(plane_copy_blob->GetBufferPointer(),
                                                    plane_copy_blob->GetBufferSize(), nullptr,
                                                    &plane_copy_shader_));
        if (!made) {
            *error = "the hardware rasteriser could not make its shaders";
            return false;
        }

        D3D11_BUFFER_DESC buffer = {};
        buffer.ByteWidth = static_cast<UINT>(kBatchVertices * sizeof(Vertex));
        buffer.Usage = D3D11_USAGE_DYNAMIC;
        buffer.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        buffer.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(device_->CreateBuffer(&buffer, nullptr, &vertices_))) {
            *error = "the hardware rasteriser could not make its vertex buffer";
            return false;
        }
        buffer.ByteWidth = sizeof(Constants);
        buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(device_->CreateBuffer(&buffer, nullptr, &constants_))) {
            *error = "the hardware rasteriser could not make its constant buffer";
            return false;
        }

        // Nothing is culled, and everything is scissored. The shader does its own blending, so
        // the output merger only writes.
        D3D11_RASTERIZER_DESC raster = {};
        raster.FillMode = D3D11_FILL_SOLID;
        raster.CullMode = D3D11_CULL_NONE;
        raster.DepthClipEnable = TRUE;
        raster.ScissorEnable = TRUE;
        D3D11_BLEND_DESC blend = {};
        blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(device_->CreateRasterizerState(&raster, &raster_state_)) ||
            FAILED(device_->CreateBlendState(&blend, &blend_state_))) {
            *error = "the hardware rasteriser could not make its pipeline states";
            return false;
        }

        // The context is this rasteriser's alone, so what never changes is set once.
        const UINT stride = sizeof(Vertex);
        const UINT offset = 0;
        context_->IASetInputLayout(layout_.Get());
        context_->IASetVertexBuffers(0, 1, vertices_.GetAddressOf(), &stride, &offset);
        context_->VSSetShader(vertex_shader_.Get(), nullptr, 0);
        context_->PSSetConstantBuffers(0, 1, constants_.GetAddressOf());
        context_->RSSetState(raster_state_.Get());
        context_->OMSetBlendState(blend_state_.Get(), nullptr, 0xFFFFFFFF);
        D3D11_VIEWPORT viewport = {};
        viewport.Width = static_cast<float>(kWidth * scale_);
        viewport.Height = static_cast<float>(kHeight * scale_);
        viewport.MaxDepth = 1.0f;
        context_->RSSetViewports(1, &viewport);
        BindTargets();

        batch_.reserve(kBatchVertices);
        Reloaded();
        // Only above 1x is there a sharper picture to hand over at all.
        if (share_ && scale_ > 1)
            StartSharing();
        return true;
    }

    // What a shared picture needs beyond a texture: a fence to say when it is drawn, which only
    // Direct3D 11.4 has (Windows 10 1703). Without one, pictures are read back as before.
    void D3D11Raster::StartSharing() {
        Microsoft::WRL::ComPtr<ID3D11Device5> device5;
        Microsoft::WRL::ComPtr<ID3D11Fence> fence;
        // Only on the card the presenter draws on, which is the one asked for: a device that
        // ended up elsewhere - the card could not be found, or made a device - has nothing a
        // presenter could open, and its pictures are read back instead.
        if (adapter_ != 0 && adapter_luid_ != adapter_)
            return;
        if (adapter_luid_ == 0 || FAILED(device_.As(&device5)) || FAILED(context_.As(&context4_)) ||
            FAILED(device5->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
            context4_.Reset();
            return;
        }
        pictures_ = std::make_shared<CardPictures>(std::move(fence), adapter_luid_);
    }

    // ------------------------------------------------------------------------------------------
    // The plane beside VRAM (psx/shared_picture.h, Docs/DLSS-Plan.md)
    // ------------------------------------------------------------------------------------------

    void D3D11Raster::SetPlanes(bool keep, emulation::psx::PlaneView view) {
        const bool was = planes();
        Flush();   // what is batched is drawn the way it was batched
        keep_planes_ = keep;
        shown_plane_ = view;
        if ((keep || view != emulation::psx::PlaneView::kPicture) && !plane_target_ &&
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
        context_->OMSetBlendState(blend_state_.Get(), nullptr, 0xFFFFFFFF);
        BindTargets();
    }

    bool D3D11Raster::MakePlanes() {
        D3D11_BLEND_DESC blend = {};
        blend.IndependentBlendEnable = TRUE;
        blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        D3D11_RENDER_TARGET_BLEND_DESC& plane = blend.RenderTarget[1];
        plane.BlendEnable = TRUE;
        plane.SrcBlend = D3D11_BLEND_SRC_ALPHA;
        plane.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        plane.BlendOp = D3D11_BLEND_OP_ADD;
        plane.SrcBlendAlpha = D3D11_BLEND_ONE;
        plane.DestBlendAlpha = D3D11_BLEND_ZERO;
        plane.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        plane.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

        // VRAM's size at scale_, eight bytes a sub-pixel: 256 MB at 8x, and its read copy as much.
        D3D11_TEXTURE2D_DESC texture = {};
        texture.Width = kWidth * scale_;
        texture.Height = kHeight * scale_;
        texture.MipLevels = 1;
        texture.ArraySize = 1;
        texture.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        texture.SampleDesc.Count = 1;
        texture.Usage = D3D11_USAGE_DEFAULT;
        texture.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        bool made = SUCCEEDED(context_.As(&context1_)) &&
                    SUCCEEDED(device_->CreateBlendState(&blend, &plane_blend_state_)) &&
                    SUCCEEDED(device_->CreateTexture2D(&texture, nullptr, &plane_target_)) &&
                    SUCCEEDED(device_->CreateRenderTargetView(plane_target_.Get(), nullptr,
                                                              &plane_target_view_)) &&
                    SUCCEEDED(device_->CreateShaderResourceView(plane_target_.Get(), nullptr,
                                                                &plane_source_));
        texture.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        made = made && SUCCEEDED(device_->CreateTexture2D(&texture, nullptr, &plane_read_copy_)) &&
               SUCCEEDED(device_->CreateShaderResourceView(plane_read_copy_.Get(), nullptr,
                                                           &plane_read_copy_view_));
        if (!made) {
            plane_target_.Reset();
            plane_target_view_.Reset();
            plane_source_.Reset();
            plane_read_copy_.Reset();
            plane_read_copy_view_.Reset();
            plane_blend_state_.Reset();
            CheckDevice();
        }
        return made;
    }

    void D3D11Raster::BindTargets() {
        ID3D11RenderTargetView* const views[2] = { target_view_.Get(), plane_target_view_.Get() };
        context_->OMSetRenderTargets(planes() ? 2 : 1, views, nullptr);
    }

    void D3D11Raster::ForgetPlanes(int32_t x, int32_t y, int32_t w, int32_t h) {
        const float unknown[4] = { emulation::psx::kUnknownMotion, emulation::psx::kUnknownMotion,
                                   0.0f, 1.0f };
        const D3D11_RECT rect = { x * scale_, y * scale_, (x + w) * scale_, (y + h) * scale_ };
        context1_->ClearView(plane_target_view_.Get(), unknown, &rect, 1);
    }

    void D3D11Raster::NewPicture(bool reset) {
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

    void D3D11Raster::SetJitter(int phases) {
        jitter_phases_ = std::max(phases, 0);
        jitter_index_ = 1;
        jitter_x_ = jitter_phases_ > 0 ? Halton(1, 2) - 0.5f : 0.0f;
        jitter_y_ = jitter_phases_ > 0 ? Halton(1, 3) - 0.5f : 0.0f;
        shown_jitter_x_ = shown_jitter_y_ = 0.0f;
    }

    void D3D11Raster::WarpCheck(uint32_t x, uint32_t y, UINT width, UINT height) {
        if (!warp_colour_ || warp_width_ != width || warp_height_ != height) {
            warp_colour_.Reset();
            warp_plane_.Reset();
            warp_last_.clear();
            D3D11_TEXTURE2D_DESC texture = {};
            texture.Width = width;
            texture.Height = height;
            texture.MipLevels = 1;
            texture.ArraySize = 1;
            texture.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            texture.SampleDesc.Count = 1;
            texture.Usage = D3D11_USAGE_STAGING;
            texture.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (FAILED(device_->CreateTexture2D(&texture, nullptr, &warp_colour_)))
                return;
            texture.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            if (FAILED(device_->CreateTexture2D(&texture, nullptr, &warp_plane_))) {
                warp_colour_.Reset();
                return;
            }
            warp_width_ = width;
            warp_height_ = height;
        }
        const D3D11_BOX box = { x * scale_, y * scale_, 0, x * scale_ + width, y * scale_ + height, 1 };
        context_->CopySubresourceRegion(warp_colour_.Get(), 0, 0, 0, 0, target_.Get(), 0, &box);
        context_->CopySubresourceRegion(warp_plane_.Get(), 0, 0, 0, 0, plane_target_.Get(), 0, &box);
        D3D11_MAPPED_SUBRESOURCE colour, plane;
        if (FAILED(context_->Map(warp_colour_.Get(), 0, D3D11_MAP_READ, 0, &colour))) {
            CheckDevice();
            return;
        }
        if (FAILED(context_->Map(warp_plane_.Get(), 0, D3D11_MAP_READ, 0, &plane))) {
            context_->Unmap(warp_colour_.Get(), 0);
            CheckDevice();
            return;
        }
        std::vector<uint8_t> now(static_cast<size_t>(width) * height * 4);
        for (UINT row = 0; row < height; ++row)
            memcpy(now.data() + static_cast<size_t>(row) * width * 4,
                   static_cast<const uint8_t*>(colour.pData) + row * colour.RowPitch, width * 4);

        WarpSums(now, warp_last_, static_cast<const uint8_t*>(plane.pData), plane.RowPitch, width,
                 height, picture_reset_, &counters_);
        context_->Unmap(warp_plane_.Get(), 0);
        context_->Unmap(warp_colour_.Get(), 0);
        warp_last_.swap(now);
    }

    bool D3D11Raster::ReadPlanes(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                                 std::vector<float>* rgba) {
        if (!planes() || w == 0 || h == 0 || x + w > kWidth || y + h > kHeight)
            return false;
        Flush();
        D3D11_TEXTURE2D_DESC texture = {};
        texture.Width = w * scale_;
        texture.Height = h * scale_;
        texture.MipLevels = 1;
        texture.ArraySize = 1;
        texture.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        texture.SampleDesc.Count = 1;
        texture.Usage = D3D11_USAGE_STAGING;
        texture.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
        if (FAILED(device_->CreateTexture2D(&texture, nullptr, &staging)))
            return false;
        const D3D11_BOX box = Scaled(static_cast<int32_t>(x), static_cast<int32_t>(y),
                                     static_cast<int32_t>(x + w), static_cast<int32_t>(y + h));
        context_->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, plane_target_.Get(), 0, &box);
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (FAILED(context_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
            CheckDevice();
            return false;
        }
        const size_t values = static_cast<size_t>(texture.Width) * 4;
        rgba->resize(values * texture.Height);
        for (UINT row = 0; row < texture.Height; ++row) {
            const uint16_t* source = reinterpret_cast<const uint16_t*>(
                static_cast<const uint8_t*>(mapped.pData) + row * mapped.RowPitch);
            for (size_t i = 0; i < values; ++i)
                (*rgba)[row * values + i] = HalfToFloat(source[i]);
        }
        context_->Unmap(staging.Get(), 0);
        return true;
    }

    void D3D11Raster::Apply(const DrawJob& job) {
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

    void D3D11Raster::Flush() {
        if (batch_.empty())
            return;
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (SUCCEEDED(context_->Map(vertices_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            memcpy(mapped.pData, batch_.data(), batch_.size() * sizeof(Vertex));
            context_->Unmap(vertices_.Get(), 0);
        }
        else {
            CheckDevice();
        }
        if (SUCCEEDED(context_->Map(constants_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            memcpy(mapped.pData, &batch_key_.constants, sizeof(Constants));
            context_->Unmap(constants_.Get(), 0);
        }
        context_->IASetPrimitiveTopology(batch_key_.topology);
        const D3D11_RECT scissor = { batch_key_.scissor.left * scale_, batch_key_.scissor.top * scale_,
                                     batch_key_.scissor.right * scale_,
                                     batch_key_.scissor.bottom * scale_ };
        context_->RSSetScissorRects(1, &scissor);
        // While the plane is kept, draws and copies write it too: a draw blending it, a copy
        // carrying it across whole - and both reading its read copy, as they read VRAM's.
        const bool planes_kept = planes();
        const bool copy = batch_key_.shader == kShaderCopy;
        ID3D11PixelShader* shader = pixel_shaders_[batch_key_.shader].Get();
        if (planes_kept) {
            shader = copy ? plane_copy_shader_.Get() : plane_draw_shader_.Get();
            context_->OMSetBlendState(copy ? blend_state_.Get() : plane_blend_state_.Get(), nullptr,
                                      0xFFFFFFFF);
        }
        context_->PSSetShader(shader, nullptr, 0);
        // Bound only while drawing, since the read copies are also what refreshes copy into.
        ID3D11ShaderResourceView* const sources[2] = {
            read_copy_view_.Get(), planes_kept ? plane_read_copy_view_.Get() : nullptr
        };
        context_->PSSetShaderResources(0, 2, sources);
        context_->Draw(static_cast<UINT>(batch_.size()), 0);
        ID3D11ShaderResourceView* const none[2] = { nullptr, nullptr };
        context_->PSSetShaderResources(0, 2, none);
        batch_.clear();
        ++batch_serial_;
    }

    void D3D11Raster::Begin(const BatchKey& key, size_t count) {
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

    bool D3D11Raster::KeyFor(const DrawJob& job, D3D11_PRIMITIVE_TOPOLOGY topology,
                             BatchKey* key) {
        memset(key, 0, sizeof(BatchKey));
        key->topology = topology;
        // The drawing area's right and bottom are inclusive; a scissor's are not.
        key->scissor.left = std::max(job.env.area_left, 0);
        key->scissor.top = std::max(job.env.area_top, 0);
        key->scissor.right = std::min(job.env.area_right + 1, kWidth);
        key->scissor.bottom = std::min(job.env.area_bottom + 1, kHeight);
        if (key->scissor.left >= key->scissor.right || key->scissor.top >= key->scissor.bottom)
            return false;
        key->shader = kShaderDraw;
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

    void D3D11Raster::AddBox(int32_t left, int32_t top, int32_t right, int32_t bottom,
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
    // Drawing
    // ------------------------------------------------------------------------------------------

    void D3D11Raster::AddTriangle(const DrawJob& job) {
        const RasterVertex& v0 = job.v[0];
        const RasterVertex& v1 = job.v[1];
        const RasterVertex& v2 = job.v[2];
        // Hardware rejects any primitive spanning more than 1023x511, as the software
        // rasteriser does.
        const int32_t min_x = std::min(v0.x, std::min(v1.x, v2.x));
        const int32_t max_x = std::max(v0.x, std::max(v1.x, v2.x));
        const int32_t min_y = std::min(v0.y, std::min(v1.y, v2.y));
        const int32_t max_y = std::max(v0.y, std::max(v1.y, v2.y));
        if (max_x - min_x >= 1024 || max_y - min_y >= 512)
            return;
        const int32_t area = (v1.x - v0.x) * (v2.y - v0.y) - (v2.x - v0.x) * (v1.y - v0.y);
        const bool precise = v0.precise || v1.precise || v2.precise;
        // A triangle flat at whole pixels draws nothing on the console. Its unrounded vertices
        // may say otherwise, and then it is needed: far off, a strip of road thinner than a pixel
        // rounds flat, and at whole pixels its neighbours cover its row between them, but drawn
        // where they really are they leave exactly its band open. AddPreciseTriangle judges it
        // on the unrounded area instead.
        if (area == 0 && !precise)
            return;

        BatchKey key;
        if (!KeyFor(job, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST, &key))
            return;
        // Jittered (Docs/DLSS-Plan.md, phase 3), a triangle at whole pixels is drawn as a precise
        // one is: sampled where the jitter puts each sub-pixel, which the integer arithmetic
        // cannot do. It gives up the exact sub-pixel, as true colour does.
        if (precise || jitter_phases_ > 0) {
            AddPreciseTriangle(job, key);
            return;
        }
        // The triangle's own extent is half-open and the drawing area's inclusive, exactly as
        // the software rasteriser's loop bounds.
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

        // In the winding the software rasteriser turns every triangle to.
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

    // A triangle with PGXP's unrounded vertices (kind 3): each where the GTE put it - a vertex
    // PGXP had nothing for at its whole position - with the same fill rule, in floating point,
    // and textured in perspective when every vertex has a depth. Whether the console would
    // draw it at all was settled on the whole positions, as it decides.
    void D3D11Raster::AddPreciseTriangle(const DrawJob& job, const BatchKey& key) {
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
            std::swap(corners[1], corners[2]);   // the winding every triangle is turned to
        const Corner& a = corners[0];
        const Corner& b = corners[1];
        const Corner& c = corners[2];
        auto left_out = [](float dx, float dy) { return !((dy < 0.0f) || (dy == 0.0f && dx > 0.0f)); };
        const uint32_t rule = (left_out(b.x - a.x, b.y - a.y) ? 1u : 0u) |
                              (left_out(c.x - b.x, c.y - b.y) ? 2u : 0u) |
                              (left_out(a.x - c.x, a.y - c.y) ? 4u : 0u);

        // Jittered, a sub-pixel just beyond the triangle's own pixels can be sampled inside it:
        // a pixel more each way.
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

    void D3D11Raster::AddRectangle(const DrawJob& job) {
        if (job.w <= 0 || job.h <= 0)
            return;
        BatchKey key;
        if (!KeyFor(job, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST, &key))
            return;
        const int32_t left = std::max<int32_t>(job.x, key.scissor.left);
        const int32_t right = std::min<int32_t>(job.x + job.w, key.scissor.right);
        const int32_t top = std::max<int32_t>(job.y, key.scissor.top);
        const int32_t bottom = std::min<int32_t>(job.y + job.h, key.scissor.bottom);
        if (left >= right || top >= bottom)
            return;

        // A flipped rectangle walks its texture backwards from the base.
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

    // A line is worked out pixel by pixel exactly as the software rasteriser's DrawLineSegment
    // does - the console's rule, not Direct3D's - and each pixel drawn as a point, or above 1x
    // as the box of sub-pixels it covers, so a line is as thick as the console's at any scale.
    void D3D11Raster::AddLine(const DrawJob& job) {
        const RasterVertex& v0 = job.v[0];
        const RasterVertex& v1 = job.v[1];
        const bool points = scale_ == 1;
        BatchKey key;
        if (!KeyFor(job, points ? D3D11_PRIMITIVE_TOPOLOGY_POINTLIST
                                : D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST, &key))
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
            }
            else {
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

    // A fill ignores the drawing area and the mask, and clips at VRAM's edge rather than
    // wrapping (Gpu::CmdFillRectangle), but leaves the displayed field alone.
    void D3D11Raster::Fill(const DrawJob& job) {
        const int32_t right = std::min(job.x + job.w, kWidth);
        const int32_t bottom = std::min(job.y + job.h, kHeight);
        if (job.x >= right || job.y >= bottom)
            return;
        BatchKey key;
        memset(&key, 0, sizeof(key));
        key.topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        key.scissor.right = kWidth;
        key.scissor.bottom = kHeight;
        key.shader = kShaderDraw;
        key.constants.skip_field = job.env.skip_field ? 1 : 0;
        key.constants.active_line_lsb = static_cast<int32_t>(job.env.active_line_lsb & 1);
        uint32_t payload[kPayload] = {};
        payload[3] = ToCard(job.fill_colour) & 0x00FFFFFF;
        payload[8] = kKindFlat << 27;
        payload[15] = 1;   // a fill, not a line's pixel: to the plane, a background standing still
        Begin(key, 6);
        AddBox(job.x, job.y, right, bottom, payload);
    }

    // A VRAM-to-VRAM copy, on the card: drawn from the read copy, made current over the source
    // first - and over the destination when the mask is checked, so the shader can see what is
    // there. Every pixel is read before any is written.
    //
    // That is only the same as the console when the source and the destination share no pixel.
    // The console, and the software rasteriser, go pixel by pixel, so a copy onto itself shifted
    // right or down reads what it has just written and smears. Such a copy is rare - a scroll,
    // a wipe - and is done here as the software rasteriser does it, on native VRAM, brought up
    // to date first and handed back to the card after.
    void D3D11Raster::Copy(const DrawJob& job) {
        const int32_t across = (job.x - job.src_x) & (kWidth - 1);
        const int32_t down = (job.y - job.src_y) & (kHeight - 1);
        if ((across < job.w || kWidth - across < job.w) &&
            (down < job.h || kHeight - down < job.h)) {
            CopyInPlace(job);
            return;
        }

        // The source made current in the read copy - and the plane's, which goes with the pixels.
        Fresh(job.src_x, job.src_y, job.w, job.h);
        if (job.env.check_mask)
            Fresh(job.x, job.y, job.w, job.h);

        BatchKey key;
        memset(&key, 0, sizeof(key));
        key.topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        key.scissor.right = kWidth;
        key.scissor.bottom = kHeight;
        key.shader = kShaderCopy;
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

    // The software rasteriser's RasterVramCopy, line for line, on native VRAM.
    void D3D11Raster::CopyInPlace(const DrawJob& job) {
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
        // Native VRAM is the newer copy of the destination now; the card takes it from there.
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

    void D3D11Raster::ReadyToRead(const DrawJob& job, int32_t left, int32_t top, int32_t right,
                                  int32_t bottom, int32_t umin, int32_t umax, int32_t vmin,
                                  int32_t vmax) {
        const RasterState& state = job.state;
        if (job.kind != DrawJob::kLine && state.textured) {
            // A texture window, or a rectangle that runs off its page, can reach any texel.
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

    // Tiles are copied from the target into the read copy a row of them at a time. What is
    // batched only has to be drawn first if it drew into one of them: anything batched that
    // reads one read it when it was fresh, so it cannot be one drawn since.
    void D3D11Raster::Fresh(int32_t x, int32_t y, int32_t w, int32_t h) {
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
                    const D3D11_BOX box = Scaled(tx * kTile, ty * kTile, end * kTile,
                                                 (ty + 1) * kTile);
                    context_->CopySubresourceRegion(read_copy_.Get(), 0, box.left, box.top, 0,
                                                    target_.Get(), 0, &box);
                    // The plane's read copy goes with it: whatever draws into a tile draws
                    // into both, so they are stale together.
                    if (planes())
                        context_->CopySubresourceRegion(plane_read_copy_.Get(), 0, box.left,
                                                        box.top, 0, plane_target_.Get(), 0, &box);
                    tx = end;
                }
            }
        });
    }

    // ------------------------------------------------------------------------------------------
    // Keeping native VRAM in step
    // ------------------------------------------------------------------------------------------

    void D3D11Raster::MarkDrawn(int32_t left, int32_t top, int32_t right, int32_t bottom) {
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

    void D3D11Raster::PrepareRead(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
        // A VRAM-to-CPU transfer asks again for every word it reads. Nothing drawn since the
        // same rectangle was last made current means it still is.
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

        // Which tiles the reader needs are newer on the card.
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

        // Copied to the staging texture a run of tiles at a time, then read in one go. Above 1x
        // each run is first brought down to native size, a console pixel's own sub-pixel apiece.
        std::vector<D3D11_RECT> runs;
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
            Pass(kShaderDownsample, target_source_.Get(), true, runs);
        ID3D11Texture2D* from = scale_ > 1 ? native_target_.Get() : target_.Get();
        for (const D3D11_RECT& run : runs) {
            const D3D11_BOX box = { static_cast<UINT>(run.left), static_cast<UINT>(run.top), 0,
                                    static_cast<UINT>(run.right), static_cast<UINT>(run.bottom), 1 };
            context_->CopySubresourceRegion(staging_.Get(), 0, box.left, box.top, 0, from, 0, &box);
        }
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (FAILED(context_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
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
                    static_cast<const uint8_t*>(mapped.pData) + row * mapped.RowPitch) + left;
                uint16_t* destination = vram_ + row * kWidth + left;
                for (int32_t col = 0; col < kTile; ++col)
                    destination[col] = FromCard(source[col]);
            }
            dirty_[tile] = 0;
        }
        context_->Unmap(staging_.Get(), 0);
    }

    void D3D11Raster::Written(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
        Flush();
        ForEachPiece(static_cast<int32_t>(x), static_cast<int32_t>(y), static_cast<int32_t>(w),
                     static_cast<int32_t>(h),
                     [&](int32_t px, int32_t py, int32_t pw, int32_t ph, int32_t, int32_t) {
            Upload(px, py, pw, ph);
            for (int32_t ty = py / kTile; ty <= (py + ph - 1) / kTile; ++ty)
                for (int32_t tx = px / kTile; tx <= (px + pw - 1) / kTile; ++tx)
                    stale_[ty * kTilesX + tx] = 1;
            // A tile the upload covered whole is the same in both copies now. One it only
            // touched keeps whatever else was drawn into it, so stays dirty.
            for (int32_t ty = (py + kTile - 1) / kTile; ty < (py + ph) / kTile; ++ty)
                for (int32_t tx = (px + kTile - 1) / kTile; tx < (px + pw) / kTile; ++tx)
                    dirty_[ty * kTilesX + tx] = 0;
        });
    }

    void D3D11Raster::Reloaded() {
        Flush();
        Upload(0, 0, kWidth, kHeight);
        std::fill(dirty_.begin(), dirty_.end(), 0);
        std::fill(stale_.begin(), stale_.end(), 1);
    }

    // A device that has gone - a driver reset (TDR), an update, the card removed - fails every
    // call from then on, and what was drawn on it is gone. Gpu reads lost() once a frame and
    // carries on in software.
    void D3D11Raster::CheckDevice() {
        if (!lost_.empty())
            return;
        const HRESULT reason = device_->GetDeviceRemovedReason();
        if (reason == S_OK)
            return;
        char text[96];
        snprintf(text, sizeof(text), "the graphics card stopped answering (0x%08lX)",
                 static_cast<unsigned long>(reason));
        lost_ = text;
    }

    void D3D11Raster::Upload(int32_t x, int32_t y, int32_t w, int32_t h) {
        upload_.resize(static_cast<size_t>(w) * h);
        for (int32_t row = 0; row < h; ++row) {
            const uint16_t* source = vram_ + (y + row) * kWidth + x;
            uint32_t* destination = upload_.data() + static_cast<size_t>(row) * w;
            for (int32_t col = 0; col < w; ++col)
                destination[col] = ToCard(source[col]);
        }
        const D3D11_BOX box = { static_cast<UINT>(x), static_cast<UINT>(y), 0,
                                static_cast<UINT>(x + w), static_cast<UINT>(y + h), 1 };
        // Above 1x, to native VRAM's copy on the card first, then each pixel over its
        // sub-pixels: an uploaded picture looks just as it would at native size.
        context_->UpdateSubresource(scale_ > 1 ? native_source_.Get() : target_.Get(), 0, &box,
                                    upload_.data(), static_cast<UINT>(w * sizeof(uint32_t)), 0);
        if (scale_ > 1)
            Pass(kShaderExpand, native_source_view_.Get(), false, { { x, y, x + w, y + h } });
        if (planes())
            ForgetPlanes(x, y, w, h);
    }

    D3D11_BOX D3D11Raster::Scaled(int32_t left, int32_t top, int32_t right, int32_t bottom) const {
        return { static_cast<UINT>(left * scale_), static_cast<UINT>(top * scale_), 0,
                 static_cast<UINT>(right * scale_), static_cast<UINT>(bottom * scale_), 1 };
    }

    void D3D11Raster::Pass(Shader shader, ID3D11ShaderResourceView* source, bool into_native,
                           const std::vector<D3D11_RECT>& boxes) {
        if (boxes.empty())
            return;
        Flush();
        std::vector<Vertex> corners;
        corners.reserve(boxes.size() * 6);
        for (const D3D11_RECT& box : boxes) {
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
        for (size_t first = 0; first < corners.size(); first += kBatchVertices) {
            const size_t count = std::min(kBatchVertices, corners.size() - first);
            D3D11_MAPPED_SUBRESOURCE mapped;
            if (FAILED(context_->Map(vertices_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                CheckDevice();
                return;
            }
            memcpy(mapped.pData, corners.data() + first, count * sizeof(Vertex));
            context_->Unmap(vertices_.Get(), 0);

            // Positions are console pixels; the viewport says how big one is.
            const int size = into_native ? 1 : scale_;
            D3D11_VIEWPORT viewport = {};
            viewport.Width = static_cast<float>(kWidth * size);
            viewport.Height = static_cast<float>(kHeight * size);
            viewport.MaxDepth = 1.0f;
            const D3D11_RECT scissor = { 0, 0, kWidth * size, kHeight * size };
            context_->OMSetRenderTargets(1, into_native ? native_target_view_.GetAddressOf()
                                                        : target_view_.GetAddressOf(),
                                         nullptr);
            context_->RSSetViewports(1, &viewport);
            context_->RSSetScissorRects(1, &scissor);
            context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            context_->PSSetShader(pixel_shaders_[shader].Get(), nullptr, 0);
            context_->PSSetShaderResources(0, 1, &source);
            context_->Draw(static_cast<UINT>(count), 0);
            ID3D11ShaderResourceView* none = nullptr;
            context_->PSSetShaderResources(0, 1, &none);
        }
        // Back to drawing into VRAM.
        D3D11_VIEWPORT viewport = {};
        viewport.Width = static_cast<float>(kWidth * scale_);
        viewport.Height = static_cast<float>(kHeight * scale_);
        viewport.MaxDepth = 1.0f;
        BindTargets();
        context_->RSSetViewports(1, &viewport);
    }

    void D3D11Raster::DrawDisplay(ID3D11RenderTargetView* into, uint32_t x, uint32_t y,
                                  UINT width, UINT height) {
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
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (FAILED(context_->Map(vertices_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            CheckDevice();
            return;
        }
        memcpy(mapped.pData, corners, sizeof(corners));
        context_->Unmap(vertices_.Get(), 0);
        D3D11_VIEWPORT viewport = {};
        viewport.Width = static_cast<float>(width);
        viewport.Height = static_cast<float>(height);
        viewport.MaxDepth = 1.0f;
        const D3D11_RECT scissor = { 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
        // The picture - or, for Video > View Depth and View Motion, the plane in its place.
        Shader shader = kShaderDisplay;
        ID3D11ShaderResourceView* source = target_source_.Get();
        if (planes() && shown_plane_ != emulation::psx::PlaneView::kPicture) {
            shader = shown_plane_ == emulation::psx::PlaneView::kDepth ? kShaderDisplayDepth
                                                                       : kShaderDisplayMotion;
            source = plane_source_.Get();
        }
        context_->OMSetRenderTargets(1, &into, nullptr);
        context_->RSSetViewports(1, &viewport);
        context_->RSSetScissorRects(1, &scissor);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->PSSetShader(pixel_shaders_[shader].Get(), nullptr, 0);
        context_->PSSetShaderResources(0, 1, &source);
        context_->Draw(6, 0);
        ID3D11ShaderResourceView* none = nullptr;
        context_->PSSetShaderResources(0, 1, &none);
        viewport.Width = static_cast<float>(kWidth * scale_);
        viewport.Height = static_cast<float>(kHeight * scale_);
        BindTargets();
        context_->RSSetViewports(1, &viewport);
    }

    // The picture left on the card for the presenter's device (psx/shared_picture.h). Nothing
    // here waits: the fence is signalled behind the drawing and the work sent to the card, and
    // the presenter waits for the fence - on its own thread - before it draws from the texture.
    bool D3D11Raster::ShareDisplay(uint32_t x, uint32_t y, UINT width, UINT height,
                                   emulation::psx::SharedPicture* shared) {
        static_assert(kPictureSlots == CardPictures::kSlots, "one view per shared texture");
        CardPictures& pictures = *pictures_;
        const int slot = pictures.Pick(width, height);
        if (slot < 0)
            return false;
        if (pictures.slot(slot).width != width || pictures.slot(slot).height != height ||
            !picture_views_[slot]) {
            picture_views_[slot].Reset();
            picture_textures_[slot].Reset();
            D3D11_TEXTURE2D_DESC texture = {};
            texture.Width = width;
            texture.Height = height;
            texture.MipLevels = 1;
            texture.ArraySize = 1;
            texture.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            texture.SampleDesc.Count = 1;
            texture.Usage = D3D11_USAGE_DEFAULT;
            texture.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            texture.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
            Microsoft::WRL::ComPtr<IDXGIResource1> resource;
            HANDLE handle = nullptr;
            if (FAILED(device_->CreateTexture2D(&texture, nullptr, &picture_textures_[slot])) ||
                FAILED(device_->CreateRenderTargetView(picture_textures_[slot].Get(), nullptr,
                                                       &picture_views_[slot])) ||
                FAILED(picture_textures_[slot].As(&resource)) ||
                FAILED(resource->CreateSharedHandle(
                    nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr,
                    &handle))) {
                // A card that will not share a texture will not share the next one either:
                // the picture is read back from here on.
                picture_views_[slot].Reset();
                picture_textures_[slot].Reset();
                pictures_.reset();
                context4_.Reset();
                CheckDevice();
                return false;
            }
            pictures.Replace(slot, handle, width, height);
            picture_planes_[slot].Reset();
        }

        DrawDisplay(picture_views_[slot].Get(), x, y, width, height);
        // The plane's same area beside it, while the plane is kept for DLSS - copied before the
        // fence, so the presenter's one wait covers both. A card that will not share this one
        // still hands the picture over, without it.
        bool with_planes = false;
        if (keep_planes_ && planes()) {
            if (!picture_planes_[slot] || pictures.slot(slot).planes == nullptr) {
                picture_planes_[slot].Reset();
                D3D11_TEXTURE2D_DESC texture = {};
                texture.Width = width;
                texture.Height = height;
                texture.MipLevels = 1;
                texture.ArraySize = 1;
                texture.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                texture.SampleDesc.Count = 1;
                texture.Usage = D3D11_USAGE_DEFAULT;
                texture.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                texture.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
                Microsoft::WRL::ComPtr<IDXGIResource1> resource;
                HANDLE handle = nullptr;
                if (SUCCEEDED(device_->CreateTexture2D(&texture, nullptr, &picture_planes_[slot])) &&
                    SUCCEEDED(picture_planes_[slot].As(&resource)) &&
                    SUCCEEDED(resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ, nullptr,
                                                           &handle)))
                    pictures.ReplacePlanes(slot, handle);
                else
                    picture_planes_[slot].Reset();
            }
            if (picture_planes_[slot]) {
                const D3D11_BOX box = { x * scale_, y * scale_, 0, x * scale_ + width,
                                        y * scale_ + height, 1 };
                context_->CopySubresourceRegion(picture_planes_[slot].Get(), 0, 0, 0, 0,
                                                plane_target_.Get(), 0, &box);
                with_planes = true;
            }
        }
        const uint64_t serial = ++picture_serial_;
        pictures.Drawn(slot, serial);
        if (FAILED(context4_->Signal(pictures.fence(), serial))) {
            CheckDevice();
            return false;
        }
        // Sent to the card now, not whenever the next batch fills: the presenter is about to
        // wait for it.
        context_->Flush();

        const CardPictures::Slot& chosen = pictures.slot(slot);
        shared->source = pictures_;
        shared->texture = chosen.handle;
        shared->texture_id = chosen.id;
        shared->serial = serial;
        shared->width = static_cast<int>(width);
        shared->height = static_cast<int>(height);
        shared->planes = with_planes ? chosen.planes : nullptr;
        shared->planes_id = with_planes ? chosen.planes_id : 0;
        shared->jitter_x = jitter_phases_ > 0 ? shown_jitter_x_ : 0.0f;
        shared->jitter_y = jitter_phases_ > 0 ? shown_jitter_y_ : 0.0f;
        return true;
    }

    // The display area above 1x, at its full size for the presenters: on the card if the
    // presenter takes it there and a texture is free, and otherwise read back.
    bool D3D11Raster::ResolveDisplay(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
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
            // A read-back after this one has no picture of its own behind it to hand over.
            display_pending_ = false;
            *scale = scale_;
            return true;
        }
        if (!lost_.empty())
            return false;

        // Read back: drawn by a pass into a B8G8R8A8 texture, alpha opaque, whose rows are then
        // the presenters' own 0xFFRRGGBB words and cross to the picture as they are. It waits
        // for the card, as a download does.
        if (!display_texture_ || display_width_ < width || display_height_ < height) {
            display_texture_.Reset();
            display_texture_view_.Reset();
            display_staging_[0].Reset();
            display_staging_[1].Reset();
            display_pending_ = false;
            D3D11_TEXTURE2D_DESC texture = {};
            texture.Width = std::max(width, display_width_);
            texture.Height = std::max(height, display_height_);
            texture.MipLevels = 1;
            texture.ArraySize = 1;
            texture.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            texture.SampleDesc.Count = 1;
            texture.Usage = D3D11_USAGE_DEFAULT;
            texture.BindFlags = D3D11_BIND_RENDER_TARGET;
            if (FAILED(device_->CreateTexture2D(&texture, nullptr, &display_texture_)) ||
                FAILED(device_->CreateRenderTargetView(display_texture_.Get(), nullptr,
                                                       &display_texture_view_)))
                return false;
            texture.Usage = D3D11_USAGE_STAGING;
            texture.BindFlags = 0;
            texture.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (FAILED(device_->CreateTexture2D(&texture, nullptr, &display_staging_[0])) ||
                FAILED(device_->CreateTexture2D(&texture, nullptr, &display_staging_[1]))) {
                display_texture_.Reset();
                return false;
            }
            display_width_ = texture.Width;
            display_height_ = texture.Height;
        }
        DrawDisplay(display_texture_view_.Get(), x, y, width, height);

        // Read back a frame behind: this frame's picture is copied out now, and last frame's -
        // which the card finished long ago - is what is handed over, so nothing here waits for
        // the card to finish drawing. One frame later on the screen, and only above 1x. The first
        // frame, or the first at a new size, has nothing behind it and waits.
        const int copied = display_next_;
        display_next_ ^= 1;
        const D3D11_BOX box = { 0, 0, 0, width, height, 1 };
        context_->CopySubresourceRegion(display_staging_[copied].Get(), 0, 0, 0, 0,
                                        display_texture_.Get(), 0, &box);
        const bool behind = display_pending_ && pending_width_ == width &&
                            pending_height_ == height;
        ID3D11Texture2D* read = display_staging_[behind ? copied ^ 1 : copied].Get();
        display_pending_ = true;
        pending_width_ = width;
        pending_height_ = height;
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (FAILED(context_->Map(read, 0, D3D11_MAP_READ, 0, &mapped))) {
            CheckDevice();
            return false;
        }
        picture->resize(static_cast<size_t>(width) * height);
        for (UINT row = 0; row < height; ++row)
            memcpy(picture->data() + static_cast<size_t>(row) * width,
                   static_cast<const uint8_t*>(mapped.pData) + row * mapped.RowPitch,
                   width * sizeof(uint32_t));
        context_->Unmap(read, 0);
        *scale = scale_;
        return true;
    }

}   // namespace psxemu
