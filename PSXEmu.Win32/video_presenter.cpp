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
#include "video_presenter.h"

#include "psx/gpu_core.h"
#include "win32_dialogs.h"

namespace psxemu {

    using emulation::host::VideoFrame;

    D3DPresenter::D3DPresenter(HWND window, std::function<void(std::function<void()>)> to_ui)
        : window_(window), to_ui_(std::move(to_ui)) {
        RECT client = {};
        GetClientRect(window_, &client);
        width_ = client.right - client.left;
        height_ = client.bottom - client.top;
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
        const GraphicsBackend preferred =
            (renderer == "d3d12") ? GraphicsBackend::kD3D12 : GraphicsBackend::kD3D11;
        std::wstring warning;
        std::string opened;
        engine_ = CreateGraphicsEngine(preferred, window_, width_, height_, &opened, &warning);
        if (engine_ == nullptr) {
            renderer_.clear();
            filter_.clear();
            return false;
        }

        renderer_ = opened;
        // Filters are a D3D12 feature here; on D3D11 nothing is loaded and nothing is ticked.
        filter_.clear();
        if (renderer_ == "d3d12") {
            LoadAllFilters(*engine_);
            engine_->SetPixelShader(filter);
            filter_ = filter;
        }

        if (!warning.empty() && to_ui_) {
            HWND window = window_;
            to_ui_([window, warning] { ShowWarning(window, warning.c_str()); });
        }
        return true;
    }

    void D3DPresenter::Present(const VideoFrame& frame) {
        if (engine_ == nullptr)
            return;

        const uint32_t* pixels = frame.pixels.data();
        int width = frame.width;
        int height = frame.height;

        // Video > View VRAM: all 1024x512 of it, converted the same way the display area already
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

        engine_->BeginFrame();
        engine_->RenderFramebuffer(pixels, width, height);
        engine_->EndFrame();
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
        if (engine_ == nullptr || renderer_ != "d3d12")
            return;
        engine_->SetPixelShader(key);
        filter_ = key;
    }

}   // namespace psxemu
