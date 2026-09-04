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

  // The running state of an XA-ADPCM stream: each block continues from where
  // the previous one left off, so this cannot be reset per sector.
  struct XaState {
    int32_t old[2];
    int32_t older[2];
    void Reset() { old[0] = old[1] = older[0] = older[1] = 0; }
  };

  // Decodes one sector's worth of sound groups - 0x900 bytes, eighteen groups
  // of 128 - into interleaved stereo at the rate the coding byte names.
  // Returns the number of stereo frames written, at most kXaFramesPerSector.
  // Mono is duplicated to both channels so callers need not care.
  //
  // Static and free of the rest of the drive so it can be exercised directly.
  static const int kXaFramesPerSector = 4032;
  static int DecodeXaAdpcm(const uint8_t* sound_groups, uint8_t coding,
                           XaState* state, int16_t* out);
  static int XaSampleRate(uint8_t coding) {
    return (coding & 0x04) ? 18900 : 37800;
  }
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
    // XA-ADPCM audio sectors taken by the decoder, and those a Setfilter
    // selection turned away.
    uint64_t xa_sectors;
    uint64_t xa_filtered;
    // CD-DA playback: sectors handed to the SPU, and sectors the drive was
    // asked for and could not get. Silence from the two looks identical.
    uint64_t cdda_sectors;
    uint64_t cdda_failures;
    uint8_t last_command;
    // Which commands were issued, and which interrupt kinds were delivered.
    // A drive that answers the wrong way and one that does not answer at all
    // look identical from the outside.
    uint32_t issued[256];
    uint32_t delivered[8];

    // The first few seeks and sector reads, so "what did it ask for and what
    // did it get" can be answered without a trace.
    struct Event {
      uint8_t kind;        // 0 setloc, 1 sector, 2 setmode, 3 command
      uint8_t mode;
      uint32_t lba;
      // For a sector: how much of the *previous* sector software had taken
      // before this one replaced it. Anything short of the whole sector means
      // a sector was dropped, which is how a stream loses its place.
      uint32_t consumed;
      uint32_t size;
    };
    static const int kEventCapacity = 4000;
    Event events[kEventCapacity];
    uint32_t event_count;
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
  uint8_t status_;
  // The lid "is or was" open. Latched on a swap and cleared by the first
  // status read once a disc is present again.
  bool shell_open_ = true;
  uint8_t StatusByte();                  // the byte Getstat returns
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
  // Whether the data fifo currently holds a sector. Arming it when it is
  // already loaded does nothing on hardware; only a fresh sector, or clearing
  // the request bit, unloads it.
  bool data_fifo_loaded_;

  // XA-ADPCM. `filter_*` is what Setfilter selected; the two histories are the
  // decoder state, which has to survive from one sector to the next because a
  // block continues where the last one stopped.
  uint8_t filter_file_;
  uint8_t filter_channel_;
  XaState xa_;

  // Drive state.
  uint32_t seek_lba_;               // set by Setloc, applied by a seek or read
  uint32_t read_lba_;               // where the next sector comes from
 public:
  // The sector the last delivered data came from, for checking that what a
  // game was handed is what is actually on the disc at that address.
  uint32_t delivered_lba() const { return read_lba_ == 0 ? 0 : read_lba_ - 1; }
 private:
  bool seek_pending_;
  bool reading_;
  bool playing_;
  uint8_t mode_;
  int32_t read_timer_;

  // Fast-forward and rewind during CD-DA play, set by the Forward and Backward
  // commands. Zero is ordinary play; positive skips forward, negative back,
  // and the magnitude grows each time the same command is sent again - which
  // is how the drive scans faster the longer a button is held. A Play command
  // clears it.
  int scan_rate_;

  Stats stats_;

  void ExecuteCommand(uint8_t command);
  void QueueResponse(Interrupt interrupt, int32_t delay,
                     const uint8_t* data, uint8_t length);
  void QueueStatus(Interrupt interrupt, int32_t delay);
  void QueueError(uint8_t error_code, int32_t delay);
  void DeliverPending();
  void StepRead(uint32_t cycles);
  void LoadSector();
  void GetPosition(uint8_t* data);   // 8 subchannel bytes, no status
  // The INT1 packet: status first, and one of the two times depending on
  // `relative` - the drive alternates between them.
  void GetReport(uint8_t* data, bool relative);

  uint8_t TakeParameter();
  void ClearParameters();

  // How long a sector takes at the current speed, in CPU cycles.
  int32_t SectorCycles() const;
};

}
}
