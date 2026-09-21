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

#include <cstdint>

// Included from psx.h, after Component and the rest; like sio.h, this does not
// pull its own dependencies in.

namespace emulation {
namespace psx {

/*
  SIO1 - the serial port.

  The 8-pin SERIAL I/O socket on the back of every PlayStation before the
  PSone. Where SIO0 (sio.h) is a synchronous link with a device-select and an
  acknowledge line, made for controllers and memory cards, this is an ordinary
  asynchronous UART: a baud rate, a character length, parity and stop bits,
  two data lines and the handshake pairs DTR/DSR and RTS/CTS, with IRQ8 to say
  something happened.

  Two things ever used it. A link cable, joining two consoles for the handful
  of games that offered it - Doom, Ridge Racer Revolution, Destruction Derby
  and a couple of dozen others - and development hardware: Net Yaroze loaded
  code down this port, and homebrew still prints to a PC through it. A retail
  BIOS never touches it: a 60-frame boot of SCPH1001 makes no access to
  1F80105xh at all.

  **Nothing is plugged in here, and that is the whole of what this models.**
  The registers exist, hold what software writes, read back what hardware
  would, and the status says there is no cable: no device asserting /DSR or
  CTS, nothing ever arriving to be read. Software that polls for a link
  partner finds none, which is the truth, and gets there through the same
  register semantics a real machine has rather than by reading zeroes from an
  address nobody decoded - which is what this did before, along with counting
  a trap for each access.

  What it does not model is a partner: no second machine, no host serial port,
  no loopback. A byte written with the transmitter enabled leaves and is gone.
  It is also gone *immediately* - a real UART holds the line for ten or so bit
  periods at the programmed baud rate, and nothing here waits that long. With
  no receiver, the only software-visible difference is how soon the transmit
  interrupt arrives.

  Because the byte is otherwise lost, EmuConfig::sio1_to_console offers it to
  the BIOS console instead (Kernel's text, the same window the BIOS's own
  putchar feeds), which is how a homebrew program that prints over the serial
  port becomes readable here. DuckStation has the same setting and sends every
  byte written to the data register; this sends what the transmitter was
  enabled to send, since with TXEN clear a real port transmits nothing.

  Register map, 1F801050h-1F80105Fh:

    1050h  DATA   write: the byte to transmit; read: the receive FIFO
    1054h  STAT   read-only status, including the baud-rate timer in bits 11-25
    1058h  MODE   baud reload factor, character length, parity, stop bits
    105Ah  CTRL   TXEN/RXEN, the DTR and RTS outputs, the interrupt enables,
                  and two strobes: acknowledge (bit 4) and reset (bit 6)
    105Eh  BAUD   the reload value the timer counts down from

  Sources: psx-spx's serial-port registers, and DuckStation's SIO for the
  reset values and the shape of a port with nothing attached. Where the two
  disagree on an unplugged port's /DSR and CTS levels, this reports them
  inactive - a line with nothing pulling it is not asserted, and software
  asking "is there a cable?" should hear no. DuckStation reports both active.
  No console test in test/test suite covers this, so that is reasoning from
  the documentation, not a measurement.
*/
class Sio1 : public Component {
 public:
  // 1F801054h SIO1_STAT.
  static const uint32_t kStatTxReady      = 1u << 0;   // the FIFO can take a byte
  static const uint32_t kStatRxNotEmpty   = 1u << 1;
  static const uint32_t kStatTxIdle       = 1u << 2;   // shift register empty too
  static const uint32_t kStatRxParity     = 1u << 3;
  static const uint32_t kStatRxOverrun    = 1u << 4;
  static const uint32_t kStatRxBadStopBit = 1u << 5;
  static const uint32_t kStatRxInputLevel = 1u << 6;
  static const uint32_t kStatDsrLevel     = 1u << 7;
  static const uint32_t kStatCtsLevel     = 1u << 8;
  static const uint32_t kStatInterrupt    = 1u << 9;
  static const int kStatTimerShift = 11;               // bits 11-25, 15 bits
  static const uint32_t kStatTimerMask = 0x7FFFu;
  // The three error bits the acknowledge strobe clears.
  static const uint32_t kStatErrors =
      kStatRxParity | kStatRxOverrun | kStatRxBadStopBit;

  // 1F80105Ah SIO1_CTRL.
  static const uint16_t kCtrlTxEnable     = 1u << 0;
  static const uint16_t kCtrlDtrOutput    = 1u << 1;
  static const uint16_t kCtrlRxEnable     = 1u << 2;
  static const uint16_t kCtrlTxOutput     = 1u << 3;   // hold the line low (break)
  static const uint16_t kCtrlAcknowledge  = 1u << 4;   // strobe, does not stick
  static const uint16_t kCtrlRtsOutput    = 1u << 5;
  static const uint16_t kCtrlReset        = 1u << 6;   // strobe, does not stick
  static const uint16_t kCtrlTxInterrupt  = 1u << 10;
  static const uint16_t kCtrlRxInterrupt  = 1u << 11;
  static const uint16_t kCtrlDsrInterrupt = 1u << 12;

  void Initialize();

  // The baud-rate timer counts down whether or not anything is being
  // transmitted - software can read it out of STAT and time with it.
  void Tick(uint32_t cycles);

  uint8_t Read08(uint32_t address);
  uint16_t Read16(uint32_t address);
  uint32_t Read32(uint32_t address);
  void Write08(uint32_t address, uint8_t data);
  void Write16(uint32_t address, uint16_t data);
  void Write32(uint32_t address, uint32_t data);

  void Serialise(StateIO& io);

 private:
  void SoftReset();
  uint32_t Status() const;
  void WriteData(uint8_t value);
  void WriteControl(uint16_t value);
  void RaiseInterrupt();
  void ReloadBaudTimer();

  uint16_t mode_ = 0;
  uint16_t ctrl_ = 0;
  uint16_t baud_ = 0;
  uint32_t stat_ = 0;
  uint32_t baud_timer_ = 1;
};

}  // namespace psx
}  // namespace emulation
