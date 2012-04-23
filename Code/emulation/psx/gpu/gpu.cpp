#include "..\system.h"

//#define DEBUG_GPU1
#define DEBUG_GPU
#define start_pix ((TransY0*1024)+TransX0)

namespace emulation {
namespace psx {

/*
-adding busy/idle states, not working so far
-cleared 'RW mode = IO' line in gpu status write::dma
*removed*-cleared 'transfer coords=0' line in read/write data
-added-'tcounter=0' line in read/write data
*/


short sDispWidths[8] = {256,320,512,640,368,384,512,640};


Gpu::Gpu() {
	
}

int Gpu::Initialize() {
Parent	=	Ptr;

	CacheInit();
	VRamLock=0;

	Status = 0x14802000;
	//GPUIsIdle;
	//GPUIsReadyForCommands;
	ReadMode=GPU_RW_IO;
	WriteMode=GPU_RW_IO;
	//GpuUnit.Data=0x400;
	TCounter= 0;

	D3DDISPLAYMODE dm;
	dm.Width = 1024;
	dm.Height = 512;
	dm.Format = D3DFMT_X8R8G8B8;
	G = new CGraphicsKernel(mhWnd);
	G->CreateDevice(dm,false);
	GP = new CGraphicsPrimitive(G);
	GP->Set(M2D);


	GV = new CG2DProjection(G);
	GV->Build();
	GV->Set();
	G->ClearBG(0xff000000);


	D3DXMATRIX matIdentity;
	D3DXMatrixIdentity(&matIdentity);


	d3dDevice = G->GetDevice();

	d3dDevice->SetTransform(D3DTS_VIEW, &matIdentity);
	d3dDevice->SetRenderState(D3DRS_ZENABLE, 0 );
	d3dDevice->SetRenderState(D3DRS_LIGHTING, 0 );
	d3dDevice->SetRenderState(D3DRS_ALPHABLENDENABLE,1);


	d3dDevice->SetTextureStageState( 0, D3DTSS_COLORARG1, D3DTA_TEXTURE );
	d3dDevice->SetTextureStageState( 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE );  
	d3dDevice->SetTextureStageState( 0, D3DTSS_COLOROP, D3DTOP_MODULATE );
	d3dDevice->SetTextureStageState(0,D3DTSS_ALPHAARG1,D3DTA_TEXTURE);
	d3dDevice->SetTextureStageState(0,D3DTSS_ALPHAARG2,D3DTA_DIFFUSE);
	d3dDevice->SetTextureStageState( 0, D3DTSS_ALPHAOP,D3DTOP_MODULATE );

	//G->DefaultLockFlag=D3DLOCK_DISCARD;
	//G->DefaultPool=D3DPOOL_MANAGED;
	//G->DefaultMipmapLevels=0;
	d3dDevice->BeginScene();
  return 0;
}
int Gpu::Deinitialize() {
  return 0;
}


Gpu::~Gpu()
{
 WriteVRamToFile();
 
 CacheDeinit();


 delete GV;
 delete GP;
 delete G;
}

void Gpu::Render()
{


	//_cprintf("primitive:%d\n",primitive->get_current_count());

 d3dDevice->EndScene();
 G->Render();//GpuUnit.DispX,GpuUnit.DispY,GpuUnit.Width,GpuUnit.Height);
 d3dDevice->BeginScene();
}

uint32_t Gpu::ReadStatus()
{
  // _cprintf("read gpu control\n");
 return Status;
}





void Gpu::WriteStatus(uint32_t _Data)
{
#define cmd (_Data>>24)
int param =(_Data&0xffffff);
#ifdef DEBUG_GPU
//	_cprintf("gpu write status %02x %06x\n",cmd,param);
#endif
 switch (cmd)
 {
  case 0x00:
	//_cprintf("Reset Gpu\n");
	  Width=256;
	  Height=240;
	  DrawingOffsetX = DrawingOffsetY = 0;
	  DisplayStartX=DisplayStartY=0;
	  UpdateDisplay();
      
	  Status = 0x74802000;//0x14802000;
	  Status|=GPUSTATUS_DISPLAYDISABLED;

      GPUIsReadyForCommands;
      GpuPrimeCount=0;
      ReadMode=GPU_RW_IO;
 	  WriteMode=GPU_RW_IO;
   break;
  case 0x01:
	  _cprintf("Reset CB\n");
	  //GpuPrimeCount=0;
	  //memset(GpuPrimeBuf,0,15*4);
  break;
  case 0x02:
	  _cprintf("Reset IRQ\n");
      //int_reg|=INT_GPU;
  break;
  case 0x03:
	Status&=~GPUSTATUS_DISPLAYDISABLED;
	Status|=((param&1)<<23);
  break;

  case 0x04:

	  Status&=~GPUSTATUS_DMABITS;
	  Status|=((param&0x03)<<29);
    
    ReadMode=WriteMode=GPU_RW_IO;
    if((param&0x03)==0x02) 
	{
		WriteMode = GPU_RW_TRANSFER;
	}
    if((param&0x03)==0x03)
	{
		ReadMode = GPU_RW_TRANSFER;
	}
  break;

  case 0x05:
    DisplayStartX=(uint16_t)(param&0x3ff);
	DisplayStartY=(uint16_t)((param>>10)&0x1ff);
  break;

  case 0x06:
	 	  {
       short x0=(short)(param & 0xfff);
       short x1=(short)((param>>12) & 0xfff);
	   //GpuUnit.Width=x1-x0;
	   //_cprintf("%d,%d\n",DispX,(x1-x0)/8);
	   //update_disp();
	  }               
	  break;
  case 0x07:
	  {
       short y0=(short)(param & 0x3ff);
       short y1=(short)((param>>10) & 0x3ff);
	   //_cprintf("%d,%d\n",DispY,(y1-y0));
	   //GpuUnit.Height=y1-y0;
	   //update_disp();
	  }                      
	  break;
  case 0x08:
	  
	  Status&=~GPUSTATUS_PAL;
	  Status&=~GPUSTATUS_RGB24;
	  Status&=~GPUSTATUS_INTERLACED;
	  if (param&BITNUM(3))
	  Status|=GPUSTATUS_PAL;
	  if (param&BITNUM(4))
	  Status|=GPUSTATUS_RGB24;
	  if (param&BITNUM(5))
	  Status|=GPUSTATUS_INTERLACED;

	  /*something wrong?*/
	  if (param&BITNUM(2))
	  {
   	   Height=480;
	   Status|=GPUSTATUS_DOUBLEHEIGHT;
	  }
	  else
	  {
 	   Height=240;
	   Status&=~GPUSTATUS_DOUBLEHEIGHT;
	  }

    Width = sDispWidths[(param & 0x03) | ((param & 0x40) >> 4)];
    Status&=~GPUSTATUS_WIDTHBITS;                   // Clear the width bits
    Status|=(((param & 0x03) << 17) | ((param & 0x40) << 10));                // Set the width bits

    UpdateDisplay();
  break;

  case 0x10:
      switch(_Data&0xff) 
	  {
	//	case 3:	GpuUnit.Data=GpuUnit.ClipX+(GpuUnit.ClipY<<10);break;
		//case 4: GpuUnit.Data=GpuUnit.ClipW+(GpuUnit.ClipH<<10);break;
		//case 5: GpuUnit.Data=GpuUnit.OffsetX+(GpuUnit.OffsetY<<10);break;
		case 7:	Data=2;break;
		default :
			_cprintf("gpu write 0x10 %d\n",Data&0xff);
			break;
			//case 0x08:
        //case 0x0F:GpuUnit.Data=0xBFC03720; break;
  	  } 
  break;
 }
}



uint32_t Gpu::ReadData()
{
 
//_cprintf("read gpu data\n");
 if (ReadMode==GPU_RW_TRANSFER)
 { 
  
	if (TCounter<TransY1*TransX1)
	{
		//GPUIsBusy;
#ifdef DEBUG_GPU1
		_cprintf("GpuWriteData::transfer with reading(%d %d %d %d) %d\n",GpuUnit.TransX0,GpuUnit.TransY0,GpuUnit.TransX1,GpuUnit.TransY1,GpuUnit.TCounter);
#endif
  		 Data = PS->MC->VRam->u32[(start_pix+TCounter)>>1];//(psxVRam->u16[(start_pix+GpuUnit.TCounter)]<<16)+psxVRam->u16[start_pix+GpuUnit.TCounter+1];
		 TCounter+=2;
	}
	else
	{
	 TCounter=0;
	 ReadMode=GPU_RW_IO;
	 TransY0=TransX0=TransY1=TransX1=0;
	 Status&=~GPUSTATUS_READYFORVRAM;
	}


 }
 
 //if (GpuUnit.ReadMode == GPU_RW_IO)
  return Data;
 //_cprintf("Gpu Read Error %d\n",GpuUnit.ReadMode);
 //return -1;
}




void Gpu::WriteData(uint32_t _Data)
{
 Data=_Data;

 GPUIsBusy;
 GPUIsNotReadyForCommands;

 if (WriteMode==GPU_RW_TRANSFER)
 { 
	if (TCounter<TransY1*TransX1)
	{
#ifdef DEBUG_GPU1
		_cprintf("GpuWriteData::transfer with writing(%d %d %d %d) %d\n",GpuUnit.TransX0,GpuUnit.TransY0,GpuUnit.TransX1,GpuUnit.TransY1,GpuUnit.TCounter);
#endif
  		PS->MC->VRam->u32[(start_pix+TCounter)>>1]=Data;
		TCounter+=2;
	}
	else
	{
	 TCounter=0;
	 WriteMode=GPU_RW_IO;
	 TransY0=TransX0=TransY1=TransX1=0;
	}
 }

 if (WriteMode == GPU_RW_IO)
  ExecPrime(Data);

 GPUIsIdle;
 GPUIsReadyForCommands;
}


void Gpu::ExecPrime(uint32_t _Data)
{
 
 if (GpuPrimeCount==0)
 {
  GpuPrimeCount=GpuPrimeSize[(Data>>24)];
  GpuPrimeCounter=0;
 }
 if (GpuPrimeCount!=0)
 {
  //if (GpuPrimeCounter<GpuPrimeCount)
  {
   GpuPrimeBuf[GpuPrimeCounter]=_Data;
   GpuPrimeCounter++;
  }
  if (GpuPrimeCounter==GpuPrimeCount)
  {
	  uint8_t p=(GpuPrimeBuf[0]>>24)&0xff;
#ifdef DEBUG_GPU
	  static uint8_t op=0;
	  if (p!=op)
	//_cprintf("using %x count == %d\n",p,GpuPrimeCount);
	op=p;
#endif
	//if ((((GpuPrimeBuf[0]>>24)&0xff)==2)&&(((GpuPrimeBuf[0]>>24)&0xff)>=0x80))
	{
		//cprintf("using %x count == %d\n",(GpuPrimeBuf[0]>>24)&0xff,GpuPrimeCount);
   GpuPrimesProc[p](this);
	}

   GpuPrimeCount=0;
  }
 }
 else
 {
  GpuPrimeCount=0;
 }


  
}


}
}