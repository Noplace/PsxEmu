#include "..\global.h"

//#define  DEBUG_GPU1

/*
dma chain :
mine is not complete :(
psx bios shows correctly with mine only
balls demo works with the other only
 */


unsigned long lUsedAddr[3];

__inline BOOL CheckForEndlessLoop(unsigned long laddr)
{
 if(laddr==lUsedAddr[1]) return TRUE;
 if(laddr==lUsedAddr[2]) return TRUE;

 if(laddr<lUsedAddr[0]) lUsedAddr[1]=laddr;
 else                   lUsedAddr[2]=laddr;
 lUsedAddr[0]=laddr;
 return FALSE;
}



long CALLBACK GPUdmaChain(uint32_t * baseAddrL, unsigned long addr)
{
 unsigned long dmaMem;
 uint8_t * baseAddrB;
 short count;unsigned int DMACommandCounter = 0;

// GPUIsBusy;

 lUsedAddr[0]=lUsedAddr[1]=lUsedAddr[2]=0xffffff;

 baseAddrB = (uint8_t*) baseAddrL;

 do
  {
//   if(iGPUHeight==512) addr&=0x1FFFFC;
   if(DMACommandCounter++ > 2000000) break;
   if(CheckForEndlessLoop(addr)) break;

   count = baseAddrB[addr+3];

   dmaMem=addr+4;

   uint32_t *mem=&baseAddrL[(dmaMem>>2)];   
   if(count>0) 
	   for (int i=0;i<count;i++)
	   {
	    PS->Gpu->WriteData(*mem++);
	   }

   addr = baseAddrL[(addr>>2)]&0xffffff;
  }
 while (addr != 0xffffff);

 //GPUIsIdle;

 return 0;
}




void CGpu::DmaChain()
{
	GPUIsBusy;

  uint32_t PacketCount;
     uint32_t vAddr=Parent->IOC->DmaChannels[2].madr&0x1fffff;


   while (1)
   {
    PacketCount = ((Parent->MC->Ram->u32[vAddr>>2] >> 24) & 0xff);
   //#ifdef DEBUG_GPU
    //_cprintf("Packet Dma %d\n",PacketCount);
   //#endif	
	if (PacketCount!=0)
	{
		for (unsigned int i=1;i<=PacketCount;i++)
		{
			WriteData(Parent->MC->Ram->u32[(vAddr>>2)+i]);
		}
		if ((Parent->MC->Ram->u32[vAddr>>2]&0xffffff)==0xffffff)
		{
			break;
		}
	}	
	vAddr=Parent->MC->Ram->u32[vAddr>>2]&0x1fffff;
	
   }

   GPUIsIdle;
}


void CGpu::Dma2()
{
  

  if ((Parent->IOC->DmaChannels[2].chcr & 0x01000401) == 0x01000401)
  {
	  //GPUdmaChain(Parent->MC->Ram->u32,Parent->IOC->DmaChannels[2].madr&0x1fffff);
	 DmaChain();
  }
  else  /*ReadWriteMode Memory to VRam*/
  if ((Parent->IOC->DmaChannels[2].chcr & 0x01000201) == 0x01000201)
  {
   uint32_t vAddr=Parent->IOC->DmaChannels[2].madr&0x1fffff;
   uint32_t size=(Parent->IOC->DmaChannels[2].bcr >> 16) * (Parent->IOC->DmaChannels[2].bcr & 0xffff);
   TCounter*=2;

#ifdef DEBUG_GPU1
    _cprintf("GpuDma2::mem w transfer(%d) <%d %d %d %d> tc= %d no of pixels=%d\n",GpuUnit.WriteMode,GpuUnit.TransX0,GpuUnit.TransY0,GpuUnit.TransX1,GpuUnit.TransY1,GpuUnit.TCounter,GpuUnit.TransX1*GpuUnit.TransY1);
	_cprintf("GpuDma2::mem transfer size = %d no of pixels=%d\n",(dma[2].bcr >> 16) * (dma[2].bcr & 0xffff)*2,GpuUnit.TransX1*GpuUnit.TransY1);
#endif

#ifdef DEBUG_GPU1
  	 // _cprintf("transfer %d\n",GpuUnit.TCounter);
#endif	 
	
	int x,y;

	if (WriteMode==GPU_RW_TRANSFER)
	{
       int c=0;
	   for (y=TransY0;y<TransY0+TransY1;y++)
	   {
		uint16_t*dst_line=(uint16_t*)TransMem;
		  //if (GpuUnit.TCounter>((dma[2].bcr >> 16) * (dma[2].bcr & 0xffff)*2))
			 // break;
	    for (x=TransX0;x<TransX0+TransX1;x++)
	 	{
	      //psxVRam->u16[((y-GpuUnit.TransY0)*1024+(x-GpuUnit.TransX0))]=psxRam->u16[(vAddr-GpuUnit.TCounter)>>1];
		  //psxVRam->u16[((y)*1024+(x))]=psxRam->u16[(vAddr-GpuUnit.TCounter)>>1];
		  *(dst_line++)=Parent->MC->Ram->u16[((vAddr-TCounter)>>1)];
		  vAddr+=2;
		  //GpuUnit.TCounter++;
	      c++;
		}
		//if (c>=size) break;
		TransMem+=1024;
	   }
	    

	   WriteMode=GPU_RW_IO;
	   TransY0=TransX0=TransY1=TransX1=0;
	   //GpuUnit.TCounter=0;
	}
	
   
  }
  else
  if ((Parent->IOC->DmaChannels[2].chcr & 0x01000200) == 0x01000200)
  {
   uint32_t vAddr=Parent->IOC->DmaChannels[2].madr&0x1fffff;
   uint32_t size=(Parent->IOC->DmaChannels[2].bcr >> 16) * (Parent->IOC->DmaChannels[2].bcr & 0xffff);

   TCounter*=2;

#ifdef DEBUG_GPU1
    _cprintf("GpuDma2::mem r transfer(%d) <%d %d %d %d> tc= %d no of pixels=%d\n",GpuUnit.WriteMode,GpuUnit.TransX0,GpuUnit.TransY0,GpuUnit.TransX1,GpuUnit.TransY1,GpuUnit.TCounter,GpuUnit.TransX1*GpuUnit.TransY1);
	_cprintf("GpuDma2::mem transfer size = %d no of pixels=%d\n",(dma[2].bcr >> 16) * (dma[2].bcr & 0xffff)*2,GpuUnit.TransX1*GpuUnit.TransY1);
#endif

	int x,y,c=0;
	if (ReadMode==GPU_RW_TRANSFER)
	{

	   for (y=0;y<TransY1;y++)
	   {
	    for (x=0;x<TransX1;x++)
	 	{
		  //psxRam->u16[(vAddr-GpuUnit.TCounter)>>1]=psxVRam->u16[((y)*1024+(x))];//*(dst_line++);
			//psxRam->u16[(vAddr-GpuUnit.TCounter)>>1]=GpuUnit.TransMem[y*1024+x];
			Parent->MC->Ram->u16[(vAddr-TCounter)>>1]=TransMem[y*1024+x];
	      vAddr+=2;
		}
	   }
	 
	   ReadMode=GPU_RW_IO;
	   TransY0=TransX0=TransY1=TransX1=0;
	   TCounter=0;
	   Status&=~GPUSTATUS_READYFORVRAM;
	}
	
  }
}


/*Create Empty List*/
void CGpu::Dma6()
{
	uint32_t *mem = &Parent->MC->Ram->u32[(Parent->IOC->DmaChannels[6].madr&0x1fffff)>>2];

    //mem+=((dma[6].madr&0x1fffff)>>2);
	if(Parent->IOC->DmaChannels[6].chcr == 0x11000002)
	{
		while(Parent->IOC->DmaChannels[6].bcr--)
		{
			*mem-- = (Parent->IOC->DmaChannels[6].madr - 4) & 0xffffff;
			Parent->IOC->DmaChannels[6].madr -= 4;
		}
		mem++; 
		*mem = 0xffffff;
	}
//	else
	//	_cprintf("dma6 %x\n",dma[6].chcr);
}