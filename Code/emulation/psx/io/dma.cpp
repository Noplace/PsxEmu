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
#include "..\system.h"

#define DMA_DEBUG

namespace emulation {
namespace psx {

Dma::Dma() {
  
}

Dma::~Dma() {

}

int Dma::Initialize() {
  memset(channels,0,sizeof(channels));
  dma_enable.raw = 0;
  interrupt_control.raw = 0;
  return 0;
}

void Dma::SetInterrupt(int channel) {
	if (interrupt_control.raw & (1 << (16 + channel))) {
		interrupt_control.raw |= (1 << (24 + channel)); 
    //system_->io().SetInterrupt(kInterruptDMA);
	}
}

void Dma::Tick() {
  if (interrupt_control.raw&0x7f000000) {
    system_->io().SetInterrupt(kInterruptDMA);
  }
}

uint32_t Dma::Read(uint32_t address) {
	switch (address)
	{
   case 0x1f801080: return channels[0].madr;
   case 0x1f801084: return channels[0].bcr;
   case 0x1f801088:	return channels[0].chcr;
   
   case 0x1f801090: return channels[1].madr;
   case 0x1f801094: return channels[1].bcr;
   case 0x1f801098:	return channels[1].chcr;
   
   case 0x1f8010a0: return channels[2].madr;
   case 0x1f8010a4: return channels[2].bcr;
   case 0x1f8010a8:	return channels[2].chcr;
   
   case 0x1f8010b0: return channels[3].madr;
   case 0x1f8010b4: return channels[3].bcr;
   case 0x1f8010b8: return channels[3].chcr;

   case 0x1f8010c0: return channels[4].madr;
   case 0x1f8010c4: return channels[4].bcr;
   case 0x1f8010c8:	return channels[4].chcr;
   
   case 0x1f8010d0: return channels[5].madr;
   case 0x1f8010d4: return channels[5].bcr;
   case 0x1f8010d8:	return channels[5].chcr;

   case 0x1f8010e0: return channels[6].madr;
   case 0x1f8010e4:	return channels[6].bcr;
   case 0x1f8010e8:	return channels[6].chcr;
   case 0x1f8010F0:	return dma_enable.raw;
   case 0x1f8010F4:	return interrupt_control.raw;
	   }
	
  BREAKPOINT
  return 0;
}


void Dma::Write(uint32_t address,uint32_t data) {

	switch (address)
	{
     case 0x1f801080:   channels[0].madr=data;  break;
     case 0x1f801084:   channels[0].bcr=data;  break;
     case 0x1f801088:  
	     if (!(channels[0].chcr&0x01000000)) { //&&channels[0].enable)	   {
		     channels[0].chcr=data;
	       channels[0].chcr&=0xfeffffff;  
         SetInterrupt(0);
	     }
	     break;

     case 0x1f801090:   channels[1].madr = data;  break;
     case 0x1f801094:   channels[1].bcr = data;  break;
     case 0x1f801098:  
	     if (!(channels[1].chcr&0x01000000)) {
		    channels[1].chcr=data; 
	      channels[1].chcr&=0xfeffffff; 
        SetInterrupt(1);
	     }
     break;
     
     case 0x1f8010a0:   channels[2].madr=data;  break;
     case 0x1f8010a4:   channels[2].bcr=data;  break;
     case 0x1f8010a8:  
      if (!(channels[2].chcr&0x01000000)) {
        channels[2].chcr=data;
        if (channels[2].chcr & 0x01000000) {// && channels[2].enable == true) { //dma_enable.raw & (8 << (2 * 4)))
            #if defined(DMA_DEBUG) && defined(_DEBUG)
            char str[255];
            sprintf(str,",,dma 2,chcr,0x%08x,bcr,0x%08x,madr,0x%08x\n",channels[2].chcr,channels[2].bcr,channels[2].madr);
            fprintf(system_->csvlog.fp,str);
            #endif
            Dma2();
        }
        channels[2].chcr&=0xfeffffff;  
        SetInterrupt(2);
      }
      break;

     case 0x1f8010b0:   channels[3].madr=data;  break;
     case 0x1f8010b4:   channels[3].bcr=data;  break;
     case 0x1f8010b8: 
      if (!(channels[3].chcr&0x01000000)) {
        channels[3].chcr=data;
        channels[3].chcr&=0xfeffffff;  
        SetInterrupt(3);
      }
      break;
  
     case 0x1f8010c0:   channels[4].madr=data;  break;
     case 0x1f8010c4:   channels[4].bcr=data;  break;
     case 0x1f8010c8:   
	     if (!(channels[4].chcr&0x01000000)) {
	      channels[4].chcr=data;  
	   	  //SpuDma4();
	      channels[4].chcr&=0xfeffffff;
	 	    SetInterrupt(4);
	     }
	     break;

     case 0x1f8010d0:   channels[5].madr=data;  break;
     case 0x1f8010d4:   channels[5].bcr=data;  break;
     case 0x1f8010d8:   
      if (!(channels[5].chcr&0x01000000))  {
        channels[5].chcr=data;
        channels[5].chcr&=0xfeffffff;  
        SetInterrupt(5);
      }
      break;

    case 0x1f8010e0:   channels[6].madr=data;  break;
    case 0x1f8010e4:   channels[6].bcr=data;  break;
    case 0x1f8010e8: 
	    if (!(channels[6].chcr&0x01000000)) {
	      channels[6].chcr=data;
	      if (channels[6].chcr & 0x01000000 && channels[6].enable == true) {
	        Dma6();
        }
	      channels[6].chcr&=0xfeffffff;
	    }
	    break;

 
    case 0x1F8010F0:
      dma_enable.raw = data;
      channels[0].enable=(data>>3)&0x1;
      channels[1].enable=(data>>7)&0x1;
      channels[2].enable=(data>>11)&0x1;
      channels[3].enable=(data>>15)&0x1;
      channels[4].enable=(data>>19)&0x1;
      channels[5].enable=(data>>23)&0x1;
      channels[6].enable=(data>>27)&0x1;
      //_cprintf("dma en:%x\n",data);
    break;

   case 0x1F8010F4: {
    interrupt_control.raw = data;
    //unsigned long tmp = (~data) & Parent->MC->HRam->u32[0x10f4>>2];
    //Parent->MC->HRam->u32[0x10f4>>2] = ((tmp ^ data) & 0xffffff) ^ tmp;
    //_ULong(REG_ICR) &= (~data)&0xff000000;
    }
    break;
  }
}

static uint32_t a1=0,a2=0,a3=0;
bool check_endless_loop(uint32_t address) {

  if(address==a2) return true;
  if(address==a3) return true;

  if(address<a1) 
    a2=address;
  else                   
    a3=address;
  a1=address;
  return false;
};

void Dma::Dma2()
{
  if ((channels[2].chcr & 0x01000401) == 0x01000401) { //chain
    auto& gpu = system_->gpu();
    auto& ram = system_->io().ram_buffer;
     
    
    gpu.status.busy = 0;


    uint32_t packet_count;
    uint32_t vaddress = channels[2].madr&0x1fffff;

    a1 = a2 = a3 = 0xFFFFFF;
    do {
      packet_count = ((ram.u32[vaddress>>2] >> 24) & 0xff);
      if (packet_count!=0) {
        #if defined(DMA_DEBUG) && defined(_DEBUG)
        char str[255];
        sprintf(str,",,dma gpu param_count,%d\n",packet_count);
        fprintf(system_->csvlog.fp,str);
        #endif
        auto mem = &ram.u32[(vaddress>>2)+1];
        for (uint32_t i=0;i<packet_count;++i) {
          gpu.WriteData(*mem++);
        }
        //if ((ram.u32[vaddress>>2]&0xffffff)==0xffffff) {
	      //  break;
        // }
        vaddress=ram.u32[vaddress>>2]&0xffffff;
      }	
      
      if (check_endless_loop(vaddress)) {
        #if defined(DMA_DEBUG) && defined(_DEBUG)
        char str[255];
        sprintf(str,",,dma endless loop detected\n");
        fprintf(system_->csvlog.fp,str);
        #endif
        break;
      } 
    } while (vaddress != 0xffffff);

    gpu.status.busy = 1;
  }
  if ((channels[2].chcr & 0x01000201) == 0x01000201) { 
    BREAKPOINT
  }
  if ((channels[2].chcr & 0x01000200) == 0x01000200) { 
    BREAKPOINT
  }
}


/*Create Empty List*/
void Dma::Dma6() {

	uint32_t *mem = &system_->io().ram_buffer.u32[(channels[6].madr&0x1fffff)>>2];

	if (channels[6].chcr == 0x11000002)	{
		while (channels[6].bcr--) {
			*mem-- = (channels[6].madr - 4) & 0xffffff;
			channels[6].madr -= 4;
		}
		mem++; 
		*mem = 0xffffff;
	}

}

}
}