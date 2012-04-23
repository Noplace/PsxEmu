namespace emulation {
namespace psx {

typedef struct
{
 unsigned long color;
 unsigned short x0,y0;
 unsigned short x1,y1;
 unsigned short x2,y2;
}PolyF3;

typedef struct
{
 unsigned long color;
 unsigned short x0,y0;
 unsigned short x1,y1;
 unsigned short x2,y2;
 unsigned short x3,y3;
}PolyF4;

typedef struct
{
 unsigned long color;
 unsigned short x0,y0;
 unsigned char  u0,v0;
 unsigned short clut;
 unsigned short x1,y1;
 unsigned char  u1,v1;
 unsigned short tpage;
 unsigned short x2,y2;
 unsigned char  u2,v2;
 unsigned short pad2;
}PolyFT3;

typedef struct
{
 unsigned long color;
 unsigned short x0,y0;
 unsigned char  u0,v0;
 unsigned short clut;
 unsigned short x1,y1;
 unsigned char  u1,v1;
 unsigned short tpage;
 unsigned short x2,y2;
 unsigned char  u2,v2;
 unsigned short pad1;
 unsigned short x3,y3;
 unsigned char  u3,v3;
 unsigned short pad2;
}PolyFT4;


typedef struct
{
 unsigned long color0;
 unsigned short x0,y0;
 unsigned long color1;
 unsigned short x1,y1;
 unsigned long color2;
 unsigned short x2,y2;
}PolyG3;


typedef struct
{
 unsigned long color0;
 unsigned short x0,y0;
 unsigned long color1;
 unsigned short x1,y1;
 unsigned long color2;
 unsigned short x2,y2;
 unsigned long color3;
 unsigned short x3,y3;
}PolyG4;


typedef struct
{
 unsigned long color0;
 unsigned short x0,y0;
 unsigned char  u0,v0;
 unsigned short clut;
 unsigned long color1;
 unsigned short x1,y1;
 unsigned char  u1,v1;
 unsigned short tpage;
 unsigned long color2;
 unsigned short x2,y2;
 unsigned char  u2,v2;
 unsigned short pad2;
 unsigned long color3;
 unsigned short x3,y3;
 unsigned char  u3,v3;
 unsigned short pad3;
}PolyGT4;


typedef struct
{
  unsigned long color;
  unsigned short x0,y0;
  unsigned short x1,y1;
}LineF2;

typedef struct
{
  unsigned long color0;
  unsigned short x0,y0;
  unsigned long color1;
  unsigned short x1,y1;
}LineG2;

typedef	struct
{
  unsigned long  color; 
  unsigned short x,y;
  unsigned short w,h;
}Rect;


typedef	struct
{
  unsigned long  color; 
  unsigned short x,y;
  unsigned char  u,v;
  unsigned short clut;
  unsigned short w,h;
}Sprite;

typedef struct
{
 unsigned long color;
 unsigned short x,y;
 unsigned char  u,v;
 unsigned short clut;
}Sprite8;

typedef struct
{
 unsigned long color;
 unsigned short x,y;
}Tile16;

extern uint32_t GpuPrimeBuf[0xf];
extern uint8_t GpuPrimeCount;
extern uint8_t GpuPrimeCounter;
extern uint8_t GpuPrimeSize[256];

extern void (*GpuPrimesProc[256])(Gpu* Gpu);
}
}