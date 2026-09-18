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
#include "engine_factory.h"

#include "d3d11_presenter.h"
#include "d3d12_graphics_engine.h"
#include "win32_dialogs.h"
#include "audio/wasapiaudioengine.h"
#include "audio/dsoundaudioengine.h"
#include "shaders/legacy_shaders.h"
#include "shaders/ps_scanline_filter.h"
#include "shaders/ps_xbrz_filter.h"

namespace psxemu {

    std::unique_ptr<IGraphicsEngine> CreateGraphicsEngine(GraphicsBackend preferred, HWND window,
                                                          int width, int height,
                                                          std::string* active_backend,
                                                          std::wstring* warning) {
        if (warning != nullptr)
            warning->clear();
        auto try_backend = [&](GraphicsBackend backend) -> std::unique_ptr<IGraphicsEngine> {
            std::unique_ptr<IGraphicsEngine> engine;
            if (backend == GraphicsBackend::kD3D12)
                engine = std::make_unique<D3D12GraphicsEngine>();
            else
                engine = std::make_unique<psxemu::D3D11Presenter>();
            if (engine->Initialize(window, width, height))
                return engine;
            return nullptr;
        };

        if (std::unique_ptr<IGraphicsEngine> engine = try_backend(preferred)) {
            *active_backend = (preferred == GraphicsBackend::kD3D12) ? "d3d12" : "d3d11";
            return engine;
        }

        const GraphicsBackend fallback = (preferred == GraphicsBackend::kD3D12)
                                             ? GraphicsBackend::kD3D11
                                             : GraphicsBackend::kD3D12;
        if (std::unique_ptr<IGraphicsEngine> engine = try_backend(fallback)) {
            *active_backend = (fallback == GraphicsBackend::kD3D12) ? "d3d12" : "d3d11";
            const wchar_t* preferred_name =
                (preferred == GraphicsBackend::kD3D12) ? L"Direct3D 12" : L"Direct3D 11";
            const wchar_t* fallback_name =
                (fallback == GraphicsBackend::kD3D12) ? L"Direct3D 12" : L"Direct3D 11";
            if (warning != nullptr) {
                *warning = preferred_name;
                *warning += L" was not available; using ";
                *warning += fallback_name;
                *warning += L" instead.";
            }
            return engine;
        }

        return nullptr;
    }

    void LoadAllFilters(IGraphicsEngine& engine) {
        engine.LoadPixelShaderFromString("nearest", kLegacyShaders[0]);
        engine.LoadPixelShaderFromString("bilinear", kLegacyShaders[1]);
        engine.LoadPixelShaderFromString("crt", kLegacyShaders[2]);
        engine.LoadPixelShaderFromString("eagle", kLegacyShaders[3]);
        engine.LoadPixelShaderFromString("hq2x", kLegacyShaders[4]);
        engine.LoadPixelShaderFromString("xbrz_legacy", kLegacyShaders[5]);
        engine.LoadCustomPixelShader("scanline", g_ps_scanline_filter,
                                     sizeof(g_ps_scanline_filter));
        engine.LoadCustomPixelShader("xbrz", g_ps_xbrz_filter, sizeof(g_ps_xbrz_filter));
    }

    namespace {

        std::unique_ptr<IAudioEngine> TryAudioEngine(AudioBackend backend) {
            using emulation::psx::Spu;
            std::unique_ptr<IAudioEngine> engine;
            if (backend == AudioBackend::kWasapi)
                engine = std::make_unique<WASAPIAudioEngine>();
            else
                engine = std::make_unique<DirectSoundAudioEngine>();
            if (!engine->Initialize(Spu::kSampleRate, 2))
                return nullptr;
            return engine;
        }

        const char* AudioBackendKey(AudioBackend backend) {
            return (backend == AudioBackend::kWasapi) ? "wasapi" : "dsound";
        }

    }   // namespace

    std::unique_ptr<IAudioEngine> CreateAudioEngine(AudioBackend preferred,
                                                    std::string* active_backend) {
        const AudioBackend fallback = (preferred == AudioBackend::kWasapi)
                                          ? AudioBackend::kDirectSound
                                          : AudioBackend::kWasapi;

        // No warning on the fallback, unlike the renderer: sound quietly coming out of the other
        // API is a better outcome than a dialog about it, and the menu tick shows which one is
        // actually running for anyone who wants to know.
        for (AudioBackend backend : { preferred, fallback }) {
            std::unique_ptr<IAudioEngine> engine = TryAudioEngine(backend);
            if (engine != nullptr) {
                if (active_backend != nullptr)
                    *active_backend = AudioBackendKey(backend);
                return engine;
            }
        }

        if (active_backend != nullptr)
            active_backend->clear();
        return nullptr;
    }

}   // namespace psxemu
