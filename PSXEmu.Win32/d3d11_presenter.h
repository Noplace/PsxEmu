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

#include <d3d11.h>
#include <cstdint>

namespace psxemu {

/*
  Presents the core's framebuffer.

  This is the *only* thing Direct3D does in this emulator. Every pixel is
  rasterised on the CPU inside PSXEmu.Core, which owns VRAM; this class uploads
  the finished frame into a texture and stretches it over the window. Nothing
  about the PlayStation's drawing is expressed in shaders, which is what keeps
  the core testable without a graphics device.

  Swapping this for a D3D12 or Vulkan presenter changes nothing else.
*/
class D3D11Presenter {
 public:
  D3D11Presenter();
  ~D3D11Presenter();

  bool Initialize(HWND window);
  void Deinitialize();

  // Uploads and draws one frame. `pixels` is width*height of XRGB8888.
  void Present(const uint32_t* pixels, int width, int height);

  // Called when the window is resized; the back buffer follows the client area.
  void Resize(int width, int height);

  bool ready() const { return device_ != nullptr; }

 private:
  HWND window_;
  int back_buffer_width_;
  int back_buffer_height_;

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

}  // namespace psxemu
