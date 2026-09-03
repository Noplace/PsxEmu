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

/*
  The interface the rest of the core talks to the GPU through.

  Deliberately knows nothing about a window, a device or a front end: the core
  owns VRAM and produces a framebuffer, and a front end reads it. Anything a
  front end needs goes through the accessors below, never the other way round.
*/
class GpuCore : public Component {
 public:
  // PSX VRAM is 1 MB seen as 1024x512 16-bit pixels.
  static const int kVramWidth  = 1024;
  static const int kVramHeight = 512;

  virtual ~GpuCore() {}
  virtual int Initialize() = 0;
  virtual int Deinitialize() = 0;

  virtual uint32_t ReadData() = 0;
  virtual uint32_t ReadStatus() = 0;
  virtual void WriteData(uint32_t data) = 0;
  virtual void WriteStatus(uint32_t data) = 0;

  // Advances the display timing by the given number of GPU dot clocks, so the
  // GPU can drive vblank. Returns true when a frame has just completed.
  virtual bool Tick(uint32_t cycles) = 0;

  // Raw VRAM, for a front end that wants to present it or a test that wants to
  // checksum it.
  virtual const uint16_t* vram() const = 0;
  virtual  uint32_t scanline() const = 0;

  // Display timing the root counters run from: counter 0 counts dot clocks
  // and pauses on hblank, counter 1 counts hblanks and pauses on vblank.
  // Take* hands over what has accumulated and clears it, so a caller that
  // asks twice does not count the same dot clock twice.
  virtual uint32_t TakeDotClocks() = 0;
  virtual uint32_t TakeHblanks() = 0;
  virtual bool in_hblank() const = 0;
  virtual bool in_vblank() const = 0;
  // The visible area, resolved out of VRAM into 32-bit XRGB. Size comes back
  // through the out parameters; the pointer stays valid until the next Tick.
  virtual const uint32_t* framebuffer(int& width, int& height) const = 0;
};

}
}
