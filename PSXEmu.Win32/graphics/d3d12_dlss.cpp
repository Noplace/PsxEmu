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

// D3D12GraphicsEngine's DLSS (Docs/DLSS-Plan.md, phase 4): Streamline started and stopped with the
// engine, the plane beside each picture turned into DLSS's inputs, and DLSS's picture drawn in the
// picture's place. The rest of the engine is d3d12_graphics_engine.cpp.

#include "graphics/d3d12_graphics_engine.h"

#include "graphics/adapters.h"
#include "graphics/dlss/streamline.h"

#include <algorithm>
#include <cmath>

namespace {

    using psxemu::DlssMode;
    using psxemu::DlssPreset;

    // The plane (psx/shared_picture.h) into DLSS's three inputs, a thread a pixel:
    //   motion  where the pixel was in the last picture minus where it is, in the picture's own
    //           pixels - DLSS's convention and scale (mvecScale 1/size) - and 0 where unknown
    //   depth   1/w as it is: nearer is larger (depthInverted), unknown 0 is infinitely far
    //   hint    how far to trust this picture over DLSS's history (the bias-current-colour hint):
    //           wholly where the motion is unknown, half where the last thing drawn was
    //           translucent, which the motion underneath it does not describe
    const char kDlssInputsHlsl[] = R"HLSL(
Texture2D<float4> planes : register(t0);
RWTexture2D<float2> motion : register(u0);
RWTexture2D<float> depth : register(u1);
RWTexture2D<float> hint : register(u2);
cbuffer Size : register(b0) { uint width; uint height; };

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= width || id.y >= height)
        return;
    const float4 plane = planes[id.xy];
    const bool unknown = plane.r >= 16384.0;   // kUnknownMotion, 32768
    motion[id.xy] = unknown ? float2(0.0, 0.0) : plane.rg;
    depth[id.xy] = plane.b / 256.0;             // kPlaneDepthScale
    hint[id.xy] = unknown ? 1.0 : (plane.a < 0.5 ? 0.5 : 0.0);
}
)HLSL";

    // DLSS reads its inputs outside the pixel shader, and the picture itself inside it too.
    const D3D12_RESOURCE_STATES kDlssInputState =
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    sl::DLSSMode ToSl(DlssMode mode) {
        switch (mode) {
        case DlssMode::kDlaa: return sl::DLSSMode::eDLAA;
        case DlssMode::kQuality: return sl::DLSSMode::eMaxQuality;
        case DlssMode::kBalanced: return sl::DLSSMode::eBalanced;
        case DlssMode::kPerformance: return sl::DLSSMode::eMaxPerformance;
        case DlssMode::kUltraPerformance: return sl::DLSSMode::eUltraPerformance;
        default: return sl::DLSSMode::eOff;
        }
    }

    sl::float4x4 Identity() {
        sl::float4x4 m;
        m.row[0] = sl::float4(1.0f, 0.0f, 0.0f, 0.0f);
        m.row[1] = sl::float4(0.0f, 1.0f, 0.0f, 0.0f);
        m.row[2] = sl::float4(0.0f, 0.0f, 1.0f, 0.0f);
        m.row[3] = sl::float4(0.0f, 0.0f, 0.0f, 1.0f);
        return m;
    }

    // Whole pixels, so DLSS's output lands one to one on the screen.
    LetterboxRect Snapped(const LetterboxRect& rect) {
        LetterboxRect snapped;
        snapped.x = std::round(rect.x);
        snapped.y = std::round(rect.y);
        snapped.width = std::round(rect.width);
        snapped.height = std::round(rect.height);
        return snapped;
    }

    ComPtr<ID3D12Resource> MakeTarget(ID3D12Device* device, int width, int height,
                                      DXGI_FORMAT format, D3D12_RESOURCE_STATES state) {
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = static_cast<UINT64>(width);
        desc.Height = static_cast<UINT>(height);
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        const CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_DEFAULT);
        ComPtr<ID3D12Resource> resource;
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state,
                                                   nullptr, IID_PPV_ARGS(&resource))))
            resource.Reset();
        return resource;
    }

}   // namespace

void D3D12GraphicsEngine::SetDlss(const psxemu::DlssChoice& choice) {
    dlss_choice_ = choice;
}

void D3D12GraphicsEngine::NoteDlss(const std::string& line) {
    if (line == dlss_last_note_)
        return;
    dlss_last_note_ = line;
    psxemu::DlssNote(line);
}

psxemu::DlssStatus D3D12GraphicsEngine::dlss_status() const {
    psxemu::DlssStatus status;
    status.ready = streamline_ != nullptr && dlss_pipeline_ != nullptr && dlss_why_.empty();
    status.why = dlss_why_;
    status.version = dlss_version_;
    return status;
}

// Streamline is started with the device and the swap chain it hooks, so it comes and goes with
// the engine: made again when DLSS goes on or off, not when it only changes mode.
bool D3D12GraphicsEngine::DlssNeedsRemaking(const psxemu::DlssChoice& from,
                                           const psxemu::DlssChoice& to) const {
    return (from.mode == DlssMode::kOff) != (to.mode == DlssMode::kOff);
}

void D3D12GraphicsEngine::StartStreamline() {
    dlss_why_.clear();
    dlss_version_.clear();
    // Not NVIDIA's card: nothing of NVIDIA's is loaded to be told so.
    ComPtr<IDXGIAdapter1> adapter = psxemu::OpenAdapter(adapter_luid_);
    DXGI_ADAPTER_DESC1 description = {};
    if (!adapter || FAILED(adapter->GetDesc1(&description)) || description.VendorId != 0x10DE) {
        dlss_why_ = "the renderer's graphics card is not NVIDIA's";
        return;
    }

    auto streamline = std::make_unique<psxemu::Streamline>();
    std::string error;
    if (!streamline->Start(psxemu::ExecutableFolder(), false, &error)) {
        dlss_why_ = error;
        return;
    }
    std::string why;
    if (!streamline->SupportsDlss(adapter_luid_, &why)) {
        dlss_why_ = why;
        streamline->Stop();
        return;
    }
    if (!streamline->SetDevice(device_.Get(), &error)) {
        dlss_why_ = error;
        streamline->Stop();
        return;
    }
    // The proxies the queue and the swap chain are made through. Each holds the object it
    // stands for; the pointer handed in comes back as the proxy, with a reference of its own.
    ID3D12Device* device = device_.Get();
    IDXGIFactory* factory = factory_.Get();
    if (streamline->slUpgradeInterface(reinterpret_cast<void**>(&device)) != sl::Result::eOk ||
        device == device_.Get()) {
        dlss_why_ = "Streamline would not take the device";
        streamline->Stop();
        return;
    }
    sl_device_.Attach(device);
    ComPtr<IDXGIFactory> proxy_factory;
    if (streamline->slUpgradeInterface(reinterpret_cast<void**>(&factory)) != sl::Result::eOk ||
        factory == factory_.Get()) {
        dlss_why_ = "Streamline would not take the DXGI factory";
        sl_device_.Reset();
        streamline->Stop();
        return;
    }
    proxy_factory.Attach(factory);
    if (FAILED(proxy_factory.As(&sl_factory_))) {
        dlss_why_ = "Streamline's DXGI factory is too old";
        sl_device_.Reset();
        proxy_factory.Reset();
        streamline->Stop();
        return;
    }
    dlss_version_ = streamline->DlssVersion();
    streamline_ = std::move(streamline);
    NoteDlss("started: DLSS " + dlss_version_);
}

void D3D12GraphicsEngine::StopStreamline() {
    if (streamline_ == nullptr)
        return;
    FlushGPU();
    streamline_->slFreeResources(sl::kFeatureDLSS, sl::ViewportHandle(0u));
    // What was made through Streamline goes while it is there to hear of it.
    for (UINT n = 0; n < kFrameCount; ++n)
        render_targets_[n].Reset();
    swap_chain_.Reset();
    command_queue_.Reset();
    sl_device_.Reset();
    sl_factory_.Reset();
    frame_token_ = nullptr;
    streamline_->Stop();
    streamline_.reset();
}

bool D3D12GraphicsEngine::CreateDlssPipeline() {
    CD3DX12_DESCRIPTOR_RANGE ranges[2];
    ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0);   // t0 the plane
    ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 3, 0);   // u0-u2 motion, depth, hint
    CD3DX12_ROOT_PARAMETER parameters[2];
    parameters[0].InitAsDescriptorTable(2, ranges);
    parameters[1].InitAsConstants(2, 0);   // b0: the size
    CD3DX12_ROOT_SIGNATURE_DESC description;
    description.Init(2, parameters);
    ComPtr<ID3DBlob> serialized, errors;
    if (FAILED(D3D12SerializeRootSignature(&description, D3D_ROOT_SIGNATURE_VERSION_1,
                                           &serialized, &errors)) ||
        FAILED(device_->CreateRootSignature(0, serialized->GetBufferPointer(),
                                            serialized->GetBufferSize(),
                                            IID_PPV_ARGS(&dlss_root_))))
        return false;

    ComPtr<ID3DBlob> cs;
    if (FAILED(D3DCompile(kDlssInputsHlsl, sizeof(kDlssInputsHlsl) - 1, nullptr, nullptr, nullptr,
                          "main", "cs_5_0", 0, 0, &cs, &errors)))
        return false;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = dlss_root_.Get();
    pso.CS = { cs->GetBufferPointer(), cs->GetBufferSize() };
    if (FAILED(device_->CreateComputePipelineState(&pso, IID_PPV_ARGS(&dlss_pipeline_))))
        return false;

    D3D12_DESCRIPTOR_HEAP_DESC heap = {};
    heap.NumDescriptors = kDlssOutputSlot + 2;
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    return SUCCEEDED(device_->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&dlss_heap_)));
}

// The screen's rectangle when DLSS takes this picture for it; otherwise the mode's own ratio of
// it, which is then scaled onto the screen (graphics/dlss/dlss_choice.h). DLSS's range for an
// output is asked once per size.
bool D3D12GraphicsEngine::ChooseDlssOutput(int width, int height, const LetterboxRect& screen,
                                           int* out_width, int* out_height) {
    const DlssMode mode = dlss_choice_.mode;
    // Whether DLSS takes this picture for an output w x h.
    auto takes = [&](int w, int h) -> bool {
        if (w <= 0 || h <= 0 || w > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
            h > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION)
            return false;
        for (const DlssRangeEntry& entry : dlss_ranges_)
            if (entry.mode == mode && entry.width == w && entry.height == h)
                return entry.taken && entry.range.Takes(width, height);
        // A window dragged through many sizes leaves many; a handful is all that is ever in use.
        if (dlss_ranges_.size() >= 16)
            dlss_ranges_.clear();
        DlssRangeEntry entry{ mode, w, h, false, psxemu::DlssRange() };
        sl::DLSSOptions options{};
        options.mode = ToSl(mode);
        options.outputWidth = static_cast<uint32_t>(w);
        options.outputHeight = static_cast<uint32_t>(h);
        sl::DLSSOptimalSettings settings{};
        const sl::Result result = streamline_->slDLSSGetOptimalSettings(options, settings);
        if (result == sl::Result::eOk && settings.renderWidthMax > 0) {
            entry.taken = true;
            entry.range.least_width = static_cast<int>(settings.renderWidthMin);
            entry.range.least_height = static_cast<int>(settings.renderHeightMin);
            entry.range.most_width = static_cast<int>(settings.renderWidthMax);
            entry.range.most_height = static_cast<int>(settings.renderHeightMax);
            psxemu::DlssNote("range for " + std::to_string(w) + "x" + std::to_string(h) + ": " +
                             std::to_string(settings.renderWidthMin) + "x" +
                             std::to_string(settings.renderHeightMin) + " - " +
                             std::to_string(settings.renderWidthMax) + "x" +
                             std::to_string(settings.renderHeightMax));
        } else {
            psxemu::DlssNote("no range for " + std::to_string(w) + "x" + std::to_string(h) +
                             ": " + psxemu::StreamlineResultText(result));
        }
        dlss_ranges_.push_back(entry);
        return entry.taken && entry.range.Takes(width, height);
    };
    const int screen_width = static_cast<int>(screen.width);
    const int screen_height = static_cast<int>(screen.height);
    if (psxemu::DlssTriesScreen(mode) && takes(screen_width, screen_height)) {
        *out_width = screen_width;
        *out_height = screen_height;
        return true;
    }
    psxemu::DlssOwnOutput(mode, width, height, out_width, out_height);
    return takes(*out_width, *out_height);
}

// DLSS's inputs at the picture's size and its output at the output's, made again when either
// changes - after the card has finished with the old ones - and its history with them.
bool D3D12GraphicsEngine::EnsureDlssTargets(int in_width, int in_height, int out_width,
                                            int out_height) {
    if (dlss_motion_ && in_width == dlss_in_width_ && in_height == dlss_in_height_ &&
        dlss_output_ && out_width == dlss_out_width_ && out_height == dlss_out_height_)
        return true;
    FlushGPU();
    dlss_output_valid_ = false;
    dlss_reset_ = true;
    dlss_options_set_ = false;
    dlss_in_width_ = dlss_in_height_ = dlss_out_width_ = dlss_out_height_ = 0;
    dlss_motion_ = MakeTarget(device_.Get(), in_width, in_height, DXGI_FORMAT_R16G16_FLOAT,
                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    dlss_depth_ = MakeTarget(device_.Get(), in_width, in_height, DXGI_FORMAT_R32_FLOAT,
                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    dlss_hint_ = MakeTarget(device_.Get(), in_width, in_height, DXGI_FORMAT_R8_UNORM,
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    // Read by the draw to the screen between evaluations, as fb_texture_ is.
    dlss_output_ = MakeTarget(device_.Get(), out_width, out_height, DXGI_FORMAT_R8G8B8A8_UNORM,
                              D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    if (!dlss_motion_ || !dlss_depth_ || !dlss_hint_ || !dlss_output_) {
        dlss_motion_.Reset();
        dlss_depth_.Reset();
        dlss_hint_.Reset();
        dlss_output_.Reset();
        return false;
    }
    // The output's pair of SRVs, t0 and t1, for the draw: the single shaders read t0.
    const UINT increment =
        device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE slot = dlss_heap_->GetCPUDescriptorHandleForHeapStart();
    slot.ptr += static_cast<SIZE_T>(kDlssOutputSlot) * increment;
    D3D12_SHADER_RESOURCE_VIEW_DESC view = {};
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    view.Texture2D.MipLevels = 1;
    for (int i = 0; i < 2; ++i) {
        device_->CreateShaderResourceView(dlss_output_.Get(), &view, slot);
        slot.ptr += increment;
    }
    dlss_in_width_ = in_width;
    dlss_in_height_ = in_height;
    dlss_out_width_ = out_width;
    dlss_out_height_ = out_height;
    return true;
}

// As OpenSharedPicture, by the plane's own ids.
ID3D12Resource* D3D12GraphicsEngine::OpenPlanes(const emulation::psx::SharedPicture& picture) {
    for (const OpenedPicture& opened : opened_planes_)
        if (opened.id == picture.planes_id)
            return opened.resource.Get();
    // A new rasteriser's, or a handful open already: the old let go of, once the card is done.
    if (!opened_planes_.empty() &&
        (planes_source_ != picture.source.get() ||
         opened_planes_.size() >= emulation::psx::kMaxOpenedPictures)) {
        FlushGPU();
        opened_planes_.clear();
    }
    planes_source_ = picture.source.get();
    ComPtr<ID3D12Resource> resource;
    if (FAILED(device_->OpenSharedHandle(static_cast<HANDLE>(picture.planes),
                                         IID_PPV_ARGS(&resource))))
        return nullptr;
    opened_planes_.push_back({ picture.planes_id, resource });
    return resource.Get();
}

bool D3D12GraphicsEngine::EvaluateDlss(const emulation::psx::SharedPicture& picture,
                                       ID3D12Resource* planes, int out_width, int out_height) {
    const int width = picture.width;
    const int height = picture.height;
    if (frame_token_ == nullptr) {
        NoteDlss("no frame token");
        return false;
    }
    if (!EnsureDlssTargets(width, height, out_width, out_height)) {
        NoteDlss("DLSS's textures could not be made");
        return false;
    }
    // Which of Streamline's steps failed, and what it said.
    auto check = [this](sl::Result result, const char* step) {
        if (result == sl::Result::eOk)
            return true;
        NoteDlss(std::string(step) + " failed: " + psxemu::StreamlineResultText(result) + " - " +
                 streamline_->last_message());
        return false;
    };

    // This frame's descriptors, which the fence says the card has finished with since this
    // slot's frame last came round.
    const UINT increment =
        device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = dlss_heap_->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += static_cast<SIZE_T>(frame_index_) * 4 * increment;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = dlss_heap_->GetGPUDescriptorHandleForHeapStart();
    gpu.ptr += static_cast<UINT64>(frame_index_) * 4 * increment;
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC view = {};
        view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        view.Texture2D.MipLevels = 1;
        device_->CreateShaderResourceView(planes, &view, cpu);
        const struct { ID3D12Resource* resource; DXGI_FORMAT format; } targets[] = {
            { dlss_motion_.Get(), DXGI_FORMAT_R16G16_FLOAT },
            { dlss_depth_.Get(), DXGI_FORMAT_R32_FLOAT },
            { dlss_hint_.Get(), DXGI_FORMAT_R8_UNORM },
        };
        for (const auto& target : targets) {
            cpu.ptr += increment;
            D3D12_UNORDERED_ACCESS_VIEW_DESC access = {};
            access.Format = target.format;
            access.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            device_->CreateUnorderedAccessView(target.resource, nullptr, &access, cpu);
        }
    }

    // The plane into DLSS's inputs. The plane lives in the common state between the two
    // devices, and goes back to it.
    {
        const CD3DX12_RESOURCE_BARRIER to_read = CD3DX12_RESOURCE_BARRIER::Transition(
            planes, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        command_list_->ResourceBarrier(1, &to_read);
        command_list_->SetComputeRootSignature(dlss_root_.Get());
        command_list_->SetPipelineState(dlss_pipeline_.Get());
        ID3D12DescriptorHeap* heaps[] = { dlss_heap_.Get() };
        command_list_->SetDescriptorHeaps(1, heaps);
        command_list_->SetComputeRootDescriptorTable(0, gpu);
        const UINT size[2] = { static_cast<UINT>(width), static_cast<UINT>(height) };
        command_list_->SetComputeRoot32BitConstants(1, 2, size, 0);
        command_list_->Dispatch((static_cast<UINT>(width) + 7) / 8,
                                (static_cast<UINT>(height) + 7) / 8, 1);
        const CD3DX12_RESOURCE_BARRIER done[] = {
            CD3DX12_RESOURCE_BARRIER::Transition(planes,
                                                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                 D3D12_RESOURCE_STATE_COMMON),
            CD3DX12_RESOURCE_BARRIER::Transition(dlss_motion_.Get(),
                                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                 kDlssInputState),
            CD3DX12_RESOURCE_BARRIER::Transition(dlss_depth_.Get(),
                                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                 kDlssInputState),
            CD3DX12_RESOURCE_BARRIER::Transition(dlss_hint_.Get(),
                                                 D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                 kDlssInputState),
        };
        command_list_->ResourceBarrier(_countof(done), done);
    }

    const sl::ViewportHandle viewport(0u);
    bool ok = true;

    // The camera, as far as there is one: the motion is all of it, so every matrix is the
    // identity and the camera's motion counts as included. Depth nearer-is-larger.
    {
        sl::Constants constants{};
        const sl::float4x4 identity = Identity();
        constants.cameraViewToClip = identity;
        constants.clipToCameraView = identity;
        constants.clipToLensClip = identity;
        constants.clipToPrevClip = identity;
        constants.prevClipToClip = identity;
        // Negated: the picture's jitter is where its samples were moved to, and DLSS's is the
        // other way - measured, not assumed (sl_probe --jitter-test: 0.025 negated, 0.246 not).
        constants.jitterOffset = sl::float2(-picture.jitter_x, -picture.jitter_y);
        constants.mvecScale = sl::float2(1.0f / static_cast<float>(width),
                                         1.0f / static_cast<float>(height));
        constants.cameraPinholeOffset = sl::float2(0.0f, 0.0f);
        constants.cameraPos = sl::float3(0.0f, 0.0f, 0.0f);
        constants.cameraUp = sl::float3(0.0f, 1.0f, 0.0f);
        constants.cameraRight = sl::float3(1.0f, 0.0f, 0.0f);
        constants.cameraFwd = sl::float3(0.0f, 0.0f, 1.0f);
        constants.cameraNear = 1.0f;
        constants.cameraFar = 65536.0f;
        constants.cameraFOV = 1.0f;
        constants.cameraAspectRatio = 4.0f / 3.0f;
        constants.depthInverted = sl::Boolean::eTrue;
        constants.cameraMotionIncluded = sl::Boolean::eTrue;
        constants.motionVectors3D = sl::Boolean::eFalse;
        constants.reset = (dlss_reset_ || picture.reset) ? sl::Boolean::eTrue
                                                           : sl::Boolean::eFalse;
        constants.orthographicProjection = sl::Boolean::eFalse;
        constants.motionVectorsDilated = sl::Boolean::eFalse;
        constants.motionVectorsJittered = sl::Boolean::eFalse;
        ok = check(streamline_->slSetConstants(constants, *frame_token_, viewport),
                   "slSetConstants");
    }

    // The mode, the output and the model - told again only when one of them changes.
    if (ok && (!dlss_options_set_ || !(dlss_options_choice_ == dlss_choice_) ||
               dlss_options_width_ != out_width || dlss_options_height_ != out_height)) {
        sl::DLSSOptions options{};
        options.mode = ToSl(dlss_choice_.mode);
        options.outputWidth = static_cast<uint32_t>(out_width);
        options.outputHeight = static_cast<uint32_t>(out_height);
        options.colorBuffersHDR = sl::Boolean::eFalse;   // the console's picture: 8 bits, 0-1
        // NVIDIA's own for each mode, or the one chosen for all of them.
        sl::DLSSPreset quality = sl::DLSSPreset::ePresetK;
        sl::DLSSPreset performance = sl::DLSSPreset::ePresetM;
        sl::DLSSPreset ultra = sl::DLSSPreset::ePresetL;
        if (dlss_choice_.preset != DlssPreset::kAuto) {
            const sl::DLSSPreset chosen = dlss_choice_.preset == DlssPreset::kK
                                              ? sl::DLSSPreset::ePresetK
                                          : dlss_choice_.preset == DlssPreset::kL
                                              ? sl::DLSSPreset::ePresetL
                                              : sl::DLSSPreset::ePresetM;
            quality = performance = ultra = chosen;
        }
        options.dlaaPreset = quality;
        options.qualityPreset = quality;
        options.balancedPreset = quality;
        options.performancePreset = performance;
        options.ultraPerformancePreset = ultra;
        options.ultraQualityPreset = quality;
        ok = check(streamline_->slDLSSSetOptions(viewport, options), "slDLSSSetOptions");
        dlss_options_set_ = ok;
        dlss_options_choice_ = dlss_choice_;
        dlss_options_width_ = out_width;
        dlss_options_height_ = out_height;
    }

    if (ok) {
        // Each in the state it is in on this command list; DLSS moves them as it needs, and back.
        sl::Resource colour(sl::ResourceType::eTex2d, fb_texture_.Get(),
                            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        sl::Resource output(sl::ResourceType::eTex2d, dlss_output_.Get(),
                            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        sl::Resource motion(sl::ResourceType::eTex2d, dlss_motion_.Get(), kDlssInputState);
        sl::Resource depth(sl::ResourceType::eTex2d, dlss_depth_.Get(), kDlssInputState);
        sl::Resource hint(sl::ResourceType::eTex2d, dlss_hint_.Get(), kDlssInputState);
        const sl::Extent input_extent{ 0, 0, static_cast<uint32_t>(width),
                                       static_cast<uint32_t>(height) };
        const sl::Extent output_extent{ 0, 0, static_cast<uint32_t>(out_width),
                                        static_cast<uint32_t>(out_height) };
        const sl::ResourceLifecycle until = sl::ResourceLifecycle::eValidUntilEvaluate;
        const sl::ResourceTag tags[] = {
            sl::ResourceTag(&colour, sl::kBufferTypeScalingInputColor, until, &input_extent),
            sl::ResourceTag(&output, sl::kBufferTypeScalingOutputColor, until, &output_extent),
            sl::ResourceTag(&motion, sl::kBufferTypeMotionVectors, until, &input_extent),
            sl::ResourceTag(&depth, sl::kBufferTypeDepth, until, &input_extent),
            sl::ResourceTag(&hint, sl::kBufferTypeBiasCurrentColorHint, until, &input_extent),
        };
        ok = check(streamline_->slSetTagForFrame(*frame_token_, viewport, tags, _countof(tags),
                                                 command_list_.Get()),
                   "slSetTagForFrame");
    }
    if (ok) {
        const sl::BaseStructure* inputs[] = { &viewport };
        ok = check(streamline_->slEvaluateFeature(sl::kFeatureDLSS, *frame_token_, inputs, 1,
                                                  command_list_.Get()),
                   "slEvaluateFeature");
    }

    const CD3DX12_RESOURCE_BARRIER back[] = {
        CD3DX12_RESOURCE_BARRIER::Transition(dlss_motion_.Get(), kDlssInputState,
                                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        CD3DX12_RESOURCE_BARRIER::Transition(dlss_depth_.Get(), kDlssInputState,
                                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        CD3DX12_RESOURCE_BARRIER::Transition(dlss_hint_.Get(), kDlssInputState,
                                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
    };
    command_list_->ResourceBarrier(_countof(back), back);

    // DLSS leaves the command list as it likes; the draw after it sets all it uses but the
    // target, which is the back buffer again.
    D3D12_CPU_DESCRIPTOR_HANDLE rtv(rtv_heap_->GetCPUDescriptorHandleForHeapStart());
    rtv.ptr += static_cast<SIZE_T>(frame_index_) * rtv_descriptor_size_;
    command_list_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

    if (ok)
        dlss_reset_ = false;
    return ok;
}

bool D3D12GraphicsEngine::DrawDlss(const emulation::psx::SharedPicture& picture) {
    auto give_up = [this](const std::string& why) {
        NoteDlss("not run: " + why);
        dlss_reset_ = true;
        dlss_output_valid_ = false;
        return false;
    };
    // Interlaced 480 lines, and pictures without the plane - the rasteriser not keeping one
    // yet - are shown as they are.
    if (picture.planes == nullptr)
        return give_up("the picture has no plane");
    if (picture.interlaced)
        return give_up("interlaced");

    const LetterboxRect screen = Snapped(ComputeLetterboxRect(width_, height_, 4.0f / 3.0f));
    int out_width = 0, out_height = 0;
    if (!ChooseDlssOutput(picture.width, picture.height, screen, &out_width, &out_height))
        return give_up("DLSS takes no output for " + std::to_string(picture.width) + "x" +
                       std::to_string(picture.height) + " on " +
                       std::to_string(static_cast<int>(screen.width)) + "x" +
                       std::to_string(static_cast<int>(screen.height)));

    // A new picture, or the same one needed at another size, is DLSS's to make; the last one
    // again - a 30 fps game's second vblank, or the overlay moving - is drawn as it was made.
    const bool resized = picture.width != dlss_in_width_ || picture.height != dlss_in_height_ ||
                         out_width != dlss_out_width_ || out_height != dlss_out_height_;
    const bool fresh = picture.serial != dlss_serial_;
    if (!dlss_output_valid_ || resized || (fresh && picture.new_picture)) {
        ID3D12Resource* const planes = OpenPlanes(picture);
        if (planes == nullptr)
            return give_up("the plane could not be opened");
        if (!EvaluateDlss(picture, planes, out_width, out_height))
            return give_up("evaluation failed");
        NoteDlss("running: " + std::to_string(picture.width) + "x" +
                 std::to_string(picture.height) + " to " + std::to_string(out_width) + "x" +
                 std::to_string(out_height));
        dlss_output_valid_ = true;
    }
    dlss_serial_ = picture.serial;

    // One to one when it is the screen's size; otherwise scaled into the letterbox as any
    // picture is.
    const LetterboxRect rect =
        out_width == static_cast<int>(screen.width) && out_height == static_cast<int>(screen.height)
            ? screen
            : ComputeLetterboxRect(width_, height_, 4.0f / 3.0f);
    D3D12_GPU_DESCRIPTOR_HANDLE table = dlss_heap_->GetGPUDescriptorHandleForHeapStart();
    table.ptr += static_cast<UINT64>(kDlssOutputSlot) *
                 device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    DrawTexture(dlss_heap_.Get(), table, out_width, out_height, rect);
    return true;
}
