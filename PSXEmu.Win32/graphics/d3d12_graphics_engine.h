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

#include "graphics/igraphicsengine.h"

#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include "graphics/d3dx12.h"
#include "tools/letterbox.h"
#include <memory>
#include <unordered_map>
#include <string>
#include <vector>

namespace psxemu { class Streamline; }
namespace sl { struct FrameToken; }

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;

/*
  Ported from GBAEmu's D3D12GraphicsEngine (GBAEmu.Win32/graphics/), stripped
  of its Dear ImGui backend - this project has none - and adapted for a
  console that changes resolution at run time, which the GBA this engine was
  written for never does.

  Unlike D3D11Presenter, this one supports pixel-shader filters: every
  loaded shader gets its own pipeline state object, sharing one root
  signature and vertex shader, and SetPixelShader just switches which PSO
  the next RenderFramebuffer draws with.

  A filter can also be a chain of those shaders (LoadShaderChain, used by
  Super-xBR): each pass renders into its own texture at a whole multiple of the
  frame, the next pass reads it as t0 with the original frame as t1, and a
  linear blit puts the last one on screen. Chains are opt-in - with none
  selected, every draw goes through the single-shader path exactly as before.
*/
class D3D12GraphicsEngine : public IGraphicsEngine {
 public:
    D3D12GraphicsEngine();
    ~D3D12GraphicsEngine() override;

    void SetPreferredAdapter(uint64_t luid, const std::string&) override { preferred_luid_ = luid; }
    bool Initialize(HWND window_handle, int width, int height) override;
    void Shutdown() override;

    void BeginFrame() override;
    void RenderFramebuffer(const void* data, int width, int height) override;
    void EndFrame() override;
    void Resize(int width, int height) override;

    // Copied on the card from the rasteriser's texture into the frame's own, which everything
    // after - filters, chains, the overlay's glass - reads as it reads an uploaded frame.
    bool RenderSharedPicture(const emulation::psx::SharedPicture& picture) override;
    uint64_t SharedPictureAdapter() const override { return adapter_luid_; }

    void SetVsync(bool enabled) override;
    void SetPixelShader(const std::string& name) override;
    bool LoadCustomPixelShader(const std::string& name, const uint8_t* bytecode,
                               size_t size) override;
    bool LoadPixelShaderFromString(const std::string& name, const char* hlsl) override;
    bool LoadShaderChain(const std::string& name, const std::vector<ShaderPass>& passes) override;
    void SetOverlay(const psxemu::OverlayDrawData* overlay) override { overlay_ = overlay; }

    void SetDlss(const psxemu::DlssChoice& choice) override;
    psxemu::DlssStatus dlss_status() const override;
    bool DlssNeedsRemaking(const psxemu::DlssChoice& from,
                           const psxemu::DlssChoice& to) const override;
    void SetFrameGenerationAllowed(bool allowed) override { generation_allowed_ = allowed; }
    bool TakesOnlyNewPictures() const override { return generation_ready_; }
    void SetDlssTiming(psxemu::DlssTiming* timing) override { dlss_timing_ = timing; }

 private:
    // ---- the overlay (ui/overlay), drawn last in EndFrame ----------------------------------
    bool CreateOverlayPipeline();
    // Into the back buffer, or into `target` - Frame Generation's UI layer - when given.
    void DrawOverlay(const D3D12_CPU_DESCRIPTOR_HANDLE* target = nullptr);
    const psxemu::OverlayDrawData* overlay_ = nullptr;
    ComPtr<ID3D12RootSignature> overlay_root_;
    ComPtr<ID3D12PipelineState> overlay_pipeline_;
    ComPtr<ID3D12DescriptorHeap> overlay_srv_heap_;
    ComPtr<ID3D12Resource> overlay_atlas_;
    ComPtr<ID3D12Resource> overlay_atlas_upload_;   // kept until the next atlas, after a flush
    uint64_t overlay_atlas_version_ = 0;
    // Vertices and indices, one pair per frame in flight - the same reason as fb_upload_heap_.
    ComPtr<ID3D12Resource> overlay_vertices_[2];
    ComPtr<ID3D12Resource> overlay_indices_[2];
    size_t overlay_vertex_capacity_[2] = {};
    size_t overlay_index_capacity_[2] = {};

    bool CreateDevice();
    bool CreateCommandQueue();
    bool CreateSwapChain(HWND window_handle);
    bool CreateDescriptorHeaps();
    bool CreateRenderTargetViews();
    bool CreateCommandAllocatorsAndList();
    bool CreateSyncObjects();
    bool CreateRootSignatureAndPSO();
    bool CreateFramebufferResources(int fb_width, int fb_height);
    // The upload heaps an uploaded frame needs, made the first time one comes - a frame drawn
    // from a shared picture needs none, and at 8x they are tens of megabytes each.
    bool EnsureUploadHeaps();
    // Draws fb_texture_, which holds a frame width x height, into the window: the letterbox,
    // the filter or chain, all of it.
    void DrawFramebuffer(int width, int height);
    // ...and the single-shader draw under it, of the texture a width x height pair of SRVs at
    // `table` in `heap` shows, into `rect`: fb_texture_'s, or DLSS's output.
    void DrawTexture(ID3D12DescriptorHeap* heap, D3D12_GPU_DESCRIPTOR_HANDLE table, int width,
                     int height, const LetterboxRect& rect);
    // The rasteriser's texture for `picture`, opened on this device, or null.
    ID3D12Resource* OpenSharedPicture(const emulation::psx::SharedPicture& picture);
    // Hands back to their sources the pictures whose frames the card has finished.
    void ReleasePictures(UINT64 completed);
    bool CreatePipelineState(const void* bytecode, size_t size,
                             ComPtr<ID3D12PipelineState>& out);

    // A registered multi-pass filter. Holds the PSOs themselves (not names) so a chain keeps
    // working whatever happens to the single-shader map afterwards.
    struct ShaderChain {
        std::vector<ComPtr<ID3D12PipelineState>> pass_pipelines;
        std::vector<int> pass_scales;   // parallel to pass_pipelines; see ShaderPass::scale
    };
    // One draw of the active chain, worked out once per (chain, frame size) by
    // EnsureChainResources: which PSO, what it reads (t0 is always the previous draw's target,
    // or the emulator frame for the first draw) and where it writes.
    struct ChainDraw {
        ID3D12PipelineState* pipeline = nullptr;
        int target = -1;   // index into chain_targets_, or -1 for the window's back buffer
        UINT in_width = 0, in_height = 0;
        UINT out_width = 0, out_height = 0;   // unused when target < 0: the letterbox rect is
    };
    bool EnsureChainResources(const ShaderChain& chain, int src_width, int src_height);
    void RenderChain(const LetterboxRect& rect);

    void MoveToNextFrame();
    void FlushGPU();

    static const UINT kFrameCount = 2;   // double buffering

    ComPtr<IDXGIFactory4> factory_;
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12CommandQueue> command_queue_;
    ComPtr<IDXGISwapChain3> swap_chain_;

    ComPtr<ID3D12DescriptorHeap> rtv_heap_;
    UINT rtv_descriptor_size_ = 0;
    ComPtr<ID3D12Resource> render_targets_[kFrameCount];

    ComPtr<ID3D12CommandAllocator> command_allocators_[kFrameCount];
    ComPtr<ID3D12GraphicsCommandList> command_list_;

    UINT frame_index_ = 0;
    HANDLE fence_event_ = nullptr;
    ComPtr<ID3D12Fence> fence_;
    UINT64 fence_values_[kFrameCount] = { 0 };

    bool vsync_ = true;
    bool tearing_support_ = false;

    int width_ = 0;
    int height_ = 0;

    ComPtr<ID3D12RootSignature> root_signature_;
    ComPtr<ID3D12PipelineState> default_pipeline_state_;
    std::unordered_map<std::string, ComPtr<ID3D12PipelineState>> custom_shaders_;
    std::string current_shader_;
    ID3D12PipelineState* current_pipeline_state_ = nullptr;

    ComPtr<ID3DBlob> vs_blob_;

    // Multi-pass filters (LoadShaderChain). active_chain_ is null for the built-in pass-through
    // and every single shader, which then draw exactly as they always did.
    std::unordered_map<std::string, ShaderChain> chains_;
    const ShaderChain* active_chain_ = nullptr;
    // Linear-filtered copy from a chain's last render target to the window.
    ComPtr<ID3D12PipelineState> blit_pipeline_state_;
    // The chain's render targets and the descriptors that read them, valid for chain_res_*.
    std::vector<ComPtr<ID3D12Resource>> chain_targets_;
    std::vector<ChainDraw> chain_draws_;
    ComPtr<ID3D12DescriptorHeap> chain_rtv_heap_;
    ComPtr<ID3D12DescriptorHeap> chain_srv_heap_;   // two SRVs per draw: t0 = input, t1 = frame
    const ShaderChain* chain_res_chain_ = nullptr;
    int chain_res_width_ = 0;
    int chain_res_height_ = 0;
    bool chain_res_valid_ = false;

    // Framebuffer texture & upload heap - recreated whenever the incoming
    // frame's own width/height changes, not sized once. The PSX changes
    // resolution mid-boot and mid-game (menus commonly run at a lower
    // horizontal sample rate than gameplay); the GBA this engine was written
    // for never does, so the original always called this once.
    ComPtr<ID3D12Resource> fb_texture_;
    // One upload heap per frame-in-flight, not a single shared one: the CPU
    // runs ahead of the GPU by design (MoveToNextFrame only waits on the
    // fence for the back-buffer slot it is about to reuse, which is stale by
    // kFrameCount-1 frames), so a single heap would have the CPU's Map+memcpy
    // for frame N+1 racing the GPU's CopyTextureRegion still reading frame
    // N's data out of that same heap - a write-after-read hazard invisible
    // to anything that doesn't run the real D3D12 pipeline (boot_runner's
    // headless PPM dumps included), and visible on screen as blocky, torn
    // pixel blocks wherever the race lands, worst on fine detail.
    ComPtr<ID3D12Resource> fb_upload_heap_[kFrameCount];
    ComPtr<ID3D12DescriptorHeap> srv_heap_;

    int fb_width_ = 0;
    int fb_height_ = 0;
    UINT64 fb_upload_buffer_size_ = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fb_placed_footprint_{};
    UINT fb_num_rows_ = 0;
    UINT64 fb_row_size_in_bytes_ = 0;

    // Shared pictures (psx/shared_picture.h). The adapter this device is on; the rasteriser's
    // textures opened here, by their ids - one source's at a time, and a handful at most; which
    // picture fb_texture_ holds, so one shown again is not copied again; and the pictures each
    // frame drew from, handed back once the fence passes that frame.
    uint64_t adapter_luid_ = 0;
    uint64_t preferred_luid_ = 0;   // the card asked for; 0 leaves it to Windows
    struct OpenedPicture {
        uint64_t id;
        ComPtr<ID3D12Resource> resource;
    };
    std::vector<OpenedPicture> opened_pictures_;
    const emulation::psx::SharedPictureSource* opened_source_ = nullptr;
    uint64_t shown_texture_id_ = 0;
    uint64_t shown_serial_ = 0;
    struct PendingRelease {
        UINT64 fence_value;
        std::shared_ptr<emulation::psx::SharedPictureSource> source;
        uint64_t serial;
    };
    std::vector<PendingRelease> pending_releases_;
    // The last shared picture drawn, until a frame of pixels hands it back.
    std::shared_ptr<emulation::psx::SharedPictureSource> trail_source_;
    uint64_t trail_serial_ = 0;

    // ---- NVIDIA DLSS through Streamline (Docs/DLSS-Plan.md, phase 4) ------------------------
    //
    // Started in Initialize when DLSS is asked for, before the queue and swap chain, which are
    // then made through Streamline's proxies of the device and factory - its manual hooking,
    // under which nothing else of this device, and nothing of any other, passes through it. Each
    // new picture that comes with its plane, and is not interlaced, is turned into DLSS's inputs
    // - motion, depth and a hint where to trust the picture over its history - by one compute
    // pass, and DLSS draws the screen's picture from them. Anything failing leaves the renderer
    // drawing as it always has, and dlss_status says why.
    void StartStreamline();
    void StopStreamline();
    bool CreateDlssPipeline();
    // The output size for a `width` x `height` picture shown in `screen`, and whether DLSS runs
    // at all (graphics/dlss/dlss_choice.h).
    bool ChooseDlssOutput(int width, int height, const LetterboxRect& screen, int* out_width,
                          int* out_height);
    bool EnsureDlssTargets(int in_width, int in_height, int out_width, int out_height);
    ID3D12Resource* OpenPlanes(const emulation::psx::SharedPicture& picture);
    // DLSS on the picture in fb_texture_, into dlss_output_. False if it could not run.
    bool EvaluateDlss(const emulation::psx::SharedPicture& picture, ID3D12Resource* planes,
                      int out_width, int out_height);
    // Draws DLSS's picture of `picture` - made now if it is a new one - in place of the picture.
    // False, drawing nothing, when DLSS cannot take it: no plane, interlaced, a size it will not
    // take, or any step failing.
    bool DrawDlss(const emulation::psx::SharedPicture& picture);
    // Streamline's frame for picture `picture`: its frames are the pictures' numbers.
    sl::FrameToken* Token(uint32_t picture);

    // ---- Frame Generation (Docs/DLSS-Plan.md, phase 5) --------------------------------------
    //
    // Streamline started with it, and Reflex and PC Latency, when it is asked for with a DLSS
    // mode. Each new picture DLSS has just made is presented once, with the overlay drawn apart
    // as its UI layer and the picture before it as the HUD-less colour, and Frame Generation
    // makes the pictures between; a repeat is not presented at all (D3DPresenter). Anything
    // else presented - a film, an interlaced picture, the overlay over a paused one - goes with
    // Frame Generation off for that present, its resources kept.
    void SetUpGeneration(HWND window);
    // Frame Generation on or off from the next present, told only when that changes.
    void SetGenerationMode(bool on);
    bool EnsureUiTargets();
    // The picture before the overlay, the overlay alone and then over it, and the tags.
    void DrawGenerationLayers();
    void ReadGenerationState();
    void MarkLatency(uint32_t marker);   // an sl::PCLMarker, for present_picture_
    bool generation_ready_ = false;
    std::string generation_why_;
    int generation_most_ = 0;
    bool generation_dynamic_ = false;
    bool generation_allowed_ = true;    // 100% speed, paced (SetFrameGenerationAllowed)
    bool generation_on_ = false;        // what it was last told
    bool generation_this_frame_ = false;   // this frame's picture made by DLSS just now
    uint32_t present_picture_ = 0;      // this frame's picture, the first time it is presented
    uint32_t presented_picture_ = 0;
    uint32_t dlss_picture_ = 0;         // the picture dlss_output_ was made from
    int generation_frames_ = 0;         // presents since its state was last read
    UINT generation_present_count_ = 0; // the swap chain's count then
    UINT generation_min_size_ = 0;      // the smallest back buffer it takes
    ComPtr<ID3D12Resource> hudless_;    // back buffer's size: the picture before the overlay
    ComPtr<ID3D12Resource> ui_;         // ...and the overlay alone, premultiplied
    ComPtr<ID3D12DescriptorHeap> ui_rtv_heap_;
    ComPtr<ID3D12PipelineState> composite_pipeline_;
    int ui_width_ = 0, ui_height_ = 0;
    bool DlssRunning() const { return streamline_ != nullptr && dlss_pipeline_ != nullptr &&
                                      dlss_choice_.mode != psxemu::DlssMode::kOff; }

    psxemu::DlssChoice dlss_choice_;
    std::unique_ptr<psxemu::Streamline> streamline_;
    std::string dlss_why_;             // why DLSS does not run here; empty when it does
    std::string dlss_version_;
    ComPtr<ID3D12Device> sl_device_;   // Streamline's proxy of device_, which makes the queue
    ComPtr<IDXGIFactory2> sl_factory_; // ...and of factory_, which makes the swap chain
    ComPtr<ID3D12RootSignature> dlss_root_;
    ComPtr<ID3D12PipelineState> dlss_pipeline_;
    // Shader-visible: per frame in flight t0 the plane, u0-u2 motion, depth and hint; then the
    // output twice, t0 and t1 for the draw to the screen (kDlssOutputSlot); then the UI layer
    // twice, for putting it over the picture (kDlssUiSlot).
    ComPtr<ID3D12DescriptorHeap> dlss_heap_;
    static const UINT kDlssOutputSlot = 4 * kFrameCount;
    static const UINT kDlssUiSlot = kDlssOutputSlot + 2;
    ComPtr<ID3D12Resource> dlss_motion_, dlss_depth_, dlss_hint_, dlss_output_;
    int dlss_in_width_ = 0, dlss_in_height_ = 0;
    int dlss_out_width_ = 0, dlss_out_height_ = 0;
    // The planes' textures opened here, as the pictures' are (opened_pictures_).
    std::vector<OpenedPicture> opened_planes_;
    const emulation::psx::SharedPictureSource* planes_source_ = nullptr;
    // Whether dlss_output_ holds a picture at all (dlss_picture_ says which).
    bool dlss_output_valid_ = false;
    // The next evaluation starts afresh: a picture DLSS did not see came between.
    bool dlss_reset_ = true;
    // DLSS's range for each output asked about (slDLSSGetOptimalSettings): the screen's and the
    // mode's own ratio's are both asked about every frame a picture misses the first, so each
    // keeps its own entry. `taken` false when DLSS gave none.
    struct DlssRangeEntry {
        psxemu::DlssMode mode;
        int width, height;
        bool taken;
        psxemu::DlssRange range;
    };
    std::vector<DlssRangeEntry> dlss_ranges_;
    bool dlss_options_set_ = false;
    int dlss_options_width_ = 0, dlss_options_height_ = 0;
    psxemu::DlssChoice dlss_options_choice_;
    // Show Timings: the card's time for each picture DLSS makes - two timestamps per frame in
    // flight, around its inputs and itself, read back once the fence says that frame is done -
    // and the frames Frame Generation puts on the screen, by the swap chain's count.
    void CreateDlssTimers();   // without them DLSS runs, unmeasured
    void CollectDlssTiming();
    psxemu::DlssTiming* dlss_timing_ = nullptr;
    ComPtr<ID3D12QueryHeap> dlss_queries_;
    ComPtr<ID3D12Resource> dlss_query_readback_;
    bool dlss_query_pending_[kFrameCount] = {};
    UINT64 timestamp_frequency_ = 0;
    UINT timing_present_count_ = 0;
    bool timing_generated_ = false;   // the last present had frames generated after it
    bool dlss_timing_noted_ = false;  // the first measure written to the DLSS log
    // What DLSS last did, for PSXEMU_DLSS_LOG (psxemu::DlssNote) - written when it changes, not
    // at every frame.
    void NoteDlss(const std::string& line);
    std::string dlss_last_note_;
};
