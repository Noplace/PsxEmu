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

// D3D12GraphicsEngine's AMD FSR (Docs/FSR-Plan.md): AMD's FidelityFX DLLs loaded, an upscaler made
// on this device, the plane beside each picture turned into its inputs by DLSS's own pass, and
// FSR's picture drawn in the picture's place by DLSS's own DrawUpscaled; and FSR Frame Generation
// through AMD's swap chain. The DLSS half is d3d12_dlss.cpp, the rest d3d12_graphics_engine.cpp.

#include "graphics/d3d12_graphics_engine.h"

#include "graphics/dlss/streamline.h"   // ExecutableFolder
#include "graphics/fsr/fidelityfx.h"

#include <algorithm>
#include <cmath>

namespace {

    using psxemu::FidelityFx;
    using psxemu::FsrMode;
    using psxemu::FsrVersion;

    // FSR's reactive mask, from the plane (d3d12_dlss.cpp's pass): how far to trust this picture
    // over FSR's history. Where the motion is not known FSR would carry the last picture along
    // the wrong way, so it is nearly all this one - AMD advise no more than 0.9. Where the last
    // thing drawn was translucent - a fade, a shadow, a window - the motion is of what is under
    // it, so a little.
    constexpr float kReactiveUnknown = 0.9f;
    constexpr float kReactiveTranslucent = 0.3f;

    // The depth FSR is given is the plane's 1/z: nearer larger, with no far plane, which with a
    // near plane of 1 is the GTE's own z back as FSR's view-space depth. Its disocclusion test is
    // in proportion to depth, so units do not matter to it; what is in metres - the motion it
    // ignores as too small for the depth, and a clamp at 65504 - wants the GTE's 64-65,000
    // scaled down. A GTE unit is taken as a centimetre, which is the order most games use.
    constexpr float kCameraNear = 1.0f;
    constexpr float kCameraFar = 65536.0f;
    constexpr float kCameraFov = 1.0f;
    constexpr float kMetresPerUnit = 0.01f;

    constexpr uint32_t kUpscaleFlags = FFX_UPSCALE_ENABLE_DEPTH_INVERTED |
                                       FFX_UPSCALE_ENABLE_DEPTH_INFINITE |
                                       FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE;
    constexpr uint32_t kGenerationFlags = FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED |
                                          FFX_FRAMEGENERATION_ENABLE_DEPTH_INFINITE;

    // A name AMD gives a version, as the menu shows it: without the mark AMD puts after FSR 4's
    // ("4.1.1 *"), which on a card without FSR 4 runs FSR 3.1 (Docs/FSR-Plan.md, phase 0).
    std::string Plain(const std::string& name) {
        std::string plain = name;
        while (!plain.empty() && (plain.back() == '*' || plain.back() == ' '))
            plain.pop_back();
        return plain;
    }

    // AMD's checks run at every dispatch: each message said once, until another comes.
    void Message(uint32_t type, const wchar_t* message) {
        std::string text;
        for (const wchar_t* c = message; c != nullptr && *c; ++c)
            text += *c < 128 ? static_cast<char>(*c) : '?';
        static std::string last;
        if (text == last)
            return;
        last = text;
        psxemu::FsrNote(std::string(type == FFX_API_MESSAGE_TYPE_ERROR ? "AMD error: "
                                                                       : "AMD warning: ") +
                        text);
    }

    // The provider AMD made for a context, by its name.
    std::string ProviderName(void** context) {
        ffxQueryGetProviderVersion provider = {};
        provider.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
        if (FidelityFx::Get().Query(context, &provider.header) != FFX_API_RETURN_OK ||
            provider.versionName == nullptr)
            return std::string();
        return provider.versionName;
    }

}   // namespace

void D3D12GraphicsEngine::NoteFsr(const std::string& line) {
    if (line == fsr_last_note_)
        return;
    fsr_last_note_ = line;
    psxemu::FsrNote(line);
}

psxemu::FsrStatus D3D12GraphicsEngine::fsr_status() const {
    psxemu::FsrStatus status;
    status.ready = fsr_started_ && dlss_pipeline_ != nullptr && fsr_why_.empty();
    status.why = fsr_why_;
    status.note = fsr_note_;
    std::string running = fsr_version_;
    if (running.empty() && !fsr_versions_.empty()) {
        // Before the first picture: what will run.
        std::string note;
        const uint64_t id = FsrVersionId(&note);
        running = fsr_versions_.front().name;
        for (const FsrVersionEntry& entry : fsr_versions_)
            if (entry.id == id)
                running = entry.name;
    }
    status.version = Plain(running);
    // AMD's mark on FSR 4: on a card without it - this laptop's Radeon 780M - the same pictures
    // as FSR 3.1.5 come out, to the bit (ffx_probe, Docs/FSR-Plan.md).
    if (status.note.empty() && !running.empty() && running.back() == '*')
        status.note = "FSR 4 only where the card and its driver have it, FSR 3.1 otherwise";
    for (const FsrVersionEntry& entry : fsr_versions_)
        status.fsr4_available = status.fsr4_available || entry.name.rfind("4", 0) == 0;
    status.generation_ready = fsr_generation_ready_ && fsr_generation_why_.empty();
    status.generation_why = fsr_generation_why_;
    status.generation_version = Plain(fsr_generation_version_);
    return status;
}

// AMD's swap chain is made with the engine, in place of DXGI's: Frame Generation going on or off
// makes the engine again. Upscaling alone needs nothing made again.
bool D3D12GraphicsEngine::FsrNeedsRemaking(const psxemu::FsrChoice& from,
                                          const psxemu::FsrChoice& to) const {
    return from.generating() != to.generating();
}

void D3D12GraphicsEngine::SetFsr(const psxemu::FsrChoice& choice) {
    const bool changed = !(choice == fsr_choice_);
    fsr_choice_ = choice;
    if (device_ == nullptr)
        return;   // before Initialize, which starts it
    if (choice.mode == FsrMode::kOff) {
        // Its memory back; the DLLs stay, for the next time.
        if (fsr_context_ != nullptr) {
            FlushGPU();
            DestroyFsrContext();
        }
        return;
    }
    // A context that would not start is tried again for a new choice.
    if (changed && fsr_started_)
        fsr_why_.clear();
    if (!fsr_started_ && streamline_ == nullptr) {
        StartFsr();
        if (fsr_started_ && dlss_pipeline_ == nullptr) {
            if (CreateDlssPipeline())
                CreateDlssTimers();
            else
                fsr_why_ = "its shaders could not be made";
        }
    }
}

void D3D12GraphicsEngine::StartFsr() {
    fsr_why_.clear();
    fsr_note_.clear();
    fsr_versions_.clear();
    fsr_generation_why_.clear();
    if (dlss_choice_.mode != psxemu::DlssMode::kOff) {
        fsr_why_ = "NVIDIA DLSS is chosen";
        return;
    }
    std::string error;
    if (!FidelityFx::Get().Load(psxemu::ExecutableFolder(), &error)) {
        fsr_why_ = error;
        NoteFsr("not started: " + error);
        return;
    }
    for (const FidelityFx::Version& version :
         FidelityFx::Get().Versions(FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE, device_.Get()))
        fsr_versions_.push_back({ version.id, version.name });
    if (fsr_versions_.empty()) {
        fsr_why_ = "AMD's runtime offers no FSR for this graphics card";
        NoteFsr("not started: " + fsr_why_);
        return;
    }
    std::string offered;
    for (const FsrVersionEntry& entry : fsr_versions_)
        offered += (offered.empty() ? "" : ", ") + entry.name;
    NoteFsr("started: FSR Upscaling " + offered);
    fsr_started_ = true;

    if (fsr_choice_.generating()) {
        if (!FidelityFx::Get().has_frame_generation()) {
            fsr_generation_why_ =
                "AMD's Frame Generation file is not beside the emulator "
                "(amd_fidelityfx_framegeneration_dx12.dll)";
        } else {
            const auto versions = FidelityFx::Get().Versions(
                FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION, device_.Get());
            if (versions.empty())
                fsr_generation_why_ = "AMD's runtime offers no Frame Generation for this card";
            else
                fsr_generation_version_ = versions.front().name;
        }
    }
}

void D3D12GraphicsEngine::StopFsr() {
    DestroyFsrGeneration();
    DestroyFsrContext();
    if (fsr_swap_chain_context_ != nullptr) {
        // AMD's swap chain: its context first, then the last of our references to it.
        for (UINT n = 0; n < kFrameCount; ++n)
            render_targets_[n].Reset();
        FidelityFx::Get().DestroyContext(&fsr_swap_chain_context_);
        fsr_swap_chain_context_ = nullptr;
        swap_chain_.Reset();
    }
    fsr_generation_ready_ = false;
    fsr_ui_registered_ = false;
    fsr_started_ = false;
}

uint64_t D3D12GraphicsEngine::FsrVersionId(std::string* note) const {
    note->clear();
    auto first = [this](const char* prefix) -> uint64_t {
        for (const FsrVersionEntry& entry : fsr_versions_)
            if (entry.name.rfind(prefix, 0) == 0)
                return entry.id;
        return 0;
    };
    switch (fsr_choice_.version) {
    case FsrVersion::kFsr4: {
        const uint64_t id = first("4");
        if (id == 0)
            *note = "AMD's runtime has no FSR 4 for this card, so FSR 3.1 runs";
        return id != 0 ? id : first("3.1");
    }
    case FsrVersion::kFsr3: return first("3.1");
    default: return 0;   // AMD's own choice for the card
    }
}

void D3D12GraphicsEngine::DestroyFsrContext() {
    if (fsr_context_ == nullptr)
        return;
    FidelityFx::Get().DestroyContext(&fsr_context_);
    fsr_context_ = nullptr;
    fsr_context_in_w_ = fsr_context_in_h_ = fsr_context_out_w_ = fsr_context_out_h_ = 0;
    fsr_version_.clear();
}

// One context for each pair of sizes and version: made again - after the card has finished
// with the old - when either changes, which starts its history afresh.
bool D3D12GraphicsEngine::EnsureFsrContext(int in_width, int in_height, int out_width,
                                           int out_height) {
    std::string note;
    const uint64_t version = FsrVersionId(&note);
    fsr_note_ = note;
    if (fsr_context_ != nullptr && in_width == fsr_context_in_w_ &&
        in_height == fsr_context_in_h_ && out_width == fsr_context_out_w_ &&
        out_height == fsr_context_out_h_ && version == fsr_context_version_)
        return true;
    FlushGPU();
    DestroyFsrContext();
    dlss_reset_ = true;

    ffxCreateBackendDX12Desc backend = {};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backend.device = device_.Get();
    ffxOverrideVersion override_version = {};
    override_version.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
    override_version.versionId = version;
    ffxCreateContextDescUpscaleVersion api = {};
    api.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
    api.version = FFX_UPSCALER_VERSION;
    ffxCreateContextDescUpscale create = {};
    create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    create.flags = kUpscaleFlags;
    // With PSXEMU_FSR_LOG, AMD's checks of what it is given, into the log.
    if (psxemu::FsrLogging()) {
        create.flags |= FFX_UPSCALE_ENABLE_DEBUG_CHECKING;
        create.fpMessage = Message;
    }
    create.maxRenderSize = { static_cast<uint32_t>(in_width), static_cast<uint32_t>(in_height) };
    create.maxUpscaleSize = { static_cast<uint32_t>(out_width),
                              static_cast<uint32_t>(out_height) };
    create.header.pNext = &api.header;
    api.header.pNext = &backend.header;
    if (version != 0)
        backend.header.pNext = &override_version.header;
    const ffxReturnCode_t made = FidelityFx::Get().CreateContext(&fsr_context_, &create.header);
    if (made != FFX_API_RETURN_OK) {
        fsr_context_ = nullptr;
        fsr_why_ = std::string("AMD's upscaler would not start: ") + psxemu::FfxResultText(made);
        NoteFsr(fsr_why_);
        return false;
    }
    fsr_context_in_w_ = in_width;
    fsr_context_in_h_ = in_height;
    fsr_context_out_w_ = out_width;
    fsr_context_out_h_ = out_height;
    fsr_context_version_ = version;
    fsr_version_ = ProviderName(&fsr_context_);
    NoteFsr("upscaler " + fsr_version_ + " made for " + std::to_string(in_width) + "x" +
            std::to_string(in_height) + " to " + std::to_string(out_width) + "x" +
            std::to_string(out_height));
    return true;
}

// AMD's own advice is its auto-exposure for a game's HDR scene; the console's picture is 8 bits a
// channel, 0-1, and an exposure of 1 is exactly it - without one FSR warns at every picture.
bool D3D12GraphicsEngine::EnsureFsrExposure() {
    if (fsr_exposure_ready_)
        return true;
    if (!fsr_exposure_) {
        const CD3DX12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE_DEFAULT);
        const CD3DX12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Tex2D(DXGI_FORMAT_R32_FLOAT, 1, 1, 1, 1);
        const CD3DX12_HEAP_PROPERTIES upload_heap(D3D12_HEAP_TYPE_UPLOAD);
        const CD3DX12_RESOURCE_DESC upload_desc =
            CD3DX12_RESOURCE_DESC::Buffer(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
        if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                    IID_PPV_ARGS(&fsr_exposure_))) ||
            FAILED(device_->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE,
                                                    &upload_desc,
                                                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                    IID_PPV_ARGS(&fsr_exposure_upload_)))) {
            fsr_exposure_.Reset();
            fsr_exposure_upload_.Reset();
            return false;
        }
        void* mapped = nullptr;
        if (FAILED(fsr_exposure_upload_->Map(0, nullptr, &mapped)))
            return false;
        const float one = 1.0f;
        memcpy(mapped, &one, sizeof(one));
        fsr_exposure_upload_->Unmap(0, nullptr);
    }
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
    const D3D12_RESOURCE_DESC desc = fsr_exposure_->GetDesc();
    device_->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, nullptr);
    const CD3DX12_TEXTURE_COPY_LOCATION to(fsr_exposure_.Get(), 0);
    const CD3DX12_TEXTURE_COPY_LOCATION from(fsr_exposure_upload_.Get(), footprint);
    command_list_->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    const CD3DX12_RESOURCE_BARRIER readable = CD3DX12_RESOURCE_BARRIER::Transition(
        fsr_exposure_.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    command_list_->ResourceBarrier(1, &readable);
    fsr_exposure_ready_ = true;
    return true;
}

// FSR takes any input for any output: the screen's rectangle, unless the input is larger
// (graphics/fsr/fsr_choice.h).
bool D3D12GraphicsEngine::ChooseFsrOutput(int width, int height, const LetterboxRect& screen,
                                          int* out_width, int* out_height) const {
    psxemu::FsrOutput(width, height, static_cast<int>(screen.width),
                      static_cast<int>(screen.height), out_width, out_height);
    return *out_width > 0 && *out_height > 0 &&
           *out_width <= D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION &&
           *out_height <= D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION;
}

bool D3D12GraphicsEngine::EvaluateFsr(const emulation::psx::SharedPicture& picture,
                                      ID3D12Resource* planes, int out_width, int out_height) {
    const int width = picture.width;
    const int height = picture.height;
    if (!EnsureDlssTargets(width, height, out_width, out_height)) {
        NoteFsr("FSR's textures could not be made");
        return false;
    }
    if (!EnsureFsrContext(width, height, out_width, out_height))
        return false;

    if (!EnsureFsrExposure()) {
        NoteFsr("FSR's exposure could not be made");
        return false;
    }
    MakeUpscalerInputs(picture, planes, kReactiveUnknown, kReactiveTranslucent);

    // The time since the last picture FSR made, which its exposure and accumulation go by.
    LARGE_INTEGER now = {}, frequency = {};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    fsr_frame_ms_ = fsr_last_time_.QuadPart != 0
                        ? static_cast<float>(static_cast<double>(now.QuadPart -
                                                                 fsr_last_time_.QuadPart) *
                                             1000.0 / static_cast<double>(frequency.QuadPart))
                        : 16.7f;
    fsr_frame_ms_ = std::clamp(fsr_frame_ms_, 1.0f, 100.0f);
    fsr_last_time_ = now;

    // Negated, as DLSS's: the picture's jitter is where its samples were moved to, FSR's the
    // other way - measured with ffx_probe --jitter-test (0.054 negated, 0.103 not). The motion
    // as the plane has it, in the picture's own pixels, which is FSR's own scale.
    fsr_jitter_x_ = -picture.jitter_x;
    fsr_jitter_y_ = -picture.jitter_y;
    fsr_last_reset_ = dlss_reset_ || picture.reset;

    ffxDispatchDescUpscale dispatch = {};
    dispatch.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    dispatch.commandList = command_list_.Get();
    // Each in the state it is in on this command list; FSR moves them as it needs, and back. The
    // picture is wherever this frame has it (frame_texture_): a copy pixel shaders read, or the
    // rasteriser's own texture, which any shader reads.
    dispatch.color = psxemu::FfxResource(
        frame_texture_.Get(), frame_state_ == D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
                                  ? FFX_API_RESOURCE_STATE_PIXEL_READ
                                  : FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    dispatch.depth = psxemu::FfxResource(dlss_depth_.Get(), FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    dispatch.motionVectors =
        psxemu::FfxResource(dlss_motion_.Get(), FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    dispatch.reactive =
        psxemu::FfxResource(dlss_hint_.Get(), FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    dispatch.exposure =
        psxemu::FfxResource(fsr_exposure_.Get(), FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    dispatch.output = psxemu::FfxResource(dlss_output_.Get(), FFX_API_RESOURCE_STATE_PIXEL_READ);
    dispatch.jitterOffset = { fsr_jitter_x_, fsr_jitter_y_ };
    dispatch.motionVectorScale = { 1.0f, 1.0f };
    dispatch.renderSize = { static_cast<uint32_t>(width), static_cast<uint32_t>(height) };
    dispatch.upscaleSize = { static_cast<uint32_t>(out_width), static_cast<uint32_t>(out_height) };
    dispatch.enableSharpening = fsr_choice_.sharpness > 0.0f;
    dispatch.sharpness = std::clamp(fsr_choice_.sharpness, 0.0f, 1.0f);
    dispatch.frameTimeDelta = fsr_frame_ms_;
    dispatch.preExposure = 1.0f;   // the console's picture: 8 bits, 0-1, as it is
    dispatch.reset = fsr_last_reset_;
    dispatch.cameraNear = kCameraNear;
    dispatch.cameraFar = kCameraFar;
    dispatch.cameraFovAngleVertical = kCameraFov;
    dispatch.viewSpaceToMetersFactor = kMetresPerUnit;
    dispatch.flags = FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB;
    const ffxReturnCode_t result = FidelityFx::Get().Dispatch(&fsr_context_, &dispatch.header);
    const bool ok = result == FFX_API_RETURN_OK;
    if (!ok)
        NoteFsr(std::string("the upscaler's dispatch failed: ") + psxemu::FfxResultText(result));

    FinishUpscalerInputs(ok);
    if (ok)
        dlss_reset_ = false;
    return ok;
}

// ---- FSR Frame Generation --------------------------------------------------------------------

bool D3D12GraphicsEngine::CreateFsrSwapChain(HWND window, const DXGI_SWAP_CHAIN_DESC1& desc) {
    fsr_generation_ready_ = false;
    if (!fsr_generation_why_.empty())
        return false;
    DXGI_SWAP_CHAIN_DESC1 chain_desc = desc;
    IDXGISwapChain4* chain = nullptr;
    ffxCreateContextDescFrameGenerationSwapChainVersionDX12 version = {};
    version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_DX12;
    version.version = FFX_FRAMEGENERATION_SWAPCHAIN_DX12_VERSION;
    ffxCreateContextDescFrameGenerationSwapChainForHwndDX12 create = {};
    create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_FOR_HWND_DX12;
    create.header.pNext = &version.header;
    create.swapchain = &chain;
    create.hwnd = window;
    create.desc = &chain_desc;
    create.fullscreenDesc = nullptr;
    create.dxgiFactory = factory_.Get();
    create.gameQueue = command_queue_.Get();
    const ffxReturnCode_t made =
        FidelityFx::Get().CreateContext(&fsr_swap_chain_context_, &create.header);
    if (made != FFX_API_RETURN_OK || chain == nullptr) {
        fsr_swap_chain_context_ = nullptr;
        fsr_generation_why_ =
            std::string("AMD's swap chain would not start: ") + psxemu::FfxResultText(made);
        NoteFsr(fsr_generation_why_);
        return false;
    }
    swap_chain_.Attach(chain);
    fsr_generation_ready_ = true;
    NoteFsr("Frame Generation's swap chain made");
    return true;
}

// The context for this window's size, and pictures up to this size: made at the first picture
// it can generate after, and again when either grows or the window changes.
bool D3D12GraphicsEngine::EnsureFsrGeneration(int render_width, int render_height) {
    if (fsr_generation_context_ != nullptr && fsr_generation_display_w_ == width_ &&
        fsr_generation_display_h_ == height_ && render_width <= fsr_generation_render_w_ &&
        render_height <= fsr_generation_render_h_)
        return true;
    DestroyFsrGeneration();
    ffxCreateBackendDX12Desc backend = {};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    backend.device = device_.Get();
    ffxCreateContextDescFrameGenerationVersion version = {};
    version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
    version.version = FFX_FRAMEGENERATION_VERSION;
    ffxCreateContextDescFrameGeneration create = {};
    create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    create.flags = kGenerationFlags;
    if (psxemu::FsrLogging())
        create.flags |= FFX_FRAMEGENERATION_ENABLE_DEBUG_CHECKING;
    create.displaySize = { static_cast<uint32_t>(width_), static_cast<uint32_t>(height_) };
    create.maxRenderSize = { static_cast<uint32_t>(render_width),
                             static_cast<uint32_t>(render_height) };
    create.backBufferFormat = FFX_API_SURFACE_FORMAT_B8G8R8A8_UNORM;
    create.header.pNext = &version.header;
    version.header.pNext = &backend.header;
    const ffxReturnCode_t made =
        FidelityFx::Get().CreateContext(&fsr_generation_context_, &create.header);
    if (made != FFX_API_RETURN_OK) {
        fsr_generation_context_ = nullptr;
        fsr_generation_why_ =
            std::string("AMD's Frame Generation would not start: ") + psxemu::FfxResultText(made);
        NoteFsr(fsr_generation_why_);
        return false;
    }
    fsr_generation_display_w_ = width_;
    fsr_generation_display_h_ = height_;
    fsr_generation_render_w_ = render_width;
    fsr_generation_render_h_ = render_height;
    const std::string name = ProviderName(&fsr_generation_context_);
    if (!name.empty())
        fsr_generation_version_ = name;
    NoteFsr("Frame Generation " + fsr_generation_version_ + " made for " +
            std::to_string(width_) + "x" + std::to_string(height_));
    return true;
}

// As AMD asks: generation off on the swap chain first, which waits for the frames it is still
// making, then the context.
void D3D12GraphicsEngine::DestroyFsrGeneration() {
    if (fsr_generation_context_ == nullptr)
        return;
    ffxConfigureDescFrameGeneration config = {};
    config.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    config.swapChain = swap_chain_.Get();
    config.frameGenerationEnabled = false;
    FidelityFx::Get().Configure(&fsr_generation_context_, &config.header);
    if (fsr_ui_registered_ && fsr_swap_chain_context_ != nullptr) {
        ffxConfigureDescFrameGenerationSwapChainRegisterUiResourceDX12 ui = {};
        ui.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_REGISTERUIRESOURCE_DX12;
        FidelityFx::Get().Configure(&fsr_swap_chain_context_, &ui.header);
        fsr_ui_registered_ = false;
    }
    FidelityFx::Get().DestroyContext(&fsr_generation_context_);
    fsr_generation_context_ = nullptr;
    fsr_generation_display_w_ = fsr_generation_display_h_ = 0;
    fsr_generation_render_w_ = fsr_generation_render_h_ = 0;
}

// Into the command list EndFrame is about to close: generation on or off for this present - and
// when on, its inputs for this picture (DLSS's motion and depth, as FSR's upscaler had them) and
// the overlay's layer, which AMD's swap chain puts over every frame it shows. Off, the overlay is
// in the back buffer already, and the layer is taken away.
void D3D12GraphicsEngine::ConfigureFsrGeneration(bool on) {
    FidelityFx& ffx = FidelityFx::Get();
    if (fsr_generation_context_ != nullptr) {
        // One frame each present, so it counts on by exactly one, as AMD asks; a picture it did
        // not see between is a reset, given below.
        ++fsr_frame_id_;
        const LetterboxRect screen = ScreenRect();
        ffxConfigureDescFrameGeneration config = {};
        config.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
        config.swapChain = swap_chain_.Get();
        config.frameGenerationCallback = [](ffxDispatchDescFrameGeneration* params,
                                            void* context) -> ffxReturnCode_t {
            return FidelityFx::Get().Dispatch(static_cast<ffxContext*>(context), &params->header);
        };
        config.frameGenerationCallbackUserContext = &fsr_generation_context_;
        config.frameGenerationEnabled = on;
        config.allowAsyncWorkloads = false;
        config.HUDLessColor = FfxApiResource{};
        config.flags = 0;
        config.onlyPresentGenerated = false;
        // Made only inside the picture, the bars left as they are.
        config.generationRect = { static_cast<int32_t>(screen.x), static_cast<int32_t>(screen.y),
                                  static_cast<int32_t>(screen.width),
                                  static_cast<int32_t>(screen.height) };
        config.frameID = fsr_frame_id_;
        const ffxReturnCode_t configured = ffx.Configure(&fsr_generation_context_, &config.header);
        if (configured != FFX_API_RETURN_OK)
            NoteFsr(std::string("Frame Generation's configuration failed: ") +
                    psxemu::FfxResultText(configured));

        if (on && configured == FFX_API_RETURN_OK) {
            ffxDispatchDescFrameGenerationPrepareV2 prepare = {};
            prepare.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
            prepare.frameID = fsr_frame_id_;
            prepare.flags = 0;
            prepare.commandList = command_list_.Get();
            prepare.renderSize = { static_cast<uint32_t>(dlss_in_width_),
                                   static_cast<uint32_t>(dlss_in_height_) };
            prepare.jitterOffset = { fsr_jitter_x_, fsr_jitter_y_ };
            prepare.motionVectorScale = { 1.0f, 1.0f };
            prepare.frameTimeDelta = fsr_frame_ms_;
            prepare.reset = fsr_last_reset_;
            prepare.cameraNear = kCameraNear;
            prepare.cameraFar = kCameraFar;
            prepare.cameraFovAngleVertical = kCameraFov;
            prepare.viewSpaceToMetersFactor = kMetresPerUnit;
            prepare.depth =
                psxemu::FfxResource(dlss_depth_.Get(), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
            prepare.motionVectors =
                psxemu::FfxResource(dlss_motion_.Get(), FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
            // No one camera in a PlayStation game: the motion is all of it.
            prepare.cameraPosition[0] = prepare.cameraPosition[1] = prepare.cameraPosition[2] = 0;
            prepare.cameraUp[1] = 1.0f;
            prepare.cameraRight[0] = 1.0f;
            prepare.cameraForward[2] = 1.0f;
            const ffxReturnCode_t prepared = ffx.Dispatch(&fsr_generation_context_, &prepare.header);
            if (prepared != FFX_API_RETURN_OK)
                NoteFsr(std::string("Frame Generation's preparation failed: ") +
                        psxemu::FfxResultText(prepared));
        }
        // Said when it changes, not at every present.
        if (on != fsr_generation_on_) {
            fsr_generation_on_ = on;
            NoteFsr(on ? "Frame Generation on" : "Frame Generation off");
        }
    }

    // The overlay's layer: copied by the swap chain at the present (its own double buffering),
    // so ui_ is free for the next frame as soon as this one is presented.
    if (fsr_swap_chain_context_ != nullptr && (on || fsr_ui_registered_)) {
        ffxConfigureDescFrameGenerationSwapChainRegisterUiResourceDX12 ui = {};
        ui.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_REGISTERUIRESOURCE_DX12;
        if (on)
            ui.uiResource = psxemu::FfxResource(ui_.Get(), FFX_API_RESOURCE_STATE_PIXEL_READ);
        ui.flags = FFX_FRAMEGENERATION_UI_COMPOSITION_FLAG_USE_PREMUL_ALPHA |
                   FFX_FRAMEGENERATION_UI_COMPOSITION_FLAG_ENABLE_INTERNAL_UI_DOUBLE_BUFFERING;
        ffx.Configure(&fsr_swap_chain_context_, &ui.header);
        fsr_ui_registered_ = on;
    }
}
