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

namespace emulation {
namespace psx {

/*
  SIO0 - the controller and memory card port.

  A synchronous serial link: software writes a byte, the device on the other
  end shifts one back, and an interrupt says the exchange finished. Both slots
  are polled the same way, and a slot with nothing in it simply never
  acknowledges - which is how software knows it is empty.

  This implements the digital pad protocol and enough of the port to let a slot
  report itself empty. Memory cards are not here yet: an unplugged card and an
  unimplemented one look the same to software, which is exactly the quiet
  failure worth being explicit about.
*/
class Sio : public Component {
 public:
  // Buttons, in the order the digital pad reports them. Active low on the
  // wire; this interface takes them the right way round and inverts on the way
  // out.
  enum Button {
    kSelect   = 1 << 0,
    kL3       = 1 << 1,
    kR3       = 1 << 2,
    kStart    = 1 << 3,
    kUp       = 1 << 4,
    kRight    = 1 << 5,
    kDown     = 1 << 6,
    kLeft     = 1 << 7,
    kL2       = 1 << 8,
    kR2       = 1 << 9,
    kL1       = 1 << 10,
    kR1       = 1 << 11,
    kTriangle = 1 << 12,
    kCircle   = 1 << 13,
    kCross    = 1 << 14,
    kSquare   = 1 << 15,
  };

  Sio();
  ~Sio();

  int Initialize();
  int Deinitialize();
  void Tick(uint32_t cycles);

  uint8_t Read08(uint32_t address);
  uint16_t Read16(uint32_t address);
  uint32_t Read32(uint32_t address);
  void Write08(uint32_t address, uint8_t data);
  void Write16(uint32_t address, uint16_t data);
  void Write32(uint32_t address, uint32_t data);

  // Set by a front end. Bit set means pressed.
  void set_buttons(int slot, uint16_t buttons) {
    if (slot >= 0 && slot < 2) pad_[slot].buttons = buttons;
  }
  void set_connected(int slot, bool connected) {
    if (slot >= 0 && slot < 2) pad_[slot].connected = connected;
  }

 private:
  struct Pad {
    bool connected;
    uint16_t buttons;
  };

  // Which device the current exchange is talking to, and how far in it is.
  enum Target { kTargetNone, kTargetPad, kTargetMemoryCard };

  Pad pad_[2];

  uint16_t control_;
  uint16_t mode_;
  uint16_t baud_;
  uint16_t status_;

  Target target_;
  int transfer_step_;
  uint8_t receive_;
  bool receive_full_;
  bool acknowledge_;
  int32_t interrupt_timer_;
  bool interrupt_pending_;

  int selected_slot() const { return (control_ & 0x2000) ? 1 : 0; }

  uint8_t Exchange(uint8_t data);
  uint8_t ExchangeMemoryCard(uint8_t data, class MC& mc);

  uint8_t mc_command_;
  uint16_t mc_sector_;
  uint8_t mc_checksum_;
  uint8_t mc_buffer_[128];
  uint8_t mc_previous_tx_;
};

}
}
