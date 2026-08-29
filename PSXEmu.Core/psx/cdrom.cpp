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

// Mode register bits.
const uint8_t kModeDoubleSpeed = 0x80;
const uint8_t kModeWholeSector = 0x20;   // 0x924 bytes rather than 0x800

}  // namespace

Cdrom::Cdrom() {
}

Cdrom::~Cdrom() {
}

int Cdrom::Initialize() {
  index_ = 0;
  // No disc and no motor until something says otherwise. The shell-open bit
  // starts set, which is what tells software the tray state is unknown.
  status_ = kStatusShellOpen;
  interrupt_enable_ = 0;
  interrupt_flag_ = 0;

  parameter_fifo_.clear();
  response_fifo_.clear();
  pending_.clear();

  memset(sector_, 0, sizeof(sector_));
  data_offset_ = 0;
  data_size_ = 0;
  data_read_ = 0;

  seek_lba_ = Disc::kLeadInSectors;
  read_lba_ = Disc::kLeadInSectors;
  seek_pending_ = false;
  reading_ = false;
  playing_ = false;
  mode_ = 0;
  read_timer_ = 0;

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
  status_ = kStatusMotorOn;
  seek_lba_ = Disc::kLeadInSectors;
  read_lba_ = Disc::kLeadInSectors;
  return true;
}

void Cdrom::CloseDisc() {
  disc_.Close();
  reading_ = false;
  playing_ = false;
  status_ = kStatusShellOpen;
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
        // Request register. Bit 7 asks for the sector to be handed over;
        // clearing it throws away what has not been read.
        if (data & 0x80) {
          data_read_ = 0;
        } else {
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
  QueueResponse(interrupt, delay, &status_, 1);
}

void Cdrom::QueueError(uint8_t error_code, int32_t delay) {
  const uint8_t data[2] = { static_cast<uint8_t>(status_ | kStatusError),
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
  if (!pending_.empty() && pending_.front().delay > 0)
    pending_.front().delay -= static_cast<int32_t>(cycles);

  StepRead(cycles);
  DeliverPending();
}

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------

void Cdrom::GetReport(uint8_t* data) {
  uint8_t minute, second, frame;
  Disc::LbaToMsf(read_lba_, &minute, &second, &frame);

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

  uint8_t track_minute, track_second, track_frame;
  Disc::LbaToMsf(read_lba_ - track_start, &track_minute, &track_second, &track_frame);

  data[0] = status_;
  data[1] = Disc::ToBcd(current_track);
  data[2] = Disc::ToBcd(index);
  data[3] = track_minute;
  data[4] = track_second;
  data[5] = track_frame;
  data[6] = minute;
  data[7] = second;
}

void Cdrom::LoadSector() {
  if (!disc_.ReadSector(read_lba_, sector_)) {
    QueueError(0x80, 0);
    reading_ = false;
    playing_ = false;
    return;
  }

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
  ++stats_.sectors_read;

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
    if (disc_.ReadSector(read_lba_, sector_)) {
      system().spu().QueueCdAudio(sector_);
      if (read_lba_ >= disc_.total_sectors()) {
        playing_ = false;
        status_ &= ~kStatusPlaying;
        QueueStatus(kIntDataEnd, 0); // INT4
      } else {
        if (mode_ & 0x04) { // Report mode
          if (pending_.empty()) {
            uint8_t data[8];
            GetReport(data);
            QueueResponse(kIntDataReady, 0, data, 8);
          }
        }
      }
      ++read_lba_;
    } else {
      QueueError(0x80, 0);
      reading_ = false;
      playing_ = false;
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
      QueueStatus(kIntAcknowledge, kAcknowledgeDelay);
      break;
    }

    case 0x03: {  // Play
      if (!disc_.loaded()) {
        QueueError(0x80, kAcknowledgeDelay);
        break;
      }
      if (!parameter_fifo_.empty()) {
        uint8_t track_bcd = TakeParameter();
        uint8_t track = ((track_bcd >> 4) * 10) + (track_bcd & 0x0F);
        if (track >= 1 && track <= disc_.track_count()) {
          seek_lba_ = disc_.track(track - 1).start_lba;
        } else {
          QueueError(0x10, kAcknowledgeDelay);
          break;
        }
      } else if (!seek_pending_) {
        seek_lba_ = read_lba_;
      }
      seek_pending_ = false;
      read_lba_ = seek_lba_;
      reading_ = false;
      playing_ = true;
      status_ = (status_ & ~kStatusReading) | kStatusPlaying | kStatusMotorOn;
      read_timer_ = SectorCycles();
      QueueStatus(kIntAcknowledge, kAcknowledgeDelay);
      QueueStatus(kIntComplete, kSecondResponseDelay);
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
      status_ = 0;
      QueueStatus(kIntAcknowledge, kAcknowledgeDelay);
      QueueStatus(kIntComplete, kInitDelay);
      break;

    case 0x09:    // Pause
      reading_ = false;
      playing_ = false;
      status_ &= ~(kStatusReading | kStatusPlaying | kStatusSeeking);
      QueueStatus(kIntAcknowledge, kAcknowledgeDelay);
      QueueStatus(kIntComplete, kSecondResponseDelay);
      break;

    case 0x0A:    // Init
      mode_ = 0;
      reading_ = false;
      playing_ = false;
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
      TakeParameter();
      TakeParameter();
      QueueStatus(kIntAcknowledge, kAcknowledgeDelay);
      break;

    case 0x0E:    // Setmode
      mode_ = TakeParameter();
      QueueStatus(kIntAcknowledge, kAcknowledgeDelay);
      break;

    case 0x0F: {  // Getparam
      // Returns current parameters: stat, mode, file, channel, sm
      // We don't fully track file/channel/sm from Setfilter yet, so return 0s
      const uint8_t data[5] = { status_, mode_, 0, 0, 0 };
      QueueResponse(kIntAcknowledge, kAcknowledgeDelay, data, 5);
      break;
    }

    case 0x10: {  // GetlocL
      // Returns sector header/subheader: stat, min, sec, frame, mode, file, channel, sm
      const uint8_t data[8] = {
        status_,
        sector_[12], sector_[13], sector_[14], sector_[15],
        sector_[16], sector_[17], sector_[18]
      };
      QueueResponse(kIntAcknowledge, kAcknowledgeDelay, data, 8);
      break;
    }

    case 0x11: {  // GetlocP - where the head is, in track and disc terms
      uint8_t data[8];
      GetReport(data);
      QueueResponse(kIntAcknowledge, kAcknowledgeDelay, data, 8);
      break;
    }

    case 0x13: {  // GetTN - first and last track numbers
      if (!disc_.loaded()) {
        QueueError(0x80, kAcknowledgeDelay);
        break;
      }
      const uint8_t data[3] = {
        status_,
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
      const uint8_t data[4] = { status_, minute, second, frame };
      QueueResponse(kIntAcknowledge, kAcknowledgeDelay, data, 4);
      break;
    }

    case 0x15:    // SeekL - seek using the data header
    case 0x16: {  // SeekP - seek using the subchannel
      if (!disc_.loaded()) {
        QueueError(0x80, kAcknowledgeDelay);
        break;
      }
      read_lba_ = seek_lba_;
      seek_pending_ = false;
      status_ = kStatusMotorOn | kStatusSeeking;
      QueueStatus(kIntAcknowledge, kAcknowledgeDelay);
      status_ = kStatusMotorOn;
      QueueStatus(kIntComplete, kSeekDelay);
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
