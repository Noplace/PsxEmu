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

// How long after a byte is exchanged the acknowledge interrupt arrives.
const int32_t kAcknowledgeCycles = 500;

// Status register bits.
const uint16_t kStatusTxReady      = 0x0001;
const uint16_t kStatusRxNotEmpty   = 0x0002;
const uint16_t kStatusTxDone       = 0x0004;
const uint16_t kStatusAcknowledge  = 0x0080;
const uint16_t kStatusInterrupt    = 0x0200;

}  // namespace

Sio::Sio() {
}

Sio::~Sio() {
}

int Sio::Initialize() {
  memset(pad_, 0, sizeof(pad_));
  // Slot 1 has a digital pad in it, slot 2 is empty. A front end overrides
  // this as soon as it knows better.
  pad_[0].connected = true;
  pad_[0].buttons = 0;
  pad_[1].connected = false;

  control_ = 0;
  mode_ = 0;
  baud_ = 0;
  status_ = kStatusTxReady | kStatusTxDone;

  target_ = kTargetNone;
  transfer_step_ = 0;
  receive_ = 0xFF;
  receive_full_ = false;
  acknowledge_ = false;
  interrupt_timer_ = 0;
  interrupt_pending_ = false;
  return S_OK;
}

int Sio::Deinitialize() {
  return S_OK;
}

void Sio::Tick(uint32_t cycles) {
  if (!interrupt_pending_)
    return;

  interrupt_timer_ -= static_cast<int32_t>(cycles);
  if (interrupt_timer_ > 0)
    return;

  interrupt_pending_ = false;
  if (acknowledge_) {
    status_ |= kStatusInterrupt;
    system().io().SetInterrupt(kInterruptSIO0);
  }
}

// One byte in, one byte out. The device decides whether to acknowledge, and a
// device that is not there never does - which ends the exchange and is how
// software discovers an empty slot.
uint8_t Sio::Exchange(uint8_t data) {
  const int slot = selected_slot();
  const Pad& pad = pad_[slot];

  if (transfer_step_ == 0) {
    // First byte selects the device: 0x01 is a controller, 0x81 a memory card.
    target_ = kTargetNone;
    if (data == 0x01 && pad.connected)
      target_ = kTargetPad;
    else if (data == 0x81 && system().mc(slot).connected())
      target_ = kTargetMemoryCard;

    acknowledge_ = (target_ != kTargetNone);
    ++transfer_step_;
    return 0xFF;
  }

  if (target_ == kTargetMemoryCard) {
    return ExchangeMemoryCard(data, system().mc(selected_slot()));
  }

  if (target_ != kTargetPad) {
    acknowledge_ = false;
    return 0xFF;
  }

  // Digital pad: id 0x5A41, then two button bytes, active low.
  const uint16_t buttons = static_cast<uint16_t>(~pad.buttons);
  uint8_t result = 0xFF;
  switch (transfer_step_) {
    case 1: result = 0x41; acknowledge_ = true;  break;   // id low
    case 2: result = 0x5A; acknowledge_ = true;  break;   // id high
    case 3: result = static_cast<uint8_t>(buttons);       // buttons low
            acknowledge_ = true;  break;
    case 4: result = static_cast<uint8_t>(buttons >> 8);  // buttons high
            acknowledge_ = false; break;                  // last byte
    default:
      acknowledge_ = false;
      break;
  }
  ++transfer_step_;
  return result;
}

uint8_t Sio::Read08(uint32_t address) {
  if ((address & 0xF) == 0x0) {
    const uint8_t value = receive_;
    receive_ = 0xFF;
    receive_full_ = false;
    status_ &= ~kStatusRxNotEmpty;
    return value;
  }
  return static_cast<uint8_t>(Read16(address & ~1u));
}

uint16_t Sio::Read16(uint32_t address) {
  switch (address & 0xF) {
    case 0x0: {
      const uint8_t value = receive_;
      receive_ = 0xFF;
      receive_full_ = false;
      status_ &= ~kStatusRxNotEmpty;
      return value;
    }
    case 0x4: return status_;
    case 0x8: return mode_;
    case 0xA: return control_;
    case 0xE: return baud_;
    default:  return 0;
  }
}

uint32_t Sio::Read32(uint32_t address) {
  if ((address & 0xF) == 0x4)
    return status_;
  return Read16(address);
}

void Sio::Write08(uint32_t address, uint8_t data) {
  if ((address & 0xF) == 0x0) {
    receive_ = Exchange(data);
    receive_full_ = true;
    status_ |= kStatusRxNotEmpty | kStatusTxReady | kStatusTxDone;
    if (acknowledge_) {
      status_ |= kStatusAcknowledge;
      interrupt_pending_ = true;
      interrupt_timer_ = kAcknowledgeCycles;
    } else {
      status_ &= ~kStatusAcknowledge;
      // Nothing answered, so the exchange is over and the next byte starts a
      // new one.
      transfer_step_ = 0;
      target_ = kTargetNone;
    }
    return;
  }
  Write16(address & ~1u, data);
}

void Sio::Write16(uint32_t address, uint16_t data) {
  switch (address & 0xF) {
    case 0x0:
      Write08(address, static_cast<uint8_t>(data));
      return;
    case 0x8:
      mode_ = data;
      return;
    case 0xA:
      control_ = data;
      if (data & 0x0040) {          // reset
        status_ = kStatusTxReady | kStatusTxDone;
        control_ = 0;
        mode_ = 0;
        transfer_step_ = 0;
        target_ = kTargetNone;
        receive_ = 0xFF;
        receive_full_ = false;
        interrupt_pending_ = false;
      }
      if (data & 0x0010) {          // acknowledge
        status_ &= ~(kStatusInterrupt | kStatusAcknowledge);
      }
      if ((data & 0x0002) == 0) {
        // Chip select dropped: the device is deselected and the next byte
        // starts a fresh exchange.
        transfer_step_ = 0;
        target_ = kTargetNone;
      }
      return;
    case 0xE:
      baud_ = data;
      return;
    default:
      return;
  }
}

void Sio::Write32(uint32_t address, uint32_t data) {
  Write16(address, static_cast<uint16_t>(data));
}

uint8_t Sio::ExchangeMemoryCard(uint8_t data, class MC& mc) {
  uint8_t result = 0xFF;
  acknowledge_ = true;

  if (transfer_step_ == 1) {
    mc_command_ = data;
    result = mc.flag();
  } else if (transfer_step_ == 2) {
    result = 0x5A;
  } else if (transfer_step_ == 3) {
    result = 0x5D;
  } else {
    // Process command specific states
    if (mc_command_ == 0x52) { // Read
      if (transfer_step_ == 4) { mc_sector_ = data << 8; result = 0x00; }
      else if (transfer_step_ == 5) { mc_sector_ |= data; result = mc_previous_tx_; mc.ReadSector(mc_sector_, mc_buffer_); }
      else if (transfer_step_ == 6) { result = 0x5C; }
      else if (transfer_step_ == 7) { result = 0x5D; }
      else if (transfer_step_ == 8) { result = (mc_sector_ >> 8) & 0xFF; }
      else if (transfer_step_ == 9) { result = mc_sector_ & 0xFF; mc_checksum_ = (mc_sector_ >> 8) ^ (mc_sector_ & 0xFF); }
      else if (transfer_step_ >= 10 && transfer_step_ <= 137) {
        int idx = transfer_step_ - 10;
        result = mc_buffer_[idx];
        mc_checksum_ ^= result;
      }
      else if (transfer_step_ == 138) { result = mc_checksum_; }
      else if (transfer_step_ == 139) { result = 0x47; acknowledge_ = false; }
    } else if (mc_command_ == 0x57) { // Write
      if (transfer_step_ == 4) { mc_sector_ = data << 8; result = 0x00; }
      else if (transfer_step_ == 5) { mc_sector_ |= data; result = mc_previous_tx_; mc_checksum_ = (mc_sector_ >> 8) ^ (mc_sector_ & 0xFF); }
      else if (transfer_step_ >= 6 && transfer_step_ <= 133) {
        int idx = transfer_step_ - 6;
        mc_buffer_[idx] = data;
        mc_checksum_ ^= data;
        result = mc_previous_tx_;
      }
      else if (transfer_step_ == 134) { 
        result = mc_previous_tx_; 
        // We write upon receiving checksum
        if (data == mc_checksum_) {
          mc.WriteSector(mc_sector_, mc_buffer_);
        }
      }
      else if (transfer_step_ == 135) { result = 0x5C; }
      else if (transfer_step_ == 136) { result = 0x5D; }
      else if (transfer_step_ == 137) { result = 0x47; acknowledge_ = false; }
    } else if (mc_command_ == 0x53) { // Get ID
      if (transfer_step_ == 4) { result = 0x5C; }
      else if (transfer_step_ == 5) { result = 0x5D; }
      else if (transfer_step_ == 6) { result = 0x04; }
      else if (transfer_step_ == 7) { result = 0x00; }
      else if (transfer_step_ == 8) { result = 0x00; }
      else if (transfer_step_ == 9) { result = 0x80; acknowledge_ = false; }
    } else {
      acknowledge_ = false;
    }
  }

  mc_previous_tx_ = data;
  ++transfer_step_;
  return result;
}

}
}
