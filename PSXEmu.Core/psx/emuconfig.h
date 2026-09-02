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

// The knobs that change how the machine behaves for the person using it, as
// opposed to emulated hardware state.
//
// Nothing outside this file should declare its own copy of a setting. A
// component that needs one reads it from the config rather than caching it -
// `system().config()` from anything deriving from Component.
//
// Measured hardware characteristics do not belong here. The CD-ROM's sector
// timing, the GPU's dot clock and the load stalls in `Cpu::Load` are all
// constants describing a PlayStation, not choices, and they stay next to the
// code that uses them.

namespace emulation {
namespace psx {

struct EmuConfig {
  // --- Audio ----------------------------------------------------------
  // A gain applied after the SPU's own main volume, so it covers the voices,
  // the reverb and CD audio alike.
  //
  // A PlayStation is quiet by modern standards: games set the main volume
  // conservatively and the mix peaks well below full scale - measured at about
  // a fifth of it on the discs tested here. That is faithful, and it is also
  // not what anyone wants out of their speakers, so this exists to make up the
  // difference without pretending the hardware did it.
  //
  // 1.0 is the hardware level. The mix is clamped afterwards, so a high value
  // distorts loud passages rather than wrapping them.
  float audio_volume = 2.0f;

  static const float kMinAudioVolume;
  static const float kMaxAudioVolume;
};

// Out of line so there is one definition; these are bounds a UI can offer
// rather than anything the emulation depends on.
inline const float EmuConfig::kMinAudioVolume = 0.0f;
inline const float EmuConfig::kMaxAudioVolume = 8.0f;

}
}
