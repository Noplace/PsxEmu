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
// Standards section 1.
#pragma once

#include <cstdint>
#include <vector>

namespace utilities {

// Stretches or squeezes a stereo stream by the speed the machine is being run
// at, so a fixed-rate sound device stays fed.
//
// The SPU produces 44,100 samples for every *emulated* second, and the device
// consumes 44,100 for every real one. At 200% the machine hands over 88,200 a
// second and the device wants half of them; at 50% it hands over 22,050 and the
// device wants twice. Resampling by the speed factor is what every emulator's
// fast-forward does, and it sounds like what it is - the pitch goes up with the
// speed, the way a tape does. The alternative, dropping and padding, crackles.
//
// Linear interpolation, and the fractional position and the last frame carry
// across calls so the joins between one frame's audio and the next are
// continuous - the same reason Spu::QueueCdSamples carries its own.
class SpeedResampler {
 public:
  // Forgets the carried position and the last frame. Call when the machine
  // stops or restarts - a pause, a reset, a state load - so the first block
  // afterwards does not interpolate from sound that is no longer playing.
  void Reset() {
    position_ = 0;
    have_last_ = false;
    last_[0] = 0;
    last_[1] = 0;
  }

  // Appends `frames` stereo frames of `in`, resampled by `speed`, to `out`.
  //
  // `speed` is the multiplier the machine is running at: 2.0 means twice as
  // fast, so half as many frames come out. A speed of 1.0 is a copy - bit for
  // bit, with no interpolation and no carried state, so the ordinary case
  // cannot be degraded by this existing.
  void Append(const int16_t* in, int frames, double speed,
              std::vector<int16_t>* out) {
    if (in == nullptr || out == nullptr || frames <= 0)
      return;
    if (speed <= 0.0)
      speed = 1.0;

    if (speed == 1.0) {
      out->insert(out->end(), in, in + static_cast<size_t>(frames) * 2);
      // Carry the state rather than Reset it. The caller trims the speed by a
      // fraction of a percent to hold the device's buffer level (see
      // App::PumpAudio), so this path is entered and left continually, and
      // leaving the carried frame behind on each crossing is the kind of
      // almost-harmless state bug that only shows up somewhere else later.
      // The position lands on zero either way, so there is nothing audible in
      // it today.
      position_ = 0;
      last_[0] = in[(frames - 1) * 2];
      last_[1] = in[(frames - 1) * 2 + 1];
      have_last_ = true;
      return;
    }

    // 16.16 fixed point, so a step is exact for the speeds the menu offers and
    // the accumulated error over a long run is none.
    const uint32_t step = static_cast<uint32_t>(speed * 65536.0 + 0.5);
    const uint32_t end = static_cast<uint32_t>(frames) << 16;

    while (position_ < end) {
      const uint32_t index = position_ >> 16;
      const uint32_t fraction = position_ & 0xFFFF;

      for (int channel = 0; channel < 2; ++channel) {
        // The sample before this one is the previous frame at the start of the
        // block, which is what makes the join continuous.
        const int32_t previous =
            (index == 0) ? (have_last_ ? last_[channel] : in[channel])
                         : in[(index - 1) * 2 + channel];
        const int32_t current = in[index * 2 + channel];
        const int32_t value =
            previous + (((current - previous) * static_cast<int32_t>(fraction)) >> 16);
        out->push_back(static_cast<int16_t>(value));
      }
      position_ += step;
    }

    // Carry the remainder into the next block rather than restarting at zero,
    // which would repeat or skip a fraction of a sample every frame.
    position_ -= end;
    last_[0] = in[(frames - 1) * 2];
    last_[1] = in[(frames - 1) * 2 + 1];
    have_last_ = true;
  }

 private:
  uint32_t position_ = 0;   // 16.16, within the current block
  int16_t last_[2] = { 0, 0 };
  bool have_last_ = false;
};

}  // namespace utilities
