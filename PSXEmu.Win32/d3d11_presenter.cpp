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
#include "d3d11_presenter.h"

#include <d3dcompiler.h>
#include <cstring>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxgi.lib")

namespace psxemu {

namespace {

template <typename T>
void Release(T** object) {
  if (*object != nullptr) {
    (*object)->Release();
    *object = nullptr;
  }
}

// The whole pipeline, inline. Compiled at startup rather than loaded from
// .cso files next to the executable, which is how the previous front end did
// it - with absolute paths baked in from somebody else's machine.
//
// No vertex buffer: the vertex shader builds a full-screen triangle from the
// vertex id alone.
const char kShaderSource[] =
    "Texture2D frame : register(t0);\n"
    "SamplerState frame_sampler : register(s0);\n"
    "\n"
    "struct VsOut {\n"
    "  float4 position : SV_POSITION;\n"
    "  float2 uv : TEXCOORD0;\n"
    "};\n"
    "\n"
    "VsOut VsMain(uint id : SV_VertexID) {\n"
    "  VsOut output;\n"
    "  output.uv = float2((id << 1) & 2, id & 2);\n"
    "  output.position = float4(output.uv * float2(2, -2) + float2(-1, 1), 0, 1);\n"
    "  return output;\n"
    "}\n"
    "\n"
    "float4 PsMain(VsOut input) : SV_TARGET {\n"
    "  return frame.Sample(frame_sampler, input.uv);\n"
    "}\n";

}  // namespace

D3D11Presenter::D3D11Presenter()
    : window_(nullptr),
      back_buffer_width_(0),
      back_buffer_height_(0),
      device_(nullptr),
      context_(nullptr),
      swap_chain_(nullptr),
      render_target_(nullptr),
      frame_texture_(nullptr),
      frame_view_(nullptr),
      texture_width_(0),
      texture_height_(0),
      vertex_shader_(nullptr),
      pixel_shader_(nullptr),
      sampler_(nullptr) {
}

D3D11Presenter::~D3D11Presenter() {
  Deinitialize();
}

bool D3D11Presenter::Initialize(HWND window) {
  window_ = window;

  RECT client;
  GetClientRect(window, &client);
  back_buffer_width_ = client.right - client.left;
  back_buffer_height_ = client.bottom - client.top;
  if (back_buffer_width_ <= 0) back_buffer_width_ = 640;
  if (back_buffer_height_ <= 0) back_buffer_height_ = 480;

  DXGI_SWAP_CHAIN_DESC description;
  memset(&description, 0, sizeof(description));
  description.BufferCount = 2;
  description.BufferDesc.Width = back_buffer_width_;
  description.BufferDesc.Height = back_buffer_height_;
  description.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  description.OutputWindow = window;
  description.SampleDesc.Count = 1;
  description.Windowed = TRUE;
  description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

  UINT flags = 0;
#ifdef _DEBUG
  flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

  const D3D_FEATURE_LEVEL levels[] = {
    D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0
  };

  HRESULT result = D3D11CreateDeviceAndSwapChain(
      nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels,
      ARRAYSIZE(levels), D3D11_SDK_VERSION, &description, &swap_chain_,
      &device_, nullptr, &context_);

  if (FAILED(result)) {
    // A debug device is not installed on every machine, so a failure with the
    // debug flag set is worth retrying without it before giving up.
    flags = 0;
    result = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels,
        ARRAYSIZE(levels), D3D11_SDK_VERSION, &description, &swap_chain_,
        &device_, nullptr, &context_);
  }
  if (FAILED(result))
    return false;

  if (!CreateRenderTarget())
    return false;

  ID3DBlob* vertex_blob = nullptr;
  ID3DBlob* pixel_blob = nullptr;
  ID3DBlob* errors = nullptr;

  result = D3DCompile(kShaderSource, sizeof(kShaderSource) - 1, nullptr,
                      nullptr, nullptr, "VsMain", "vs_4_0", 0, 0, &vertex_blob,
                      &errors);
  if (FAILED(result)) {
    Release(&errors);
    return false;
  }
  Release(&errors);

  result = D3DCompile(kShaderSource, sizeof(kShaderSource) - 1, nullptr,
                      nullptr, nullptr, "PsMain", "ps_4_0", 0, 0, &pixel_blob,
                      &errors);
  if (FAILED(result)) {
    Release(&errors);
    Release(&vertex_blob);
    return false;
  }
  Release(&errors);

  device_->CreateVertexShader(vertex_blob->GetBufferPointer(),
                              vertex_blob->GetBufferSize(), nullptr,
                              &vertex_shader_);
  device_->CreatePixelShader(pixel_blob->GetBufferPointer(),
                             pixel_blob->GetBufferSize(), nullptr,
                             &pixel_shader_);
  Release(&vertex_blob);
  Release(&pixel_blob);

  D3D11_SAMPLER_DESC sampler;
  memset(&sampler, 0, sizeof(sampler));
  // Point sampling: the PSX output is a small buffer of exact pixels, and
  // smoothing it on the way up is a decision for a filter, not the presenter.
  sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
  sampler.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  device_->CreateSamplerState(&sampler, &sampler_);

  return vertex_shader_ != nullptr && pixel_shader_ != nullptr;
}

bool D3D11Presenter::CreateRenderTarget() {
  ID3D11Texture2D* back_buffer = nullptr;
  if (FAILED(swap_chain_->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                    reinterpret_cast<void**>(&back_buffer))))
    return false;
  const HRESULT result =
      device_->CreateRenderTargetView(back_buffer, nullptr, &render_target_);
  Release(&back_buffer);
  return SUCCEEDED(result);
}

void D3D11Presenter::ReleaseRenderTarget() {
  Release(&render_target_);
}

void D3D11Presenter::Deinitialize() {
  Release(&sampler_);
  Release(&pixel_shader_);
  Release(&vertex_shader_);
  Release(&frame_view_);
  Release(&frame_texture_);
  ReleaseRenderTarget();
  Release(&swap_chain_);
  Release(&context_);
  Release(&device_);
}

void D3D11Presenter::Resize(int width, int height) {
  if (swap_chain_ == nullptr || width <= 0 || height <= 0)
    return;
  if (width == back_buffer_width_ && height == back_buffer_height_)
    return;

  back_buffer_width_ = width;
  back_buffer_height_ = height;

  ReleaseRenderTarget();
  swap_chain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
  CreateRenderTarget();
}

// The core changes resolution during boot, so the texture is rebuilt whenever
// the frame size changes rather than being sized once.
bool D3D11Presenter::EnsureFrameTexture(int width, int height) {
  if (frame_texture_ != nullptr && width == texture_width_ &&
      height == texture_height_)
    return true;

  Release(&frame_view_);
  Release(&frame_texture_);

  D3D11_TEXTURE2D_DESC description;
  memset(&description, 0, sizeof(description));
  description.Width = width;
  description.Height = height;
  description.MipLevels = 1;
  description.ArraySize = 1;
  description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  description.SampleDesc.Count = 1;
  description.Usage = D3D11_USAGE_DYNAMIC;
  description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  description.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

  if (FAILED(device_->CreateTexture2D(&description, nullptr, &frame_texture_)))
    return false;
  if (FAILED(device_->CreateShaderResourceView(frame_texture_, nullptr,
                                               &frame_view_))) {
    Release(&frame_texture_);
    return false;
  }

  texture_width_ = width;
  texture_height_ = height;
  return true;
}

void D3D11Presenter::Present(const uint32_t* pixels, int width, int height) {
  if (device_ == nullptr || render_target_ == nullptr)
    return;
  if (pixels == nullptr || width <= 0 || height <= 0)
    return;
  if (!EnsureFrameTexture(width, height))
    return;

  D3D11_MAPPED_SUBRESOURCE mapped;
  if (SUCCEEDED(context_->Map(frame_texture_, 0, D3D11_MAP_WRITE_DISCARD, 0,
                              &mapped))) {
    // Copy row by row: the mapped pitch is whatever the driver chose and is
    // rarely width*4.
    uint8_t* destination = static_cast<uint8_t*>(mapped.pData);
    for (int y = 0; y < height; ++y) {
      memcpy(destination + y * mapped.RowPitch, pixels + y * width,
             static_cast<size_t>(width) * 4);
    }
    context_->Unmap(frame_texture_, 0);
  }

  const float clear[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
  context_->OMSetRenderTargets(1, &render_target_, nullptr);
  context_->ClearRenderTargetView(render_target_, clear);

  // Letterbox rather than stretch, so the aspect the game chose survives.
  const float target_aspect = static_cast<float>(width) / height;
  float view_width = static_cast<float>(back_buffer_width_);
  float view_height = view_width / target_aspect;
  if (view_height > back_buffer_height_) {
    view_height = static_cast<float>(back_buffer_height_);
    view_width = view_height * target_aspect;
  }

  D3D11_VIEWPORT viewport;
  viewport.TopLeftX = (back_buffer_width_ - view_width) * 0.5f;
  viewport.TopLeftY = (back_buffer_height_ - view_height) * 0.5f;
  viewport.Width = view_width;
  viewport.Height = view_height;
  viewport.MinDepth = 0.0f;
  viewport.MaxDepth = 1.0f;
  context_->RSSetViewports(1, &viewport);

  context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  context_->IASetInputLayout(nullptr);
  context_->VSSetShader(vertex_shader_, nullptr, 0);
  context_->PSSetShader(pixel_shader_, nullptr, 0);
  context_->PSSetShaderResources(0, 1, &frame_view_);
  context_->PSSetSamplers(0, 1, &sampler_);
  context_->Draw(3, 0);

  swap_chain_->Present(1, 0);
}

}  // namespace psxemu
