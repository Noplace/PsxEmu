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
#include "opengl_engine.h"
#include "win32_dialogs.h"
#include "audio/wasapiaudioengine.h"
#include "audio/dsoundaudioengine.h"
#include "shaders/glsl_filters.h"
#include "shaders/legacy_shaders.h"
#include "shaders/ps_scanline_filter.h"
#include "shaders/ps_xbrz_filter.h"
#include "shaders/superxbr_pass0.h"
#include "shaders/superxbr_pass1.h"
#include "shaders/superxbr_pass2.h"

namespace psxemu {

    std::unique_ptr<IGraphicsEngine> CreateGraphicsEngine(GraphicsBackend preferred, HWND window,
                                                          HWND gl_window, int width, int height,
                                                          std::string* active_backend,
                                                          std::wstring* warning) {
        if (warning != nullptr)
            warning->clear();
        auto try_backend = [&](GraphicsBackend backend) -> std::unique_ptr<IGraphicsEngine> {
            std::unique_ptr<IGraphicsEngine> engine;
            if (backend == GraphicsBackend::kD3D12)
                engine = std::make_unique<D3D12GraphicsEngine>();
            else if (backend == GraphicsBackend::kOpenGL)
                engine = std::make_unique<OpenGLGraphicsEngine>();
            else
                engine = std::make_unique<psxemu::D3D11Presenter>();
            const HWND target = backend == GraphicsBackend::kOpenGL ? gl_window : window;
            if (target != nullptr && engine->Initialize(target, width, height))
                return engine;
            return nullptr;
        };
        auto name = [](GraphicsBackend backend) {
            switch (backend) {
                case GraphicsBackend::kD3D12: return L"Direct3D 12";
                case GraphicsBackend::kOpenGL: return L"OpenGL 3.3";
                default: return L"Direct3D 11";
            }
        };

        if (std::unique_ptr<IGraphicsEngine> engine = try_backend(preferred)) {
            *active_backend = GraphicsBackendKey(preferred);
            return engine;
        }

        for (const GraphicsBackend fallback :
             { GraphicsBackend::kD3D11, GraphicsBackend::kD3D12, GraphicsBackend::kOpenGL }) {
            if (fallback == preferred)
                continue;
            if (std::unique_ptr<IGraphicsEngine> engine = try_backend(fallback)) {
                *active_backend = GraphicsBackendKey(fallback);
                if (warning != nullptr) {
                    *warning = name(preferred);
                    *warning += L" was not available; using ";
                    *warning += name(fallback);
                    *warning += L" instead.";
                }
                return engine;
            }
        }

        return nullptr;
    }

    GraphicsBackend ParseGraphicsBackend(const std::string& key) {
        if (key == "d3d12")
            return GraphicsBackend::kD3D12;
        if (key == "opengl")
            return GraphicsBackend::kOpenGL;
        return GraphicsBackend::kD3D11;
    }

    const char* GraphicsBackendKey(GraphicsBackend backend) {
        switch (backend) {
            case GraphicsBackend::kD3D12: return "d3d12";
            case GraphicsBackend::kOpenGL: return "opengl";
            default: return "d3d11";
        }
    }

    void LoadAllFilters(IGraphicsEngine& engine, GraphicsBackend backend) {
        if (backend == GraphicsBackend::kOpenGL) {
            // The GLSL ports, under the same keys. A Super-xBR pass is joined from its pieces.
            for (const GlslFilter& filter : kGlslFilters) {
                std::string source;
                for (const char* part : filter.parts) {
                    if (part != nullptr)
                        source += part;
                }
                engine.LoadPixelShaderFromString(filter.key, source.c_str());
            }
            engine.LoadShaderChain("superxbr", { { "superxbr_pass0", 2 },
                                                 { "superxbr_pass1", 2 },
                                                 { "superxbr_pass2", 2 } });
            return;
        }

        engine.LoadPixelShaderFromString("nearest", kLegacyShaders[0]);
        engine.LoadPixelShaderFromString("bilinear", kLegacyShaders[1]);
        engine.LoadPixelShaderFromString("crt", kLegacyShaders[2]);
        engine.LoadPixelShaderFromString("eagle", kLegacyShaders[3]);
        engine.LoadPixelShaderFromString("hq2x", kLegacyShaders[4]);
        engine.LoadPixelShaderFromString("xbrz_legacy", kLegacyShaders[5]);
        engine.LoadCustomPixelShader("scanline", g_ps_scanline_filter,
                                     sizeof(g_ps_scanline_filter));
        engine.LoadCustomPixelShader("xbrz", g_ps_xbrz_filter, sizeof(g_ps_xbrz_filter));

        // Super-xBR: three passes, each rendering at twice the emulator frame (pass 0 reads the
        // frame, pass 1 the pass 0 result plus the frame, pass 2 the pass 1 result), then the
        // engine's own linear blit to the window. Engines without chain support return false and
        // the entry simply does nothing there.
        engine.LoadCustomPixelShader("superxbr_pass0", g_superxbr_pass0, sizeof(g_superxbr_pass0));
        engine.LoadCustomPixelShader("superxbr_pass1", g_superxbr_pass1, sizeof(g_superxbr_pass1));
        engine.LoadCustomPixelShader("superxbr_pass2", g_superxbr_pass2, sizeof(g_superxbr_pass2));
        engine.LoadShaderChain("superxbr", { { "superxbr_pass0", 2 },
                                             { "superxbr_pass1", 2 },
                                             { "superxbr_pass2", 2 } });
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
