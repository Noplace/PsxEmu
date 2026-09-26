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
#include <unordered_map>
#include <string>
#include <vector>

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

    bool Initialize(HWND window_handle, int width, int height) override;
    void Shutdown() override;

    void BeginFrame() override;
    void RenderFramebuffer(const void* data, int width, int height) override;
    void EndFrame() override;
    void Resize(int width, int height) override;

    void SetVsync(bool enabled) override;
    void SetPixelShader(const std::string& name) override;
    bool LoadCustomPixelShader(const std::string& name, const uint8_t* bytecode,
                               size_t size) override;
    bool LoadPixelShaderFromString(const std::string& name, const char* hlsl) override;
    bool LoadShaderChain(const std::string& name, const std::vector<ShaderPass>& passes) override;
    void SetOverlay(const psxemu::OverlayDrawData* overlay) override { overlay_ = overlay; }

 private:
    // ---- the overlay (ui/overlay), drawn last in EndFrame ----------------------------------
    bool CreateOverlayPipeline();
    void DrawOverlay();
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
};
