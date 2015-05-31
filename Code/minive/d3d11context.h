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
#ifndef MINIVE_D3D11CONTEXT_H
#define MINIVE_D3D11CONTEXT_H

#include <d3d11.h>
#include <DirectXMath.h>
#include <DirectXPackedVector.h>
using namespace DirectX;
using namespace DirectX::PackedVector;
#include <WinCore/types.h>

namespace minive {


struct Vertex {
  FLOAT x, y, z;
	XMVECTOR color;
	FLOAT u, v;
};

__declspec(align(16))
class D3D11Context : public Context {
 public:
  void *operator new( size_t stAllocateBlock);
  void   operator delete (void* p);

  D3D11Context();
  ~D3D11Context();
  int Initialize(int width, int height, bool vsync, HWND hwnd, bool fullscreen, float depth, float near);
  int Deinitialize();
  int Clear();
  int Present();
 private:
  bool vsync_enabled_;
	size_t vc_mem_;
	char vc_desc_[128];
	IDXGISwapChain* swapchain;
	ID3D11Device* device;
	ID3D11DeviceContext* devicecontext;
	ID3D11RenderTargetView* rendertargetview;
	ID3D11Texture2D* depthstencilbuffer;
	ID3D11DepthStencilState* depthstencilstate;
	ID3D11DepthStencilView* depthstencilview;
	ID3D11RasterizerState* rasterstate;
  ID3D11PixelShader* ps;
  ID3D11PixelShader* ps_tex;
  ID3D11VertexShader* vs;
  ID3D11InputLayout* ia;
  struct {
	  XMMATRIX world;
	  XMMATRIX view;
	  XMMATRIX projection;
  } matrixBufferData;
  ID3D11Buffer* matrixBuffer;
	ID3D11DepthStencilState* depthdisabledstencilstate;
  int CreateShaders();
};

}

#endif