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
#include "global.h"

//#define GPU_DEBUG

namespace emulation {
namespace psx {


inline uint32_t SelectBits(uint32_t data,uint32_t bitno,uint32_t size) {
  return (data>>bitno) & ((1<<size)-1);
}

const uint8_t kPrimitiveSize[256]=
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

GpuMiniVE::Primitive GpuMiniVE::primitives[256] =
{
//0x00
&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,
&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,
&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,
&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,
//0x10
&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,
&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,
&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,
&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,
//0x20
&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,
&GpuMiniVE::PrimitivePolyFT3,&GpuMiniVE::PrimitivePolyFT3,&GpuMiniVE::PrimitivePolyFT3,&GpuMiniVE::PrimitivePolyFT3,
&GpuMiniVE::PrimitivePolyF4,&GpuMiniVE::PrimitivePolyF4,&GpuMiniVE::PrimitivePolyF4,&GpuMiniVE::PrimitivePolyF4,
&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,
//0x30
&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,
&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,
&GpuMiniVE::PrimitivePolyG4,&GpuMiniVE::PrimitivePolyG4,&GpuMiniVE::PrimitivePolyG4,&GpuMiniVE::PrimitivePolyG4,
&GpuMiniVE::PrimitivePolyGT4,&GpuMiniVE::PrimitivePolyGT4,&GpuMiniVE::PrimitivePolyGT4,&GpuMiniVE::PrimitivePolyGT4,
//0x40
&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,
&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,
&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,
&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,
//0x50
&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,
&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,
&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,
&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,
//0x60
&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,
&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,
&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,
&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,
//0x70
&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,
&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,
&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,
&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,
//0x80
&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,
&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,
&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,
&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,
//0x90
&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,
&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,
&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,
&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,
//0xa0
&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,
&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,
&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,
&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,
//0xb0
&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,
&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,
&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,
&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,
//0xc0
&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,
&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,
&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,
&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,
//0xd0
&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,
&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,
&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,
&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,
//0xe0
&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveDrawModeSetting,&GpuMiniVE::PrimitiveTextureWindow,&GpuMiniVE::PrimitiveClipAreaStart,
&GpuMiniVE::PrimitiveClipAreaEnd,&GpuMiniVE::PrimitiveDrawOffset,&GpuMiniVE::PrimitiveMaskSetting,&GpuMiniVE::PrimitiveUnknown,
&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,
&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,
//0xf0
&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,
&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,
&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,
&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown,&GpuMiniVE::PrimitiveUnknown
};


GpuMiniVE::GpuMiniVE():GpuCore(),gfx(nullptr) {
  
}

GpuMiniVE::~GpuMiniVE() {

 
}



int GpuMiniVE::Initialize() {
  Deinitialize();
  gfx = new minive::D3D11Context();
  gfx->Initialize(640,480,false,handle_,false,1,0);
 
  data = 0;
  status.raw = 0x14802000;
  memset(&command_buffer,0,sizeof(command_buffer));
  memset(&drawing,0,sizeof(drawing));
  return 0;
}

int GpuMiniVE::Deinitialize() {
  if (gfx)
    gfx->Deinitialize();
  SafeDelete(&gfx);
  return 0;
}

int GpuMiniVE::Render() {
  static int counter = 0;
  gfx->Clear();
  auto result = gfx->Present();
  auto& timing = system_->timing();
  ++timing.fps_counter;

  if (timing.fps_time_span >= 1000.0) {
    timing.fps = timing.fps_counter * (1000.0/timing.fps_time_span);
    timing.fps_counter = 0;
    timing.fps_time_span = 0;
    char caption[256];
    //sprintf(caption,"Freq : %0.2f MHz",nes.frequency_mhz());
    //sprintf(caption,"CPS: %llu ",nes.cycles_per_second());
    sprintf_s(caption,"FPS: %02.3f - %d",timing.fps,counter++);
    SetWindowText(handle(),caption);
  }
  return result;
}


uint32_t GpuMiniVE::ReadData() {
  if (status.dmadir==0x3) {
    BREAKPOINT
  }
  if (status.dmadir==0x0) {
    return data;
  }
  BREAKPOINT
  return 0;
}

uint32_t GpuMiniVE::ReadStatus() {
  return status.raw;
}


void GpuMiniVE::WriteData(uint32_t data) {
  status.busy = 0;
  status.com = 0;
  

  if (status.dmadir==0x2) {
    if ((system_->io().dma.channel(2).chcr & 0x401) == 0x401) {
      FillCommandBuffer(data);
    } else {
      BREAKPOINT
    }
  }
  if (status.dmadir==0x0) {
    FillCommandBuffer(data);
  }

  status.com = 1;
  status.busy = 1;
  return;
}


void GpuMiniVE::WriteStatus(uint32_t data) {
  uint16_t command = (data & 0xFF000000) >> 24;
  uint32_t params = (data & 0x00FFFFFF);
  switch (command) {
    case 0x00:
      memset(&command_buffer,0,sizeof(command_buffer));
      status.raw = 0x14802000;
      break;
    case 0x01:
      memset(&command_buffer,0,sizeof(command_buffer));
      break;
    case 0x02:
      status.irq1 = 0;
      break;
    case 0x03:
      status.den = !(data & 0x1);
      break;
    case 0x04:
      status.dmadir = data & 0x3;
      break;
    case 0x05: {
      int x = data&0x3FF;
      int y = (data&0x7FC00)>>10;
      break;
      }
    case 0x06: {
      int x = data&0xFFF;
      int y = (data&0xFFF000)>>12;
      break;
      }
    case 0x07: {
      int x = data&0xFFF;
      int y = (data&0xFFF000)>>12;
      break;
      }
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
    case 0x09:
      status.texdisable = data&0x1;
      break;
    default:
      BREAKPOINT
  }
  return;
}

void GpuMiniVE::FillCommandBuffer(uint32_t data) {
  this->data = data;
  if (command_buffer.param_count == 0) {
    
    command_buffer.command = (data & 0xFF000000) >> 24;
    data = (data & 0x00FFFFFF);
    if (kPrimitiveSize[command_buffer.command] == 0)
      return;

    #if defined(GPU_DEBUG) && defined(_DEBUG)
    char str[255];
    sprintf(str,",,gpu command begin,0x%x,size,%d\n",command_buffer.command,primitive_size[command_buffer.command]);
    fprintf(system_->csvlog.fp,str);
    #endif
  } 
  command_buffer.buffer[command_buffer.param_count++] = data;
  if (command_buffer.param_count == kPrimitiveSize[command_buffer.command]) {
    #if defined(GPU_DEBUG) && defined(_DEBUG)
    char str[255];
    sprintf(str,",,gpu command end,0x%x,param_count,%d\n",command_buffer.command,command_buffer.param_count);
    fprintf(system_->csvlog.fp,str);
    #endif
    (this->*(primitives[command_buffer.command]))();
    command_buffer.param_count = 0;
  }
}

void GpuMiniVE::UpdateGSSize() {
  int width_table[] = {256,384,320,0,512,0,640};
  int height = 240 << status.height;
  //gs->gfx->window()->SetClientSize(width_table[status.width],height);
  //gs->camera.Ortho2D(0,0,(FLOAT)width_table[status.width],(FLOAT)height);

 // auto world = XMMatrixTranslation((FLOAT)drawing.offset_x,(FLOAT)drawing.offset_y,0);
  //D3DXMATRIX matIdentity;
 // gs->gfx->device()->SetTransform(D3DTS_WORLD,(D3DXMATRIX*)&world);

  //gs->gfx->SetCamera(&gs->camera);
  //gs->gfx->SetViewport((float)-drawing.clip_x,(float)-drawing.clip_y,(float)drawing.clip_w-drawing.clip_x,(float)drawing.clip_h-drawing.clip_y,0.0f,1.0f);
}

void GpuMiniVE::PrimitiveUnknown() {
  BREAKPOINT
}


void GpuMiniVE::PrimitivePolyFT3() {
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

void GpuMiniVE::PrimitivePolyF4() {
 PolyF4 poly;
 //G2DPoly4 destpoly;
 memcpy(&poly,command_buffer.buffer,sizeof(PolyF4));
 
 uint32_t color = 0xff000000|poly.color;

   emulation::psx::GfxVertex v[4] = {
      {(float)poly.x0,(float)poly.y0,0,color,0.0f,0.0f},
      {(float)poly.x1,(float)poly.y1,0,color,0.0f,0.0f},
      {(float)poly.x2,(float)poly.y2,0,color,0.0f,0.0f},
      {(float)poly.x3,(float)poly.y3,0,color,0.0f,0.0f}
    };
    
    
    //gs->gfx->device()->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,v,sizeof(emulation::psx::GfxVertex));


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

void GpuMiniVE::PrimitivePolyG4() {
 PolyG4 poly;
 //G2DPoly4 destpoly;
 memcpy(&poly,command_buffer.buffer,sizeof(PolyG4));


   emulation::psx::GfxVertex v[4] = {
      {(float)poly.x0,(float)poly.y0,0,0xff000000|poly.color0,0.0f,0.0f},
      {(float)poly.x1,(float)poly.y1,0,0xff000000|poly.color1,0.0f,0.0f},
      {(float)poly.x2,(float)poly.y2,0,0xff000000|poly.color2,0.0f,0.0f},
      {(float)poly.x3,(float)poly.y3,0,0xff000000|poly.color3,0.0f,0.0f}
    };
    
    
   // gs->gfx->device()->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,v,sizeof(emulation::psx::GfxVertex));


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


void GpuMiniVE::PrimitivePolyGT4() {
 PolyGT4 poly;
 //G2DPoly4 destpoly;
 unsigned long tp;

 memcpy(&poly,command_buffer.buffer,sizeof(PolyGT4));
 tp=poly.tpage;


   emulation::psx::GfxVertex v[4] = {
      {(float)poly.x0,(float)poly.y0,0,0xff000000|poly.color0,0,0},
      {(float)poly.x1,(float)poly.y1,0,0xff000000|poly.color1,0,0},
      {(float)poly.x2,(float)poly.y2,0,0xff000000|poly.color2,0,0},
      {(float)poly.x3,(float)poly.y3,0,0xff000000|poly.color3,0,0}
    };
    
    
    //gs->gfx->device()->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,v,sizeof(emulation::psx::GfxVertex));


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

void GpuMiniVE::PrimitiveDrawModeSetting() {
 status.raw &= 0xfffff800;
 status.raw |= command_buffer.buffer[0]&0x7ff;
}

void GpuMiniVE::PrimitiveTextureWindow() {
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

void GpuMiniVE::PrimitiveClipAreaStart() {
  drawing.clip_x = (uint16_t)(command_buffer.buffer[0]&0x3ff);
  drawing.clip_y = (uint16_t)((command_buffer.buffer[0]&0xffc00)>>10);
  UpdateGSSize();
}

void GpuMiniVE::PrimitiveClipAreaEnd() {
  drawing.clip_w = (uint16_t)((command_buffer.buffer[0]&0x3ff));
  drawing.clip_h = (uint16_t)((command_buffer.buffer[0]&0xffc00)>>10);
  UpdateGSSize();
}

void GpuMiniVE::PrimitiveDrawOffset() {
  drawing.offset_x = (uint16_t)(command_buffer.buffer[0] & 0x7ff);
  drawing.offset_y = (uint16_t)((command_buffer.buffer[0]&0x3ff800) >>11);
  UpdateGSSize();
}

void GpuMiniVE::PrimitiveMaskSetting() {
 //Gpu->Mask1=GpuPrimeBuf[0]&0x1;
 //Gpu->Mask2=GpuPrimeBuf[0]&0x2;
}


}
}