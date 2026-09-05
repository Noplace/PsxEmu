/*****************************************************************************************************************
* Copyright (c) 2014 Khalid Ali Al-Kooheji                                                                       *
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
#pragma once

namespace emulation {
namespace psx {

struct DmaChannel{
 uint32_t madr;
 uint32_t bcr;
 uint32_t chcr;
 bool enable;
 // A completed transfer's cleanup - clearing the busy bit and raising the
 // interrupt - is deferred by this many cycles rather than happening within
 // the same write that triggered it. Zero means nothing pending. See
 // Dma::RunChannel and Dma::Tick.
 int32_t busy_cycles = 0;
 bool busy_acknowledge = false;
};

class Dma : public Component {
 public:
  Dma();
  ~Dma();
  int Initialize();
  void SetInterrupt(int channel);
  // Advances every channel's pending completion by this many cycles,
  // finishing (clearing the busy bit, raising the interrupt) whichever ones
  // run out. Called from IOInterface::RunPending with the same batch every
  // other device is ticked with, so a channel's busy window lines up with
  // real elapsed time rather than with how many times Write() happened to
  // be called.
  void Tick(uint32_t cycles);
  uint32_t Read(uint32_t address);
  void Write(uint32_t address,uint32_t data);
  DmaChannel& channel(int i) { return channels[i]; }
  // The first few transfers on each channel, for working out why one of them
  // landed somewhere it should not have.
  struct Transfer { uint32_t chcr, bcr, madr, words, end, lba, first, pc; };
  static const int kTransferCapacity = 3000;
  struct Stats {
    Transfer transfers[7][kTransferCapacity];
    uint32_t counts[7];
  };
  const Stats& stats() const { return stats_; }
  void NoteTransfer(int channel, uint32_t words, uint32_t end,
                    uint32_t lba = 0, uint32_t first = 0);
 private:

  // ---- how long a transfer takes ------------------------------------------
  //
  // A DMA is not free. It holds the bus for roughly a cycle a word, and the
  // CPU is stopped for that time while everything else keeps running. Making
  // it instantaneous - which it was - runs a game that moves a lot of data
  // faster than the hardware relative to its own timers, its CD and its SPU.
  //
  // The rate is the one DuckStation uses and is a model, not a measurement:
  // DRAM in page mode gives about one word per cycle, with one extra cycle
  // every sixteen words for the page boundary.
  static uint32_t RamCycles(uint32_t words) {
    return words + (words + 15) / 16;
  }
  // Accrued while a transfer runs, charged to the CPU once it finishes.
  uint32_t transfer_cycles_ = 0;
  void ChargeWords(uint32_t words) { transfer_cycles_ += RamCycles(words); }
  void ChargeCycles(uint32_t cycles) { transfer_cycles_ += cycles; }
  // Runs one channel's transfer - moving the data is still immediate, only
  // the busy bit and the interrupt are deferred, see Tick - and arms its
  // completion. `acknowledge` is false only for the OTC channel, which this
  // has never raised an interrupt for; that is left exactly as it was.
  void RunChannel(int channel, bool acknowledge = true);
  // Clears the busy bit and raises the interrupt, if this channel's
  // completion asked for one. The one place a pending transfer actually
  // finishes.
  void CompletePending(int channel);
  DmaChannel channels[7];
  Stats stats_ = {};
  union {
    struct {
      uint32_t unused;
    };
    uint32_t raw;
  } dma_enable;
  union {
    struct {
      uint32_t fast_dma0:1;
      uint32_t fast_dma1:1;
      uint32_t fast_dma2:1;
      uint32_t fast_dma3:1;
      uint32_t fast_dma4:1;
      uint32_t fast_dma5:1;
      uint32_t fast_dma6:1;
      uint32_t unused1:9;
      uint32_t enable_dma0_interrupt:1;
      uint32_t enable_dma1_interrupt:1;
      uint32_t enable_dma2_interrupt:1;
      uint32_t enable_dma3_interrupt:1;
      uint32_t enable_dma4_interrupt:1;
      uint32_t enable_dma5_interrupt:1;
      uint32_t enable_dma6_interrupt:1;
      uint32_t unused2:1;
      uint32_t acknowledge_dma0_interrupt:1;
      uint32_t acknowledge_dma1_interrupt:1;
      uint32_t acknowledge_dma2_interrupt:1;
      uint32_t acknowledge_dma3_interrupt:1;
      uint32_t acknowledge_dma4_interrupt:1;
      uint32_t acknowledge_dma5_interrupt:1;
      uint32_t acknowledge_dma6_interrupt:1;
      uint32_t unused3:1;
    };
    uint32_t raw;
  } interrupt_control;
  // Recomputes the master flag in bit 31 and raises the CPU interrupt on its
  // rising edge only. The interrupt is an edge, not a level: re-raising it
  // while the flag stays set floods the BIOS with an interrupt no handler
  // claims, and it gives up and unwinds.
  void UpdateMasterFlag();
  bool master_flag_;

  void Dma0();
  void Dma1();
  void Dma2();
  void Dma3();
  void Dma4();
  void Dma6();
};

}
}
