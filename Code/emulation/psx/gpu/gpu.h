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
#ifndef EMULATION_PSX_GPU_H
#define EMULATION_PSX_GPU_H


#include "../../../minive/minive.h"

namespace emulation {
namespace psx {

struct GfxStruct;

struct GfxVertex {
  float x,y,z;
  uint32_t color;
  float u,v;
};

typedef struct
{
 uint32_t color;
 uint16_t x0,y0;
 uint16_t x1,y1;
 uint16_t x2,y2;
}PolyF3;

typedef struct
{
 uint32_t color;
 uint16_t x0,y0;
 uint16_t x1,y1;
 uint16_t x2,y2;
 uint16_t x3,y3;
}PolyF4;

typedef struct
{
 uint32_t color;
 uint16_t x0,y0;
 uint8_t  u0,v0;
 uint16_t clut;
 uint16_t x1,y1;
 uint8_t  u1,v1;
 uint16_t tpage;
 uint16_t x2,y2;
 uint8_t  u2,v2;
 uint16_t pad2;
}PolyFT3;

typedef struct
{
 uint32_t color;
 uint16_t x0,y0;
 uint8_t  u0,v0;
 uint16_t clut;
 uint16_t x1,y1;
 uint8_t  u1,v1;
 uint16_t tpage;
 uint16_t x2,y2;
 uint8_t  u2,v2;
 uint16_t pad1;
 uint16_t x3,y3;
 uint8_t  u3,v3;
 uint16_t pad2;
}PolyFT4;


typedef struct
{
 uint32_t color0;
 uint16_t x0,y0;
 uint32_t color1;
 uint16_t x1,y1;
 uint32_t color2;
 uint16_t x2,y2;
}PolyG3;


typedef struct
{
 uint32_t color0;
 uint16_t x0,y0;
 uint32_t color1;
 uint16_t x1,y1;
 uint32_t color2;
 uint16_t x2,y2;
 uint32_t color3;
 uint16_t x3,y3;
}PolyG4;


typedef struct
{
 uint32_t color0;
 uint16_t x0,y0;
 uint8_t  u0,v0;
 uint16_t clut;
 uint32_t color1;
 uint16_t x1,y1;
 uint8_t  u1,v1;
 uint16_t tpage;
 uint32_t color2;
 uint16_t x2,y2;
 uint8_t  u2,v2;
 uint16_t pad2;
 uint32_t color3;
 uint16_t x3,y3;
 uint8_t  u3,v3;
 uint16_t pad3;
}PolyGT4;


typedef struct
{
  uint32_t color;
  uint16_t x0,y0;
  uint16_t x1,y1;
}LineF2;

typedef struct
{
  uint32_t color0;
  uint16_t x0,y0;
  uint32_t color1;
  uint16_t x1,y1;
}LineG2;

typedef	struct
{
  uint32_t  color; 
  uint16_t x,y;
  uint16_t w,h;
}Rect;


typedef	struct
{
  uint32_t  color; 
  uint16_t x,y;
  uint8_t  u,v;
  uint16_t clut;
  uint16_t w,h;
}Sprite;

typedef struct
{
 uint32_t color;
 uint16_t x,y;
 uint8_t  u,v;
 uint16_t clut;
}Sprite8;

typedef struct
{
 uint32_t color;
 uint16_t x,y;
}Tile16;

class Gpu : public Component {
 friend Dma;
 public:
  typedef void (Gpu::*Primitive)();
  Gpu();
  ~Gpu();
  void set_gfx(minive::Context* gfx);
  int Initialize();
  int Deinitialize();
  uint32_t  ReadData();
  uint32_t  ReadStatus();
  void WriteData(uint32_t data);
  void WriteStatus(uint32_t data);
  void FillCommandBuffer(uint32_t data);
 private:
  static Primitive primitives[256];
  union {
    struct {
      uint32_t tx:4;
      uint32_t ty:1;
      uint32_t abr:2;
      uint32_t tp:2;
      uint32_t dtd:1;
      uint32_t dfe:1;
      uint32_t md:1;
      uint32_t me:1;
      uint32_t reserved:1;
      uint32_t revflag:1;
      uint32_t texdisable:1;
      uint32_t width:3;
      uint32_t height:1;
      uint32_t video:1;
      uint32_t isrgb24:1;
      uint32_t isinter:1;
      uint32_t den:1;
      uint32_t irq1:1;
      uint32_t dmareq:1;
      uint32_t busy:1;
      uint32_t img:1;
      uint32_t com:1;
      uint32_t dmadir:2;
      uint32_t lcf:1;
    };
    uint32_t raw;
  }status;
  uint32_t data;
  struct {
    uint8_t command;
    int param_count;
    uint32_t buffer[0xFF];
  }command_buffer;
  struct {
    uint16_t clip_x;
    uint16_t clip_y;
    uint16_t clip_w;
    uint16_t clip_h;
    uint16_t offset_x;
    uint16_t offset_y;
  }drawing;
  GfxStruct* gs;
  void UpdateGSSize();
  void PrimitiveUnknown();
  void PrimitivePolyFT3();
  void PrimitivePolyF4();
  void PrimitivePolyG4();
  void PrimitivePolyGT4();
  void PrimitiveDrawModeSetting();
  void PrimitiveTextureWindow();
  void PrimitiveClipAreaStart();
  void PrimitiveClipAreaEnd();
  void PrimitiveDrawOffset();
  void PrimitiveMaskSetting();
};

}
}

#endif