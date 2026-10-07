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
#include "graphics/video_presenter.h"

#include "app/const.h"   // RendererHasFilters
#include "psx/gpu_core.h"
#include "app/win32_dialogs.h"

namespace psxemu {

    using emulation::host::VideoFrame;

    D3DPresenter::D3DPresenter(const RenderWindows& windows,
                               std::function<void(std::function<void()>)> to_ui,
                               FrameStatsRing* stats, DlssTiming* dlss_timing)
        : window_(windows.main), windows_(windows), to_ui_(std::move(to_ui)), stats_(stats),
          dlss_timing_(dlss_timing) {
        RECT client = {};
        GetClientRect(window_, &client);
        width_ = client.right - client.left;
        height_ = client.bottom - client.top;
        overlay_.SetDpi(static_cast<int>(GetDpiForWindow(window_)));
    }

    D3DPresenter::~D3DPresenter() {
        // On the video thread, and before the window goes: a swap chain outliving its window is
        // the one ordering DXGI does not forgive.
        if (engine_ != nullptr)
            engine_->Shutdown();
    }

    bool D3DPresenter::Open(const std::string& renderer, const std::string& filter) {
        return Create(renderer, filter);
    }

    bool D3DPresenter::Create(const std::string& renderer, const std::string& filter) {
        const GraphicsBackend preferred = ParseGraphicsBackend(renderer);
        std::wstring warning;
        std::string opened;
        engine_ = CreateGraphicsEngine(preferred, windows_, width_, height_, &opened, &warning,
                                       card_luid_, card_name_, dlss_, fsr_);
        if (engine_ == nullptr && card_luid_ != 0) {
            // No engine would start on the card chosen. Better a picture on the one Windows
            // picks than none, with the reason - the card is dropped from here on, so the menu
            // and the rasteriser are told what really happened (card_luid()).
            card_luid_ = 0;
            std::string reason = card_name_;
            card_name_.clear();
            engine_ = CreateGraphicsEngine(preferred, windows_, width_, height_, &opened,
                                           &warning, 0, std::string(), dlss_, fsr_);
            if (engine_ != nullptr) {
                std::wstring text = L"No renderer would start on " +
                                    std::wstring(reason.begin(), reason.end()) +
                                    L"; using the graphics card Windows chose instead.";
                warning = warning.empty() ? text : text + L"\n" + warning;
            }
        }
        if (engine_ == nullptr) {
            renderer_.clear();
            filter_.clear();
            return false;
        }

        renderer_ = opened;
        engine_->SetFrameGenerationAllowed(generation_allowed_);
        engine_->SetDlssTiming(dlss_timing_);
        // What the caller hears of from here: not this, which it asks for itself.
        dlss_reported_ = engine_->dlss_status();
        fsr_reported_ = engine_->fsr_status();
        last_picture_ = 0;
        // Filters run on Direct3D 12 and OpenGL; on D3D11 nothing is loaded and nothing is ticked.
        filter_.clear();
        if (RendererHasFilters(renderer_)) {
            LoadAllFilters(*engine_, ParseGraphicsBackend(renderer_));
            engine_->SetPixelShader(filter);
            filter_ = filter;
        }

        if (!warning.empty() && to_ui_) {
            HWND window = window_;
            to_ui_([window, warning] { ShowWarning(window, warning.c_str()); });
        }
        overlay_.SetRendererInfo(renderer_, filter_);
        return true;
    }

    void D3DPresenter::Present(const VideoFrame& frame) { PresentFrame(frame, false); }

    void D3DPresenter::PresentFrame(const VideoFrame& frame, bool again) {
        if (engine_ == nullptr)
            return;

        // The hardware rasteriser's picture, left on the card. One this engine cannot open - the
        // renderer just switched to one that cannot, and the rasteriser has not heard yet - is
        // not shown; the frames after it come as pixels. One never finished - the rasteriser's
        // card lost - is not shown either.
        if (frame.shared) {
            // Under Frame Generation each picture is presented once, and the repeats of it at
            // the vblanks after - a 30 fps game's every other one - not at all: Frame Generation
            // makes the pictures between. By its number, so one whose first vblank the mailbox
            // dropped is still presented, at its repeat.
            const uint32_t picture = frame.shared.picture;
            if (engine_->TakesOnlyNewPictures() && picture != 0 && picture == last_picture_ &&
                !again)
                return;
            // One drawn on the renderer's own device is waited for on the card, by the renderer;
            // one drawn on another device it is not using - the renderer just made again, and
            // the rasteriser not yet - cannot be shown by it at all.
            const void* device = frame.shared.source->device();
            if (engine_->SharedPictureAdapter() != frame.shared.source->adapter() ||
                (device != nullptr && device != engine_->SharedPictureDevice()) ||
                (device == nullptr &&
                 !frame.shared.source->WaitReady(frame.shared.serial, kSharedPictureWaitMs)))
                return;
            if (picture != last_picture_) {
                last_picture_ = picture;
                last_picture_time_ = std::chrono::steady_clock::now();
            }
            DrawShared(frame.shared);
            ReportDlss();
            ReportFsr();
            return;
        }

        const uint32_t* pixels = frame.pixels.data();
        int width = frame.width;
        int height = frame.height;

        // View > VRAM: all 1024x512 of it, converted the same way the display area already
        // is. Done here rather than on the machine's thread - this one has the time.
        if (frame.is_vram) {
            vram_scratch_.resize(frame.vram.size());
            for (size_t i = 0; i < frame.vram.size(); ++i) {
                const uint16_t p = frame.vram[i];
                const uint32_t r = ((p & 0x1F) << 3) | ((p & 0x1F) >> 2);
                const uint32_t g = (((p >> 5) & 0x1F) << 3) | (((p >> 5) & 0x1F) >> 2);
                const uint32_t b = (((p >> 10) & 0x1F) << 3) | (((p >> 10) & 0x1F) >> 2);
                vram_scratch_[i] = 0xFF000000u | (r << 16) | (g << 8) | b;
            }
            pixels = vram_scratch_.data();
        }

        if (pixels == nullptr || width <= 0 || height <= 0)
            return;

        Draw(pixels, width, height);
    }

    void D3DPresenter::Draw(const uint32_t* pixels, int width, int height) {
        const auto start = Overlay::Clock::now();
        if (stats_ != nullptr)
            stats_->Drain([this](const emulation::host::FrameSample& s) { overlay_.AddSample(s); });
        const OverlayDrawData& overlay = overlay_.Build(width_, height_, width, height, start);
        engine_->SetOverlay(&overlay);
        engine_->BeginFrame();
        engine_->RenderFramebuffer(pixels, width, height);
        engine_->EndFrame();
        engine_->SetOverlay(nullptr);
        overlay_.NotePresent(
            std::chrono::duration<double, std::milli>(Overlay::Clock::now() - start).count());
    }

    void D3DPresenter::DrawShared(const emulation::psx::SharedPicture& picture) {
        const auto start = Overlay::Clock::now();
        if (stats_ != nullptr)
            stats_->Drain([this](const emulation::host::FrameSample& s) { overlay_.AddSample(s); });
        const OverlayDrawData& overlay =
            overlay_.Build(width_, height_, picture.width, picture.height, start);
        engine_->SetOverlay(&overlay);
        engine_->BeginFrame();
        engine_->RenderSharedPicture(picture);
        engine_->EndFrame();
        engine_->SetOverlay(nullptr);
        overlay_.NotePresent(
            std::chrono::duration<double, std::milli>(Overlay::Clock::now() - start).count());
    }

    bool D3DPresenter::WantsRefresh() {
        if (engine_ == nullptr)
            return false;
        // Under Frame Generation, while pictures come, the overlay moves with them: a present
        // between two would be one Frame Generation had to go off for, and the pace would stumble.
        if (engine_->TakesOnlyNewPictures() &&
            std::chrono::steady_clock::now() - last_picture_time_ < std::chrono::milliseconds(200))
            return false;
        return overlay_.NeedsRedraw(Overlay::Clock::now());
    }

    // The last frame again, for the overlay moving over it - or, before there has been one, a
    // black picture to put it over.
    void D3DPresenter::Refresh(const VideoFrame* last) {
        if (engine_ == nullptr)
            return;
        if (last != nullptr) {
            PresentFrame(*last, true);
            return;
        }
        const int kWidth = 320, kHeight = 240;
        blank_.assign(static_cast<size_t>(kWidth) * kHeight, 0xFF000000u);
        Draw(blank_.data(), kWidth, kHeight);
    }

    void D3DPresenter::Resize(int width, int height) {
        width_ = width;
        height_ = height;
        if (engine_ != nullptr)
            engine_->Resize(width, height);
    }

    // Live renderer switch. The engine being replaced was working moments ago, so the only case
    // left to handle is the very unlikely one where the new engine fails and the old one cannot be
    // brought back either - which leaves the front end with nothing to draw with, and says so.
    void D3DPresenter::SetRenderer(const std::string& key) {
        if (key == renderer_)
            return;
        Rebuild(key);
    }

    // Settings > Video > Graphics Card. The engine is made again on the new card - unless it is
    // OpenGL, which no card can be asked of, so there is nothing to make again.
    void D3DPresenter::SetGraphicsCard(uint64_t luid, const std::string& name) {
        if (luid == card_luid_ && name == card_name_)
            return;
        card_luid_ = luid;
        card_name_ = name;
        if (engine_ != nullptr && renderer_ != "opengl")
            Rebuild(renderer_);
    }

    void D3DPresenter::SetFrameGenerationAllowed(bool allowed) {
        generation_allowed_ = allowed;
        if (engine_ != nullptr)
            engine_->SetFrameGenerationAllowed(allowed);
    }

    void D3DPresenter::ReportDlss() {
        const DlssStatus status = engine_->dlss_status();
        if (status == dlss_reported_)
            return;
        dlss_reported_ = status;
        if (dlss_listener_)
            dlss_listener_(status);
    }

    void D3DPresenter::ReportFsr() {
        const FsrStatus status = engine_->fsr_status();
        if (status == fsr_reported_)
            return;
        fsr_reported_ = status;
        if (fsr_listener_)
            fsr_listener_(status);
    }

    void D3DPresenter::SetFsr(const FsrChoice& choice) {
        if (choice == fsr_)
            return;
        const FsrChoice before = fsr_;
        fsr_ = choice;
        if (engine_ == nullptr)
            return;
        if (engine_->FsrNeedsRemaking(before, choice))
            Rebuild(renderer_);
        else
            engine_->SetFsr(choice);
    }

    void D3DPresenter::SetDlss(const DlssChoice& choice) {
        if (choice == dlss_)
            return;
        const DlssChoice before = dlss_;
        dlss_ = choice;
        if (engine_ == nullptr)
            return;
        if (engine_->DlssNeedsRemaking(before, choice))
            Rebuild(renderer_);
        else
            engine_->SetDlss(choice);
    }

    void D3DPresenter::Rebuild(const std::string& key) {
        const std::string keep_filter = filter_;
        if (engine_ != nullptr)
            engine_->Shutdown();
        engine_.reset();
        if (!Create(key, keep_filter) && to_ui_) {
            HWND window = window_;
            to_ui_([window] {
                ShowError(window,
                          L"Could not switch renderer, and the previous one could not "
                          L"be restored either. Restart the emulator.");
            });
        }
    }

    void D3DPresenter::SetFilter(const std::string& key) {
        if (engine_ == nullptr || !RendererHasFilters(renderer_))
            return;
        engine_->SetPixelShader(key);
        filter_ = key;
        overlay_.SetRendererInfo(renderer_, filter_);
    }

}   // namespace psxemu
