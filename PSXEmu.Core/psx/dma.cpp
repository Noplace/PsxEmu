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
#include "psx/psx.h"

//#define DMA_DEBUG

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
  master_flag_ = false;
  return 0;
}

// A channel finished. Its flag latches only if that channel's interrupt is
// enabled; the master flag then follows from the flags and the master enable.
void Dma::SetInterrupt(int channel) {
  if (interrupt_control.raw & (1u << (16 + channel)))
    interrupt_control.raw |= (1u << (24 + channel));
  UpdateMasterFlag();
}

// Bit 31 is read-only and derived: the bus-error flag, or the master enable
// together with any latched channel flag that is also enabled.
void Dma::UpdateMasterFlag() {
  const uint32_t enabled = (interrupt_control.raw >> 16) & 0x7F;
  const uint32_t flagged = (interrupt_control.raw >> 24) & 0x7F;
  const bool master_enable = (interrupt_control.raw & 0x00800000) != 0;
  const bool bus_error = (interrupt_control.raw & 0x00008000) != 0;

  const bool flag = bus_error || (master_enable && (enabled & flagged) != 0);
  if (flag)
    interrupt_control.raw |= 0x80000000u;
  else
    interrupt_control.raw &= ~0x80000000u;

  // Only the rising edge reaches the CPU. Raising it on every tick while the
  // flag stays set meant that once any transfer had completed, I_STAT bit 3
  // was permanently on. The BIOS has no handler for it, so its handler chain
  // found nothing that would claim the interrupt, took its unhandled-exception
  // path, and unwound with a longjmp - leaving the Cop0 status register stuck
  // "inside an exception" so no interrupt was ever delivered again.
  if (flag && !master_flag_)
    system_->io().SetInterrupt(kInterruptDMA);
  master_flag_ = flag;
}

void Dma::Tick() {
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

// Whether a channel written with this CHCR should actually start.
//
// Two conditions, and channels 2, 3 and 4 used to check neither. The channel
// has to be enabled in DPCR - software sets a channel up while it is switched
// off and expects nothing to happen until it is switched on - and in burst
// mode the transfer only begins once the trigger in bit 28 is set as well as
// the enable in bit 24. Starting on the enable alone runs a transfer with
// whatever MADR and BCR happened to be there, which is how a CD read ended up
// writing a sector over a structure elsewhere in RAM.
bool ShouldStart(uint32_t chcr, bool channel_enabled) {
  if (!channel_enabled)
    return false;
  if ((chcr & 0x01000000) == 0)
    return false;
  const uint32_t sync = (chcr >> 9) & 3;
  if (sync == 0)
    return (chcr & 0x10000000) != 0;
  return true;
}


void Dma::Write(uint32_t address,uint32_t data) {

	switch (address)
	{
     case 0x1f801080:   channels[0].madr=data;  break;
     case 0x1f801084:   channels[0].bcr=data;  break;
     case 0x1f801088:
      channels[0].chcr = data;
      if (ShouldStart(channels[0].chcr, channels[0].enable)) {
        Dma0();
        channels[0].chcr &= 0xfeffffff;
        SetInterrupt(0);
      }
      break;

     case 0x1f801090:   channels[1].madr = data;  break;
     case 0x1f801094:   channels[1].bcr = data;  break;
     case 0x1f801098:
      channels[1].chcr = data;
      if (ShouldStart(channels[1].chcr, channels[1].enable)) {
        Dma1();
        channels[1].chcr &= 0xfeffffff;
        SetInterrupt(1);
      }
      break;
     
     case 0x1f8010a0:   channels[2].madr=data;  break;
     case 0x1f8010a4:   channels[2].bcr=data;  break;
     case 0x1f8010a8:  
      if (!(channels[2].chcr&0x01000000)) {
        channels[2].chcr=data;
        if (ShouldStart(channels[2].chcr, channels[2].enable)) {
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
      channels[3].chcr = data;
      if (ShouldStart(channels[3].chcr, channels[3].enable)) {
        Dma3();
        channels[3].chcr &= 0xfeffffff;
        SetInterrupt(3);
      }
      break;
  
     case 0x1f8010c0:   channels[4].madr=data;  break;
     case 0x1f8010c4:   channels[4].bcr=data;  break;
     case 0x1f8010c8:
      channels[4].chcr = data;
      if (ShouldStart(channels[4].chcr, channels[4].enable)) {
        Dma4();
        channels[4].chcr &= 0xfeffffff;
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
    // The channel flags in bits 24-30 and the bus-error flag in bit 15 are
    // write-one-to-clear; bit 31 is read-only and derived. Assigning the
    // written value wholesale meant an acknowledge never actually cleared
    // anything, so the flags stayed latched for the rest of the run.
    const uint32_t kWritable = 0x00FF803F;      // bits 0-5, 15, 16-23
    const uint32_t acknowledged = (data >> 24) & 0x7F;

    interrupt_control.raw =
        (interrupt_control.raw & ~kWritable) | (data & kWritable);
    interrupt_control.raw &= ~(acknowledged << 24);
    if (data & 0x00008000)
      interrupt_control.raw &= ~0x00008000u;    // bus error, also W1C

    UpdateMasterFlag();
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

// DMA channel 2 - the GPU.
//
// Three sync modes, and all three are used during a boot. Linked list walks a
// chain of display-list packets in RAM; block and burst move a straight run of
// words either way. Block mode is how image data - textures and colour lookup
// tables - reaches VRAM, and it used to be a BREAKPOINT stub, so every texture
// the BIOS uploaded that way simply never arrived. The primitives still drew,
// sampling an empty texture page, and every texel came back transparent.
// How many words a transfer moves, from the block control register and the
// sync mode the channel is running in.
//
// The two are not interchangeable. In burst mode the whole length is the low
// half of BCR and the upper half means nothing - games leave whatever was
// there last time. Multiplying by it regardless turns a one-sector CD read
// into a transfer hundreds of times too long, which walks straight over
// whatever the game had in RAM after the buffer. A zero field means the
// maximum, not nothing.
uint32_t TransferWords(uint32_t bcr, uint32_t sync) {
  uint32_t size = bcr & 0xFFFF;
  if (size == 0)
    size = 0x10000;
  if (sync != 1)
    return size;
  uint32_t blocks = (bcr >> 16) & 0xFFFF;
  if (blocks == 0)
    blocks = 0x10000;
  return size * blocks;
}

// MDEC in: compressed macroblocks out of RAM and into the decoder.
void Dma::Dma0() {
  auto& ram = system_->io().ram_buffer;
  auto& mdec = system_->io().mdec;

  const uint32_t words =
      TransferWords(channels[0].bcr, (channels[0].chcr >> 9) & 3);
  const int32_t step = (channels[0].chcr & 0x02) ? -4 : 4;
  uint32_t address = channels[0].madr & 0x1FFFFC;

  for (uint32_t i = 0; i < words; ++i) {
    mdec.WriteWord(ram.u32[address >> 2]);
    address = (address + step) & 0x1FFFFC;
  }
  NoteTransfer(0, words, address);
  channels[0].madr = address;
}

// MDEC out: decoded pixels back into RAM, for whatever is going to upload them
// to VRAM. The decoder holds one macroblock at a time, so a transfer longer
// than that reads zeroes once it runs dry rather than repeating the last one.
void Dma::Dma1() {
  auto& ram = system_->io().ram_buffer;
  auto& mdec = system_->io().mdec;

  const uint32_t words =
      TransferWords(channels[1].bcr, (channels[1].chcr >> 9) & 3);
  const int32_t step = (channels[1].chcr & 0x02) ? -4 : 4;
  uint32_t address = channels[1].madr & 0x1FFFFC;

  for (uint32_t i = 0; i < words; ++i) {
    const uint32_t word = mdec.ReadWord();
    system_->cpu().NoteExternalWrite(0xD1, address, word);
    ram.u32[address >> 2] = word;
    address = (address + step) & 0x1FFFFC;
  }
  NoteTransfer(1, words, address);
  channels[1].madr = address;
}

void Dma::Dma2() {
  const uint32_t chcr = channels[2].chcr;
  const uint32_t sync = (chcr >> 9) & 3;
  const bool from_ram = (chcr & 1) != 0;
  const int32_t step = (chcr & 2) ? -4 : 4;

  auto gpu = system_->gpu_core();
  auto& ram = system_->io().ram_buffer;

  if (sync == 2) {
    uint32_t address = channels[2].madr & 0x1FFFFF;
    uint32_t guard = 0;
    a1 = a2 = a3 = 0xFFFFFF;

    do {
      if (guard++ > 2000000)
        break;
      if (check_endless_loop(address))
        break;

      // Each node is a header word: a count in the top byte and the address of
      // the next node in the low 24 bits.
      const uint32_t header = ram.u32[(address & 0x1FFFFC) >> 2];
      const uint32_t count = (header >> 24) & 0xFF;

      for (uint32_t i = 0; i < count; ++i) {
        const uint32_t word_address = (address + 4 + i * 4) & 0x1FFFFC;
        gpu->WriteData(ram.u32[word_address >> 2]);
      }

      address = header & 0xFFFFFF;
    } while (address != 0xFFFFFF && (address & 0x800000) == 0);

    channels[2].madr = 0xFFFFFF;
    return;
  }

  // Burst and block are the same transfer; only where the length comes from
  // differs. A field of zero means the maximum, not nothing.
  uint32_t words;
  if (sync == 0) {
    words = channels[2].bcr & 0xFFFF;
    if (words == 0)
      words = 0x10000;
  } else {
    uint32_t block = channels[2].bcr & 0xFFFF;
    if (block == 0)
      block = 0x10000;
    uint32_t blocks = (channels[2].bcr >> 16) & 0xFFFF;
    if (blocks == 0)
      blocks = 1;
    words = block * blocks;
  }

  uint32_t address = channels[2].madr & 0x1FFFFC;
  for (uint32_t i = 0; i < words; ++i) {
    if (from_ram)
      gpu->WriteData(ram.u32[address >> 2]);
    else
      {
        const uint32_t word = gpu->ReadData();
        system_->cpu().NoteExternalWrite(0xD2, address, word);
        ram.u32[address >> 2] = word;
      }
    address = static_cast<uint32_t>(address + step) & 0x1FFFFC;
  }

  channels[2].madr = address;
}

void Dma::NoteTransfer(int channel, uint32_t words, uint32_t end,
                       uint32_t lba, uint32_t first) {
  // A ring, so what survives is the last few rather than the first few. When
  // a run ends in a crash the transfers that matter are the ones just before
  // it, and keeping the earliest instead means the log stops at the boot.
  const uint32_t n = stats_.counts[channel]++;
  {
    Transfer& t = stats_.transfers[channel][n % kTransferCapacity];
    t.chcr = channels[channel].chcr;
    t.bcr = channels[channel].bcr;
    t.madr = channels[channel].madr;
    t.words = words;
    t.end = end;
    t.lba = lba;
    t.first = first;
    t.pc = system_->cpu().context()->prev_pc;
  }
}



void Dma::Dma3() {
  auto& ram = system_->io().ram_buffer;
  auto& cdrom = system_->io().cdrom;
  uint32_t first_word = 0;

  const uint32_t words =
      TransferWords(channels[3].bcr, (channels[3].chcr >> 9) & 3);

  // Bit 1 of chcr picks the step direction: forwards or backwards.
  const int32_t step = (channels[3].chcr & 0x02) ? -4 : 4;
  uint32_t address = channels[3].madr & 0x1FFFFC;

  for (uint32_t i = 0; i < words; ++i) {
    {
      const uint32_t word = cdrom.ReadDataWord();
      if (i == 0)
        first_word = word;
      system_->cpu().NoteExternalWrite(0xD3, address, word);
      ram.u32[address >> 2] = word;
    }
    address = (address + step) & 0x1FFFFC;
  }
  NoteTransfer(3, words, address, cdrom.delivered_lba(), first_word);
  channels[3].madr = address;
}

/*Create Empty List*/
// Sound RAM, in either direction. Sample data is far too big to move a
// halfword at a time through the data port, so every game uploads it this way -
// which is why a channel that did nothing meant a machine with no sound.
void Dma::Dma4() {
  auto& ram = system_->io().ram_buffer;
  auto& spu = system_->spu();

  const uint32_t words =
      TransferWords(channels[4].bcr, (channels[4].chcr >> 9) & 3);

  const bool to_spu = (channels[4].chcr & 1) != 0;
  const int32_t step = (channels[4].chcr & 0x02) ? -4 : 4;
  uint32_t address = channels[4].madr & 0x1FFFFC;

  for (uint32_t i = 0; i < words; ++i) {
    if (to_spu)
      spu.WriteDataWord(ram.u32[address >> 2]);
    else
      {
        const uint32_t word = spu.ReadDataWord();
        system_->cpu().NoteExternalWrite(0xD4, address, word);
        ram.u32[address >> 2] = word;
      }
    address = static_cast<uint32_t>(address + step) & 0x1FFFFC;
  }
  NoteTransfer(4, words, address);
  channels[4].madr = address;
}

void Dma::Dma6() {
  // The ordering table is built backwards from the address in MADR: each entry
  // points at the one below it, and the last holds the end marker.
  //
  // This walked a raw pointer down through the RAM buffer with no bound and
  // used the whole of BCR as the count, so a block count left over in the top
  // half - or a count of zero, which means the maximum and underflowed to four
  // billion - ran off the front of the allocation. Every address is masked
  // into RAM now, and only the low half of BCR is the length.
  auto& ram = system_->io().ram_buffer;
  if (channels[6].chcr != 0x11000002)
    return;

  uint32_t words = channels[6].bcr & 0xFFFF;
  if (words == 0)
    words = 0x10000;

  uint32_t address = channels[6].madr & 0x1FFFFC;
  for (uint32_t i = 0; i < words; ++i) {
    // The final entry terminates the list; every other one points at its
    // predecessor, four bytes lower.
    const uint32_t next =
        (i + 1 == words) ? 0x00FFFFFF : ((address - 4) & 0x001FFFFF);
    system_->cpu().NoteExternalWrite(0xD6, address, next);
    ram.u32[address >> 2] = next;
    address = (address - 4) & 0x1FFFFC;
  }
  NoteTransfer(6, words, address);
  channels[6].madr = address;
}

}
}
