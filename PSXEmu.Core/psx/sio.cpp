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
  // Pad() gives every field its at-rest default - centred sticks, digital
  // mode, an unmapped rumble table - which a plain memset would not, since
  // "every byte zero" is the wrong default for a byte convention where 0x80
  // means centred and 0xFF means unmapped.
  pad_[0] = Pad();
  pad_[1] = Pad();
  // Slot 1 has a digital pad in it, slot 2 is empty. A front end overrides
  // this as soon as it knows better.
  pad_[0].connected = true;
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
  pad_command_ = 0;
  legacy_rumble_byte2_ = 0;
  return S_OK;
}

int Sio::Deinitialize() {
  return S_OK;
}

void Sio::set_connected(int slot, bool connected) {
  if (slot < 0 || slot >= 2)
    return;
  if (connected && !pad_[slot].connected) {
    // A freshly connected pad has negotiated nothing yet - a real DualShock
    // that has just been plugged in does not remember being in analog mode
    // on some other console, and neither should this one. What it was doing
    // before this moment (buttons, axes) does not matter and is overwritten
    // by the next poll regardless.
    Pad fresh;
    fresh.connected = true;
    pad_[slot] = fresh;
  }
  pad_[slot].connected = connected;
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

  return ExchangeController(data, slot);
}

// The ID a pad's reply starts with. It depends only on what the pad
// currently is, never on what is being asked of it - on real hardware the
// pad has already committed to this byte before it has seen enough of the
// command to know what it is.
//
// The high nibble says which of the three shapes a reply is: 4 for a plain
// digital pad, 7 for one in analog mode, F for one that is inside
// configuration mode (which stays true regardless of analog/digital, since
// entering configuration mode is itself a DualShock-only thing to be able to
// do at all). The low nibble is fixed at 1 or 3 by the same analog/digital
// split, and doubles as how long the reply is: one halfword of data beyond
// the ID and status for a digital pad, three for an analog one.
uint8_t Sio::PadIdByte(const Pad& pad) const {
  const uint8_t high = pad.config_mode ? 0xF : (pad.analog_mode ? 0x7 : 0x4);
  const uint8_t low = pad.analog_mode ? 0x3 : 0x1;
  return static_cast<uint8_t>((high << 4) | low);
}

// One byte of a command-0x42 poll reply, at `payload_index` counting from
// the first byte after the ID and status - 0 and 1 are the button bytes,
// which exist regardless of mode, and 2 through 5 are the four analog axes,
// which only exist - and are only ever asked for - once the pad is in
// analog mode.
//
// The same bytes are simultaneously carrying whatever the host is asking the
// motors to do, exactly as every other exchange on this bus is full duplex:
// once a game has mapped them with command 0x4D, `incoming` at a mapped
// position becomes that motor's new speed. Before any game has ever done
// that, the pad falls back to the pattern every original one answered to -
// a fixed two-byte code that only ever turns the small motor fully on or
// fully off.
uint8_t Sio::PollPayloadByte(Pad& pad, int payload_index, uint8_t incoming) {
  uint8_t out = 0x00;
  const uint16_t buttons = static_cast<uint16_t>(~pad.buttons);
  switch (payload_index) {
    case 0: out = static_cast<uint8_t>(buttons); break;
    case 1: out = static_cast<uint8_t>(buttons >> 8); break;
    case 2: out = pad.right_x; break;
    case 3: out = pad.right_y; break;
    case 4: out = pad.left_x; break;
    case 5: out = pad.left_y; break;
    default: break;
  }

  if (pad.dualshock_enabled) {
    if (payload_index >= 0 && payload_index < 5) {
      const uint8_t motor = pad.rumble_map[payload_index];
      if (motor == kSmallMotor)
        pad.motor_small = incoming;
      else if (motor == kLargeMotor)
        pad.motor_large = incoming;
    }
  } else if (payload_index == 0) {
    legacy_rumble_byte2_ = incoming;
  } else if (payload_index == 1) {
    const bool on =
        (legacy_rumble_byte2_ & 0xC0) == 0x40 && (incoming & 0x01) != 0;
    pad.motor_small = on ? 255 : 0;
  }
  return out;
}

// The controller side of an exchange, from the second byte on - the first
// was already consumed by Exchange() to pick the device.
uint8_t Sio::ExchangeController(uint8_t data, int slot) {
  Pad& pad = pad_[slot];
  const int step = transfer_step_;

  // Step 1 is the command byte itself (0x42 to poll, 0x43 to enter or leave
  // configuration mode, and so on) - remembered for the rest of the
  // exchange, and answered with the pad's current ID regardless of what it
  // turns out to be, because the pad does not know that yet either.
  if (step == 1) {
    pad_command_ = data;
    acknowledge_ = true;
    ++transfer_step_;
    return PadIdByte(pad);
  }

  // Step 2 is the status byte, which is always this one value everywhere
  // else on this bus already uses for the same purpose.
  if (step == 2) {
    acknowledge_ = true;
    ++transfer_step_;
    return 0x5A;
  }

  // The configuration-mode-only commands do nothing at all - not even 0x44 -
  // unless the pad has actually been put into configuration mode first with
  // 0x43. A game that never negotiates DualShock can never end up with one
  // switching itself into analog mode by accident.
  const bool config_command =
      pad_command_ == 0x44 || pad_command_ == 0x45 || pad_command_ == 0x46 ||
      pad_command_ == 0x47 || pad_command_ == 0x4C || pad_command_ == 0x4D;
  const bool recognised = pad_command_ == 0x42 || pad_command_ == 0x43 ||
                          (config_command && pad.config_mode);

  // A poll's length follows the pad's mode, since that is genuinely how much
  // there is to say. Every other recognised command is a fixed eight bytes,
  // deliberately not recomputed from the mode again after this point - 0x44
  // can change analog_mode partway through its own exchange, and a length
  // that could change under it mid-transaction is not a length a real host
  // could keep up with. An unrecognised command is given the shape of an
  // ordinary poll in whatever mode the pad is already in, which is only ever
  // reached on a mode this exchange cannot itself have just changed.
  const int total_length =
      (pad_command_ == 0x42 || !recognised) ? (pad.analog_mode ? 8 : 4) : 8;

  if (step > total_length) {
    acknowledge_ = false;
    return 0xFF;
  }

  const int payload_index = step - 3;
  uint8_t out = 0x00;

  if (recognised) {
    switch (pad_command_) {
      case 0x42:
        out = PollPayloadByte(pad, payload_index, data);
        break;

      case 0x43:
        // The only byte that matters is the first: 1 to enter, anything else
        // to leave. Entering marks the pad as a DualShock for good - real
        // hardware does not forget that just because the game later takes it
        // back out of configuration mode.
        if (payload_index == 0) {
          pad.config_mode = (data == 1);
          if (pad.config_mode)
            pad.dualshock_enabled = true;
        }
        break;

      case 0x44:
        // Byte 0 is the mode to switch to, byte 1 whether to lock it there.
        // Values outside the two each byte actually uses are left alone
        // rather than guessed at.
        if (payload_index == 0 && (data == 0x00 || data == 0x01))
          pad.analog_mode = (data == 0x01);
        else if (payload_index == 1 && (data == 0x02 || data == 0x03))
          pad.analog_locked = (data == 0x03);
        break;

      case 0x45:
        // A fixed status block bar one byte: whether the pad is currently in
        // analog mode.
        if (payload_index == 0) out = 0x01;
        else if (payload_index == 1) out = 0x02;
        else if (payload_index == 2) out = pad.analog_mode ? 0x01 : 0x00;
        else if (payload_index == 3) out = 0x02;
        else if (payload_index == 4) out = 0x01;
        break;

      case 0x46:
      case 0x47:
        // Capability queries close to nothing exercises. Acknowledged with
        // the right shape so a game that tries them does not stall waiting
        // for a reply that never comes; the exact bytes have not been
        // checked against real hardware and default to zero rather than a
        // guess.
        out = 0x00;
        break;

      case 0x4C:
        // Which kind of DualShock this is - 0x04 here, since pressure-
        // sensitive buttons (which would make it 0x07, a DualShock 2) are
        // not implemented.
        out = (payload_index == 3) ? 0x04 : 0x00;
        break;

      case 0x4D:
        // Read-modify-write: the reply carries the mapping this byte held
        // before, and what the host sends becomes the new one, in the same
        // exchange - the same as every other byte on this bus. The last
        // byte is not part of the mapping; it is where a motor nothing maps
        // to any more gets switched off rather than left running.
        if (payload_index < 5) {
          out = pad.rumble_map[payload_index];
          pad.rumble_map[payload_index] = data;
        } else if (payload_index == 5) {
          bool has_small = false;
          bool has_large = false;
          for (uint8_t motor : pad.rumble_map) {
            has_small = has_small || (motor == kSmallMotor);
            has_large = has_large || (motor == kLargeMotor);
          }
          if (!has_small)
            pad.motor_small = 0;
          if (!has_large)
            pad.motor_large = 0;
        }
        break;

      default:
        break;
    }
  }

  ++transfer_step_;
  acknowledge_ = (transfer_step_ <= total_length);
  return out;
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
        // This resets the SIO peripheral interface, not the controllers on
        // the other end of it - a real DualShock does not forget it is in
        // analog mode just because the console reset the port, and neither
        // should this one. Pad state is untouched here.
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
