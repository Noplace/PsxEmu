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

// The hardware rasteriser: the GPU's DrawJobs drawn by Direct3D 11 on the host's graphics card,
// into a copy of VRAM that lives there (Docs/Hardware-Renderer-Plan.md). At native resolution
// it draws what the software rasteriser draws, to the pixel.
//
//   - VRAM on the card is a 1024x512 RGBA8 render target, alpha the mask bit. Every pixel is
//     cut to the five bits VRAM keeps and widened back the way the software rasteriser widens a
//     VRAM pixel, so a download is a shift.
//   - The pixel shader is the software rasteriser's arithmetic, in integers. A triangle is
//     drawn as its bounding box, and the shader decides each pixel with the same edge functions
//     and fill rule, and interpolates its colour and texture coordinates with the same integer
//     division - so which pixels a triangle covers, and what colour each is, cannot come out
//     differently. Rectangles, lines and fills carry what they need the same way.
//   - Textures - 4- and 8-bit through a CLUT, 15-bit direct, the texture window - are decoded
//     from the *read copy*, a second texture of VRAM: Direct3D cannot sample a texture while
//     drawing into it. So are semi-transparency and the mask check, which need the pixel already
//     there. A region something has drawn into is refreshed in the read copy only when a
//     primitive is about to read it, which is when a batch has to end.
//   - Lines are worked out pixel by pixel as the console draws them, and drawn as points.
//   - Fills, and VRAM-to-VRAM copies with the mask rules, on the card.
//   - Native VRAM - Gpu's, which the machine reads - is kept in step by 16x16 tiles: a tile a
//     draw touched is newer on the card, and is downloaded only when something is about to read
//     it (PrepareRead). A CPU upload is written to both (Written). The card's copy is always the
//     whole truth; native VRAM is the truth for every tile that is not dirty.
//
// It owns its own Direct3D device and never touches a presenter's. Every call comes from the
// rasteriser's thread or the machine's, one at a time - Gpu makes sure of that - which is all
// an immediate context needs. Above 1x the picture it shows is handed over either as pixels read
// back, or - for a presenter that can open it, on the same card - left on the card in a texture
// the two devices share (psx/shared_picture.h), which costs nothing to hand over.

#include "psx/raster.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11_4.h>
#include <wrl/client.h>

#include <memory>
#include <string>
#include <vector>

namespace psxemu {

    class CardPictures;

    class D3D11Raster : public emulation::psx::RasterBackend {
     public:
        // A rasteriser drawing for `vram` (Gpu's native VRAM, 1024x512) at `options`' scale, on
        // `options`' adapter or Windows' default - or on WARP, Windows' own software Direct3D,
        // which is deterministic and needs no graphics card: what the headless comparisons run
        // on. Null, with `error` saying why, if Direct3D 11 cannot be had or cannot hold VRAM
        // that large. Asked for shared pictures and unable to make them, it reads back instead.
        static std::unique_ptr<D3D11Raster> Create(uint16_t* vram,
                                                   const emulation::psx::RasterOptions& options,
                                                   bool warp, std::string* error);
        ~D3D11Raster() override;

        bool ResolveDisplay(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                            std::vector<uint32_t>* picture,
                            emulation::psx::SharedPicture* shared, int* scale) override;

        // Whether it hands the picture over on the card; false once it has fallen back to reading
        // it back, or was never asked.
        bool sharing_pictures() const { return pictures_ != nullptr; }
        // How many textures a shared picture can be in at once (psx/shared_picture.h). With none
        // free the picture is read back.
        static constexpr int kSharedPictures = emulation::psx::kSharedTextureCount;

        // A shared picture's pixels, 0xFFRRGGBB rows, read back through a device of its own on
        // the picture's card - for a screenshot now and then, from any thread. False if it
        // cannot be opened, or is not drawn within a second.
        static bool ReadSharedPicture(const emulation::psx::SharedPicture& picture,
                                      std::vector<uint32_t>* pixels);

        void Apply(const emulation::psx::DrawJob& job) override;
        void PrepareRead(uint32_t x, uint32_t y, uint32_t w, uint32_t h) override;
        void Written(uint32_t x, uint32_t y, uint32_t w, uint32_t h) override;
        void Reloaded() override;
        void SetPlanes(bool keep, emulation::psx::PlaneView view) override;
        void NewPicture(bool reset) override;
        void set_motion_check(bool on) override { motion_check_ = on; }

        // Whether the plane beside VRAM is being drawn (psx/shared_picture.h).
        bool planes() const {
            return plane_target_ && (keep_planes_ || shown_plane_ != emulation::psx::PlaneView::kPicture);
        }
        // The plane over a rectangle of VRAM, every sub-pixel of it as four floats - R, G, B, A,
        // rows of w x scale - read back, for hw_raster_test. False if it is not being drawn.
        bool ReadPlanes(uint32_t x, uint32_t y, uint32_t w, uint32_t h, std::vector<float>* rgba);
        int scale() const { return scale_; }
        emulation::psx::RasterCounters& counters() override { return counters_; }
        const char* lost() const override { return lost_.empty() ? nullptr : lost_.c_str(); }
        std::string device() const override { return adapter_name_; }

        // For hw_raster_test: from here on, report the device lost, as a driver reset would.
        void SimulateLoss() { lost_ = "simulated for a test"; }
        void set_watch(const emulation::psx::RasterWatch&) override {}
        void NoteWatchWrite(uint32_t, uint32_t) override {}

        // A pixel as the card keeps it, and back: the 16-bit VRAM value <-> RGBA8, with alpha
        // the mask bit. Exact both ways for every one of the 65,536 values.
        static uint32_t ToCard(uint16_t pixel);
        static uint16_t FromCard(uint32_t rgba);

     private:
        // Words a vertex carries for the pixel shader: 16 for the primitive, and 4 for its
        // motion, which only the plane takes (Docs/DLSS-Plan.md, phase 2).
        static constexpr int kPayload = 20;

        // A corner of what is drawn, and - the same at every corner - what the pixel shader
        // needs to work out each pixel of the primitive: see the layout in d3d11_raster.cpp.
        struct Vertex {
            float x, y;
            uint32_t p[kPayload];
        };

        // What the pixel shaders are told, per batch. The layout is the HLSL cbuffer's.
        struct Constants {
            int32_t skip_field;        // leave the rows of the displayed field alone
            int32_t active_line_lsb;   // ...which are those with this low bit
            int32_t force_mask;        // GP0(E6h) bit 0
            int32_t check_mask;        // GP0(E6h) bit 1
            int32_t tw_mask_x, tw_mask_y, tw_offset_x, tw_offset_y;   // the texture window
        };

        enum Shader {
            kShaderDraw, kShaderCopy, kShaderDownsample, kShaderExpand, kShaderDisplay,
            kShaderDisplayDepth, kShaderDisplayMotion,
            kShaderCount
        };

        // What one batch shares. A job needing anything different flushes first.
        struct BatchKey {
            D3D11_PRIMITIVE_TOPOLOGY topology;
            D3D11_RECT scissor;
            int32_t shader;
            Constants constants;
        };

        D3D11Raster(uint16_t* vram, const emulation::psx::RasterOptions& options)
            : vram_(vram), scale_(options.scale), true_color_(options.true_color),
              adapter_(options.adapter), share_(options.shared_picture) {}
        bool Initialize(bool warp, std::string* error);
        // The textures and fence shared pictures need; without them the picture is read back.
        void StartSharing();

        // Draws the display area, from (x, y) of VRAM, into `into` - width x height, which is
        // scale_ times the area - as 0xFFRRGGBB.
        void DrawDisplay(ID3D11RenderTargetView* into, uint32_t x, uint32_t y, UINT width,
                         UINT height);
        // Above 1x: the display area drawn into a texture the presenter's device opens. False if
        // every one is still in use, and the picture has to be read back this time.
        bool ShareDisplay(uint32_t x, uint32_t y, UINT width, UINT height,
                          emulation::psx::SharedPicture* shared);

        // The plane's textures, made when it is first asked for: false, and none, if the card
        // cannot hold them.
        bool MakePlanes();
        // What the draws write into: VRAM on the card, and the plane beside it while it is kept.
        void BindTargets();
        // The plane over a rectangle of VRAM - inside it - set to "not known": uploads, which may
        // be anything from a still background to a film.
        void ForgetPlanes(int32_t x, int32_t y, int32_t w, int32_t h);

        // Draws what has been batched.
        void Flush();
        // Makes room for `count` more vertices batched under `key`, flushing first if the batch
        // is under a different one or full.
        void Begin(const BatchKey& key, size_t count);
        // A key for drawing `job` inside its drawing area - false if that is empty - with its
        // field, mask and texture-window rules.
        static bool KeyFor(const emulation::psx::DrawJob& job, D3D11_PRIMITIVE_TOPOLOGY topology,
                           BatchKey* key);
        // Batches a rectangle of pixels - right and bottom exclusive - as two triangles carrying
        // `payload`, and marks it drawn.
        void AddBox(int32_t left, int32_t top, int32_t right, int32_t bottom,
                    const uint32_t (&payload)[kPayload]);
        static uint32_t Attributes(const emulation::psx::RasterState& state, uint32_t kind,
                                   bool textured, bool dither);

        void AddTriangle(const emulation::psx::DrawJob& job);
        void AddPreciseTriangle(const emulation::psx::DrawJob& job, const BatchKey& key);
        void AddRectangle(const emulation::psx::DrawJob& job);
        void AddLine(const emulation::psx::DrawJob& job);
        void Fill(const emulation::psx::DrawJob& job);
        void Copy(const emulation::psx::DrawJob& job);
        // A copy whose source and destination overlap, done on native VRAM pixel by pixel.
        void CopyInPlace(const emulation::psx::DrawJob& job);

        // Makes the read copy current for what a primitive covering the given rectangle of VRAM
        // is about to read: its texture and CLUT if it is textured, and the pixels underneath if
        // it blends or checks the mask.
        void ReadyToRead(const emulation::psx::DrawJob& job, int32_t left, int32_t top, int32_t right,
                         int32_t bottom, int32_t umin, int32_t umax, int32_t vmin, int32_t vmax);
        // The read copy made current for a rectangle of VRAM, which wraps as VRAM does.
        void Fresh(int32_t x, int32_t y, int32_t w, int32_t h);

        // A rectangle of VRAM - right and bottom exclusive, clipped to VRAM - drawn into on the
        // card: newer there than in native VRAM and in the read copy.
        void MarkDrawn(int32_t left, int32_t top, int32_t right, int32_t bottom);
        // Uploads a rectangle of native VRAM to the card - it must lie inside VRAM.
        void Upload(int32_t x, int32_t y, int32_t w, int32_t h);
        // After a call to the card failed: whether the device itself has gone, and why.
        void CheckDevice();
        // Above 1x: draws `boxes` - rectangles of console pixels - with `shader` from `source`,
        // into the native-sized target (a download) or the target itself (an upload).
        void Pass(Shader shader, ID3D11ShaderResourceView* source, bool into_native,
                  const std::vector<D3D11_RECT>& boxes);
        // A rectangle of console pixels as a box of the target, `scale_` times the size.
        D3D11_BOX Scaled(int32_t left, int32_t top, int32_t right, int32_t bottom) const;

        static constexpr int kWidth = 1024;
        static constexpr int kHeight = 512;
        static constexpr int kTile = 16;
        static constexpr int kTilesX = kWidth / kTile;
        static constexpr int kTilesY = kHeight / kTile;
        static constexpr size_t kBatchVertices = 6 * 4096;

        uint16_t* vram_;
        const int scale_;         // the target's size, in multiples of VRAM's
        const bool true_color_;
        const uint64_t adapter_;  // the LUID asked for, or 0
        const bool share_;        // asked to hand pictures over on the card
        Microsoft::WRL::ComPtr<ID3D11Device> device_;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> target_;       // VRAM on the card, at scale_
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> target_view_;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> target_source_;   // above 1x: downloads
        Microsoft::WRL::ComPtr<ID3D11Texture2D> read_copy_;    // what the shaders read
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> read_copy_view_;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> staging_;      // downloads, native size
        // Above 1x, native-sized: the downsampled target a download is read from, and native
        // VRAM's pixels an upload is expanded from.
        Microsoft::WRL::ComPtr<ID3D11Texture2D> native_target_;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> native_target_view_;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> native_source_;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> native_source_view_;
        // Above 1x, the display area drawn out for showing and read back, grown to the largest
        // asked for.
        Microsoft::WRL::ComPtr<ID3D11Texture2D> display_texture_;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> display_texture_view_;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> display_staging_[2];   // this frame's, and last
        int display_next_ = 0;             // which one this frame is copied into
        bool display_pending_ = false;     // the other holds last frame's picture...
        UINT pending_width_ = 0, pending_height_ = 0;   // ...at this size
        UINT display_width_ = 0, display_height_ = 0;
        // Shared pictures: the textures (their handles and bookkeeping are the source's, which
        // outlives this), a view of each to draw into, the fence each picture's drawing signals,
        // and the card's LUID. Null when pictures are read back.
        std::shared_ptr<CardPictures> pictures_;
        static constexpr int kPictureSlots = kSharedPictures;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> picture_textures_[kPictureSlots];
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> picture_views_[kPictureSlots];
        uint64_t picture_serial_ = 0;   // the last picture handed over; the fence's value
        Microsoft::WRL::ComPtr<ID3D11DeviceContext4> context4_;
        uint64_t adapter_luid_ = 0;
        Microsoft::WRL::ComPtr<ID3D11VertexShader> vertex_shader_;
        Microsoft::WRL::ComPtr<ID3D11PixelShader> pixel_shaders_[kShaderCount];   // by Shader
        // PsDraw and PsCopy writing the plane too, for while it is kept.
        Microsoft::WRL::ComPtr<ID3D11PixelShader> plane_draw_shader_;
        Microsoft::WRL::ComPtr<ID3D11PixelShader> plane_copy_shader_;
        Microsoft::WRL::ComPtr<ID3D11InputLayout> layout_;
        Microsoft::WRL::ComPtr<ID3D11Buffer> vertices_;
        Microsoft::WRL::ComPtr<ID3D11Buffer> constants_;
        Microsoft::WRL::ComPtr<ID3D11RasterizerState> raster_state_;
        // Everything written as the shader says: VRAM, and for copies the plane too.
        Microsoft::WRL::ComPtr<ID3D11BlendState> blend_state_;
        // Drawing while the plane is kept: VRAM as ever, and the plane kept as it was wherever
        // the shader's plane alpha is 0 - where it drew something translucent - with that alpha
        // stored, saying so. Blending each target its own way needs feature level 10.1.
        Microsoft::WRL::ComPtr<ID3D11BlendState> plane_blend_state_;

        // The plane beside VRAM (psx/shared_picture.h, Docs/DLSS-Plan.md): VRAM's layout at
        // scale_, RGBA16F. Made the first time it is asked for, and kept after. Its read copy is
        // what copies and 15-bit texels carry along, made current tile by tile with VRAM's.
        bool keep_planes_ = false;
        emulation::psx::PlaneView shown_plane_ = emulation::psx::PlaneView::kPicture;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> plane_target_;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> plane_target_view_;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> plane_source_;   // for showing it
        Microsoft::WRL::ComPtr<ID3D11Texture2D> plane_read_copy_;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> plane_read_copy_view_;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext1> context1_;   // ClearView, for ForgetPlanes
        // Beside each shared picture, the same area of the plane, while it is kept.
        Microsoft::WRL::ComPtr<ID3D11Texture2D> picture_planes_[kPictureSlots];

        // The warp check (RasterBackend::set_motion_check): a new picture's display area and
        // plane read back, and the last new picture moved by its motion compared with it -
        // against the last new picture left still - into the counters.
        void WarpCheck(uint32_t x, uint32_t y, UINT width, UINT height);
        bool motion_check_ = false;
        bool picture_new_ = false;     // NewPicture since the last ResolveDisplay
        bool picture_reset_ = false;
        std::vector<uint8_t> warp_last_;   // the last new picture, RGBA, warp_width_ wide
        UINT warp_width_ = 0, warp_height_ = 0;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> warp_colour_, warp_plane_;   // staging

        std::vector<Vertex> batch_;
        BatchKey batch_key_ = {};
        std::vector<uint32_t> upload_;   // one upload's pixels, converted
        // Per tile: newer on the card than in native VRAM, and than in the read copy.
        std::vector<uint8_t> dirty_ = std::vector<uint8_t>(kTilesX * kTilesY, 0);
        std::vector<uint8_t> stale_ = std::vector<uint8_t>(kTilesX * kTilesY, 1);
        // Per tile, the batch that last drew into it; a batch is numbered by batch_serial_, which
        // each Flush moves on.
        std::vector<uint32_t> batch_tile_ = std::vector<uint32_t>(kTilesX * kTilesY, 0);
        uint32_t batch_serial_ = 1;
        // Moved on by every draw. The last rectangle PrepareRead made current, and when.
        uint64_t drawn_serial_ = 0;
        uint64_t last_read_serial_ = 0;
        uint32_t last_read_[4] = {};
        bool last_read_valid_ = false;
        std::vector<uint8_t> wanted_ = std::vector<uint8_t>(kTilesX * kTilesY, 0);   // PrepareRead's
        std::string lost_;   // why the card can no longer be drawn with, once it cannot
        std::string adapter_name_;   // the card drawn on
        emulation::psx::RasterCounters counters_ = {};
    };

}   // namespace psxemu
