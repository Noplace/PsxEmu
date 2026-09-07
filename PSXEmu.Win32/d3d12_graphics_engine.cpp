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
#include "d3d12_graphics_engine.h"

#include "tools/letterbox.h"

#include <cstring>

D3D12GraphicsEngine::D3D12GraphicsEngine() {}

D3D12GraphicsEngine::~D3D12GraphicsEngine() {
  Shutdown();
}

bool D3D12GraphicsEngine::Initialize(HWND window_handle, int width,
                                     int height) {
  width_ = width;
  height_ = height;

  if (!CreateDevice()) return false;
  if (!CreateCommandQueue()) return false;
  if (!CreateSwapChain(window_handle)) return false;
  if (!CreateDescriptorHeaps()) return false;
  if (!CreateRenderTargetViews()) return false;
  if (!CreateCommandAllocatorsAndList()) return false;
  if (!CreateSyncObjects()) return false;
  if (!CreateRootSignatureAndPSO()) return false;
  // A harmless first guess - RenderFramebuffer resizes this correctly as
  // soon as the first real frame arrives, the same as EnsureFrameTexture
  // does for the D3D11 path. Unlike the GBA this engine was written for,
  // the PSX has no single fixed resolution to seed it with instead.
  if (!CreateFramebufferResources(320, 240)) return false;

  return true;
}

void D3D12GraphicsEngine::SetVsync(bool enabled) {
  vsync_ = enabled;
}

bool D3D12GraphicsEngine::CreateDevice() {
  UINT dxgi_factory_flags = 0;
#if defined(_DEBUG)
  ComPtr<ID3D12Debug> debug_controller;
  if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug_controller)))) {
    debug_controller->EnableDebugLayer();
    dxgi_factory_flags |= DXGI_CREATE_FACTORY_DEBUG;
  }
#endif

  if (FAILED(CreateDXGIFactory2(dxgi_factory_flags, IID_PPV_ARGS(&factory_))))
    return false;

  if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0,
                               IID_PPV_ARGS(&device_)))) {
    return false;
  }

  ComPtr<IDXGIFactory5> factory5;
  if (SUCCEEDED(factory_.As(&factory5))) {
    BOOL allow_tearing = FALSE;
    if (SUCCEEDED(factory5->CheckFeatureSupport(
            DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow_tearing,
            sizeof(allow_tearing))) &&
        allow_tearing) {
      tearing_support_ = true;
    }
  }

  return true;
}

bool D3D12GraphicsEngine::CreateCommandQueue() {
  D3D12_COMMAND_QUEUE_DESC queue_desc = {};
  queue_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
  queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

  return SUCCEEDED(
      device_->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&command_queue_)));
}

bool D3D12GraphicsEngine::CreateSwapChain(HWND window_handle) {
  DXGI_SWAP_CHAIN_DESC1 swap_chain_desc = {};
  swap_chain_desc.BufferCount = kFrameCount;
  swap_chain_desc.Width = width_;
  swap_chain_desc.Height = height_;
  // Matches Gpu::ResolveFramebuffer's own byte order (B,G,R,A in memory) and
  // D3D11Presenter's swap chain format - every shader ported into this
  // engine assumes that, having had its opposite-order channel swap removed
  // during the port (see shaders/legacy_shaders.h's own comment).
  swap_chain_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  swap_chain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  swap_chain_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  swap_chain_desc.SampleDesc.Count = 1;
  if (tearing_support_) {
    swap_chain_desc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
  }

  ComPtr<IDXGISwapChain1> swap_chain;
  if (FAILED(factory_->CreateSwapChainForHwnd(command_queue_.Get(),
                                              window_handle, &swap_chain_desc,
                                              nullptr, nullptr,
                                              &swap_chain))) {
    return false;
  }

  factory_->MakeWindowAssociation(window_handle, DXGI_MWA_NO_ALT_ENTER);
  return SUCCEEDED(swap_chain.As(&swap_chain_));
}

bool D3D12GraphicsEngine::CreateDescriptorHeaps() {
  D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc = {};
  rtv_heap_desc.NumDescriptors = kFrameCount;
  rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  rtv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;

  if (FAILED(device_->CreateDescriptorHeap(&rtv_heap_desc,
                                           IID_PPV_ARGS(&rtv_heap_))))
    return false;

  rtv_descriptor_size_ =
      device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  return true;
}

bool D3D12GraphicsEngine::CreateRenderTargetViews() {
  D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle(
      rtv_heap_->GetCPUDescriptorHandleForHeapStart());

  for (UINT n = 0; n < kFrameCount; n++) {
    if (FAILED(swap_chain_->GetBuffer(n, IID_PPV_ARGS(&render_targets_[n]))))
      return false;
    device_->CreateRenderTargetView(render_targets_[n].Get(), nullptr,
                                    rtv_handle);
    rtv_handle.ptr += rtv_descriptor_size_;
  }
  return true;
}

bool D3D12GraphicsEngine::CreateCommandAllocatorsAndList() {
  for (UINT n = 0; n < kFrameCount; n++) {
    if (FAILED(device_->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&command_allocators_[n])))) {
      return false;
    }
  }

  return SUCCEEDED(device_->CreateCommandList(
             0, D3D12_COMMAND_LIST_TYPE_DIRECT, command_allocators_[0].Get(),
             nullptr, IID_PPV_ARGS(&command_list_))) &&
         SUCCEEDED(command_list_->Close());
}

bool D3D12GraphicsEngine::CreateSyncObjects() {
  if (FAILED(device_->CreateFence(fence_values_[0], D3D12_FENCE_FLAG_NONE,
                                  IID_PPV_ARGS(&fence_))))
    return false;

  fence_values_[0]++;
  fence_event_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);
  if (fence_event_ == nullptr) return false;

  frame_index_ = swap_chain_->GetCurrentBackBufferIndex();
  return true;
}

void D3D12GraphicsEngine::BeginFrame() {
  command_allocators_[frame_index_]->Reset();
  command_list_->Reset(command_allocators_[frame_index_].Get(), nullptr);

  if (!render_targets_[frame_index_]) {
    // Can't render, just return early. EndFrame will close the list.
    return;
  }

  D3D12_RESOURCE_BARRIER barrier = {};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
  barrier.Transition.pResource = render_targets_[frame_index_].Get();
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  command_list_->ResourceBarrier(1, &barrier);

  D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle(
      rtv_heap_->GetCPUDescriptorHandleForHeapStart());
  rtv_handle.ptr += (frame_index_ * rtv_descriptor_size_);
  command_list_->OMSetRenderTargets(1, &rtv_handle, FALSE, nullptr);

  const float clear_color[] = { 0.0f, 0.0f, 0.0f, 1.0f };
  command_list_->ClearRenderTargetView(rtv_handle, clear_color, 0, nullptr);
}

void D3D12GraphicsEngine::RenderFramebuffer(const void* data, int width,
                                            int height) {
  if (!command_list_ || !render_targets_[frame_index_]) return;
  if (data == nullptr || width <= 0 || height <= 0) return;

  // The PSX changes resolution mid-boot and mid-game - a menu commonly runs
  // at a lower horizontal sample rate than gameplay - so the framebuffer
  // resources are recreated whenever the incoming size actually changes,
  // the same trigger D3D11Presenter::EnsureFrameTexture already uses.
  if (width != fb_width_ || height != fb_height_) {
    // The old texture may still be referenced by GPU work the fence hasn't
    // retired yet - flush before replacing it, exactly as Resize() already
    // does before touching the render targets.
    FlushGPU();
    if (!CreateFramebufferResources(width, height)) return;
  }

  if (!fb_texture_ || !fb_upload_heap_) return;

  void* mapped_data = nullptr;
  D3D12_RANGE read_range = { 0, 0 };
  if (SUCCEEDED(fb_upload_heap_->Map(0, &read_range, &mapped_data))) {
    const uint8_t* src_data = static_cast<const uint8_t*>(data);
    uint8_t* dest_data =
        static_cast<uint8_t*>(mapped_data) + fb_placed_footprint_.Offset;

    const size_t src_pitch = static_cast<size_t>(width) * 4;
    for (UINT y = 0; y < static_cast<UINT>(height); ++y) {
      memcpy(dest_data + y * fb_placed_footprint_.Footprint.RowPitch,
             src_data + y * src_pitch, src_pitch);
    }
    fb_upload_heap_->Unmap(0, nullptr);
  }

  D3D12_RESOURCE_BARRIER barrier = {};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = fb_texture_.Get();
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  command_list_->ResourceBarrier(1, &barrier);

  CD3DX12_TEXTURE_COPY_LOCATION dest_loc(fb_texture_.Get(), 0);
  CD3DX12_TEXTURE_COPY_LOCATION src_loc(fb_upload_heap_.Get(),
                                        fb_placed_footprint_);
  command_list_->CopyTextureRegion(&dest_loc, 0, 0, 0, &src_loc, nullptr);

  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  command_list_->ResourceBarrier(1, &barrier);

  command_list_->SetPipelineState(current_pipeline_state_ != nullptr
                                      ? current_pipeline_state_
                                      : default_pipeline_state_.Get());
  command_list_->SetGraphicsRootSignature(root_signature_.Get());

  // Fixed 4:3, not derived from the frame's own width:height - see bug 47
  // (Docs/Bugs-Found.md) for why the latter is wrong for this console: PSX
  // horizontal resolution and the vertical/interlace range are independent
  // registers, both sampling the same physical ~4:3 frame real hardware
  // always drives. Reuses the same headless-tested helper D3D11Presenter
  // uses, so both engines agree on where the picture goes.
  const LetterboxRect rect = ComputeLetterboxRect(width_, height_, 4.0f / 3.0f);

  // outW/outH are the letterboxed viewport size, not the full window size -
  // Sharp Bilinear (legacy_shaders.h[1]) computes its texel scale directly
  // from these and would over- or under-scale against black bars otherwise.
  float shader_params[4] = { rect.width, rect.height, static_cast<float>(width),
                             static_cast<float>(height) };
  command_list_->SetGraphicsRoot32BitConstants(1, 4, shader_params, 0);

  ID3D12DescriptorHeap* pp_heaps[] = { srv_heap_.Get() };
  command_list_->SetDescriptorHeaps(_countof(pp_heaps), pp_heaps);
  command_list_->SetGraphicsRootDescriptorTable(
      0, srv_heap_->GetGPUDescriptorHandleForHeapStart());

  const D3D12_VIEWPORT viewport = { rect.x,     rect.y,      rect.width,
                                    rect.height, 0.0f,       1.0f };
  const D3D12_RECT scissor = {
      static_cast<LONG>(rect.x), static_cast<LONG>(rect.y),
      static_cast<LONG>(rect.x + rect.width),
      static_cast<LONG>(rect.y + rect.height)
  };
  command_list_->RSSetViewports(1, &viewport);
  command_list_->RSSetScissorRects(1, &scissor);
  command_list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

  command_list_->DrawInstanced(3, 1, 0, 0);
}

void D3D12GraphicsEngine::EndFrame() {
  if (!render_targets_[frame_index_]) {
    command_list_->Close();
    return;
  }

  D3D12_RESOURCE_BARRIER barrier = {};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
  barrier.Transition.pResource = render_targets_[frame_index_].Get();
  barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
  barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  command_list_->ResourceBarrier(1, &barrier);

  command_list_->Close();

  ID3D12CommandList* pp_command_lists[] = { command_list_.Get() };
  command_queue_->ExecuteCommandLists(_countof(pp_command_lists),
                                      pp_command_lists);

  const UINT sync_interval = vsync_ ? 1 : 0;
  const UINT present_flags =
      (tearing_support_ && !vsync_) ? DXGI_PRESENT_ALLOW_TEARING : 0;
  swap_chain_->Present(sync_interval, present_flags);

  MoveToNextFrame();
}

void D3D12GraphicsEngine::MoveToNextFrame() {
  const UINT64 current_fence_value = fence_values_[frame_index_];
  command_queue_->Signal(fence_.Get(), current_fence_value);

  frame_index_ = swap_chain_->GetCurrentBackBufferIndex();

  if (fence_->GetCompletedValue() < fence_values_[frame_index_]) {
    fence_->SetEventOnCompletion(fence_values_[frame_index_], fence_event_);
    WaitForSingleObjectEx(fence_event_, INFINITE, FALSE);
  }

  fence_values_[frame_index_] = current_fence_value + 1;
}

void D3D12GraphicsEngine::FlushGPU() {
  if (command_queue_ && fence_) {
    const UINT64 fence_value = fence_values_[frame_index_];
    command_queue_->Signal(fence_.Get(), fence_value);
    fence_->SetEventOnCompletion(fence_value, fence_event_);
    WaitForSingleObjectEx(fence_event_, INFINITE, FALSE);
    fence_values_[frame_index_]++;
  }
}

void D3D12GraphicsEngine::Resize(int width, int height) {
  if (!swap_chain_ || width <= 0 || height <= 0) return;

  width_ = width;
  height_ = height;

  FlushGPU();

  for (UINT n = 0; n < kFrameCount; n++) {
    render_targets_[n].Reset();
    fence_values_[n] = fence_values_[frame_index_];
  }

  const UINT flags = tearing_support_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
  if (FAILED(swap_chain_->ResizeBuffers(kFrameCount, width_, height_,
                                        DXGI_FORMAT_B8G8R8A8_UNORM, flags)))
    return;

  frame_index_ = swap_chain_->GetCurrentBackBufferIndex();
  CreateRenderTargetViews();
}

void D3D12GraphicsEngine::Shutdown() {
  FlushGPU();

  if (fence_event_) {
    CloseHandle(fence_event_);
    fence_event_ = nullptr;
  }

  // ComPtr members release themselves on destruction; nothing else to do.
}

void D3D12GraphicsEngine::SetPixelShader(const std::string& name) {
  current_shader_ = name;

  const auto it = custom_shaders_.find(current_shader_);
  if (!current_shader_.empty() && it != custom_shaders_.end()) {
    current_pipeline_state_ = it->second.Get();
  } else {
    current_pipeline_state_ = default_pipeline_state_.Get();
  }
}

bool D3D12GraphicsEngine::CreateRootSignatureAndPSO() {
  CD3DX12_DESCRIPTOR_RANGE range;
  range.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);

  CD3DX12_ROOT_PARAMETER root_params[2];
  root_params[0].InitAsDescriptorTable(1, &range, D3D12_SHADER_VISIBILITY_PIXEL);
  // 4 floats (outW, outH, inW, inH) = 16 bytes = 4 32-bit values.
  root_params[1].InitAsConstants(4, 0, 0, D3D12_SHADER_VISIBILITY_PIXEL);

  CD3DX12_STATIC_SAMPLER_DESC samplers[2];
  samplers[0].Init(0, D3D12_FILTER_MIN_MAG_MIP_POINT);
  samplers[1].Init(1, D3D12_FILTER_MIN_MAG_MIP_LINEAR);

  CD3DX12_ROOT_SIGNATURE_DESC root_sig_desc;
  root_sig_desc.Init(2, root_params, 2, samplers,
                     D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

  ComPtr<ID3DBlob> serialized_root_sig, error_blob;
  if (FAILED(D3D12SerializeRootSignature(&root_sig_desc,
                                         D3D_ROOT_SIGNATURE_VERSION_1,
                                         &serialized_root_sig, &error_blob))) {
    return false;
  }
  if (FAILED(device_->CreateRootSignature(
          0, serialized_root_sig->GetBufferPointer(),
          serialized_root_sig->GetBufferSize(),
          IID_PPV_ARGS(&root_signature_)))) {
    return false;
  }

  // Standard "big triangle" full-screen technique: no vertex/index buffer,
  // the three vertices are synthesised from SV_VertexID.
  const char* vs_code = R"HLSL(
  struct VSOut { float4 pos:SV_POSITION; float2 uv:TEXCOORD0; };
  VSOut main(uint vid : SV_VertexID) {
      float2 verts[3] = { float2(-1,-1), float2(-1,3), float2(3,-1) };
      VSOut o;
      o.pos = float4(verts[vid], 0, 1);
      o.uv = (verts[vid] + 1.0) * 0.5;
      o.uv.y = 1.0 - o.uv.y;
      return o;
  })HLSL";

  // The built-in pass-through - point sampling, no filtering, no swizzle
  // (this engine's framebuffer texture already matches the byte order
  // Gpu::ResolveFramebuffer packs, unlike GBAEmu's original).
  const char* default_shader =
      "Texture2D g_Tex : register(t0); SamplerState g_Point : register(s0);"
      "float4 main(float4 pos:SV_POSITION, float2 uv:TEXCOORD0) : SV_TARGET {"
      "    return g_Tex.Sample(g_Point, uv);"
      "}";

  if (FAILED(D3DCompile(vs_code, strlen(vs_code), nullptr, nullptr, nullptr,
                        "main", "vs_5_0", 0, 0, &vs_blob_, nullptr)))
    return false;

  ComPtr<ID3DBlob> ps_blob;
  if (FAILED(D3DCompile(default_shader, strlen(default_shader), nullptr,
                        nullptr, nullptr, "main", "ps_5_0", 0, 0, &ps_blob,
                        nullptr)))
    return false;

  D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc = {};
  pso_desc.pRootSignature = root_signature_.Get();
  pso_desc.VS = { vs_blob_->GetBufferPointer(), vs_blob_->GetBufferSize() };
  pso_desc.PS = { ps_blob->GetBufferPointer(), ps_blob->GetBufferSize() };
  pso_desc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
  pso_desc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
  pso_desc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
  pso_desc.DepthStencilState.DepthEnable = FALSE;
  pso_desc.SampleMask = UINT_MAX;
  pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pso_desc.NumRenderTargets = 1;
  // Matches Gpu::ResolveFramebuffer's byte order - see CreateSwapChain's own
  // comment. Every PSO in this engine (default and every loaded filter)
  // targets this same format.
  pso_desc.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
  pso_desc.SampleDesc.Count = 1;

  return SUCCEEDED(device_->CreateGraphicsPipelineState(
      &pso_desc, IID_PPV_ARGS(&default_pipeline_state_)));
}

bool D3D12GraphicsEngine::CreateFramebufferResources(int fb_width,
                                                      int fb_height) {
  fb_width_ = fb_width;
  fb_height_ = fb_height;

  D3D12_RESOURCE_DESC tex_desc = {};
  tex_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  tex_desc.Width = fb_width_;
  tex_desc.Height = fb_height_;
  tex_desc.DepthOrArraySize = 1;
  tex_desc.MipLevels = 1;
  tex_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  tex_desc.SampleDesc.Count = 1;

  const CD3DX12_HEAP_PROPERTIES default_heap(D3D12_HEAP_TYPE_DEFAULT);
  if (FAILED(device_->CreateCommittedResource(
          &default_heap, D3D12_HEAP_FLAG_NONE, &tex_desc,
          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
          IID_PPV_ARGS(&fb_texture_))))
    return false;

  device_->GetCopyableFootprints(&tex_desc, 0, 1, 0, &fb_placed_footprint_,
                                 &fb_num_rows_, &fb_row_size_in_bytes_,
                                 &fb_upload_buffer_size_);

  const CD3DX12_HEAP_PROPERTIES upload_heap_props(D3D12_HEAP_TYPE_UPLOAD);
  const CD3DX12_RESOURCE_DESC upload_desc =
      CD3DX12_RESOURCE_DESC::Buffer(fb_upload_buffer_size_);
  if (FAILED(device_->CreateCommittedResource(
          &upload_heap_props, D3D12_HEAP_FLAG_NONE, &upload_desc,
          D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
          IID_PPV_ARGS(&fb_upload_heap_))))
    return false;

  D3D12_DESCRIPTOR_HEAP_DESC srv_heap_desc = {};
  srv_heap_desc.NumDescriptors = 1;  // just the framebuffer - no ImGui here
  srv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  srv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  if (FAILED(device_->CreateDescriptorHeap(&srv_heap_desc,
                                           IID_PPV_ARGS(&srv_heap_))))
    return false;

  D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
  srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv_desc.Format = tex_desc.Format;
  srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srv_desc.Texture2D.MipLevels = 1;

  device_->CreateShaderResourceView(
      fb_texture_.Get(), &srv_desc, srv_heap_->GetCPUDescriptorHandleForHeapStart());

  return true;
}

bool D3D12GraphicsEngine::LoadCustomPixelShader(const std::string& name,
                                                const uint8_t* bytecode,
                                                size_t size) {
  if (!device_ || !root_signature_ || !vs_blob_) return false;

  ComPtr<ID3D12PipelineState> custom_pipeline_state;
  D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc = {};
  pso_desc.pRootSignature = root_signature_.Get();
  pso_desc.VS = { vs_blob_->GetBufferPointer(), vs_blob_->GetBufferSize() };
  pso_desc.PS = { bytecode, size };
  pso_desc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
  pso_desc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
  pso_desc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
  pso_desc.DepthStencilState.DepthEnable = FALSE;
  pso_desc.SampleMask = UINT_MAX;
  pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pso_desc.NumRenderTargets = 1;
  pso_desc.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
  pso_desc.SampleDesc.Count = 1;

  if (FAILED(device_->CreateGraphicsPipelineState(
          &pso_desc, IID_PPV_ARGS(&custom_pipeline_state)))) {
    return false;
  }

  custom_shaders_[name] = custom_pipeline_state;
  return true;
}

bool D3D12GraphicsEngine::LoadPixelShaderFromString(const std::string& name,
                                                    const char* hlsl) {
  ComPtr<ID3DBlob> ps_blob;
  if (FAILED(D3DCompile(hlsl, strlen(hlsl), nullptr, nullptr, nullptr, "main",
                        "ps_5_0", 0, 0, &ps_blob, nullptr)))
    return false;
  return LoadCustomPixelShader(
      name, static_cast<const uint8_t*>(ps_blob->GetBufferPointer()),
      ps_blob->GetBufferSize());
}
