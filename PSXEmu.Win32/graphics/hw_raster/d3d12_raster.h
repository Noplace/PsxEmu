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

// The hardware rasteriser in Direct3D 12: Direct3D 11's (d3d11_raster.h) drawn with the newer
// API, from the same shaders and by the same rules (raster_common.h), so the two draw the same
// pictures and native VRAM comes out the same from either. Everything d3d11_raster.h says of what
// is drawn holds here; what differs is only what Direct3D 12 asks to be said outright:
//
//   - One command list is recorded at a time. It is sent to the card when the CPU needs what it
//     drew - a download, a picture read back - and when a picture is handed over; nothing else
//     waits for the card.
//   - Every texture's state is tracked, and changed by a barrier where Direct3D 11 would have
//     changed it unasked.
//   - Vertices, and pixels on their way to the card, go through upload memory that lives as long
//     as the command list that reads it; pixels on their way back come through read-back
//     buffers.
//   - A shared picture is a Direct3D 12 resource, which every renderer opens: Direct3D 11 and 12
//     as they open Direct3D 11's, Vulkan and OpenGL by the Direct3D 12 handle type
//     (SharedPictureSource::d3d12).
//
// It owns its own device and queue and never touches a presenter's. Every call comes from the
// rasteriser's thread or the machine's, one at a time - Gpu makes sure of that.

#include "graphics/hw_raster/hardware_raster.h"
#include "graphics/hw_raster/raster_common.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <memory>
#include <string>
#include <vector>

namespace psxemu {

    class D3D12Pictures;

    class D3D12Raster : public HardwareRaster {
     public:
        // See HardwareRaster::Create. Null, with `error` saying why, if Direct3D 12 cannot be had
        // or cannot hold VRAM that large.
        static std::unique_ptr<D3D12Raster> Create(uint16_t* vram,
                                                   const emulation::psx::RasterOptions& options,
                                                   bool warp, std::string* error);
        ~D3D12Raster() override;

        // A picture this rasteriser shared, read back through a Direct3D 12 device of its own
        // (HardwareRaster::ReadSharedPicture).
        static bool ReadSharedPicture(const emulation::psx::SharedPicture& picture,
                                      std::vector<uint32_t>* pixels);

        // RasterBackend.
        void Apply(const emulation::psx::DrawJob& job) override;
        void PrepareRead(uint32_t x, uint32_t y, uint32_t w, uint32_t h) override;
        void Written(uint32_t x, uint32_t y, uint32_t w, uint32_t h) override;
        void Reloaded() override;
        bool ResolveDisplay(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                            std::vector<uint32_t>* picture,
                            emulation::psx::SharedPicture* shared, int* scale) override;
        void SetPlanes(bool keep, emulation::psx::PlaneView view) override;
        void NewPicture(bool reset) override;
        void set_motion_check(bool on) override { motion_check_ = on; }
        void SetJitter(int phases) override;
        emulation::psx::RasterCounters& counters() override { return counters_; }
        const char* lost() const override { return lost_.empty() ? nullptr : lost_.c_str(); }
        std::string device() const override { return adapter_name_; }
        void set_watch(const emulation::psx::RasterWatch&) override {}
        void NoteWatchWrite(uint32_t, uint32_t) override {}

        // HardwareRaster.
        Api api() const override { return Api::kD3D12; }
        bool sharing_pictures() const override { return pictures_ != nullptr; }
        float jitter_x() const override { return jitter_x_; }
        float jitter_y() const override { return jitter_y_; }
        bool planes() const override {
            return plane_target_.resource &&
                   (keep_planes_ || shown_plane_ != emulation::psx::PlaneView::kPicture);
        }
        bool ReadPlanes(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                        std::vector<float>* rgba) override;
        int scale() const override { return scale_; }
        void SimulateLoss() override { lost_ = "simulated for a test"; }

     private:
        using Vertex = raster::Vertex;
        using Constants = raster::Constants;
        using Shader = raster::Shader;
        static constexpr int kPayload = raster::kPayload;

        // A texture, and the state the command list being recorded leaves it in.
        struct Texture {
            Microsoft::WRL::ComPtr<ID3D12Resource> resource;
            D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
        };

        // What one batch shares. A job needing anything different flushes first.
        struct BatchKey {
            D3D_PRIMITIVE_TOPOLOGY topology;
            D3D12_RECT scissor;
            int32_t shader;
            Constants constants;
        };

        // Upload memory: what one command list reads from it is kept until the card has finished
        // that list (Recycle). Pages are kept, and handed out again from the start.
        struct UploadPage {
            Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
            uint8_t* cpu = nullptr;
            D3D12_GPU_VIRTUAL_ADDRESS gpu = 0;
            size_t size = 0;
            size_t used = 0;
        };
        struct UploadSpace {
            uint8_t* cpu;
            D3D12_GPU_VIRTUAL_ADDRESS gpu;
            ID3D12Resource* buffer;
            uint64_t offset;
        };

        // The command lists in flight: one allocator each, the fence value that says the card has
        // finished it, and the upload pages it read.
        static constexpr int kFrames = 3;
        struct Frame {
            Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
            uint64_t fence = 0;
            std::vector<UploadPage> pages;
        };

        // The pipelines: what each draws into, with which shader, and how.
        enum Pipeline {
            kPipeDraw, kPipeDrawPoints, kPipeDrawPlanes, kPipeDrawPlanesPoints, kPipeCopy,
            kPipeCopyPlanes, kPipeDownsample, kPipeExpand, kPipeDisplay, kPipeDisplayDepth,
            kPipeDisplayMotion,
            kPipeCount
        };

        // Shader-visible descriptor tables of two (t0, t1), by what they hold.
        enum Table {
            kTableDraw,      // the read copy, and the plane's read copy
            kTableTarget,    // the target, for downloads and showing it
            kTableNative,    // native VRAM's copy on the card, for uploads
            kTablePlane,     // the plane, for showing it
            kTableCount
        };

        // Render-target views, by what they draw into.
        enum Rtv {
            kRtvTarget, kRtvPlane, kRtvNative, kRtvDisplay, kRtvPicture,
            kRtvCount = kRtvPicture + emulation::psx::kSharedTextureCount
        };

        D3D12Raster(uint16_t* vram, const emulation::psx::RasterOptions& options)
            : vram_(vram), scale_(options.scale), true_color_(options.true_color),
              adapter_(options.adapter), share_(options.shared_picture) {}
        bool Initialize(bool warp, std::string* error);
        bool CreatePipelines(std::string* error);
        // The textures and fence shared pictures need; without them the picture is read back.
        void StartSharing();

        // ---- Command lists ----
        // The list being recorded, opened if it is not: its allocator's last list finished.
        ID3D12GraphicsCommandList* List();
        // Sends what is recorded to the card, and moves on to the next frame's allocator.
        // Returns the fence value that says it is done.
        uint64_t Submit();
        // Waits for the card to reach `value`. False if it never will - the device has gone.
        bool WaitFor(uint64_t value);
        // Submit, and wait for it.
        bool Finish() { return WaitFor(Submit()); }
        // Space in upload memory for `size` bytes, aligned to `alignment`, that the list being
        // recorded may read from.
        UploadSpace Allocate(size_t size, size_t alignment);
        // A texture into `state` on the list being recorded.
        void Use(Texture& texture, D3D12_RESOURCE_STATES state);
        // Everything a draw sets, set again: a list opened afresh has none of it.
        void SetDrawState(Pipeline pipeline, Table table, const D3D12_CPU_DESCRIPTOR_HANDLE* targets,
                          UINT target_count, UINT width, UINT height, const D3D12_RECT& scissor);
        D3D12_CPU_DESCRIPTOR_HANDLE RtvHandle(int index) const;
        D3D12_GPU_DESCRIPTOR_HANDLE TableHandle(Table table) const;
        // Fills table `table`'s two descriptors: `first` as t0 and `second` as t1, either null.
        void WriteTable(Table table, ID3D12Resource* first, DXGI_FORMAT first_format,
                        ID3D12Resource* second, DXGI_FORMAT second_format);
        // A texture on the card, `width` x `height`: drawn into and read by shaders, or only
        // read. False if the card would not make it.
        bool MakeTexture(Texture* texture, UINT width, UINT height, DXGI_FORMAT format,
                         bool render_target, D3D12_RESOURCE_STATES initial,
                         D3D12_HEAP_FLAGS heap_flags = D3D12_HEAP_FLAG_NONE);
        // A read-back buffer of `size` bytes.
        bool MakeReadback(Microsoft::WRL::ComPtr<ID3D12Resource>* buffer, size_t size);
        // Copies a box of `from` into `buffer` as rows `pitch` bytes apart, starting at
        // (dest_x, dest_y) of a footprint `width` x `height`.
        void CopyToBuffer(ID3D12Resource* buffer, UINT width, UINT height, UINT pitch,
                          DXGI_FORMAT format, UINT dest_x, UINT dest_y, ID3D12Resource* from,
                          const D3D12_BOX& box);

        // ---- What d3d11_raster.cpp has, the same here ----
        void DrawDisplay(D3D12_CPU_DESCRIPTOR_HANDLE into, Texture& texture, uint32_t x,
                         uint32_t y, UINT width, UINT height);
        bool ShareDisplay(uint32_t x, uint32_t y, UINT width, UINT height,
                          emulation::psx::SharedPicture* shared);
        bool MakePlanes();
        void ForgetPlanes(int32_t x, int32_t y, int32_t w, int32_t h);
        void Flush();
        void Begin(const BatchKey& key, size_t count);
        static bool KeyFor(const emulation::psx::DrawJob& job, D3D_PRIMITIVE_TOPOLOGY topology,
                           BatchKey* key);
        void AddBox(int32_t left, int32_t top, int32_t right, int32_t bottom,
                    const uint32_t (&payload)[kPayload]);
        void AddTriangle(const emulation::psx::DrawJob& job);
        void AddPreciseTriangle(const emulation::psx::DrawJob& job, const BatchKey& key);
        void AddRectangle(const emulation::psx::DrawJob& job);
        void AddLine(const emulation::psx::DrawJob& job);
        void Fill(const emulation::psx::DrawJob& job);
        void Copy(const emulation::psx::DrawJob& job);
        void CopyInPlace(const emulation::psx::DrawJob& job);
        void ReadyToRead(const emulation::psx::DrawJob& job, int32_t left, int32_t top,
                         int32_t right, int32_t bottom, int32_t umin, int32_t umax, int32_t vmin,
                         int32_t vmax);
        void Fresh(int32_t x, int32_t y, int32_t w, int32_t h);
        void MarkDrawn(int32_t left, int32_t top, int32_t right, int32_t bottom);
        void Upload(int32_t x, int32_t y, int32_t w, int32_t h);
        void CheckDevice();
        void Pass(Shader shader, bool into_native, const std::vector<D3D12_RECT>& boxes);
        D3D12_BOX Scaled(int32_t left, int32_t top, int32_t right, int32_t bottom) const;
        void WarpCheck(uint32_t x, uint32_t y, UINT width, UINT height);

        static constexpr int kWidth = 1024;
        static constexpr int kHeight = 512;
        static constexpr int kTile = 16;
        static constexpr int kTilesX = kWidth / kTile;
        static constexpr int kTilesY = kHeight / kTile;
        static constexpr size_t kBatchVertices = 6 * 4096;
        static constexpr int kPictureSlots = kSharedPictures;

        uint16_t* vram_;
        const int scale_;
        const bool true_color_;
        const uint64_t adapter_;
        const bool share_;

        Microsoft::WRL::ComPtr<ID3D12Device> device_;
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue_;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list_;
        Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
        HANDLE fence_event_ = nullptr;
        uint64_t fence_value_ = 0;   // the last value signalled
        Frame frames_[kFrames];
        int frame_ = 0;
        bool recording_ = false;

        Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> pipelines_[kPipeCount];
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtv_heap_;
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srv_heap_;
        UINT rtv_size_ = 0, srv_size_ = 0;

        Texture target_;       // VRAM on the card, at scale_
        Texture read_copy_;    // what the shaders read
        Texture native_target_, native_source_;   // above 1x: downloads and uploads
        Microsoft::WRL::ComPtr<ID3D12Resource> vram_readback_;   // downloads, native size
        Texture display_texture_;
        Microsoft::WRL::ComPtr<ID3D12Resource> display_readback_[2];   // this frame's, and last
        uint64_t display_fence_[2] = {};
        int display_next_ = 0;
        bool display_pending_ = false;
        UINT pending_width_ = 0, pending_height_ = 0;
        UINT display_width_ = 0, display_height_ = 0;
        UINT display_pitch_ = 0;   // the read-back buffers' row pitch

        std::shared_ptr<D3D12Pictures> pictures_;
        Texture picture_textures_[kPictureSlots];
        Texture picture_planes_[kPictureSlots];
        uint64_t picture_serial_ = 0;
        uint64_t adapter_luid_ = 0;

        bool keep_planes_ = false;
        emulation::psx::PlaneView shown_plane_ = emulation::psx::PlaneView::kPicture;
        Texture plane_target_;
        Texture plane_read_copy_;

        bool motion_check_ = false;
        bool picture_new_ = false;
        bool picture_reset_ = false;
        std::vector<uint8_t> warp_last_;
        UINT warp_width_ = 0, warp_height_ = 0;
        Microsoft::WRL::ComPtr<ID3D12Resource> warp_colour_, warp_plane_;   // read-back
        // Draws recorded since the list was opened: past a few thousand it is sent on, so the
        // card is not left idle while a long run of drawing waits for a read.
        int recorded_draws_ = 0;
        int jitter_phases_ = 0;
        uint32_t jitter_index_ = 1;
        float jitter_x_ = 0.0f, jitter_y_ = 0.0f;
        float shown_jitter_x_ = 0.0f, shown_jitter_y_ = 0.0f;

        std::vector<Vertex> batch_;
        BatchKey batch_key_ = {};
        std::vector<uint8_t> dirty_ = std::vector<uint8_t>(kTilesX * kTilesY, 0);
        std::vector<uint8_t> stale_ = std::vector<uint8_t>(kTilesX * kTilesY, 1);
        std::vector<uint32_t> batch_tile_ = std::vector<uint32_t>(kTilesX * kTilesY, 0);
        uint32_t batch_serial_ = 1;
        uint64_t drawn_serial_ = 0;
        uint64_t last_read_serial_ = 0;
        uint32_t last_read_[4] = {};
        bool last_read_valid_ = false;
        std::vector<uint8_t> wanted_ = std::vector<uint8_t>(kTilesX * kTilesY, 0);
        std::string lost_;
        std::string adapter_name_;
        emulation::psx::RasterCounters counters_ = {};
    };

}   // namespace psxemu
