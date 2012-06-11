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
#include <VisualEssence/Code/ve.h>
#include "../system.h"

#define GPU_DEBUG

namespace emulation {
namespace psx {


inline uint32_t SelectBits(uint32_t data,uint32_t bitno,uint32_t size) {
  return (data>>bitno) & ((1<<size)-1);
}

struct GfxStruct {
  graphics::ContextD3D9* gfx;
  graphics::Camera camera;
  graphics::InputLayout input_layout;
};

uint8_t primitive_size[256]=
{
//0x00
0x00,0x01,0x03,0x00,
0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,
//0x10
0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,
//0x20
0x04,0x04,0x04,0x04,
0x07,0x07,0x07,0x07,
0x05,0x05,0x05,0x05,
0x09,0x09,0x09,0x09,
//0x30
0x06,0x06,0x06,0x06,
0x09,0x09,0x09,0x09,
0x08,0x08,0x08,0x08,
0x0c,0x0c,0x0c,0x0c,
//0x40
0x03,0x03,0x03,0x03,
0x00,0x00,0x00,0x00,
0x05,0x05,0x05,0x05,
0x06,0x06,0x06,0x06,
//0x50
0x04,0x04,0x04,0x04,
0x00,0x00,0x00,0x00,
0x07,0x07,0x07,0x07,
0x09,0x09,0x09,0x09,
//0x60
0x03,0x03,0x03,0x03,
0x04,0x04,0x04,0x04,
0x02,0x02,0x02,0x02,
0x00,0x00,0x00,0x00,
//0x70
0x02,0x02,0x02,0x02,
0x03,0x03,0x03,0x03,
0x02,0x02,0x02,0x02,
0x03,0x03,0x03,0x03,
//0x80
0x04,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,
//0x90
0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,
//0xa0
0x03,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,
//0xb0
0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,
//0xc0
0x03,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,
//0xd0
0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,
//0xe0
0x00,0x01,0x01,0x01,
0x01,0x01,0x01,0x00,
0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,
//0xf0
0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00,
0x00,0x00,0x00,0x00
};

Gpu::Primitive Gpu::primitives[256] =
{
//0x00
&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,
&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,
&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,
&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,
//0x10
&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,
&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,
&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,
&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,
//0x20
&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,
&Gpu::PrimitivePolyFT3,&Gpu::PrimitivePolyFT3,&Gpu::PrimitivePolyFT3,&Gpu::PrimitivePolyFT3,
&Gpu::PrimitivePolyF4,&Gpu::PrimitivePolyF4,&Gpu::PrimitivePolyF4,&Gpu::PrimitivePolyF4,
&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,
//0x30
&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,
&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,
&Gpu::PrimitivePolyG4,&Gpu::PrimitivePolyG4,&Gpu::PrimitivePolyG4,&Gpu::PrimitivePolyG4,
&Gpu::PrimitivePolyGT4,&Gpu::PrimitivePolyGT4,&Gpu::PrimitivePolyGT4,&Gpu::PrimitivePolyGT4,
//0x40
&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,
&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,
&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,
&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,
//0x50
&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,
&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,
&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,
&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,
//0x60
&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,
&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,
&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,
&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,
//0x70
&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,
&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,
&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,
&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,
//0x80
&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,
&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,
&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,
&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,
//0x90
&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,
&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,
&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,
&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,
//0xa0
&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,
&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,
&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,
&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,
//0xb0
&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,
&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,
&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,
&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,
//0xc0
&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,
&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,
&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,
&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,
//0xd0
&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,
&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,
&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,
&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,
//0xe0
&Gpu::PrimitiveUnknown,&Gpu::PrimitiveDrawModeSetting,&Gpu::PrimitiveTextureWindow,&Gpu::PrimitiveClipAreaStart,
&Gpu::PrimitiveClipAreaEnd,&Gpu::PrimitiveDrawOffset,&Gpu::PrimitiveMaskSetting,&Gpu::PrimitiveUnknown,
&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,
&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,
//0xf0
&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,
&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,
&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,
&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown,&Gpu::PrimitiveUnknown
};

IDirect3DTexture9* vram_texture=0;
IDirect3DSurface9* surface=0;

Gpu::Gpu() {
  gs = new GfxStruct();
}

Gpu::~Gpu() {
  vram_texture->Release();
  surface->Release();
  delete gs;
}

void Gpu::set_gfx(graphics::Context* gfx) {
  this->gs->gfx = (graphics::ContextD3D9*)gfx;
}

int Gpu::Initialize() {
  graphics::InputElement gielements[] = 
  {
      {0, 0 , D3DDECLTYPE_FLOAT3  , D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
      {0, 12, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR   , 0},
      {0, 16, D3DDECLTYPE_FLOAT2  , D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
      D3DDECL_END()
  };

  gs->gfx->CreateInputLayout(gielements,gs->input_layout);
  gs->gfx->SetInputLayout(gs->input_layout);
  gs->camera.Initialize(gs->gfx);
  gs->camera.Ortho2D();
  gs->gfx->SetCamera(&gs->camera);
  
  D3DXMATRIX matIdentity;
	D3DXMatrixIdentity(&matIdentity);
  gs->gfx->device()->SetTransform(D3DTS_WORLD,&matIdentity);
	gs->gfx->device()->SetRenderState(D3DRS_ZENABLE, 0 );
	gs->gfx->device()->SetRenderState(D3DRS_LIGHTING, 0 );
	gs->gfx->device()->SetRenderState(D3DRS_ALPHABLENDENABLE,1);


	gs->gfx->device()->SetTextureStageState( 0, D3DTSS_COLORARG1, D3DTA_TEXTURE );
	gs->gfx->device()->SetTextureStageState( 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE );  
	gs->gfx->device()->SetTextureStageState( 0, D3DTSS_COLOROP, D3DTOP_MODULATE );
	gs->gfx->device()->SetTextureStageState(0,D3DTSS_ALPHAARG1,D3DTA_TEXTURE);
	gs->gfx->device()->SetTextureStageState(0,D3DTSS_ALPHAARG2,D3DTA_DIFFUSE);
	gs->gfx->device()->SetTextureStageState( 0, D3DTSS_ALPHAOP,D3DTOP_MODULATE );
  
  gs->gfx->device()->CreateTexture(1024,512,1,D3DUSAGE_RENDERTARGET,D3DFMT_A8R8G8B8,D3DPOOL_DEFAULT,&vram_texture,0);
  
  vram_texture->GetSurfaceLevel(0,&surface);
  //auto r = gs->gfx->device()->SetRenderTarget(0,surface);

  gs->gfx->ClearTarget();
	//G->DefaultLockFlag=D3DLOCK_DISCARD;
	//G->DefaultPool=D3DPOOL_MANAGED;
	//G->DefaultMipmapLevels=0;
	gs->gfx->Begin();

  data = 0;
  status.raw = 0x14802000;
  memset(&command_buffer,0,sizeof(command_buffer));
  memset(&drawing,0,sizeof(drawing));
  return 0;
}

int Gpu::Deinitialize() {
  gs->gfx->DestoryInputLayout(gs->input_layout);
  return 0;
}

uint32_t Gpu::ReadData() {
  if (status.dma==0x3) {
    BREAKPOINT
  }
  if (status.dma==0x0) {
    return data;
  }
  BREAKPOINT
  return 0;
}

uint32_t Gpu::ReadStatus() {
  return status.raw;
}


void Gpu::WriteData(uint32_t data) {
  status.busy = 0;
  status.com = 0;
  

  if (status.dma==0x2) {
    if ((system_->io().dma.channel(2).chcr & 0x401) == 0x401) {
      FillCommandBuffer(data);
    } else {
      BREAKPOINT
    }
  }
  if (status.dma==0x0) {
    FillCommandBuffer(data);
  }

  status.com = 1;
  status.busy = 1;
  return;
}


void Gpu::WriteStatus(uint32_t data) {
  uint16_t command = (data & 0xFF000000) >> 24;
  uint32_t params = (data & 0x00FFFFFF);
  switch (command) {
    case 0x00:
      status.raw = 0x14802000;
      status.den = 1;
      memset(&command_buffer,0,sizeof(command_buffer));
      break;
    case 0x01:
      memset(&command_buffer,0,sizeof(command_buffer));
      break;
    case 0x02:
      BREAKPOINT
      break;
    case 0x03:
      status.den = !(data & 0x1);
      break;
    case 0x04:
      status.dma = data & 0x3;
      break;
    case 0x08: {
	    int width0 = SelectBits(data,0,2);
      int width1 = SelectBits(data,6,1);
      status.width = (width0 << 1) | width1;
      status.height = SelectBits(data,2,1);
      status.video =  SelectBits(data,3,1);
      status.isrgb24 =  SelectBits(data,4,1);
      status.isinter =  SelectBits(data,5,1);
      UpdateGSSize();
      }
      break;
  }
  return;
}

void Gpu::FillCommandBuffer(uint32_t data) {
  this->data = data;
  if (command_buffer.param_count == 0) {
    
    command_buffer.command = (data & 0xFF000000) >> 24;
    data = (data & 0x00FFFFFF);
    if (primitive_size[command_buffer.command] == 0)
      return;

    #if defined(GPU_DEBUG) && defined(_DEBUG)
    char str[255];
    sprintf(str,",,gpu command begin,0x%x,size,%d\n",command_buffer.command,primitive_size[command_buffer.command]);
    fprintf(system_->csvlog.fp,str);
    #endif
  } 
  command_buffer.buffer[command_buffer.param_count++] = data;
  if (command_buffer.param_count == primitive_size[command_buffer.command]) {
    #if defined(GPU_DEBUG) && defined(_DEBUG)
    char str[255];
    sprintf(str,",,gpu command end,0x%x,param_count,%d\n",command_buffer.command,command_buffer.param_count);
    fprintf(system_->csvlog.fp,str);
    #endif
    (this->*(primitives[command_buffer.command]))();
    command_buffer.param_count = 0;
  }
}

void Gpu::UpdateGSSize() {
  int width_table[] = {256,384,320,0,512,0,640};
  int height = 240 << status.height;
  gs->gfx->window()->SetClientSize(width_table[status.width],height);
  gs->camera.Ortho2D(0,0,(FLOAT)width_table[status.width],(FLOAT)height);

  auto world = XMMatrixTranslation((FLOAT)drawing.offset_x,(FLOAT)drawing.offset_y,0);
  //D3DXMATRIX matIdentity;
  gs->gfx->device()->SetTransform(D3DTS_WORLD,(D3DXMATRIX*)&world);

  gs->gfx->SetCamera(&gs->camera);
  gs->gfx->SetViewport((float)-drawing.clip_x,(float)-drawing.clip_y,(float)drawing.clip_w-drawing.clip_x,(float)drawing.clip_h-drawing.clip_y,0.0f,1.0f);
}

void Gpu::PrimitiveUnknown() {
  BREAKPOINT
}


void Gpu::PrimitivePolyFT3() {
 PolyFT3 poly;
 //G2DPoly3 destpoly;
 memcpy(&poly,command_buffer.buffer,sizeof(PolyFT3));
 //_cprintf("PolyFT3:%d %d\n",poly.clut,poly.tpage);
 
 /*poly.color=swap_color(poly.color);

memset(&destpoly,0,sizeof(destpoly));
 destpoly.v[0].c=0xff000000|poly.color&0xffffff;
 destpoly.v[1].c=destpoly.v[2].c=destpoly.v[0].c;
 
 destpoly.v[0].x=poly.x0;
 destpoly.v[0].y=poly.y0;
 destpoly.v[0].u=poly.u0*0.00390625f;
 destpoly.v[0].v=poly.v0*0.00390625f;
 destpoly.v[1].x=poly.x1;
 destpoly.v[1].y=poly.y1;
 destpoly.v[1].u=poly.u1*0.00390625f;
 destpoly.v[1].v=poly.v1*0.00390625f;
 destpoly.v[2].x=poly.x2;
 destpoly.v[2].y=poly.y2;
 destpoly.v[2].u=poly.u2*0.00390625f;
 destpoly.v[2].v=poly.v2*0.00390625f;
 //SetTransMode(poly.c);
 unsigned long tp;
 tp=poly.tpage;
  destpoly.tex=Gpu->LoadTexture(tp,poly.clut,poly.color);
 Gpu->GP->GP2DPoly3(&destpoly);*/
}

void Gpu::PrimitivePolyF4() {
 PolyF4 poly;
 //G2DPoly4 destpoly;
 memcpy(&poly,command_buffer.buffer,sizeof(PolyF4));
 
 uint32_t color = 0xff000000|poly.color;

   emulation::psx::GfxVertex v[4] = {
     {poly.x0,poly.y0,0,color,0,0},
      {poly.x1,poly.y1,0,color,0,0},
      {poly.x2,poly.y2,0,color,0,0},
      {poly.x3,poly.y3,0,color,0,0}
    };
    
    
    gs->gfx->device()->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,v,sizeof(emulation::psx::GfxVertex));


 /*memset(&destpoly,0,sizeof(destpoly));
 destpoly.v[0].c=0xff000000|color&0xffffff;
 destpoly.v[1].c=destpoly.v[2].c=destpoly.v[3].c=destpoly.v[0].c;
 destpoly.v[0].x=poly.x0;
 destpoly.v[0].y=poly.y0;
 destpoly.v[1].x=poly.x1;
 destpoly.v[1].y=poly.y1;
 destpoly.v[2].x=poly.x2;
 destpoly.v[2].y=poly.y2;
 destpoly.v[3].x=poly.x3;
 destpoly.v[3].y=poly.y3;
 destpoly.tex=0;


 Gpu->GP->GP2DPoly4(&destpoly);*/
}

void Gpu::PrimitivePolyG4() {
 PolyG4 poly;
 //G2DPoly4 destpoly;
 memcpy(&poly,command_buffer.buffer,sizeof(PolyG4));


   emulation::psx::GfxVertex v[4] = {
     {poly.x0,poly.y0,0,0xff000000|poly.color0,0,0},
      {poly.x1,poly.y1,0,0xff000000|poly.color1,0,0},
      {poly.x2,poly.y2,0,0xff000000|poly.color2,0,0},
      {poly.x3,poly.y3,0,0xff000000|poly.color3,0,0}
    };
    
    
    gs->gfx->device()->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,v,sizeof(emulation::psx::GfxVertex));


 /*memset(&destpoly,0,sizeof(destpoly));
 destpoly.v[0].c=0xff000000|(swap_color(poly.color0)&0xffffff);
 destpoly.v[1].c=0xff000000|(swap_color(poly.color1)&0xffffff);
 destpoly.v[2].c=0xff000000|(swap_color(poly.color2)&0xffffff);
 destpoly.v[3].c=0xff000000|(swap_color(poly.color3)&0xffffff);
 destpoly.v[0].x=poly.x0;
 destpoly.v[0].y=poly.y0;
 destpoly.v[1].x=poly.x1;
 destpoly.v[1].y=poly.y1;
 destpoly.v[2].x=poly.x2;
 destpoly.v[2].y=poly.y2;
 destpoly.v[3].x=poly.x3;
 destpoly.v[3].y=poly.y3;
destpoly.tex=0;
 Gpu->GP->GP2DPoly4(&destpoly);*/
}


void Gpu::PrimitivePolyGT4() {
 PolyGT4 poly;
 //G2DPoly4 destpoly;
 unsigned long tp;

 memcpy(&poly,command_buffer.buffer,sizeof(PolyGT4));
 tp=poly.tpage;


   emulation::psx::GfxVertex v[4] = {
     {poly.x0,poly.y0,0,0xff000000|poly.color0,0,0},
      {poly.x1,poly.y1,0,0xff000000|poly.color1,0,0},
      {poly.x2,poly.y2,0,0xff000000|poly.color2,0,0},
      {poly.x3,poly.y3,0,0xff000000|poly.color3,0,0}
    };
    
    
    gs->gfx->device()->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,v,sizeof(emulation::psx::GfxVertex));


 /*
 //U32Bit color = swap_color(poly.color0);
 memset(&destpoly,0,sizeof(destpoly));
 destpoly.v[0].c=Gpu->SetTransMode(tp,swap_color(poly.color0));
 destpoly.v[1].c=Gpu->SetTransMode(tp,swap_color(poly.color1));
 destpoly.v[2].c=Gpu->SetTransMode(tp,swap_color(poly.color2));
 destpoly.v[3].c=Gpu->SetTransMode(tp,swap_color(poly.color3));
 destpoly.v[0].x=poly.x0;
 destpoly.v[0].y=poly.y0;
 destpoly.v[0].u=poly.u0*0.00390625f;
 destpoly.v[0].v=poly.v0*0.00390625f;
 destpoly.v[1].x=poly.x1;
 destpoly.v[1].y=poly.y1;
 destpoly.v[1].u=poly.u1*0.00390625f;
 destpoly.v[1].v=poly.v1*0.00390625f;
 destpoly.v[2].x=poly.x2;
 destpoly.v[2].y=poly.y2;
 destpoly.v[2].u=poly.u2*0.00390625f;
 destpoly.v[2].v=poly.v2*0.00390625f;
 destpoly.v[3].x=poly.x3;
 destpoly.v[3].y=poly.y3;
 destpoly.v[3].u=poly.u3*0.00390625f;
 destpoly.v[3].v=poly.v3*0.00390625f;


 destpoly.tex=Gpu->LoadTexture(tp,poly.clut,poly.color0);
 Gpu->GP->GP2DPoly4(&destpoly);*/
}

void Gpu::PrimitiveDrawModeSetting() {
 status.raw &= 0xfffff800;
 status.raw |= command_buffer.buffer[0]&0x7ff;
}

void Gpu::PrimitiveTextureWindow() {
/*#define _get_tw(tw)	
		(tw ? ((0xe2000000)|
	((((tw)->y&0xff)>>3)<<15)| 
		((((tw)->x&0xff)>>3)<<10)|
		(((~((tw)->h-1)&0xff)>>3)<<5)| 
		(((~((tw)->w-1)&0xff)>>3))) : 0)
*/
	int x,y,w,h;
	x = ((command_buffer.buffer[1]>>10)<<3)&0xff;
	y = ((command_buffer.buffer[1]>>15)<<3)&0xff;
	w = ((command_buffer.buffer[1])<<3)&0xff;
	h = ((command_buffer.buffer[1]>>5)<<3)&0xff;
	
	//_cprintf("tex win %d %d %d %d %x %x\n",x,y,w,h,GpuPrimeBuf[0],GpuPrimeBuf[1]);
}

void Gpu::PrimitiveClipAreaStart() {
  drawing.clip_x = (uint16_t)(command_buffer.buffer[0]&0x3ff);
  drawing.clip_y = (uint16_t)((command_buffer.buffer[0]&0xffc00)>>10);
  UpdateGSSize();
}

void Gpu::PrimitiveClipAreaEnd() {
  drawing.clip_w = (uint16_t)((command_buffer.buffer[0]&0x3ff));
  drawing.clip_h = (uint16_t)((command_buffer.buffer[0]&0xffc00)>>10);
  UpdateGSSize();
}

void Gpu::PrimitiveDrawOffset() {
  drawing.offset_x = (uint16_t)(command_buffer.buffer[0] & 0x7ff);
  drawing.offset_y = (uint16_t)((command_buffer.buffer[0]&0x3ff800) >>11);
  UpdateGSSize();
}

void Gpu::PrimitiveMaskSetting() {
 //Gpu->Mask1=GpuPrimeBuf[0]&0x1;
 //Gpu->Mask2=GpuPrimeBuf[0]&0x2;
}


}
}