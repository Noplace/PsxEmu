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

#include <cstring>

namespace emulation {
namespace psx {

namespace {

// Roughly how long the real controller takes to answer. Exact values are not
// known and games do not depend on them, but the *shape* matters: an
// acknowledge comes back quickly, a seek or a spin-up takes far longer, and
// nothing may arrive before software has had a chance to install its handler.
const int32_t kAcknowledgeDelay = 20000;
const int32_t kSecondResponseDelay = 25000;
const int32_t kInitDelay = 120000;
const int32_t kSeekDelay = 400000;
const int32_t kGetIdDelay = 33868;

// One sector at single speed, in CPU cycles: 33868800 / 75.
const int32_t kSectorCyclesSingleSpeed = 451584;
// Fast-forward/rewind scan geometry. A scan level advances this many sectors
// per sector time instead of one, so level 1 is roughly eight times play
// speed, and the level climbs each time Forward or Backward is sent again.
const int kScanSectorsPerStep = 8;
const int kMaxScanRate = 4;

// Mode register bits.
const uint8_t kModeDoubleSpeed = 0x80;
const uint8_t kModeWholeSector = 0x20;   // 0x924 bytes rather than 0x800
const uint8_t kModeXaAdpcm     = 0x40;   // send XA-ADPCM sectors to the SPU
const uint8_t kModeXaFilter    = 0x08;   // and only those matching Setfilter

// Subheader submode bits, at sector offset 18.
const uint8_t kSubmodeAudio    = 0x04;
const uint8_t kSubmodeForm2    = 0x20;

}  // namespace

Cdrom::Cdrom() {
}

Cdrom::~Cdrom() {
}

int Cdrom::Initialize() {
  index_ = 0;
  // No disc and no motor until something says otherwise. The shell-open bit
  // starts set, which is what tells software the tray state is unknown.
  status_ = 0;
  shell_open_ = true;   // nothing in the drive until something is put there
  interrupt_enable_ = 0;
  interrupt_flag_ = 0;

  parameter_fifo_.clear();
  response_fifo_.clear();
  pending_.clear();

  memset(sector_, 0, sizeof(sector_));
  data_offset_ = 0;
  data_size_ = 0;
  data_read_ = 0;
  data_fifo_loaded_ = false;
  filter_file_ = 0;
  filter_channel_ = 0;
  xa_.Reset();

  seek_lba_ = Disc::kLeadInSectors;
  read_lba_ = Disc::kLeadInSectors;
  seek_pending_ = false;
  reading_ = false;
  playing_ = false;
  mode_ = 0;
  read_timer_ = 0;
  scan_rate_ = 0;

  memset(&stats_, 0, sizeof(stats_));

  if (disc_.loaded())
    status_ = kStatusMotorOn;
  return S_OK;
}

int Cdrom::Deinitialize() {
  disc_.Close();
  return S_OK;
}

bool Cdrom::OpenDisc(const char* path) {
  if (path == nullptr || path[0] == '\0') {
    CloseDisc();
    return true;
  }
  if (!disc_.Open(path))
    return false;
  // Swapping a disc is a lid opening and closing as far as software is
  // concerned, and it has to be told. Whatever was playing stops, and the
  // shell-open latch is set so the next status read says the disc may have
  // changed - see StatusByte.
  reading_ = false;
  playing_ = false;
  shell_open_ = true;
  status_ = kStatusMotorOn;
  seek_lba_ = Disc::kLeadInSectors;
  read_lba_ = Disc::kLeadInSectors;
  return true;
}

void Cdrom::CloseDisc() {
  disc_.Close();
  reading_ = false;
  playing_ = false;
  shell_open_ = true;
  status_ = 0;              // no disc, no motor
}

int32_t Cdrom::SectorCycles() const {
  return (mode_ & kModeDoubleSpeed) ? (kSectorCyclesSingleSpeed / 2)
                                    : kSectorCyclesSingleSpeed;
}

// ---------------------------------------------------------------------------
// Register interface
// ---------------------------------------------------------------------------

uint8_t Cdrom::Read(uint32_t address) {
  switch (address & 3) {
    case 0: {
      // Status: the index, plus how full the FIFOs are. Bit 4 says the
      // parameter FIFO can take more, bit 3 that it is empty, bit 5 that a
      // response is waiting, bit 6 that sector data is waiting.
      uint8_t value = index_ & 3;
      if (parameter_fifo_.empty())      value |= 0x08;
      if (parameter_fifo_.size() < 16)  value |= 0x10;
      if (!response_fifo_.empty())      value |= 0x20;
      if (data_available())             value |= 0x40;
      if (!pending_.empty())            value |= 0x80;   // busy
      return value;
    }

    case 1: {
      // Response FIFO. Reading past the end returns zero rather than blocking.
      if (response_fifo_.empty())
        return 0;
      const uint8_t value = response_fifo_.front();
      response_fifo_.pop_front();
      return value;
    }

    case 2: {
      // Data FIFO, a byte at a time. DMA uses ReadDataWord instead.
      if (!data_available())
        return 0;
      return sector_[data_offset_ + data_read_++];
    }

    default:
      // Index 0 and 2 read the enable register, 1 and 3 the flag register.
      if ((index_ & 1) == 0)
        return interrupt_enable_ | 0xE0;
      return interrupt_flag_ | 0xE0;
  }
}

void Cdrom::Write(uint32_t address, uint8_t data) {
  switch (address & 3) {
    case 0:
      index_ = data & 3;
      return;

    case 1:
      if (index_ == 0) {
        ExecuteCommand(data);
      }
      // Index 1-3 are the sound map registers, which nothing needs yet.
      return;

    case 2:
      if (index_ == 0) {
        if (parameter_fifo_.size() < 16)
          parameter_fifo_.push_back(data);
      } else if (index_ == 1) {
        interrupt_enable_ = data & 0x1F;
        if (interrupt_flag_ > 0) {
          uint8_t flag_bit = 1 << (interrupt_flag_ - 1);
          if (flag_bit & interrupt_enable_) {
            system().io().SetInterrupt(kInterruptCDROM);
          }
        }
      }
      return;

    default:
      if (index_ == 0) {
        // Request register. Bit 7 loads the data fifo with the current
        // sector; clearing it throws away what has not been read.
        //
        // Loading a fifo that is already loaded does nothing - the hardware
        // only reloads once bit 7 has been taken back to 0. Software reads a
        // sector in two pieces, the twelve-byte header and subheader first and
        // the payload after, and arms the fifo before each read. Rewinding on
        // the second arm handed it the header a second time where it expected
        // the payload, so every sector of a stream came through twelve bytes
        // out of step.
        if (data & 0x80) {
          if (!data_fifo_loaded_) {
            data_fifo_loaded_ = true;
            data_read_ = 0;
          }
        } else {
          data_fifo_loaded_ = false;
          data_read_ = data_size_;
        }
      } else if (index_ == 1) {
        // Acknowledging an interrupt clears the bits written, and frees the
        // controller to deliver whatever is queued behind it.
        interrupt_flag_ &= ~(data & 0x1F);
        if (interrupt_flag_ == 0)
          system().io().ClearInterrupt(kInterruptCDROM);
        if (data & 0x40)
          ClearParameters();
      }
      return;
  }
}

uint32_t Cdrom::ReadDataWord() {
  uint32_t value = 0;
  for (int i = 0; i < 4; ++i) {
    uint8_t byte = 0;
    if (data_available())
      byte = sector_[data_offset_ + data_read_++];
    value |= static_cast<uint32_t>(byte) << (i * 8);
  }
  return value;
}

uint8_t Cdrom::TakeParameter() {
  if (parameter_fifo_.empty())
    return 0;
  const uint8_t value = parameter_fifo_.front();
  parameter_fifo_.pop_front();
  return value;
}

void Cdrom::ClearParameters() {
  parameter_fifo_.clear();
}

// ---------------------------------------------------------------------------
// Responses
// ---------------------------------------------------------------------------


// The status byte as software sees it, which is not quite the live status.
//
// Bit 4 does not say "the lid is open", it says "the lid is or was open", and
// it stays set after the lid closes until something reads it. That latch is
// the only way software finds out a disc has been swapped: the BIOS CD player
// polls the status and re-reads the table of contents when it sees the bit.
// Without it a disc can be changed under software that has no reason to look
// again, and it goes on believing the tray is empty - which is exactly what
// choosing a track in the CD player and hearing nothing looks like.
uint8_t Cdrom::StatusByte() {
  uint8_t value = status_;
  if (shell_open_)
    value |= kStatusShellOpen;
  // Reading clears it, but only once there is a disc to read again. With the
  // tray genuinely empty the bit stays set, which is what it is for.
  if (shell_open_ && disc_.loaded())
    shell_open_ = false;
  return value;
}
void Cdrom::QueueResponse(Interrupt interrupt, int32_t delay,
                          const uint8_t* data, uint8_t length) {
  PendingResponse response;
  response.delay = delay;
  response.interrupt = interrupt;
  response.length = length;
  memset(response.data, 0, sizeof(response.data));
  if (data != nullptr && length > 0)
    memcpy(response.data, data, length > 16 ? 16 : length);
  pending_.push_back(response);
}

void Cdrom::QueueStatus(Interrupt interrupt, int32_t delay) {
  const uint8_t value = StatusByte();
  QueueResponse(interrupt, delay, &value, 1);
}

void Cdrom::QueueError(uint8_t error_code, int32_t delay) {
  const uint8_t data[2] = { static_cast<uint8_t>(StatusByte() | kStatusError),
                            error_code };
  QueueResponse(kIntError, delay, data, 2);
}

// A queued response is only delivered once software has acknowledged the one
// before it. Delivering regardless would lose responses that arrive in pairs,
// which is most of them.
void Cdrom::DeliverPending() {
  if (pending_.empty() || interrupt_flag_ != 0)
    return;

  PendingResponse& response = pending_.front();
  if (response.delay > 0)
    return;

  response_fifo_.clear();
  for (uint8_t i = 0; i < response.length; ++i)
    response_fifo_.push_back(response.data[i]);

  interrupt_flag_ = static_cast<uint8_t>(response.interrupt);
  ++stats_.interrupts;
  ++stats_.delivered[response.interrupt & 7];
  pending_.pop_front();

  if (interrupt_flag_ > 0) {
    uint8_t flag_bit = 1 << (interrupt_flag_ - 1);
    if (flag_bit & interrupt_enable_) {
      system().io().SetInterrupt(kInterruptCDROM);
    }
  }
}

void Cdrom::Tick(uint32_t cycles) {
  // Only the response at the head of the queue is counting down; the ones
  // behind it have not been issued yet as far as software is concerned.
  // Furthermore, only count down if the controller is not waiting for an acknowledge.
  if (!pending_.empty() && interrupt_flag_ == 0 && pending_.front().delay > 0)
    pending_.front().delay -= static_cast<int32_t>(cycles);

  StepRead(cycles);
  DeliverPending();
}

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------

// Where the head is, as the subchannel reports it: the track and index it is
// inside, the time since that track started, and the time since the start of
// the disc. Eight bytes, all BCD.
//
// This is what GetlocP answers with, and there is no status byte in front of
// it. Putting one there shifts every field along by one, so software reads
// the status as the track number, the track number as the index, and a time
// that is one byte out of step - which is why the BIOS CD player could list
// the tracks off a disc and then sit at 00:00 for ever when told to play one.
void Cdrom::GetPosition(uint8_t* data) {
  uint8_t absolute_minute, absolute_second, absolute_frame;
  Disc::LbaToMsf(read_lba_, &absolute_minute, &absolute_second,
                 &absolute_frame);

  uint8_t current_track = 1;
  uint8_t index = 1;
  uint32_t track_start = Disc::kLeadInSectors;

  for (int i = 0; i < disc_.track_count(); ++i) {
    const Disc::Track& t = disc_.track(i);
    if (read_lba_ >= t.start_lba && read_lba_ < t.start_lba + t.length) {
      current_track = static_cast<uint8_t>(t.number);
      track_start = t.start_lba;
      break;
    }
  }

  uint8_t relative_minute, relative_second, relative_frame;
  if (read_lba_ >= track_start) {
    Disc::LbaToMsf(read_lba_ - track_start, &relative_minute,
                   &relative_second, &relative_frame);
  } else {
    relative_minute = relative_second = relative_frame = 0;
  }

  data[0] = Disc::ToBcd(current_track);
  data[1] = Disc::ToBcd(index);
  data[2] = relative_minute;
  data[3] = relative_second;
  data[4] = relative_frame;
  data[5] = absolute_minute;
  data[6] = absolute_second;
  data[7] = absolute_frame;
}

// The unsolicited position packet the drive sends while playing audio, which
// software turns on with mode bit 2.
//
// Same information as GetlocP, but this one carries the status first and only
// one of the two times, and it ends with a peak level rather than the third
// byte of the other. Which time it carries is not a separate field: bit 7 of
// the seconds byte says so, and software that wants both waits for both to
// come round.
void Cdrom::GetReport(uint8_t* data, bool relative) {
  uint8_t position[8];
  GetPosition(position);
  data[0] = StatusByte();
  data[1] = position[0];                 // track
  data[2] = position[1];                 // index
  if (relative) {
    data[3] = position[2];               // minute within the track
    data[4] = position[3] | 0x80;        // bit 7: this is the track time
    data[5] = position[4];
  } else {
    data[3] = position[5];               // minute from the start of the disc
    data[4] = position[6];
    data[5] = position[7];
  }
  // The peak level the signal processor would have measured. Nothing here
  // measures it, and only a VU meter would miss it.
  data[6] = 0;
  data[7] = 0;
}
// ---------------------------------------------------------------------------
// XA-ADPCM
// ---------------------------------------------------------------------------

namespace {

// The four filters XA uses. The SPU's own ADPCM has five; this is not that
// table and the fifth entry does not belong here.
const int32_t kXaFilterPositive[4] = { 0, 60, 115, 98 };
const int32_t kXaFilterNegative[4] = { 0, 0, -52, -55 };

int16_t ClampToSample(int32_t value) {
  if (value < -32768)
    return -32768;
  if (value > 32767)
    return 32767;
  return static_cast<int16_t>(value);
}

}  // namespace

// One sector of XA-ADPCM: eighteen sound groups of 128 bytes, each sixteen
// parameter bytes and 112 bytes of packed samples.
//
// The parameter bytes are stored twice over, which is a disc format hedging
// against a read error: 00h-03h repeats 04h-07h, and 0Ch-0Fh repeats 08h-0Bh.
// The originals are the second of each pair, so blocks 0 to 7 take their
// parameters from bytes 04h onwards.
//
// In four-bit mode a group holds eight blocks of 28 samples, one nibble of
// each 32-bit word per block; in eight-bit mode, four blocks, one byte per
// block. In stereo the blocks alternate left and right, which is why the
// filter history is a pair.
int Cdrom::DecodeXaAdpcm(const uint8_t* sound_groups, uint8_t coding,
                         XaState* state, int16_t* out) {
  const bool stereo = (coding & 0x01) != 0;
  const bool eight_bit = (coding & 0x10) != 0;
  const int blocks_per_group = eight_bit ? 4 : 8;

  int frames = 0;

  for (int group = 0; group < 18; ++group) {
    const uint8_t* base = sound_groups + group * 128;
    const uint8_t* data = base + 16;

    for (int block = 0; block < blocks_per_group; ++block) {
      const uint8_t parameter = base[4 + block];
      const int32_t shift = parameter & 0x0F;
      const int32_t filter = (parameter >> 4) & 0x03;

      // A shift above 12 is not a valid encoding. The hardware behaves as if
      // it were 9; software does not produce them, but a scratched disc can.
      const int32_t shift_amount = (shift > 12) ? 9 : shift;

      // In stereo the even blocks are the left channel and the odd ones the
      // right, and each keeps its own history. In mono there is one history
      // and both output channels get the same sample.
      const int channel = stereo ? (block & 1) : 0;

      for (int sample = 0; sample < 28; ++sample) {
        const uint32_t word =
            static_cast<uint32_t>(data[sample * 4]) |
            (static_cast<uint32_t>(data[sample * 4 + 1]) << 8) |
            (static_cast<uint32_t>(data[sample * 4 + 2]) << 16) |
            (static_cast<uint32_t>(data[sample * 4 + 3]) << 24);

        // Put the sample in the top of a 16-bit value and shift it down, which
        // is what "shift" means here: zero is loudest.
        int32_t value;
        if (eight_bit) {
          const int8_t byte = static_cast<int8_t>((word >> (block * 8)) & 0xFF);
          value = static_cast<int32_t>(byte) << 8;
        } else {
          const uint32_t nibble = (word >> (block * 4)) & 0x0F;
          value = static_cast<int32_t>(static_cast<int16_t>(nibble << 12));
        }
        value >>= shift_amount;

        // The filter carries the previous two outputs forward.
        const int32_t old = state->old[channel];
        const int32_t older = state->older[channel];
        const int32_t filtered =
            value + ((old * kXaFilterPositive[filter] +
                      older * kXaFilterNegative[filter] + 32) / 64);
        const int16_t result = ClampToSample(filtered);

        state->older[channel] = old;
        state->old[channel] = result;

        if (stereo) {
          // A stereo pair is only complete once its right-hand block has been
          // decoded, so the left channel writes and the right one fills in.
          if (channel == 0) {
            out[(frames + sample) * 2] = result;
          } else {
            out[(frames + sample) * 2 + 1] = result;
          }
        } else {
          out[(frames + sample) * 2] = result;
          out[(frames + sample) * 2 + 1] = result;
        }
      }

      // A mono block is 28 finished frames. A stereo pair of blocks is 28
      // frames between them, counted once the right-hand one is done.
      if (!stereo || (block & 1) == 1)
        frames += 28;
    }
  }

  return frames;
}


void Cdrom::LoadSector() {
  if (!disc_.ReadSector(read_lba_, sector_)) {
    QueueError(0x04, kAcknowledgeDelay);
    reading_ = false;
    playing_ = false;
    return;
  }
  // An audio sector belongs to the ADPCM decoder, not to software.
  //
  // On a disc carrying full-motion video the audio is interleaved with the
  // video in the same track, one sector of sound every so many of picture. The
  // drive keeps them apart: an audio sector goes to the SPU and raises no
  // data-ready interrupt at all, so what software reads is an unbroken run of
  // video. Handing it every sector puts compressed audio in the middle of the
  // video stream, which looks like a broken video decoder and is not.
  const uint8_t submode = sector_[18];
  if ((submode & kSubmodeAudio) != 0 && (mode_ & kModeXaAdpcm) != 0) {
    const uint8_t file = sector_[16];
    const uint8_t channel = sector_[17];
    const bool wanted = (mode_ & kModeXaFilter) == 0 ||
                        (file == filter_file_ && channel == filter_channel_);
    if (wanted) {
      const uint8_t coding = sector_[19];
      static int16_t decoded[kXaFramesPerSector * 2];
      const int frames = DecodeXaAdpcm(sector_ + 24, coding, &xa_, decoded);
      system().spu().QueueCdSamples(decoded, frames, XaSampleRate(coding));
      ++stats_.xa_sectors;
    } else {
      ++stats_.xa_filtered;
    }
    // Either way software never sees it, and the next sector is already on its
    // way, so nothing here touches the data fifo or queues an interrupt.
    ++read_lba_;
    return;
  }


  // How much of the sector this one replaces had actually been taken. A short
  // count means software never read the last sector before the drive handed
  // over the next, and on a stream that is a lost packet.
  const uint32_t consumed_previous = data_read_;
  const uint32_t previous_size = data_size_;

  // Whole-sector mode hands over the 0x924 bytes that start at the subheader;
  // otherwise it is the 0x800 bytes of user data in a Mode 2 Form 1 sector.
  if (mode_ & kModeWholeSector) {
    data_offset_ = 12;
    data_size_ = 0x924;
  } else {
    data_offset_ = 24;
    data_size_ = 0x800;
  }
  data_read_ = 0;
  // A fresh sector needs arming again before software can read it.
  data_fifo_loaded_ = false;
  ++stats_.sectors_read;
  {
    Stats::Event& e = stats_.events[stats_.event_count++ % Stats::kEventCapacity];
    e.kind = 1; e.mode = mode_; e.lba = read_lba_;
    e.consumed = consumed_previous; e.size = previous_size;
  }

  QueueStatus(kIntDataReady, 0);
  ++read_lba_;
}

void Cdrom::StepRead(uint32_t cycles) {
  if (!reading_ && !playing_)
    return;

  read_timer_ -= static_cast<int32_t>(cycles);
  if (read_timer_ > 0)
    return;
  read_timer_ += SectorCycles();

  if (playing_) {
    if (!disc_.ReadSector(read_lba_, sector_)) {
      ++stats_.cdda_failures;
      QueueError(0x04, kAcknowledgeDelay);
      reading_ = false;
      playing_ = false;
      scan_rate_ = 0;
      return;
    }
    ++stats_.cdda_sectors;
    system().spu().QueueCdAudio(sector_);

    // Report mode. The drive does not report on every sector - that would be
    // seventy-five interrupts a second - but on eight of the seventy-five,
    // alternating between the time from the start of the disc and the time
    // within the track. Which eight is not arbitrary: software reads the
    // pattern off the absolute frame number, so the phase matters as much as
    // the rate.
    if ((mode_ & 0x04) && pending_.empty()) {
      const uint8_t absolute_frame =
          Disc::ToBcd(static_cast<uint8_t>(read_lba_ % 75));
      if ((absolute_frame & 0x0F) == 0) {
        uint8_t data[8];
        GetReport(data, (absolute_frame & 0x10) != 0);
        QueueResponse(kIntDataReady, 0, data, 8);
      }
    }

    // Advance. Ordinary play steps one sector; a scan skips a block, and the
    // two ends of the disc are where scanning ends: forward off the last track
    // stops the motor, backward to the first track drops into ordinary play,
    // which is what a real drive does with a held scan button.
    if (scan_rate_ == 0) {
      ++read_lba_;
      if (read_lba_ >= disc_.total_sectors()) {
        playing_ = false;
        status_ &= ~kStatusPlaying;
        QueueStatus(kIntDataEnd, 0);  // INT4
      }
    } else if (scan_rate_ > 0) {
      read_lba_ += static_cast<uint32_t>(scan_rate_ * kScanSectorsPerStep);
      if (read_lba_ >= disc_.total_sectors()) {
        read_lba_ = disc_.total_sectors();
        playing_ = false;
        scan_rate_ = 0;
        status_ = disc_.loaded() ? kStatusMotorOn : 0;
        QueueStatus(kIntDataEnd, 0);  // INT4
      }
    } else {
      const uint32_t back =
          static_cast<uint32_t>(-scan_rate_ * kScanSectorsPerStep);
      const uint32_t track1_start =
          disc_.track_count() > 0 ? disc_.track(0).start_lba
                                  : Disc::kLeadInSectors;
      if (read_lba_ <= track1_start + back) {
        read_lba_ = track1_start;
        scan_rate_ = 0;             // reached the start; ordinary play resumes
      } else {
        read_lba_ -= back;
      }
    }
    return;
  }

  // Do not stack sectors up behind an unacknowledged one; the real drive would
  // simply overwrite its buffer, and queuing without bound is worse.
  if (!pending_.empty())
    return;

  LoadSector();
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

void Cdrom::ExecuteCommand(uint8_t command) {
  ++stats_.commands;
  stats_.last_command = command;
  ++stats_.issued[command];
  // The commands that move the head or start and stop a read, in sequence with
  // the sectors, so a stream can be read as a story rather than a total.
  if (command == 0x06 || command == 0x09 || command == 0x15 || command == 0x03 ||
      command == 0x16 || command == 0x04 || command == 0x05 || command == 0x12 ||
      command == 0x1C ||
      command == 0x1B || command == 0x0A || command == 0x08) {
    {
      Stats::Event& e = stats_.events[stats_.event_count++ % Stats::kEventCapacity];
      e.kind = 3; e.mode = command; e.lba = read_lba_;
      e.consumed = data_read_; e.size = data_size_;
    }
  }


  switch (command) {
    case 0x01:  // Getstat
      QueueStatus(kIntAcknowledge, kAcknowledgeDelay);
      break;

    case 0x02: {  // Setloc - remembers a position, does not move the head
      const uint8_t minute = TakeParameter();
      const uint8_t second = TakeParameter();
      const uint8_t frame = TakeParameter();
      seek_lba_ = Disc::MsfToLba(minute, second, frame);
      seek_pending_ = true;
      {
        Stats::Event& e = stats_.events[stats_.event_count++ % Stats::kEventCapacity];
        e.kind = 0; e.mode = mode_; e.lba = seek_lba_;
      }
      QueueStatus(kIntAcknowledge, kAcknowledgeDelay);
      break;
    }

    case 0x03: {  // Play
      if (!disc_.loaded()) {
        QueueError(0x80, kAcknowledgeDelay);
        break;
      }
      // The parameter, when there is one, is a track number in BCD - but
      // zero is not a track, it means "carry on from wherever the head is",
      // and the BIOS CD player sends exactly that every frame while a track
      // is playing. Treating it as an out of range track answered every one
      // of them with an error, the player retried for ever, and choosing a
      // track in it did nothing at all.
      bool play_from_here = true;
      if (!parameter_fifo_.empty()) {
        const uint8_t track = Disc::FromBcd(TakeParameter());
        if (track != 0) {
          if (track > disc_.track_count()) {
            QueueError(0x10, kAcknowledgeDelay);
            break;
          }
          seek_lba_ = disc_.track(track - 1).start_lba;
          play_from_here = false;
        }
      }
      if (play_from_here && !seek_pending_)
        seek_lba_ = read_lba_;
      seek_pending_ = false;
      read_lba_ = seek_lba_;
      reading_ = false;
      playing_ = true;
      scan_rate_ = 0;   // a Play ends any fast-forward or rewind in progress
      status_ = (status_ & ~kStatusReading) | kStatusPlaying | kStatusMotorOn;
      read_timer_ = SectorCycles();
      QueueStatus(kIntAcknowledge, kAcknowledgeDelay);
      break;
    }

    case 0x04:    // Forward - fast-forward scan, only while a track is playing
    case 0x05: {  // Backward - fast rewind
      if (playing_) {
        // Each press bumps the scan one level in its direction, starting from
        // a standstill if the last scan was the other way. A held button
        // sends the command over and over, so the drive scans ever faster -
        // and a Play, which the UI sends on release, drops it back to normal.
        const int direction = (command == 0x04) ? +1 : -1;
        if ((direction > 0 && scan_rate_ < 0) ||
            (direction < 0 && scan_rate_ > 0))
          scan_rate_ = 0;
        scan_rate_ += direction;
        if (scan_rate_ > kMaxScanRate) scan_rate_ = kMaxScanRate;
        if (scan_rate_ < -kMaxScanRate) scan_rate_ = -kMaxScanRate;
      }
      // The drive answers with its status whether or not it was playing;
      // sending Forward to a stopped drive is simply a no-op that still acks.
      QueueStatus(kIntAcknowledge, kAcknowledgeDelay);
      break;
    }

    case 0x06:    // ReadN
    case 0x1B: {  // ReadS
      if (!disc_.loaded()) {
        QueueError(0x80, kAcknowledgeDelay);
        break;
      }
      if (seek_pending_) {
        read_lba_ = seek_lba_;
        seek_pending_ = false;
      }
      reading_ = true;
      playing_ = false;
      status_ = kStatusMotorOn | kStatusReading;
      read_timer_ = SectorCycles();
      QueueStatus(kIntAcknowledge, kAcknowledgeDelay);
      break;
    }

    case 0x07:    // MotorOn
      status_ |= kStatusMotorOn;
      QueueStatus(kIntAcknowledge, kAcknowledgeDelay);
      QueueStatus(kIntComplete, kSecondResponseDelay);
      break;

    case 0x08:    // Stop
      reading_ = false;
      playing_ = false;
      scan_rate_ = 0;
      status_ = 0;
      QueueStatus(kIntAcknowledge, kAcknowledgeDelay);
      QueueStatus(kIntComplete, kInitDelay);
      break;

    case 0x09:    // Pause
      reading_ = false;
      playing_ = false;
      scan_rate_ = 0;
      status_ &= ~(kStatusReading | kStatusPlaying | kStatusSeeking);
      QueueStatus(kIntAcknowledge, kAcknowledgeDelay);
      QueueStatus(kIntComplete, kSecondResponseDelay);
      break;

    case 0x0A:    // Init
      mode_ = 0;
      reading_ = false;
      playing_ = false;
      scan_rate_ = 0;
      status_ = disc_.loaded() ? kStatusMotorOn : 0;
      parameter_fifo_.clear();
      QueueStatus(kIntAcknowledge, kAcknowledgeDelay);
      QueueStatus(kIntComplete, kInitDelay);
      break;

    case 0x0B:    // Mute
    case 0x0C:    // Demute
      QueueStatus(kIntAcknowledge, kAcknowledgeDelay);
      break;

    case 0x0D:    // Setfilter
      // Which interleaved stream to listen to. A disc carries several XA
      // channels in the one track and software picks one; with the filter off
      // in Setmode, every audio sector is taken regardless.
      filter_file_ = TakeParameter();
      filter_channel_ = TakeParameter();
      QueueStatus(kIntAcknowledge, kAcknowledgeDelay);
      break;

    case 0x0E:    // Setmode
      mode_ = TakeParameter();
      {
        Stats::Event& e = stats_.events[stats_.event_count++ % Stats::kEventCapacity];
        e.kind = 2; e.mode = mode_; e.lba = 0;
        e.consumed = 0; e.size = 0;
      }
      QueueStatus(kIntAcknowledge, kAcknowledgeDelay);
      break;

    case 0x0F: {  // Getparam
      // Returns current parameters: stat, mode, file, channel, sm
      // We don't fully track file/channel/sm from Setfilter yet, so return 0s
      const uint8_t data[5] = { StatusByte(), mode_, 0, 0, 0 };
      QueueResponse(kIntAcknowledge, kAcknowledgeDelay, data, 5);
      break;
    }

    case 0x10: {  // GetlocL
      // Returns sector header/subheader: stat, min, sec, frame, mode, file, channel, sm
      const uint8_t data[8] = {
        StatusByte(),
        sector_[12], sector_[13], sector_[14], sector_[15],
        sector_[16], sector_[17], sector_[18]
      };
      QueueResponse(kIntAcknowledge, kAcknowledgeDelay, data, 8);
      break;
    }

    case 0x11: {  // GetlocP - where the head is, in track and disc terms
      uint8_t data[8];
      GetPosition(data);
      QueueResponse(kIntAcknowledge, kAcknowledgeDelay, data, 8);
      break;
    }

    case 0x13: {  // GetTN - first and last track numbers
      if (!disc_.loaded()) {
        QueueError(0x80, kAcknowledgeDelay);
        break;
      }
      const uint8_t data[3] = {
        StatusByte(),
        Disc::ToBcd(1),
        Disc::ToBcd(static_cast<uint8_t>(disc_.track_count()))
      };
      QueueResponse(kIntAcknowledge, kAcknowledgeDelay, data, 3);
      break;
    }

    case 0x14: {  // GetTD - where a track starts
      if (!disc_.loaded()) {
        QueueError(0x80, kAcknowledgeDelay);
        break;
      }
      const uint8_t requested = Disc::FromBcd(TakeParameter());
      uint32_t lba = disc_.total_sectors();
      if (requested >= 1 && requested <= disc_.track_count())
        lba = disc_.track(requested - 1).start_lba;
      uint8_t minute, second, frame;
      Disc::LbaToMsf(lba, &minute, &second, &frame);
      const uint8_t data[4] = { StatusByte(), minute, second, frame };
      QueueResponse(kIntAcknowledge, kAcknowledgeDelay, data, 4);
      break;
    }

    case 0x15:    // SeekL - seek using the data header
    case 0x16: {  // SeekP - seek using the subchannel
      if (!disc_.loaded()) {
        QueueError(0x80, kAcknowledgeDelay);
        break;
      }
      // A seek stops whatever was being read. Leaving the read running let it
      // deliver one more sector *after* the seek had moved the head, which
      // advanced the position past the seek target - so the read that followed
      // began one sector late. A game loading an executable that way gets its
      // header cut off and every field it reads out of it is one sector of
      // rubbish.
      reading_ = false;
      playing_ = false;
      scan_rate_ = 0;
      read_lba_ = seek_lba_;
      seek_pending_ = false;
      status_ = kStatusMotorOn | kStatusSeeking;
      QueueStatus(kIntAcknowledge, kAcknowledgeDelay);
      status_ = kStatusMotorOn;
      QueueStatus(kIntComplete, kSeekDelay);
      break;
    }

    case 0x12: {  // SetSession - move to a session on a multi-session disc
      if (!disc_.loaded()) {
        QueueError(0x80, kAcknowledgeDelay);
        break;
      }
      const uint8_t session = TakeParameter();
      reading_ = false;
      playing_ = false;
      scan_rate_ = 0;
      if (session == 1) {
        // The one session every game disc has. Move to its start and answer
        // like a seek: acknowledge, then complete once the head is there.
        seek_lba_ = Disc::kLeadInSectors;
        read_lba_ = seek_lba_;
        status_ = kStatusMotorOn | kStatusSeeking;
        QueueStatus(kIntAcknowledge, kAcknowledgeDelay);
        status_ = kStatusMotorOn;
        QueueStatus(kIntComplete, kSeekDelay);
      } else {
        // These images are single-session, so any other session number is a
        // seek to somewhere that is not there. Acknowledge, then fail.
        status_ = kStatusMotorOn;
        QueueStatus(kIntAcknowledge, kAcknowledgeDelay);
        const uint8_t data[2] = {
          static_cast<uint8_t>(StatusByte() | kStatusError | kStatusSeekError),
          0x40   // seek failed
        };
        QueueResponse(kIntError, kSeekDelay, data, 2);
      }
      break;
    }

    case 0x19: {  // Test
      const uint8_t sub = TakeParameter();
      if (sub == 0x20) {
        // Controller firmware date and version. This particular set is what a
        // retail SCPH-1001 reports, and some software checks it.
        const uint8_t data[4] = { 0x94, 0x09, 0x19, 0xC0 };
        QueueResponse(kIntAcknowledge, kAcknowledgeDelay, data, 4);
      } else {
        QueueStatus(kIntAcknowledge, kAcknowledgeDelay);
      }
      break;
    }

    case 0x1A: {  // GetID - what is in the drive
      QueueStatus(kIntAcknowledge, kAcknowledgeDelay);
      if (!disc_.loaded()) {
        // "No disc". This is the answer the BIOS shell is waiting for when the
        // tray is empty, and without it the boot never leaves its timeout.
        const uint8_t data[8] = { 0x08, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
        QueueResponse(kIntError, kGetIdDelay, data, 8);
      } else {
        // A licensed disc: region "SCEA" here, with the flags that mean an
        // ordinary game disc.
        const uint8_t data[8] = { 0x02, 0x00, 0x20, 0x00, 'S', 'C', 'E', 'A' };
        QueueResponse(kIntComplete, kGetIdDelay, data, 8);
      }
      break;
    }

    case 0x1C:    // Reset - reboot the drive controller
      // A power-on in a command: the mode goes back to zero, anything in
      // flight is abandoned, and the head returns to the start with the motor
      // spun up if a disc is in. The controller is briefly unresponsive on
      // hardware; here it simply comes back reset. Answered like Init so
      // software waiting on the completion is not left hanging.
      mode_ = 0;
      reading_ = false;
      playing_ = false;
      scan_rate_ = 0;
      seek_pending_ = false;
      seek_lba_ = Disc::kLeadInSectors;
      read_lba_ = Disc::kLeadInSectors;
      status_ = disc_.loaded() ? kStatusMotorOn : 0;
      QueueStatus(kIntAcknowledge, kAcknowledgeDelay);
      QueueStatus(kIntComplete, kInitDelay);
      break;

    case 0x1D: {  // GetQ - read one subchannel Q entry from the table of
                  // contents. Almost nothing uses it, but a full answer is
                  // cheaper than a plausible-looking wrong one: the TOC is the
                  // track table, and that is exactly what this returns.
      if (!disc_.loaded()) {
        QueueError(0x80, kAcknowledgeDelay);
        break;
      }
      const uint8_t adr = TakeParameter();     // subchannel adr, normally 1
      const uint8_t point = TakeParameter();   // which track, in BCD
      QueueStatus(kIntAcknowledge, kAcknowledgeDelay);

      // Ten bytes of subchannel Q: the control/adr byte, the track and index,
      // a zeroed track-relative time this synthesises no better than zero, and
      // the track's absolute start in minutes, seconds and frames.
      uint8_t q[10];
      memset(q, 0, sizeof(q));
      q[0] = (adr & 0x0F) ? adr : 0x01;
      q[1] = point;
      q[2] = Disc::ToBcd(1);
      const uint8_t track = Disc::FromBcd(point);
      if (track >= 1 && track <= disc_.track_count()) {
        uint8_t amin, asec, aframe;
        Disc::LbaToMsf(disc_.track(track - 1).start_lba, &amin, &asec,
                       &aframe);
        q[7] = amin;
        q[8] = asec;
        q[9] = aframe;
      }
      QueueResponse(kIntComplete, kSeekDelay, q, 10);
      break;
    }

    case 0x1E:    // GetTOC
      QueueStatus(kIntAcknowledge, kAcknowledgeDelay);
      QueueStatus(kIntComplete, kInitDelay);
      break;

    default:
      // An unknown command still has to answer, or software waits forever.
      ++stats_.unknown_commands;
      QueueError(0x40, kAcknowledgeDelay);
      break;
  }

  ClearParameters();
}

}
}
