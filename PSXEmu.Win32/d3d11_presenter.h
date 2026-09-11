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

#include <d3d11.h>
#include <cstdint>
#include <string>

namespace psxemu {

    /*
  Presents the core's framebuffer.

  This is the *only* thing Direct3D does in this emulator. Every pixel is
  rasterised on the CPU inside PSXEmu.Core, which owns VRAM; this class uploads
  the finished frame into a texture and stretches it over the window. Nothing
  about the PlayStation's drawing is expressed in shaders, which is what keeps
  the core testable without a graphics device.

  Implements IGraphicsEngine so the front end can hold this behind a pointer
  alongside a D3D12 alternative and switch between them; the three-call frame
  shape (BeginFrame/RenderFramebuffer/EndFrame) is new, but each was already
  a distinct phase inside the old single-call Present, just not split out.

  This does not support pixel-shader filters - SetPixelShader and the two
  loaders are no-ops/failures here on purpose. The D3D12 engine is the one
  that grew that capability; a caller offering filters as a choice is
  expected to check which engine is active first, not call these blind.
*/
    class D3D11Presenter : public IGraphicsEngine {
     public:
        D3D11Presenter();
        ~D3D11Presenter() override;

        bool Initialize(HWND window, int width, int height) override;
        void Shutdown() override;

        void BeginFrame() override;
        void RenderFramebuffer(const void* data, int width, int height) override;
        void EndFrame() override;

        // Called when the window is resized; the back buffer follows the client area.
        void Resize(int width, int height) override;

        void SetVsync(bool enabled) override { vsync_ = enabled; }

        // No filter support - see the class comment above.
        void SetPixelShader(const std::string&) override {}
        bool LoadCustomPixelShader(const std::string&, const uint8_t*, size_t) override {
            return false;
        }
        bool LoadPixelShaderFromString(const std::string&, const char*) override { return false; }

        bool ready() const { return device_ != nullptr; }

     private:
        HWND window_;
        int back_buffer_width_;
        int back_buffer_height_;
        bool vsync_ = true;

        ID3D11Device* device_;
        ID3D11DeviceContext* context_;
        IDXGISwapChain* swap_chain_;
        ID3D11RenderTargetView* render_target_;

        // The frame, as a texture. Recreated whenever the core changes resolution -
        // the PSX does that mid-boot, so it cannot be assumed fixed.
        ID3D11Texture2D* frame_texture_;
        ID3D11ShaderResourceView* frame_view_;
        int texture_width_;
        int texture_height_;

        ID3D11VertexShader* vertex_shader_;
        ID3D11PixelShader* pixel_shader_;
        ID3D11SamplerState* sampler_;

        bool CreateRenderTarget();
        void ReleaseRenderTarget();
        bool EnsureFrameTexture(int width, int height);
    };

}   // namespace psxemu
