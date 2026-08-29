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
#pragma once

#include <deque>

namespace emulation {
namespace psx {

/*
  CD-ROM controller.

  Four registers at 0x1F801800, most of which mean different things depending
  on the index in the low two bits of the first one. The controller talks to
  software entirely through FIFOs and an interrupt: a command goes in with its
  parameters, and one or more responses come back later, each announced by an
  interrupt that software has to acknowledge before the next one is delivered.

  Responses are queued with a delay rather than produced immediately, because
  software waits on the interrupt and a controller that answered instantly
  would deadlock anything that sets up its handler after issuing the command.
*/
class Cdrom : public Component {
 public:
  // Interrupt kinds, as they appear in the low three bits of the flag register.
  enum Interrupt {
    kIntNone      = 0,
    kIntDataReady = 1,   // INT1 - a sector has arrived
    kIntComplete  = 2,   // INT2 - the command finished
    kIntAcknowledge = 3, // INT3 - the command was accepted
    kIntDataEnd   = 4,   // INT4 - the end of the track
    kIntError     = 5,   // INT5 - the command failed, or the lid was opened
  };

  // Status byte returned by nearly every command.
  enum StatusBits {
    kStatusError     = 0x01,
    kStatusMotorOn   = 0x02,
    kStatusSeekError = 0x04,
    kStatusIdError   = 0x08,
    kStatusShellOpen = 0x10,
    kStatusReading   = 0x20,
    kStatusSeeking   = 0x40,
    kStatusPlaying   = 0x80,
  };

  Cdrom();
  ~Cdrom();

  int Initialize();
  int Deinitialize();
  void Tick(uint32_t cycles);

  uint8_t Read(uint32_t address);
  void Write(uint32_t address, uint8_t data);

  // Sector delivery to DMA channel 3.
  bool data_available() const { return data_read_ < data_size_; }
  uint32_t ReadDataWord();

  // Mounting. Passing nullptr or an empty path ejects.
  bool OpenDisc(const char* path);
  void CloseDisc();
  bool disc_loaded() const { return disc_.loaded(); }
  Disc& disc() { return disc_; }

  // Tallies, for the headless harnesses.
  struct Stats {
    uint64_t commands;
    uint64_t sectors_read;
    uint64_t interrupts;
    uint64_t unknown_commands;
    uint8_t last_command;
  };
  const Stats& stats() const { return stats_; }

 private:
  // A response waiting to be handed over once software is ready for it.
  struct PendingResponse {
    int32_t delay;                  // in CPU cycles
    Interrupt interrupt;
    uint8_t data[16];
    uint8_t length;
  };

  Disc disc_;

  uint8_t index_;                   // low two bits of 0x1F801800
  uint8_t status_;                  // the byte Getstat returns
  uint8_t interrupt_enable_;
  uint8_t interrupt_flag_;

  std::deque<uint8_t> parameter_fifo_;
  std::deque<uint8_t> response_fifo_;
  std::deque<PendingResponse> pending_;

  // Sector buffer, and where DMA has got to in it.
  uint8_t sector_[Disc::kRawSectorSize];
  uint32_t data_offset_;            // where the useful bytes start
  uint32_t data_size_;
  uint32_t data_read_;

  // Drive state.
  uint32_t seek_lba_;               // set by Setloc, applied by a seek or read
  uint32_t read_lba_;               // where the next sector comes from
  bool seek_pending_;
  bool reading_;
  bool playing_;
  uint8_t mode_;
  int32_t read_timer_;

  Stats stats_;

  void ExecuteCommand(uint8_t command);
  void QueueResponse(Interrupt interrupt, int32_t delay,
                     const uint8_t* data, uint8_t length);
  void QueueStatus(Interrupt interrupt, int32_t delay);
  void QueueError(uint8_t error_code, int32_t delay);
  void DeliverPending();
  void StepRead(uint32_t cycles);
  void LoadSector();
  void GetReport(uint8_t* data);

  uint8_t TakeParameter();
  void ClearParameters();

  // How long a sector takes at the current speed, in CPU cycles.
  int32_t SectorCycles() const;
};

}
}
