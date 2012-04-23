#include "..\system.h"

//#define DEBUG_GPU

namespace emulation {
namespace psx {

__inline unsigned long swap_adjust_color(int x) 
{
 int r=(x&0xff0000)>>24;
 int g=(x&0xff00)>>16;
 int b=(x&0xff);
 //r = (r/255)*31;
 //g = (g/255)*31;
 //b = (b/255)*31;
 r*=2;
 r-=2;
 g*=2;
 g-=2;
 b*=2;
 b-=2;
 return (x&0xff000000)|(r>>16)|g|(b<<16);
}

__inline unsigned long swap_color(int x) 
{
 int r=(x&0xff0000);
 int g=(x&0xff00);
 int b=(x&0xff);
 return (0xff000000)|(r>>16)|g|(b<<16);
//	return swap_adjust_color(x);
}



uint32_t GpuPrimeBuf[0xf];
uint8_t GpuPrimeCount;
uint8_t GpuPrimeCounter;

void GpuPrimeUnknown(Gpu *Gpu);

void GpuPrimeCacheFlush(Gpu *Gpu);
void GpuPrimeBlockFill(Gpu *Gpu);

void GpuPrimePolyF3(Gpu *Gpu);
void GpuPrimePolyFT3(Gpu *Gpu);
void GpuPrimePolyG3(Gpu *Gpu);

void GpuPrimePolyF4(Gpu *Gpu);
void GpuPrimePolyFT4(Gpu *Gpu);
void GpuPrimePolyG4(Gpu *Gpu);
void GpuPrimePolyGT4(Gpu *Gpu);

void GpuPrimeLineF2(Gpu *Gpu);
void GpuPrimeLineG2(Gpu *Gpu);

void GpuPrimeRect(Gpu *Gpu);

void GpuPrimeSprite(Gpu *Gpu);
void GpuPrimeSprite8(Gpu *Gpu);
void GpuPrimeSprite16(Gpu *Gpu);

void GpuPrimeTile8(Gpu *Gpu);
void GpuPrimeTile16(Gpu *Gpu);

void GpuPrimeMoveImage(Gpu *Gpu);

void GpuPrimeMem2VRam(Gpu *Gpu);
void GpuPrimeVRam2Mem(Gpu *Gpu);

void GpuPrimeDrawModeSetting(Gpu *Gpu);
void GpuPrimeDrawOffset(Gpu *Gpu);
void GpuPrimeClipAreaEnd(Gpu *Gpu);
void GpuPrimeClipAreaStart(Gpu *Gpu);
void GpuPrimeTextureWindow(Gpu *Gpu);
void GpuPrimeMaskSetting(Gpu *Gpu);



void (*GpuPrimesProc[256])(Gpu *Gpu)=
{
//0x00
GpuPrimeUnknown,GpuPrimeCacheFlush,GpuPrimeBlockFill,GpuPrimeUnknown,
GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,
GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,
GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,
//0x10
GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,
GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,
GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,
GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,
//0x20
GpuPrimePolyF3,GpuPrimePolyF3,GpuPrimePolyF3,GpuPrimePolyF3,
GpuPrimePolyFT3,GpuPrimePolyFT3,GpuPrimePolyFT3,GpuPrimePolyFT3,
GpuPrimePolyF4,GpuPrimePolyF4,GpuPrimePolyF4,GpuPrimePolyF4,
GpuPrimePolyFT4,GpuPrimePolyFT4,GpuPrimePolyFT4,GpuPrimePolyFT4,
//0x30
GpuPrimePolyG3,GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,
GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,
GpuPrimePolyG4,GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,
GpuPrimePolyGT4,GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,
//0x40
GpuPrimeLineF2,GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,
GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,
GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,
GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,
//0x50
GpuPrimeLineG2,GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,
GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,
GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,
GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,
//0x60
GpuPrimeRect,GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,
GpuPrimeSprite,GpuPrimeSprite,GpuPrimeSprite,GpuPrimeSprite,
GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,
GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,
//0x70
GpuPrimeTile8,GpuPrimeTile8,GpuPrimeTile8,GpuPrimeTile8,
GpuPrimeSprite8,GpuPrimeSprite8,GpuPrimeSprite8,GpuPrimeSprite8,
GpuPrimeTile16,GpuPrimeTile16,GpuPrimeTile16,GpuPrimeTile16,
GpuPrimeSprite16,GpuPrimeSprite16,GpuPrimeSprite16,GpuPrimeSprite16,
//0x80
GpuPrimeMoveImage,GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,
GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,
GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,
GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,
//0x90
GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,
GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,
GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,
GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,
//0xa0
GpuPrimeMem2VRam,GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,
GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,
GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,
GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,
//0xb0
GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,
GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,
GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,
GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,
//0xc0
GpuPrimeVRam2Mem,GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,
GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,
GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,
GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,
//0xd0
GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,
GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,
GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,
GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,
//0xe0
GpuPrimeUnknown,GpuPrimeDrawModeSetting,GpuPrimeTextureWindow,GpuPrimeClipAreaStart,
GpuPrimeClipAreaEnd,GpuPrimeDrawOffset,GpuPrimeMaskSetting,GpuPrimeUnknown,
GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,
GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,
//0xf0
GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,
GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,
GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,
GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown,GpuPrimeUnknown
};

uint8_t GpuPrimeSize[256]=
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



void GpuPrimeUnknown(Gpu *Gpu)
{
#ifdef DEBUG_GPU
 _cprintf("unknown prime %x\n",(GpuPrimeBuf[0]>>24));
#endif
}

void cache()
{
 	D3DLOCKED_RECT rect;
	int i;
	for (i=0;i<32;i++)
	{
	  //TexturePage.Clut[i]=0;
	  //gfx->LockTexture(i],0,0,256,256,&rect);
	  //memset(rect.pBits,0,256*256*2 + 1);
	  //gfx->UnlockTexture(i]);
	}
}

void GpuPrimeCacheFlush(Gpu *Gpu)
{
	//Gpu->GP->BufProc(cache);
	//cache();
	for (int i=0;i<32;i++)
	{
	  Gpu->TexturePage.Clut[i]=0;
	  //gfx->LockTexture(i],0,0,256,256,&rect);
	  //memset(rect.pBits,0,256*256*2 + 1);
	  //gfx->UnlockTexture(i]);
	}
}

void GpuPrimeBlockFill(Gpu *Gpu)
{
  struct
 {
  unsigned long  color;
  unsigned short x,y;
  unsigned short w,h;
 }Box;

  memcpy(&Box,GpuPrimeBuf,sizeof(Box));
  uint32_t color = swap_color(Box.color);
  D3DRECT rc={Box.x,Box.y,Box.x+Box.w,Box.y+Box.h};
  
 //Gpu->d3dDevice->Clear(1,&rc,D3DCLEAR_TARGET,color,1.0f,0);
  
 G2DPoly4 destpoly;
 memset(&destpoly,0,sizeof(destpoly));


 destpoly.v[0].c=0xff000000|color&0xffffff;
 destpoly.v[1].c=destpoly.v[2].c=destpoly.v[3].c=destpoly.v[0].c;
 destpoly.v[0].x=Box.x;
 destpoly.v[0].y=Box.y;
 destpoly.v[1].x=Box.x+Box.w;
 destpoly.v[1].y=Box.y;
 destpoly.v[2].x=Box.x;
 destpoly.v[2].y=Box.y+Box.h;
 destpoly.v[3].x=Box.x+Box.w;
 destpoly.v[3].y=Box.y+Box.h;
 destpoly.tex=0;
 
 D3DXMATRIX m,id;
 D3DXMatrixIdentity(&id);
 

Gpu->d3dDevice->GetTransform(D3DTS_WORLD,&m);
Gpu->d3dDevice->SetTransform(D3DTS_WORLD,&id);
 Gpu->GP->GP2DPoly4(&destpoly);
Gpu->d3dDevice->SetTransform(D3DTS_WORLD,&m);

}

void GpuPrimePolyF3(Gpu *Gpu)
{
 PolyF3 poly;
 G2DPoly3 destpoly;
 memcpy(&poly,GpuPrimeBuf,sizeof(PolyF3));
 
 memset(&destpoly,0,sizeof(destpoly));
 destpoly.v[0].c=0xff000000|(swap_color(poly.color)&0xffffff);
 destpoly.v[1].c=destpoly.v[2].c=destpoly.v[0].c;
 destpoly.v[0].x=poly.x0;
 destpoly.v[0].y=poly.y0;
 destpoly.v[1].x=poly.x1;
 destpoly.v[1].y=poly.y1;
 destpoly.v[2].x=poly.x2;
 destpoly.v[2].y=poly.y2;
 destpoly.tex=0;
  
 Gpu->GP->GP2DPoly3(&destpoly);
}

void GpuPrimePolyG3(Gpu *Gpu)
{
 PolyG3 poly;
 G2DPoly3 destpoly;
 memcpy(&poly,GpuPrimeBuf,sizeof(PolyG3));

 memset(&destpoly,0,sizeof(destpoly));
 destpoly.v[0].c=0xff000000|(swap_color(poly.color0)&0xffffff);
 destpoly.v[1].c=0xff000000|(swap_color(poly.color1)&0xffffff);
 destpoly.v[2].c=0xff000000|(swap_color(poly.color2)&0xffffff);
 destpoly.v[0].x=poly.x0;
 destpoly.v[0].y=poly.y0;
 destpoly.v[1].x=poly.x1;
 destpoly.v[1].y=poly.y1;
 destpoly.v[2].x=poly.x2;
 destpoly.v[2].y=poly.y2;
 destpoly.tex=0;
  Gpu->d3dDevice->SetTexture(0,0);
 Gpu->GP->GP2DPoly3(&destpoly);
}

void GpuPrimePolyFT3(Gpu *Gpu)
{
 PolyFT3 poly;
 G2DPoly3 destpoly;
 memcpy(&poly,GpuPrimeBuf,sizeof(PolyFT3));
 //_cprintf("PolyFT3:%d %d\n",poly.clut,poly.tpage);
 
 poly.color=swap_color(poly.color);

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
 Gpu->GP->GP2DPoly3(&destpoly);
}




void GpuPrimePolyF4(Gpu *Gpu)
{
 PolyF4 poly;
 G2DPoly4 destpoly;
 memcpy(&poly,GpuPrimeBuf,sizeof(PolyF4));
 
 uint32_t color = swap_color(poly.color);

 memset(&destpoly,0,sizeof(destpoly));
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


 Gpu->GP->GP2DPoly4(&destpoly);
}

void GpuPrimePolyFT4(Gpu *Gpu)
{
 PolyFT4 poly;
 G2DPoly4 destpoly;
 unsigned long tp;
 memcpy(&poly,GpuPrimeBuf,sizeof(PolyFT4));
 tp=poly.tpage;

 uint32_t color = swap_color(poly.color);
 
  memset(&destpoly,0,sizeof(destpoly));
 destpoly.v[0].c=0xff000000|color;
 destpoly.v[1].c=destpoly.v[2].c=destpoly.v[3].c=destpoly.v[0].c;
 destpoly.v[0].x=poly.x0;
 destpoly.v[0].y=poly.y0;

 destpoly.v[1].x=poly.x1;
 destpoly.v[1].y=poly.y1;
 destpoly.v[2].x=poly.x2;
 destpoly.v[2].y=poly.y2;
 destpoly.v[3].x=poly.x3;
 destpoly.v[3].y=poly.y3;

 destpoly.v[0].u=poly.u0*0.00390625f;
 destpoly.v[0].v=poly.v0*0.00390625f;
 destpoly.v[1].u=poly.u1*0.00390625f;
 destpoly.v[1].v=poly.v1*0.00390625f;//(poly.v[1].v&1)?poly.v[1].v+1:poly.v[1].v;
 destpoly.v[2].u=poly.u2*0.00390625f;//(poly.v[2].u&1)?poly.v[2].u+1:poly.v[2].u;
 destpoly.v[2].v=poly.v2*0.00390625f;
 destpoly.v[3].u=poly.u3*0.00390625f;//(poly.v[3].u&1)?poly.v[3].u+1:poly.v[3].u;
 destpoly.v[3].v=poly.v3*0.00390625f;//(poly.v[3].v&1)?poly.v[3].v+1:poly.v[3].v;

 Gpu->SetTransMode(tp,color);
 destpoly.tex=Gpu->LoadTexture(tp,poly.clut,poly.color);
 Gpu->GP->GP2DPoly4(&destpoly);
}

void GpuPrimePolyGT4(Gpu *Gpu)
{
 PolyGT4 poly;
 G2DPoly4 destpoly;
 unsigned long tp;

 memcpy(&poly,GpuPrimeBuf,sizeof(PolyGT4));
 tp=poly.tpage;

  
 //uint32_t color = swap_color(poly.color0);
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
 Gpu->GP->GP2DPoly4(&destpoly);
}

void GpuPrimePolyG4(Gpu *Gpu)
{
 PolyG4 poly;
 G2DPoly4 destpoly;
 memcpy(&poly,GpuPrimeBuf,sizeof(PolyG4));

 memset(&destpoly,0,sizeof(destpoly));
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
 Gpu->GP->GP2DPoly4(&destpoly);
}


void GpuPrimeLineF2(Gpu *Gpu)
{
 LineF2 line;
 G2DLine destline;
 memcpy(&line,GpuPrimeBuf,sizeof(LineF2));

 

 uint32_t color = swap_color(line.color)|0xff000000;
memset(&destline,0,sizeof(destline));
 destline.v[0].c=destline.v[1].c=color;
 destline.v[0].x=line.x0;
 destline.v[0].y=line.y0;
 destline.v[1].x=line.x1;
 destline.v[1].y=line.y1;

 unsigned long tp;
 tp=Gpu->Status&0x7ff;
 Gpu->SetTransMode(tp,line.color);
 Gpu->GP->GP2DLine(&destline);
}

void GpuPrimeLineG2(Gpu *Gpu)
{
 LineG2 line;
 G2DLine destline;

 memcpy(&line,GpuPrimeBuf,sizeof(LineG2));


 uint32_t color0 = swap_color(line.color0)|0xff000000;
 uint32_t color1 = swap_color(line.color1)|0xff000000;
 memset(&destline,0,sizeof(destline));
 destline.v[0].c=color0;
 destline.v[1].c=color1;
 destline.v[0].x=line.x0;
 destline.v[0].y=line.y0;
 destline.v[1].x=line.x1;
 destline.v[1].y=line.y1;
 Gpu->GP->GP2DLine(&destline);
}

void GpuPrimeRect(Gpu *Gpu)
{
 Rect rect;
 G2DRect destrect;
 memcpy(&rect,GpuPrimeBuf,sizeof(Rect));

 uint32_t color = swap_color(rect.color)|0xff000000;
memset(&destrect,0,sizeof(destrect));
 destrect.c0=destrect.c1=destrect.c2=destrect.c3=color;
 destrect.x = rect.x;
 destrect.y = rect.y;
 destrect.w = rect.w;
 destrect.h = rect.h;

 Gpu->GP->GP2DRect(&destrect);
}

void GpuPrimeSprite(Gpu *Gpu)
{
 Sprite sprite;
 G2DPoly4 destpoly;
 memcpy(&sprite,GpuPrimeBuf,sizeof(Sprite));
memset(&destpoly,0,sizeof(destpoly));
 uint32_t color = swap_adjust_color(sprite.color);
 destpoly.v[0].c=0xff000000|(color&0xffffff);
 destpoly.v[1].c=destpoly.v[2].c=destpoly.v[3].c=destpoly.v[0].c;
 destpoly.v[0].x=sprite.x;
 destpoly.v[0].y=sprite.y;
 destpoly.v[1].x=(sprite.x+sprite.w);
 destpoly.v[1].y=sprite.y;
 destpoly.v[2].x=sprite.x;
 destpoly.v[2].y=(sprite.y+sprite.h);
 destpoly.v[3].x=(sprite.x+sprite.w);
 destpoly.v[3].y=(sprite.y+sprite.h);

 destpoly.v[0].u=sprite.u*0.00390625f;
 destpoly.v[0].v=sprite.v*0.00390625f;
 destpoly.v[1].u=(sprite.u+sprite.w)*0.00390625f;
 destpoly.v[1].v=sprite.v*0.00390625f;
 destpoly.v[2].u=sprite.u*0.00390625f;
 destpoly.v[2].v=(sprite.v+sprite.h)*0.00390625f;
 destpoly.v[3].u=(sprite.u+sprite.w)*0.00390625f;
 destpoly.v[3].v=(sprite.v+sprite.h)*0.00390625f;

 unsigned long tp;
 tp=Gpu->Status&0x7ff;
 Gpu->SetTransMode(tp,sprite.color);
 destpoly.tex=Gpu->LoadTexture(tp,sprite.clut,sprite.color);
 Gpu->GP->GP2DPoly4(&destpoly);
}

void GpuPrimeSprite8(Gpu *Gpu)
{
 Sprite8 sprite;
 G2DPoly4 destpoly;
 memcpy(&sprite,GpuPrimeBuf,sizeof(Sprite8));
 memset(&destpoly,0,sizeof(destpoly));
 uint32_t color = swap_adjust_color(sprite.color);


 destpoly.v[0].c=color;
 destpoly.v[1].c=destpoly.v[2].c=destpoly.v[3].c=destpoly.v[0].c;
 destpoly.v[0].x=sprite.x;
 destpoly.v[0].y=sprite.y;
 destpoly.v[1].x=(sprite.x+8);
 destpoly.v[1].y=sprite.y;

 destpoly.v[2].x=sprite.x;
 destpoly.v[2].y=(sprite.y+8);
 destpoly.v[3].x=(sprite.x+8);
 destpoly.v[3].y=(sprite.y+8);

 destpoly.v[0].u=sprite.u*0.00390625f;
 destpoly.v[0].v=sprite.v*0.00390625f;
 destpoly.v[1].u=(sprite.u+8)*0.00390625f;
 destpoly.v[1].v=sprite.v*0.00390625f;
 destpoly.v[2].u=sprite.u*0.00390625f;
 destpoly.v[2].v=(sprite.v+8)*0.00390625f;
 destpoly.v[3].u=(sprite.u+8)*0.00390625f;
 destpoly.v[3].v=(sprite.v+8)*0.00390625f;

 unsigned long tp;
 tp=Gpu->Status&0x7ff;
 Gpu->SetTransMode(tp,sprite.color);
 destpoly.tex=Gpu->LoadTexture(tp,sprite.clut,sprite.color);
 Gpu->GP->GP2DPoly4(&destpoly);
}

void GpuPrimeSprite16(Gpu *Gpu)
{
 Sprite8 sprite;
 G2DPoly4 destpoly;
 memcpy(&sprite,GpuPrimeBuf,sizeof(Sprite8));
memset(&destpoly,0,sizeof(destpoly));
 uint32_t color = 0xffffffff;//swap_adjust_color(sprite.color);

 destpoly.v[0].c=color;
 destpoly.v[1].c=destpoly.v[2].c=destpoly.v[3].c=destpoly.v[0].c;
 destpoly.v[0].x=sprite.x;
 destpoly.v[0].y=sprite.y;
 destpoly.v[1].x=sprite.x+16;
 destpoly.v[1].y=sprite.y;
 destpoly.v[2].x=sprite.x;
 destpoly.v[2].y=sprite.y+16;
 destpoly.v[3].x=sprite.x+16;
 destpoly.v[3].y=sprite.y+16;

 destpoly.v[0].u=sprite.u*0.00390625f;
 destpoly.v[0].v=sprite.v*0.00390625f;
 destpoly.v[1].u=(sprite.u+16)*0.00390625f;
 destpoly.v[1].v=sprite.v*0.00390625f;
 destpoly.v[2].u=sprite.u*0.00390625f;
 destpoly.v[2].v=(sprite.v+16)*0.00390625f;
 destpoly.v[3].u=(sprite.u+16)*0.00390625f;
 destpoly.v[3].v=(sprite.v+16)*0.00390625f;
 unsigned long tp;
 tp=Gpu->Status&0x7ff;
 
 Gpu->SetTransMode(tp,sprite.color);
 destpoly.tex=Gpu->LoadTexture(tp,sprite.clut,sprite.color);
 Gpu->GP->GP2DPoly4(&destpoly);
}


void GpuPrimeTile8(Gpu *Gpu)
{
 Tile16 tile;
 G2DPoly4 destpoly;
 memcpy(&tile,GpuPrimeBuf,sizeof(Tile16));

 uint32_t color = swap_adjust_color(tile.color);
 memset(&destpoly,0,sizeof(destpoly));
 destpoly.v[0].c=color;
 destpoly.v[1].c=destpoly.v[2].c=destpoly.v[3].c=destpoly.v[0].c;
 destpoly.v[0].x=tile.x;
 destpoly.v[0].y=tile.y;
 destpoly.v[1].x=tile.x+8;
 destpoly.v[1].y=tile.y;
 destpoly.v[2].x=tile.x;
 destpoly.v[2].y=tile.y+8;
 destpoly.v[3].x=tile.x+8;
 destpoly.v[3].y=tile.y+8;
 unsigned long tp;
 tp=Gpu->Status&0x7ff;
 //Gpu->SetTransMode(tp,tile.color);
 //destpoly.tex=(unsigned long)Gpu->LoadTexture(tp,sprite.clut)];
 Gpu->GP->GP2DPoly4(&destpoly);
}

void GpuPrimeTile16(Gpu *Gpu)
{
 Tile16 tile;
 G2DPoly4 destpoly;
 memcpy(&tile,GpuPrimeBuf,sizeof(Tile16));

 uint32_t color = swap_adjust_color(tile.color);
 memset(&destpoly,0,sizeof(destpoly));
 destpoly.v[0].c=color;
 destpoly.v[1].c=destpoly.v[2].c=destpoly.v[3].c=destpoly.v[0].c;
 destpoly.v[0].x=tile.x;
 destpoly.v[0].y=tile.y;
 destpoly.v[1].x=tile.x+16;
 destpoly.v[1].y=tile.y;
 destpoly.v[2].x=tile.x;
 destpoly.v[2].y=tile.y+16;
 destpoly.v[3].x=tile.x+16;
 destpoly.v[3].y=tile.y+16;
 unsigned long tp;
 tp=Gpu->Status&0x7ff;
 //Gpu->SetTransMode(tp,tile.color);
 //destpoly.tex=(unsigned long)Gpu->LoadTexture(tp,sprite.clut)];
 Gpu->GP->GP2DPoly4(&destpoly);
}



void GpuPrimeMoveImage(Gpu *Gpu)
{
	uint16_t W,H;
	int x,y;

	Gpu->TransX0 = (uint16_t)(GpuPrimeBuf[1] & 0xffff);
	Gpu->TransY0 = (uint16_t)(GpuPrimeBuf[1] >>16);
	Gpu->TransX1 = (uint16_t)(GpuPrimeBuf[2] & 0xffff);
	Gpu->TransY1 = (uint16_t)(GpuPrimeBuf[2] >>16);

	W = (uint16_t)(GpuPrimeBuf[3] & 0xffff);
	H = (uint16_t)(GpuPrimeBuf[3] >>16);


	if (Gpu->TransX0==Gpu->TransX1&&Gpu->TransY0==Gpu->TransY1) return;
	//_cprintf("moveimage %d %d to  %d %d size %d %d\n",Gpu->TransX0,Gpu->TransY0,Gpu->TransX1,Gpu->TransY1,W,H);
	  
	   for (y=Gpu->TransY0;y<Gpu->TransY0+H;y++)
	   {
		
	    for (x=Gpu->TransX0;x<Gpu->TransX0+W;x++)
	 	{
		 Gpu->Parent->MC->VRam->u16[((Gpu->TransY1)*1024+(Gpu->TransX1))]=Gpu->Parent->MC->VRam->u16[((y)*1024+(x))];
		}
	   }

	   //Gpu->WriteMode=GPU_RW_IO;
	 //Gpu->TransY0=Gpu->TransX0=Gpu->TransY1=Gpu->TransX1=0;
}

void GpuPrimeMem2VRam(Gpu *Gpu)
{
	Gpu->TransX0 = (uint16_t)(GpuPrimeBuf[1] & 0x3ff);
	Gpu->TransY0 = (uint16_t)(GpuPrimeBuf[1] >>16)&0x1ff;
	Gpu->TransX1 = (uint16_t)(GpuPrimeBuf[2] & 0xffff);
	Gpu->TransY1 = (uint16_t)(GpuPrimeBuf[2] >>16);
	Gpu->TransMem=&Gpu->Parent->MC->VRam->u16[Gpu->TransY0*1024+Gpu->TransX0];
    Gpu->WriteMode = GPU_RW_TRANSFER;
	Gpu->TCounter= 0;


	//_cprintf("loadimage <%d %d  %d %d>\n",Gpu->TransX0,Gpu->TransY0,Gpu->TransX1,Gpu->TransY1);
}

void GpuPrimeVRam2Mem(Gpu *Gpu)
{
	Gpu->TransX0 = (uint16_t)(GpuPrimeBuf[1] & 0x3ff);
	Gpu->TransY0 = (uint16_t)(GpuPrimeBuf[1] >>16)&0x1ff;
	Gpu->TransX1 = (uint16_t)(GpuPrimeBuf[2] & 0xffff);
	Gpu->TransY1 = (uint16_t)(GpuPrimeBuf[2] >>16);
	Gpu->TransMem=&Gpu->Parent->MC->VRam->u16[Gpu->TransY0*1024+Gpu->TransX0];
    Gpu->ReadMode= GPU_RW_TRANSFER;
	Gpu->TCounter= 0;
	Gpu->Status|=GPUSTATUS_READYFORVRAM;

	//_cprintf("storeimage <%d %d  %d %d>\n",Gpu->TransX0,Gpu->TransY0,Gpu->TransX1,Gpu->TransY1);
}

void GpuPrimeDrawModeSetting(Gpu *Gpu)
{
	//11111111111
 Gpu->Status&=0xfffff800;
 Gpu->Status|=GpuPrimeBuf[0]&0x7ff;
 //Gpu->Status.Bits.TexPage.Data=(GpuPrimeBuf[0]&0x7ff);
}

void GpuPrimeClipAreaStart(Gpu *Gpu)
{
  Gpu->DrawingClipX = (unsigned short)(GpuPrimeBuf[0]&0x3ff);
  Gpu->DrawingClipY = (unsigned short)((GpuPrimeBuf[0]&0xffc00)>>10);
  //Gpu->GP->BufDrawOffset(Gpu->ClipX+Gpu->OffsetX,Gpu->ClipY+Gpu->OffsetY);
  //_cprintf("cx,cy:%d %d\n",Gpu->ClipX,Gpu->ClipY);
}

void GpuPrimeClipAreaEnd(Gpu *Gpu)
{
  Gpu->DrawingClipW = (unsigned short)((GpuPrimeBuf[0]&0x3ff));
  Gpu->DrawingClipH = (unsigned short)((GpuPrimeBuf[0]&0xffc00)>>10);
  //_cprintf("cw,ch:%d %d\n",Gpu->ClipW,Gpu->ClipH);
}

void GpuPrimeDrawOffset(Gpu *Gpu)
{
	D3DXMATRIX m;
  Gpu->DrawingOffsetX = (unsigned short)(GpuPrimeBuf[0] & 0x7ff);
  Gpu->DrawingOffsetY = (unsigned short)((GpuPrimeBuf[0]&0x3ff800) >>11);
  D3DXMatrixTransformation2D(&m,0,0,0,0,0,&D3DXVECTOR2(Gpu->DrawingOffsetX,Gpu->DrawingOffsetY));
  //D3DXMatrixTranslation(&m, (float)Gpu->DrawingOffsetX, -(float)Gpu->DrawingOffsetY, 0.0f);
  Gpu->d3dDevice->SetTransform(D3DTS_WORLD,&m);
 // _cprintf("offset %d %d",Gpu->OffsetX,Gpu->OffsetY);
}


void GpuPrimeTextureWindow(Gpu *Gpu)
{
/*#define _get_tw(tw)	
		(tw ? ((0xe2000000)|
	((((tw)->y&0xff)>>3)<<15)| 
		((((tw)->x&0xff)>>3)<<10)|
		(((~((tw)->h-1)&0xff)>>3)<<5)| 
		(((~((tw)->w-1)&0xff)>>3))) : 0)
*/
	int x,y,w,h;
	x = ((GpuPrimeBuf[1]>>10)<<3)&0xff;
	y = ((GpuPrimeBuf[1]>>15)<<3)&0xff;
	w = ((GpuPrimeBuf[1])<<3)&0xff;
	h = ((GpuPrimeBuf[1]>>5)<<3)&0xff;
	
	//_cprintf("tex win %d %d %d %d %x %x\n",x,y,w,h,GpuPrimeBuf[0],GpuPrimeBuf[1]);
}

void GpuPrimeMaskSetting(Gpu *Gpu)
{
 Gpu->Mask1=GpuPrimeBuf[0]&0x1;
 Gpu->Mask2=GpuPrimeBuf[0]&0x2;
}

}
}