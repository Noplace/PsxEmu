#ifndef EMULATION_PSX_GPU_H
#define EMULATION_PSX_GPU_H

#include "gpu_primitives.h"

#define BGR2ARGB555(c)  ((c&0x0000)|((unsigned char)(c&31)<<10)|((unsigned char)((c>>5)&31)<<5)|(unsigned char)((c>>10)&31))

#define GPU_RW_IO 0xf0
#define GPU_RW_TRANSFER 0xf1



#define GPUSTATUS_ODDLINES            0x80000000
#define GPUSTATUS_DMABITS             0x60000000 // Two bits
#define GPUSTATUS_READYFORCOMMANDS    0x10000000
#define GPUSTATUS_READYFORVRAM        0x08000000
#define GPUSTATUS_IDLE                0x04000000
#define GPUSTATUS_DISPLAYDISABLED     0x00800000
#define GPUSTATUS_INTERLACED          0x00400000
#define GPUSTATUS_RGB24               0x00200000
#define GPUSTATUS_PAL                 0x00100000
#define GPUSTATUS_DOUBLEHEIGHT        0x00080000
#define GPUSTATUS_WIDTHBITS           0x00070000 // Three bits
#define GPUSTATUS_MASKENABLED         0x00001000
#define GPUSTATUS_MASKDRAWN           0x00000800
#define GPUSTATUS_DRAWINGALLOWED      0x00000400
#define GPUSTATUS_DITHER              0x00000200

#define GPUIsBusy (Status &= ~GPUSTATUS_IDLE)
#define GPUIsIdle (Status |= GPUSTATUS_IDLE)
#define GPUIsNotReadyForCommands (Status &= ~GPUSTATUS_READYFORCOMMANDS)
#define GPUIsReadyForCommands (Status |= GPUSTATUS_READYFORCOMMANDS)



#define NTSC 0
#define PAL 1
/*
typedef	union 
{
		struct 
		{
	     unsigned X:4;     
	     unsigned Y:1;
	     unsigned ABR:2;     
	     unsigned CLUT:2;
	     unsigned DTD:1;     
	     unsigned DFE:1;
		}Bits;
		unsigned Data:11;
}GpuTexPage;


typedef struct
	{
     GpuTexPage TexPage;//0-10
  	 unsigned MaskE:1;     //11
	 unsigned MaskD:1;//12
     unsigned Unknown1:3;//13-15
     unsigned Width1:1;//16
     unsigned Width0:2;//17-18
	 unsigned Height:1;//19
	 unsigned Video:1;//PAL = 1  20
	 unsigned Rgb24:1;// 21
	 unsigned Inter:1;//22
     unsigned DD:1;//display disabled 23
	 unsigned Unknown0:2;//24-25
     unsigned Busy:1;//26
     unsigned DmaReady:1;//27
     unsigned CmdReady:1;//28
	 unsigned Dma:2;//29-30
	 unsigned OIM:1;//odd lines in interlace mode 31
}GpuStatusBits;

typedef struct
{
 uint16_t DispX,DispY;
 uint16_t Width,Height;
 uint16_t ClipX,ClipY;
 uint16_t ClipW,ClipH;
 uint16_t OffsetX,OffsetY;

 uint16_t TransX0,TransY0,TransX1,TransY1,TCounter;
 uint16_t *TransMem;

 uint8_t ReadMode,WriteMode;

 uint32_t Data;

 uint8_t Mask1,Mask2;

  /*union
  {
	GpuStatusBits Bits;
	uint32_t Data;
  } Status;*/
// uint32_t Status;
//}Gpu;


namespace emulation {
namespace psx {

class CTexture;

typedef struct
{
 unsigned short *blocks[16];
 CTexture *Tex[32];
 uint32_t  Clut[32];
}TP;



class Gpu : public Component
{
public:
	Gpu();
	~Gpu();
  int Initialize();
  int Deinitialize();
	void Render();
	uint32_t ReadData();
	uint32_t ReadStatus();
	void WriteData(uint32_t _Data);
	void WriteStatus(uint32_t _Data);
	void ExecPrime(uint32_t _Data);

	void DmaChain();
	void Dma2();
	void Dma6();

	void CacheInit();
	void CacheDeinit();

	int WriteVRamToFile();

	void UpdateDisplay();

	CTexture *LoadTexture(uint32_t tp,uint32_t clut_id,uint32_t code);
	uint32_t SetTransMode(uint32_t tp,uint32_t in_color);

	//CPlaystation *Parent;

	/*Graphics Interface*/
	//CGraphicsKernel *G;
	//CGraphicsPrimitive *GP;
	//CG2DProjection *GV;
	//LPDIRECT3DDEVICE9 d3dDevice;


	int VRamLock;
	TP TexturePage;

	uint32_t Data;
	uint32_t Status;
	
	uint16_t Width,Height;

	uint16_t DisplayStartX,DisplayEndX;
	uint16_t DisplayStartY,DisplayEndY;

	uint16_t DrawingClipX,DrawingClipY;
	uint16_t DrawingClipW,DrawingClipH;
	uint16_t DrawingOffsetX,DrawingOffsetY;

	uint16_t TransX0,TransY0,TransX1,TransY1,TCounter;
	uint16_t *TransMem;

	uint8_t ReadMode,WriteMode;
	uint8_t Mask1,Mask2;
};



uint32_t A1R5G5B5_to_A8R8G8B8(uint16_t source);
uint16_t A8R8G8B8_to_A1R5G5B5(uint32_t source);

}
}
#endif
