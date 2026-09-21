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

namespace emulation {
namespace psx {

void Sio1::Initialize() {
  SoftReset();
}

// Reset values: DuckStation's, which are the ones a BIOS finds. The
// transmitter is idle and ready, nothing is connected, no interrupt is
// latched.
void Sio1::SoftReset() {
  mode_ = 0;
  ctrl_ = 0;
  baud_ = 0x00DC;
  stat_ = kStatTxReady | kStatTxIdle;
  ReloadBaudTimer();
}

void Sio1::Tick(uint32_t cycles) {
  while (cycles > 0) {
    const uint32_t step = (baud_timer_ > cycles) ? cycles : baud_timer_;
    baud_timer_ -= step;
    cycles -= step;
    if (baud_timer_ == 0)
      ReloadBaudTimer();
  }
}

// The registers are halfwords, except the 32-bit status; a word access covers
// the pair that shares its word (MODE with CTRL, and BAUD with the unused
// 105Ch), and a byte access one half of a halfword.
uint16_t Sio1::Read16(uint32_t address) {
  switch (address & 0x0Eu) {
    // Nothing is transmitting into this port, so the receive FIFO is always
    // empty and a read sees the idle line: all ones.
    case 0x00: case 0x02: return 0xFFFFu;
    case 0x04: return static_cast<uint16_t>(Status());
    case 0x06: return static_cast<uint16_t>(Status() >> 16);
    case 0x08: return mode_;
    case 0x0A: return ctrl_;
    case 0x0E: return baud_;
    default:   return 0;   // 105Ch, which nothing documents and nothing uses
  }
}

uint8_t Sio1::Read08(uint32_t address) {
  const uint16_t half = Read16(address & ~1u);
  return static_cast<uint8_t>(half >> ((address & 1) * 8));
}

uint32_t Sio1::Read32(uint32_t address) {
  switch (address & 0x0Cu) {
    case 0x00: return 0xFFFFFFFFu;
    case 0x04: return Status();
    case 0x08: return static_cast<uint32_t>(mode_) |
                      (static_cast<uint32_t>(ctrl_) << 16);
    default:   return static_cast<uint32_t>(baud_) << 16;   // 105Ch and BAUD
  }
}

void Sio1::Write16(uint32_t address, uint16_t data) {
  switch (address & 0x0Eu) {
    case 0x00: case 0x02: WriteData(static_cast<uint8_t>(data)); return;
    case 0x08: mode_ = data; return;
    case 0x0A: WriteControl(data); return;
    case 0x0E: baud_ = data; ReloadBaudTimer(); return;
    default:   return;     // STAT is read-only, and 105Ch is nothing
  }
}

void Sio1::Write08(uint32_t address, uint8_t data) {
  // The data register takes the byte as it is; anywhere else, the byte
  // replaces its half of the halfword it lands in.
  if ((address & 0x0Eu) == 0x00) {
    WriteData(data);
    return;
  }
  const uint32_t half_address = address & ~1u;
  uint16_t half = Read16(half_address);
  if (address & 1)
    half = static_cast<uint16_t>((half & 0x00FFu) | (data << 8));
  else
    half = static_cast<uint16_t>((half & 0xFF00u) | data);
  Write16(half_address, half);
}

void Sio1::Write32(uint32_t address, uint32_t data) {
  switch (address & 0x0Cu) {
    case 0x00: WriteData(static_cast<uint8_t>(data)); return;
    case 0x04: return;     // status is read-only
    case 0x08:
      mode_ = static_cast<uint16_t>(data);
      WriteControl(static_cast<uint16_t>(data >> 16));
      return;
    default:
      baud_ = static_cast<uint16_t>(data >> 16);
      ReloadBaudTimer();
      return;
  }
}

uint32_t Sio1::Status() const {
  return stat_ | ((baud_timer_ & kStatTimerMask) << kStatTimerShift);
}

void Sio1::WriteData(uint8_t value) {
  if ((ctrl_ & kCtrlTxEnable) == 0)
    return;

  // Sent, and with no cable attached, gone - except that the front end can
  // ask to read along (EmuConfig::sio1_to_console).
  if (system().config().sio1_to_console)
    system().kernel().WriteConsoleChar(static_cast<char>(value));

  // A real transmission ends a character's worth of bit periods later; this
  // one has already finished, so the FIFO and the shift register are both
  // free again and the transmit interrupt, if armed, is due now.
  stat_ |= kStatTxReady | kStatTxIdle;
  if (ctrl_ & kCtrlTxInterrupt)
    RaiseInterrupt();
}

void Sio1::WriteControl(uint16_t value) {
  if (value & kCtrlReset) {
    SoftReset();
    return;
  }
  if (value & kCtrlAcknowledge)
    stat_ &= ~(kStatErrors | kStatInterrupt);

  // The two strobes are actions, not state, and read back as zero.
  ctrl_ = value & static_cast<uint16_t>(~(kCtrlAcknowledge | kCtrlReset));

  // Arming the transmit interrupt while the transmitter is already idle
  // raises it: the condition is a level, not the edge of the last byte
  // leaving.
  if ((ctrl_ & kCtrlTxInterrupt) && (ctrl_ & kCtrlTxEnable) &&
      (stat_ & kStatTxIdle))
    RaiseInterrupt();
}

// The interrupt is a latch: it stays until the acknowledge strobe clears it,
// and only its first edge reaches I_STAT.
void Sio1::RaiseInterrupt() {
  if (stat_ & kStatInterrupt)
    return;
  stat_ |= kStatInterrupt;
  system().io().SetInterrupt(kInterruptSIO1);
}

// The timer counts down from BAUD scaled by MODE's reload factor: 1, 16 or 64
// times. A factor of zero means the same as one.
void Sio1::ReloadBaudTimer() {
  static const uint32_t kFactors[4] = { 1, 1, 16, 64 };
  const uint32_t reload = baud_ * kFactors[mode_ & 3];
  baud_timer_ = (reload > 0) ? reload : 1;
}

void Sio1::Serialise(StateIO& io) {
  io.Plain(mode_);
  io.Plain(ctrl_);
  io.Plain(baud_);
  io.Plain(stat_);
  io.Plain(baud_timer_);
}

}  // namespace psx
}  // namespace emulation
