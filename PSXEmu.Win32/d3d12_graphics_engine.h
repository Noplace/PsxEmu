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

#include "igraphicsengine.h"

#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include "d3dx12.h"
#include <unordered_map>
#include <string>

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

 private:
    bool CreateDevice();
    bool CreateCommandQueue();
    bool CreateSwapChain(HWND window_handle);
    bool CreateDescriptorHeaps();
    bool CreateRenderTargetViews();
    bool CreateCommandAllocatorsAndList();
    bool CreateSyncObjects();
    bool CreateRootSignatureAndPSO();
    bool CreateFramebufferResources(int fb_width, int fb_height);

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

    // Framebuffer texture & upload heap - recreated whenever the incoming
    // frame's own width/height changes, not sized once. The PSX changes
    // resolution mid-boot and mid-game (menus commonly run at a lower
    // horizontal sample rate than gameplay); the GBA this engine was written
    // for never does, so the original always called this once.
    ComPtr<ID3D12Resource> fb_texture_;
    ComPtr<ID3D12Resource> fb_upload_heap_;
    ComPtr<ID3D12DescriptorHeap> srv_heap_;

    int fb_width_ = 0;
    int fb_height_ = 0;
    UINT64 fb_upload_buffer_size_ = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fb_placed_footprint_{};
    UINT fb_num_rows_ = 0;
    UINT64 fb_row_size_in_bytes_ = 0;
};
