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
#include "graphics/d3d12_graphics_engine.h"

#include "shaders/overlay_shaders.h"
#include "tools/letterbox.h"

#include <algorithm>
#include <cstring>

D3D12GraphicsEngine::D3D12GraphicsEngine() {}

D3D12GraphicsEngine::~D3D12GraphicsEngine() {
    Shutdown();
}

bool D3D12GraphicsEngine::Initialize(HWND window_handle, int width, int height) {
    width_ = width;
    height_ = height;

    if (!CreateDevice())
        return false;
    if (!CreateCommandQueue())
        return false;
    if (!CreateSwapChain(window_handle))
        return false;
    if (!CreateDescriptorHeaps())
        return false;
    if (!CreateRenderTargetViews())
        return false;
    if (!CreateCommandAllocatorsAndList())
        return false;
    if (!CreateSyncObjects())
        return false;
    if (!CreateRootSignatureAndPSO())
        return false;
    // A harmless first guess - RenderFramebuffer resizes this correctly as
    // soon as the first real frame arrives, the same as EnsureFrameTexture
    // does for the D3D11 path. Unlike the GBA this engine was written for,
    // the PSX has no single fixed resolution to seed it with instead.
    if (!CreateFramebufferResources(320, 240))
        return false;
    // The overlay is an extra: if it cannot be made, the picture still is.
    if (!CreateOverlayPipeline()) {
        overlay_pipeline_.Reset();
        overlay_root_.Reset();
    }

    return true;
}

bool D3D12GraphicsEngine::CreateOverlayPipeline() {
    static_assert(sizeof(overlay_vertices_) / sizeof(overlay_vertices_[0]) == kFrameCount,
                  "one overlay buffer per frame in flight");
    CD3DX12_DESCRIPTOR_RANGE range;
    range.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);
    CD3DX12_ROOT_PARAMETER parameter;
    parameter.InitAsDescriptorTable(1, &range, D3D12_SHADER_VISIBILITY_PIXEL);
    CD3DX12_STATIC_SAMPLER_DESC sampler;
    sampler.Init(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                 D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
    CD3DX12_ROOT_SIGNATURE_DESC description;
    description.Init(1, &parameter, 1, &sampler,
                     D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
    ComPtr<ID3DBlob> serialized, errors;
    if (FAILED(D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1,
                                           &serialized, &errors)) ||
        FAILED(device_->CreateRootSignature(0, serialized->GetBufferPointer(),
                                            serialized->GetBufferSize(),
                                            IID_PPV_ARGS(&overlay_root_))))
        return false;

    ComPtr<ID3DBlob> vs, ps;
    if (FAILED(D3DCompile(psxemu::kOverlayHlsl, sizeof(psxemu::kOverlayHlsl) - 1, nullptr, nullptr,
                          nullptr, "VsMain", "vs_5_0", 0, 0, &vs, nullptr)) ||
        FAILED(D3DCompile(psxemu::kOverlayHlsl, sizeof(psxemu::kOverlayHlsl) - 1, nullptr, nullptr,
                          nullptr, "PsMain", "ps_5_0", 0, 0, &ps, nullptr)))
        return false;

    const D3D12_INPUT_ELEMENT_DESC layout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 16,
          D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = overlay_root_.Get();
    pso.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    pso.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    pso.InputLayout = { layout, 3 };
    pso.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    D3D12_RENDER_TARGET_BLEND_DESC& blend = pso.BlendState.RenderTarget[0];
    blend.BlendEnable = TRUE;
    blend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
    blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    blend.BlendOp = D3D12_BLEND_OP_ADD;
    blend.SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    pso.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
    pso.DepthStencilState.DepthEnable = FALSE;
    pso.SampleMask = UINT_MAX;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
    pso.SampleDesc.Count = 1;
    if (FAILED(device_->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&overlay_pipeline_))))
        return false;

    D3D12_DESCRIPTOR_HEAP_DESC heap = {};
    heap.NumDescriptors = 1;
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    return SUCCEEDED(device_->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&overlay_srv_heap_)));
}

// Over everything else, into the back buffer, in the command list EndFrame is about to close.
void D3D12GraphicsEngine::DrawOverlay() {
    const psxemu::OverlayDrawData* data = overlay_;
    overlay_ = nullptr;
    if (data == nullptr || data->empty() || !overlay_pipeline_ || !render_targets_[frame_index_])
        return;

    if (!overlay_atlas_ || overlay_atlas_version_ != data->atlas_version) {
        // The old atlas and its upload may still be in a frame the GPU has not finished.
        FlushGPU();
        overlay_atlas_.Reset();
        overlay_atlas_upload_.Reset();
        D3D12_RESOURCE_DESC texture = {};
        texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texture.Width = static_cast<UINT64>(data->atlas_width);
        texture.Height = static_cast<UINT>(data->atlas_height);
        texture.DepthOrArraySize = 1;
        texture.MipLevels = 1;
        texture.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        texture.SampleDesc.Count = 1;
        const CD3DX12_HEAP_PROPERTIES default_heap(D3D12_HEAP_TYPE_DEFAULT);
        if (FAILED(device_->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &texture,
                                                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                    IID_PPV_ARGS(&overlay_atlas_))))
            return;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
        UINT rows = 0;
        UINT64 row_bytes = 0, total = 0;
        device_->GetCopyableFootprints(&texture, 0, 1, 0, &footprint, &rows, &row_bytes, &total);
        const CD3DX12_HEAP_PROPERTIES upload_heap(D3D12_HEAP_TYPE_UPLOAD);
        const CD3DX12_RESOURCE_DESC upload = CD3DX12_RESOURCE_DESC::Buffer(total);
        if (FAILED(device_->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &upload,
                                                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                    IID_PPV_ARGS(&overlay_atlas_upload_)))) {
            overlay_atlas_.Reset();
            return;
        }
        void* mapped = nullptr;
        const D3D12_RANGE nothing = { 0, 0 };
        if (FAILED(overlay_atlas_upload_->Map(0, &nothing, &mapped))) {
            overlay_atlas_.Reset();
            return;
        }
        for (UINT y = 0; y < rows; ++y) {
            memcpy(static_cast<uint8_t*>(mapped) + footprint.Offset + y * footprint.Footprint.RowPitch,
                   data->atlas + static_cast<size_t>(y) * data->atlas_width * 4,
                   static_cast<size_t>(data->atlas_width) * 4);
        }
        overlay_atlas_upload_->Unmap(0, nullptr);
        CD3DX12_TEXTURE_COPY_LOCATION to(overlay_atlas_.Get(), 0);
        CD3DX12_TEXTURE_COPY_LOCATION from(overlay_atlas_upload_.Get(), footprint);
        command_list_->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        const CD3DX12_RESOURCE_BARRIER ready = CD3DX12_RESOURCE_BARRIER::Transition(
            overlay_atlas_.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        command_list_->ResourceBarrier(1, &ready);

        D3D12_SHADER_RESOURCE_VIEW_DESC view = {};
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Texture2D.MipLevels = 1;
        device_->CreateShaderResourceView(overlay_atlas_.Get(), &view,
                                          overlay_srv_heap_->GetCPUDescriptorHandleForHeapStart());
        overlay_atlas_version_ = data->atlas_version;
    }

    // This frame's buffers, grown to the largest overlay so far. The fence MoveToNextFrame
    // waited on means the GPU is done with them from the last time this slot came round.
    auto ensure = [this](ComPtr<ID3D12Resource>& buffer, size_t& capacity, size_t bytes) {
        if (buffer && capacity >= bytes)
            return true;
        buffer.Reset();
        const size_t grown = (std::max)((std::max)(bytes, capacity * 2), static_cast<size_t>(65536));
        const CD3DX12_HEAP_PROPERTIES upload_heap(D3D12_HEAP_TYPE_UPLOAD);
        const CD3DX12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(grown);
        if (FAILED(device_->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                    IID_PPV_ARGS(&buffer))))
            return false;
        capacity = grown;
        return true;
    };
    const size_t vertex_bytes = data->vertex_count * sizeof(psxemu::OverlayVertex);
    const size_t index_bytes = data->index_count * sizeof(uint32_t);
    ComPtr<ID3D12Resource>& vertices = overlay_vertices_[frame_index_];
    ComPtr<ID3D12Resource>& indices = overlay_indices_[frame_index_];
    if (!ensure(vertices, overlay_vertex_capacity_[frame_index_], vertex_bytes) ||
        !ensure(indices, overlay_index_capacity_[frame_index_], index_bytes))
        return;
    void* mapped = nullptr;
    const D3D12_RANGE nothing = { 0, 0 };
    if (FAILED(vertices->Map(0, &nothing, &mapped)))
        return;
    psxemu::WriteOverlayVertices(*data, width_, height_, false,
                                 static_cast<psxemu::OverlayVertex*>(mapped));
    vertices->Unmap(0, nullptr);
    if (FAILED(indices->Map(0, &nothing, &mapped)))
        return;
    memcpy(mapped, data->indices, index_bytes);
    indices->Unmap(0, nullptr);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv(rtv_heap_->GetCPUDescriptorHandleForHeapStart());
    rtv.ptr += static_cast<SIZE_T>(frame_index_) * rtv_descriptor_size_;
    command_list_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    const D3D12_VIEWPORT viewport = { 0.0f, 0.0f, static_cast<float>(width_),
                                      static_cast<float>(height_), 0.0f, 1.0f };
    const D3D12_RECT scissor = { 0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_) };
    command_list_->RSSetViewports(1, &viewport);
    command_list_->RSSetScissorRects(1, &scissor);
    command_list_->SetPipelineState(overlay_pipeline_.Get());
    command_list_->SetGraphicsRootSignature(overlay_root_.Get());
    ID3D12DescriptorHeap* heaps[] = { overlay_srv_heap_.Get() };
    command_list_->SetDescriptorHeaps(1, heaps);
    command_list_->SetGraphicsRootDescriptorTable(
        0, overlay_srv_heap_->GetGPUDescriptorHandleForHeapStart());
    command_list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D12_VERTEX_BUFFER_VIEW vertex_view = {};
    vertex_view.BufferLocation = vertices->GetGPUVirtualAddress();
    vertex_view.SizeInBytes = static_cast<UINT>(vertex_bytes);
    vertex_view.StrideInBytes = sizeof(psxemu::OverlayVertex);
    D3D12_INDEX_BUFFER_VIEW index_view = {};
    index_view.BufferLocation = indices->GetGPUVirtualAddress();
    index_view.SizeInBytes = static_cast<UINT>(index_bytes);
    index_view.Format = DXGI_FORMAT_R32_UINT;
    command_list_->IASetVertexBuffers(0, 1, &vertex_view);
    command_list_->IASetIndexBuffer(&index_view);
    command_list_->DrawIndexedInstanced(static_cast<UINT>(data->index_count), 1, 0, 0, 0);
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

    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_)))) {
        return false;
    }

    ComPtr<IDXGIFactory5> factory5;
    if (SUCCEEDED(factory_.As(&factory5))) {
        BOOL allow_tearing = FALSE;
        if (SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                                                    &allow_tearing, sizeof(allow_tearing))) &&
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

    return SUCCEEDED(device_->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&command_queue_)));
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
    if (FAILED(factory_->CreateSwapChainForHwnd(command_queue_.Get(), window_handle,
                                                &swap_chain_desc, nullptr, nullptr, &swap_chain))) {
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

    if (FAILED(device_->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(&rtv_heap_))))
        return false;

    rtv_descriptor_size_ =
        device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    return true;
}

bool D3D12GraphicsEngine::CreateRenderTargetViews() {
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle(rtv_heap_->GetCPUDescriptorHandleForHeapStart());

    for (UINT n = 0; n < kFrameCount; n++) {
        if (FAILED(swap_chain_->GetBuffer(n, IID_PPV_ARGS(&render_targets_[n]))))
            return false;
        device_->CreateRenderTargetView(render_targets_[n].Get(), nullptr, rtv_handle);
        rtv_handle.ptr += rtv_descriptor_size_;
    }
    return true;
}

bool D3D12GraphicsEngine::CreateCommandAllocatorsAndList() {
    for (UINT n = 0; n < kFrameCount; n++) {
        if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                   IID_PPV_ARGS(&command_allocators_[n])))) {
            return false;
        }
    }

    return SUCCEEDED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                command_allocators_[0].Get(), nullptr,
                                                IID_PPV_ARGS(&command_list_))) &&
           SUCCEEDED(command_list_->Close());
}

bool D3D12GraphicsEngine::CreateSyncObjects() {
    if (FAILED(
            device_->CreateFence(fence_values_[0], D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_))))
        return false;

    fence_values_[0]++;
    fence_event_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (fence_event_ == nullptr)
        return false;

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

    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle(rtv_heap_->GetCPUDescriptorHandleForHeapStart());
    rtv_handle.ptr += (frame_index_ * rtv_descriptor_size_);
    command_list_->OMSetRenderTargets(1, &rtv_handle, FALSE, nullptr);

    const float clear_color[] = { 0.0f, 0.0f, 0.0f, 1.0f };
    command_list_->ClearRenderTargetView(rtv_handle, clear_color, 0, nullptr);
}

void D3D12GraphicsEngine::RenderFramebuffer(const void* data, int width, int height) {
    if (!command_list_ || !render_targets_[frame_index_])
        return;
    if (data == nullptr || width <= 0 || height <= 0)
        return;

    // The PSX changes resolution mid-boot and mid-game - a menu commonly runs
    // at a lower horizontal sample rate than gameplay - so the framebuffer
    // resources are recreated whenever the incoming size actually changes,
    // the same trigger D3D11Presenter::EnsureFrameTexture already uses.
    if (width != fb_width_ || height != fb_height_) {
        // The old texture may still be referenced by GPU work the fence hasn't
        // retired yet - flush before replacing it, exactly as Resize() already
        // does before touching the render targets.
        FlushGPU();
        if (!CreateFramebufferResources(width, height))
            return;
    }

    ID3D12Resource* const upload_heap = fb_upload_heap_[frame_index_].Get();
    if (!fb_texture_ || !upload_heap)
        return;

    void* mapped_data = nullptr;
    D3D12_RANGE read_range = { 0, 0 };
    if (SUCCEEDED(upload_heap->Map(0, &read_range, &mapped_data))) {
        const uint8_t* src_data = static_cast<const uint8_t*>(data);
        uint8_t* dest_data = static_cast<uint8_t*>(mapped_data) + fb_placed_footprint_.Offset;

        const size_t src_pitch = static_cast<size_t>(width) * 4;
        for (UINT y = 0; y < static_cast<UINT>(height); ++y) {
            memcpy(dest_data + y * fb_placed_footprint_.Footprint.RowPitch,
                   src_data + y * src_pitch, src_pitch);
        }
        upload_heap->Unmap(0, nullptr);
    }

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = fb_texture_.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    command_list_->ResourceBarrier(1, &barrier);

    CD3DX12_TEXTURE_COPY_LOCATION dest_loc(fb_texture_.Get(), 0);
    CD3DX12_TEXTURE_COPY_LOCATION src_loc(upload_heap, fb_placed_footprint_);
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
    LetterboxRect rect = ComputeLetterboxRect(width_, height_, 4.0f / 3.0f);

    // A multi-pass filter takes over from here: it draws every pass itself, the last into the
    // same letterbox rect. If its render targets can't be made it falls through and the frame
    // is drawn by the pass-through below, which SetPixelShader left selected for that case.
    if (active_chain_ != nullptr && EnsureChainResources(*active_chain_, width, height)) {
        RenderChain(rect);
        return;
    }

    // Point sampling (Nearest Neighbor, and the built-in default when no
    // filter is selected) maps source texel columns onto destination pixels
    // through the rasterizer's own fractional interpolation. When the
    // letterboxed rect isn't an exact whole multiple of the source frame,
    // some texel columns land under more destination pixels than their
    // neighbours - the picture is still the right overall size, but
    // individual columns/rows are uneven. That reads as "extra width" on
    // anything with fine vertical detail (foliage, dithered shadow edges)
    // while flat ground and walls hide it completely, which is why it looks
    // like specific sprites are glitched rather than the whole screen.
    // Snapping the displayed size down to the nearest whole multiple of the
    // frame keeps every source pixel the same size on screen; the pixels
    // freed up just widen the letterbox border. Shaders that already handle
    // fractional scale correctly (Sharp Bilinear and friends) don't need
    // this and are left alone.
    /*
    const bool point_filtered = current_pipeline_state_ == nullptr ||
        current_pipeline_state_ == default_pipeline_state_.Get();
    if (point_filtered && width > 0 && height > 0) {
        const int scale_x = static_cast<int>(rect.width) / width;
        const int scale_y = static_cast<int>(rect.height) / height;
        int scale = (scale_x < scale_y) ? scale_x : scale_y;
        if (scale < 1)
            scale = 1;
        const float snapped_width = static_cast<float>(width * scale);
        const float snapped_height = static_cast<float>(height * scale);
        rect.x += (rect.width - snapped_width) * 0.5f;
        rect.y += (rect.height - snapped_height) * 0.5f;
        rect.width = snapped_width;
        rect.height = snapped_height;
    }*/

    // outW/outH are the letterboxed viewport size, not the full window size -
    // Sharp Bilinear (legacy_shaders.h[1]) computes its texel scale directly
    // from these and would over- or under-scale against black bars otherwise.
    float shader_params[4] = { rect.width, rect.height, static_cast<float>(width),
                               static_cast<float>(height) };
    command_list_->SetGraphicsRoot32BitConstants(1, 4, shader_params, 0);

    ID3D12DescriptorHeap* pp_heaps[] = { srv_heap_.Get() };
    command_list_->SetDescriptorHeaps(_countof(pp_heaps), pp_heaps);
    command_list_->SetGraphicsRootDescriptorTable(0,
                                                  srv_heap_->GetGPUDescriptorHandleForHeapStart());

    const D3D12_VIEWPORT viewport = { rect.x, rect.y, rect.width, rect.height, 0.0f, 1.0f };
    const D3D12_RECT scissor = { static_cast<LONG>(rect.x), static_cast<LONG>(rect.y),
                                 static_cast<LONG>(rect.x + rect.width),
                                 static_cast<LONG>(rect.y + rect.height) };
    command_list_->RSSetViewports(1, &viewport);
    command_list_->RSSetScissorRects(1, &scissor);
    command_list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    command_list_->DrawInstanced(3, 1, 0, 0);
}

void D3D12GraphicsEngine::EndFrame() {
    if (!render_targets_[frame_index_]) {
        overlay_ = nullptr;
        command_list_->Close();
        return;
    }
    DrawOverlay();

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
    command_queue_->ExecuteCommandLists(_countof(pp_command_lists), pp_command_lists);

    const UINT sync_interval = vsync_ ? 1 : 0;
    const UINT present_flags = (tearing_support_ && !vsync_) ? DXGI_PRESENT_ALLOW_TEARING : 0;
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
    if (!swap_chain_ || width <= 0 || height <= 0)
        return;

    width_ = width;
    height_ = height;

    FlushGPU();

    for (UINT n = 0; n < kFrameCount; n++) {
        render_targets_[n].Reset();
        fence_values_[n] = fence_values_[frame_index_];
    }

    const UINT flags = tearing_support_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    if (FAILED(swap_chain_->ResizeBuffers(kFrameCount, width_, height_, DXGI_FORMAT_B8G8R8A8_UNORM,
                                          flags)))
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
    active_chain_ = nullptr;

    // Chains and single shaders live in separate maps under separate keys.
    const auto chain = chains_.find(current_shader_);
    if (!current_shader_.empty() && chain != chains_.end()) {
        active_chain_ = &chain->second;
        current_pipeline_state_ = default_pipeline_state_.Get();
        return;
    }

    const auto it = custom_shaders_.find(current_shader_);
    if (!current_shader_.empty() && it != custom_shaders_.end()) {
        current_pipeline_state_ = it->second.Get();
    } else {
        current_pipeline_state_ = default_pipeline_state_.Get();
    }
}

bool D3D12GraphicsEngine::CreateRootSignatureAndPSO() {
    // Two SRVs: t0 is the shader's input, t1 the untouched emulator frame. Single shaders only
    // ever read t0 (both slots hold the frame for them); a multi-pass chain's later passes read
    // t0 = the previous pass's output and t1 = the original.
    CD3DX12_DESCRIPTOR_RANGE range;
    range.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0);

    CD3DX12_ROOT_PARAMETER root_params[2];
    root_params[0].InitAsDescriptorTable(1, &range, D3D12_SHADER_VISIBILITY_PIXEL);
    // 4 floats (outW, outH, inW, inH) = 16 bytes = 4 32-bit values.
    root_params[1].InitAsConstants(4, 0, 0, D3D12_SHADER_VISIBILITY_PIXEL);

    // s0/s1 wrap at the edges and are what every single shader is written against - they must
    // stay as they are. s2/s3 are the same filters clamped to the edge, for the multi-pass
    // chains (the blit below and the Super-xBR passes), whose taps reach past the picture.
    CD3DX12_STATIC_SAMPLER_DESC samplers[4];
    samplers[0].Init(0, D3D12_FILTER_MIN_MAG_MIP_POINT);
    samplers[1].Init(1, D3D12_FILTER_MIN_MAG_MIP_LINEAR);
    samplers[2].Init(2, D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                     D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
    samplers[3].Init(3, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                     D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP);

    CD3DX12_ROOT_SIGNATURE_DESC root_sig_desc;
    root_sig_desc.Init(2, root_params, 4, samplers,
                       D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

    ComPtr<ID3DBlob> serialized_root_sig, error_blob;
    if (FAILED(D3D12SerializeRootSignature(&root_sig_desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                           &serialized_root_sig, &error_blob))) {
        return false;
    }
    if (FAILED(device_->CreateRootSignature(0, serialized_root_sig->GetBufferPointer(),
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

    if (FAILED(D3DCompile(vs_code, strlen(vs_code), nullptr, nullptr, nullptr, "main", "vs_5_0", 0,
                          0, &vs_blob_, nullptr)))
        return false;

    ComPtr<ID3DBlob> ps_blob;
    if (FAILED(D3DCompile(default_shader, strlen(default_shader), nullptr, nullptr, nullptr, "main",
                          "ps_5_0", 0, 0, &ps_blob, nullptr)))
        return false;

    if (!CreatePipelineState(ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(),
                             default_pipeline_state_))
        return false;

    // The last step of a multi-pass filter whose final pass rendered at some multiple of the
    // frame: stretch that texture onto the letterboxed picture area, bilinear, edge-clamped.
    const char* blit_shader =
        "Texture2D g_Tex : register(t0); SamplerState g_Linear : register(s3);"
        "float4 main(float4 pos:SV_POSITION, float2 uv:TEXCOORD0) : SV_TARGET {"
        "    return g_Tex.Sample(g_Linear, uv);"
        "}";
    ComPtr<ID3DBlob> blit_blob;
    if (FAILED(D3DCompile(blit_shader, strlen(blit_shader), nullptr, nullptr, nullptr, "main",
                          "ps_5_0", 0, 0, &blit_blob, nullptr)))
        return false;
    return CreatePipelineState(blit_blob->GetBufferPointer(), blit_blob->GetBufferSize(),
                               blit_pipeline_state_);
}

bool D3D12GraphicsEngine::CreatePipelineState(const void* bytecode, size_t size,
                                              ComPtr<ID3D12PipelineState>& out) {
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
    // Matches Gpu::ResolveFramebuffer's byte order - see CreateSwapChain's own
    // comment. Every PSO in this engine (default, every loaded filter and every
    // chain render target) uses this same format.
    pso_desc.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
    pso_desc.SampleDesc.Count = 1;

    return SUCCEEDED(device_->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&out)));
}

bool D3D12GraphicsEngine::CreateFramebufferResources(int fb_width, int fb_height) {
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
    if (FAILED(device_->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &tex_desc,
                                                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                                                IID_PPV_ARGS(&fb_texture_))))
        return false;

    device_->GetCopyableFootprints(&tex_desc, 0, 1, 0, &fb_placed_footprint_, &fb_num_rows_,
                                   &fb_row_size_in_bytes_, &fb_upload_buffer_size_);

    // A separate heap per swap-chain slot - see the member comment in the
    // header for why one shared heap is a CPU/GPU race.
    const CD3DX12_HEAP_PROPERTIES upload_heap_props(D3D12_HEAP_TYPE_UPLOAD);
    const CD3DX12_RESOURCE_DESC upload_desc = CD3DX12_RESOURCE_DESC::Buffer(fb_upload_buffer_size_);
    for (UINT n = 0; n < kFrameCount; ++n) {
        if (FAILED(device_->CreateCommittedResource(&upload_heap_props, D3D12_HEAP_FLAG_NONE,
                                                    &upload_desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                    nullptr, IID_PPV_ARGS(&fb_upload_heap_[n]))))
            return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC srv_heap_desc = {};
    srv_heap_desc.NumDescriptors = 2;   // the framebuffer twice: t0 and t1 (see the root signature)
    srv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device_->CreateDescriptorHeap(&srv_heap_desc, IID_PPV_ARGS(&srv_heap_))))
        return false;

    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
    srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv_desc.Format = tex_desc.Format;
    srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MipLevels = 1;

    D3D12_CPU_DESCRIPTOR_HANDLE srv_handle = srv_heap_->GetCPUDescriptorHandleForHeapStart();
    const UINT srv_increment =
        device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    for (int slot = 0; slot < 2; ++slot) {
        device_->CreateShaderResourceView(fb_texture_.Get(), &srv_desc, srv_handle);
        srv_handle.ptr += srv_increment;
    }

    // A chain's descriptors point at the texture just replaced.
    chain_res_valid_ = false;

    return true;
}

bool D3D12GraphicsEngine::LoadCustomPixelShader(const std::string& name, const uint8_t* bytecode,
                                                size_t size) {
    if (!device_ || !root_signature_ || !vs_blob_)
        return false;

    ComPtr<ID3D12PipelineState> custom_pipeline_state;
    if (!CreatePipelineState(bytecode, size, custom_pipeline_state))
        return false;

    custom_shaders_[name] = custom_pipeline_state;
    return true;
}

bool D3D12GraphicsEngine::LoadPixelShaderFromString(const std::string& name, const char* hlsl) {
    ComPtr<ID3DBlob> ps_blob;
    if (FAILED(D3DCompile(hlsl, strlen(hlsl), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0,
                          &ps_blob, nullptr)))
        return false;
    return LoadCustomPixelShader(name, static_cast<const uint8_t*>(ps_blob->GetBufferPointer()),
                                 ps_blob->GetBufferSize());
}

bool D3D12GraphicsEngine::LoadShaderChain(const std::string& name,
                                          const std::vector<ShaderPass>& passes) {
    if (!device_ || !root_signature_ || name.empty() || passes.empty())
        return false;

    ShaderChain chain;
    for (size_t i = 0; i < passes.size(); ++i) {
        // Only the last pass may draw straight to the window; every earlier one has to leave a
        // texture behind for the pass after it to read.
        if (passes[i].scale < 0 || (passes[i].scale == 0 && i + 1 != passes.size()))
            return false;
        const auto it = custom_shaders_.find(passes[i].shader);
        if (it == custom_shaders_.end())
            return false;
        chain.pass_pipelines.push_back(it->second);
        chain.pass_scales.push_back(passes[i].scale);
    }

    chains_[name] = std::move(chain);
    // Replacing a chain in place keeps its map node, so anything holding a pointer to it - the
    // active selection, the cached render targets - is now describing the old contents.
    chain_res_valid_ = false;
    if (name == current_shader_)
        SetPixelShader(name);
    return true;
}

bool D3D12GraphicsEngine::EnsureChainResources(const ShaderChain& chain, int src_width,
                                               int src_height) {
    if (chain_res_valid_ && chain_res_chain_ == &chain && chain_res_width_ == src_width &&
        chain_res_height_ == src_height)
        return true;

    // Anything recorded for an earlier frame may still be reading the targets about to go.
    FlushGPU();
    chain_res_valid_ = false;
    chain_targets_.clear();
    chain_draws_.clear();
    chain_rtv_heap_.Reset();
    chain_srv_heap_.Reset();

    const size_t pass_count = chain.pass_pipelines.size();
    // The last pass is followed by a blit unless it already drew into the window.
    const bool needs_blit = chain.pass_scales.back() > 0;
    const size_t target_count = needs_blit ? pass_count : pass_count - 1;
    const size_t draw_count = pass_count + (needs_blit ? 1 : 0);

    D3D12_DESCRIPTOR_HEAP_DESC srv_heap_desc = {};
    srv_heap_desc.NumDescriptors = static_cast<UINT>(draw_count * 2);
    srv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device_->CreateDescriptorHeap(&srv_heap_desc, IID_PPV_ARGS(&chain_srv_heap_))))
        return false;

    if (target_count > 0) {
        D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc = {};
        rtv_heap_desc.NumDescriptors = static_cast<UINT>(target_count);
        rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        if (FAILED(device_->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(&chain_rtv_heap_))))
            return false;
    }

    const UINT srv_increment =
        device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE srv_handle = chain_srv_heap_->GetCPUDescriptorHandleForHeapStart();
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = {};
    if (chain_rtv_heap_)
        rtv_handle = chain_rtv_heap_->GetCPUDescriptorHandleForHeapStart();

    D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
    srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MipLevels = 1;

    // Render targets are created ready to sample: each draw flips its own target to a render
    // target and back, so between draws (and between frames) they are always in this state.
    const CD3DX12_HEAP_PROPERTIES default_heap(D3D12_HEAP_TYPE_DEFAULT);
    UINT in_width = static_cast<UINT>(src_width);
    UINT in_height = static_cast<UINT>(src_height);

    chain_targets_.resize(target_count);
    for (size_t d = 0; d < draw_count; ++d) {
        ChainDraw draw;
        draw.in_width = in_width;
        draw.in_height = in_height;

        const bool is_blit = d == pass_count;
        draw.pipeline = is_blit ? blit_pipeline_state_.Get() : chain.pass_pipelines[d].Get();

        // t0: the previous draw's target (the emulator frame for the first draw); t1: the frame.
        ID3D12Resource* input = (d == 0) ? fb_texture_.Get() : chain_targets_[d - 1].Get();
        device_->CreateShaderResourceView(input, &srv_desc, srv_handle);
        srv_handle.ptr += srv_increment;
        device_->CreateShaderResourceView(fb_texture_.Get(), &srv_desc, srv_handle);
        srv_handle.ptr += srv_increment;

        if (!is_blit && chain.pass_scales[d] > 0) {
            draw.target = static_cast<int>(d);
            draw.out_width = static_cast<UINT>(src_width) * chain.pass_scales[d];
            draw.out_height = static_cast<UINT>(src_height) * chain.pass_scales[d];

            D3D12_RESOURCE_DESC tex_desc = {};
            tex_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            tex_desc.Width = draw.out_width;
            tex_desc.Height = draw.out_height;
            tex_desc.DepthOrArraySize = 1;
            tex_desc.MipLevels = 1;
            tex_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            tex_desc.SampleDesc.Count = 1;
            tex_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            if (FAILED(device_->CreateCommittedResource(
                    &default_heap, D3D12_HEAP_FLAG_NONE, &tex_desc,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                    IID_PPV_ARGS(&chain_targets_[d]))))
                return false;

            device_->CreateRenderTargetView(chain_targets_[d].Get(), nullptr, rtv_handle);
            rtv_handle.ptr += rtv_descriptor_size_;

            in_width = draw.out_width;
            in_height = draw.out_height;
        }
        // else: this draw goes to the back buffer (target stays -1), sized at draw time.

        chain_draws_.push_back(draw);
    }

    chain_res_chain_ = &chain;
    chain_res_width_ = src_width;
    chain_res_height_ = src_height;
    chain_res_valid_ = true;
    return true;
}

void D3D12GraphicsEngine::RenderChain(const LetterboxRect& rect) {
    command_list_->SetGraphicsRootSignature(root_signature_.Get());
    command_list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    ID3D12DescriptorHeap* heaps[] = { chain_srv_heap_.Get() };
    command_list_->SetDescriptorHeaps(_countof(heaps), heaps);

    D3D12_CPU_DESCRIPTOR_HANDLE back_buffer_rtv(rtv_heap_->GetCPUDescriptorHandleForHeapStart());
    back_buffer_rtv.ptr += static_cast<SIZE_T>(frame_index_) * rtv_descriptor_size_;

    const UINT srv_increment =
        device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_GPU_DESCRIPTOR_HANDLE srv_table = chain_srv_heap_->GetGPUDescriptorHandleForHeapStart();

    for (const ChainDraw& draw : chain_draws_) {
        const bool to_back_buffer = draw.target < 0;

        // Constants follow the single shaders' layout: the target's size, then the input's.
        float out_width = rect.width;
        float out_height = rect.height;

        if (to_back_buffer) {
            command_list_->OMSetRenderTargets(1, &back_buffer_rtv, FALSE, nullptr);
            const D3D12_VIEWPORT viewport = { rect.x, rect.y, rect.width, rect.height, 0.0f, 1.0f };
            const D3D12_RECT scissor = { static_cast<LONG>(rect.x), static_cast<LONG>(rect.y),
                                         static_cast<LONG>(rect.x + rect.width),
                                         static_cast<LONG>(rect.y + rect.height) };
            command_list_->RSSetViewports(1, &viewport);
            command_list_->RSSetScissorRects(1, &scissor);
        } else {
            out_width = static_cast<float>(draw.out_width);
            out_height = static_cast<float>(draw.out_height);

            const CD3DX12_RESOURCE_BARRIER to_render_target = CD3DX12_RESOURCE_BARRIER::Transition(
                chain_targets_[draw.target].Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_RENDER_TARGET);
            command_list_->ResourceBarrier(1, &to_render_target);

            D3D12_CPU_DESCRIPTOR_HANDLE target_rtv =
                chain_rtv_heap_->GetCPUDescriptorHandleForHeapStart();
            target_rtv.ptr += static_cast<SIZE_T>(draw.target) * rtv_descriptor_size_;
            command_list_->OMSetRenderTargets(1, &target_rtv, FALSE, nullptr);

            const D3D12_VIEWPORT viewport = { 0.0f, 0.0f, out_width, out_height, 0.0f, 1.0f };
            const D3D12_RECT scissor = { 0, 0, static_cast<LONG>(draw.out_width),
                                         static_cast<LONG>(draw.out_height) };
            command_list_->RSSetViewports(1, &viewport);
            command_list_->RSSetScissorRects(1, &scissor);
        }

        command_list_->SetPipelineState(draw.pipeline);
        const float shader_params[4] = { out_width, out_height, static_cast<float>(draw.in_width),
                                         static_cast<float>(draw.in_height) };
        command_list_->SetGraphicsRoot32BitConstants(1, 4, shader_params, 0);
        command_list_->SetGraphicsRootDescriptorTable(0, srv_table);
        srv_table.ptr += static_cast<UINT64>(srv_increment) * 2;

        command_list_->DrawInstanced(3, 1, 0, 0);

        if (!to_back_buffer) {
            const CD3DX12_RESOURCE_BARRIER to_shader_resource = CD3DX12_RESOURCE_BARRIER::Transition(
                chain_targets_[draw.target].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            command_list_->ResourceBarrier(1, &to_shader_resource);
        }
    }
}
