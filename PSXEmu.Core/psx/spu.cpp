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

#include <algorithm>
#include <cstring>

namespace emulation {
namespace psx {

namespace {

// ADPCM prediction filters, as 1/64ths.
const int kFilterPositive[5] = { 0, 60, 115, 98, 122 };
const int kFilterNegative[5] = { 0,  0, -52, -55, -60 };

// Envelope steps. The rate splits into a shift and a step; the shift decides
// how often the envelope moves, the step by how much.
const int kIncrementStep[4] = { 7, 6, 5, 4 };
const int kDecrementStep[4] = { -8, -7, -6, -5 };

int32_t Clamp16(int32_t value) {
  if (value < -32768) return -32768;
  if (value > 32767) return 32767;
  return value;
}

// The interpolation the hardware applies between ADPCM samples. A quarter of
// the symmetric table is stored and the rest is read backwards, which is what
// the hardware's own table does.
const int16_t kGauss[512] = {
  -0x001,-0x001,-0x001,-0x001,-0x001,-0x001,-0x001,-0x001,
  -0x001,-0x001,-0x001,-0x001,-0x001,-0x001,-0x001,-0x001,
   0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000,
   0x001, 0x001, 0x001, 0x001, 0x001, 0x002, 0x002, 0x002,
   0x003, 0x003, 0x003, 0x004, 0x004, 0x005, 0x005, 0x006,
   0x007, 0x007, 0x008, 0x009, 0x009, 0x00A, 0x00B, 0x00C,
   0x00D, 0x00E, 0x00F, 0x010, 0x011, 0x012, 0x013, 0x015,
   0x016, 0x018, 0x019, 0x01B, 0x01C, 0x01E, 0x020, 0x021,
   0x023, 0x025, 0x027, 0x029, 0x02C, 0x02E, 0x030, 0x033,
   0x035, 0x038, 0x03A, 0x03D, 0x040, 0x043, 0x046, 0x049,
   0x04D, 0x050, 0x054, 0x057, 0x05B, 0x05F, 0x063, 0x067,
   0x06B, 0x06F, 0x074, 0x078, 0x07D, 0x082, 0x087, 0x08C,
   0x091, 0x096, 0x09C, 0x0A1, 0x0A7, 0x0AD, 0x0B3, 0x0BA,
   0x0C0, 0x0C7, 0x0CD, 0x0D4, 0x0DB, 0x0E3, 0x0EA, 0x0F2,
   0x0FA, 0x101, 0x10A, 0x112, 0x11B, 0x123, 0x12C, 0x136,
   0x13F, 0x149, 0x152, 0x15C, 0x167, 0x171, 0x17C, 0x187,
   0x192, 0x19D, 0x1A9, 0x1B5, 0x1C1, 0x1CE, 0x1DB, 0x1E8,
   0x1F5, 0x203, 0x211, 0x21F, 0x22E, 0x23D, 0x24C, 0x25C,
   0x26C, 0x27C, 0x28D, 0x29E, 0x2B0, 0x2C1, 0x2D4, 0x2E6,
   0x2F9, 0x30C, 0x320, 0x334, 0x349, 0x35E, 0x373, 0x389,
   0x39F, 0x3B6, 0x3CD, 0x3E4, 0x3FC, 0x415, 0x42E, 0x447,
   0x461, 0x47B, 0x496, 0x4B2, 0x4CE, 0x4EA, 0x507, 0x525,
   0x543, 0x562, 0x581, 0x5A1, 0x5C1, 0x5E2, 0x604, 0x626,
   0x649, 0x66C, 0x690, 0x6B5, 0x6DA, 0x700, 0x726, 0x74D,
   0x775, 0x79E, 0x7C7, 0x7F1, 0x81B, 0x847, 0x873, 0x89F,
   0x8CD, 0x8FB, 0x92A, 0x959, 0x98A, 0x9BB, 0x9EC, 0xA1F,
   0xA52, 0xA86, 0xABB, 0xAF1, 0xB27, 0xB5E, 0xB96, 0xBCF,
   0xC09, 0xC43, 0xC7F, 0xCBB, 0xCF8, 0xD35, 0xD74, 0xDB3,
   0xDF3, 0xE34, 0xE76, 0xEB9, 0xEFC, 0xF41, 0xF86, 0xFCC,
  0x1013,0x105A,0x10A3,0x10EC,0x1136,0x1181,0x11CD,0x121A,
  0x1267,0x12B5,0x1304,0x1354,0x13A5,0x13F7,0x1449,0x149C,
  0x14F0,0x1545,0x159A,0x15F0,0x1647,0x169F,0x16F7,0x1750,
  0x17AA,0x1804,0x185F,0x18BB,0x1917,0x1974,0x19D2,0x1A30,
  0x1A8F,0x1AEE,0x1B4E,0x1BAE,0x1C0F,0x1C71,0x1CD3,0x1D36,
  0x1D99,0x1DFC,0x1E60,0x1EC5,0x1F2A,0x1F8F,0x1FF5,0x205B,
  0x20C1,0x2128,0x2190,0x21F7,0x225F,0x22C7,0x2330,0x2399,
  0x2402,0x246B,0x24D5,0x253F,0x25A9,0x2613,0x267E,0x26E8,
  0x2753,0x27BE,0x2829,0x2894,0x28FF,0x296A,0x29D6,0x2A41,
  0x2AAC,0x2B18,0x2B83,0x2BEE,0x2C59,0x2CC4,0x2D2F,0x2D9A,
  0x2E05,0x2E70,0x2EDA,0x2F44,0x2FAE,0x3018,0x3082,0x30EB,
  0x3154,0x31BD,0x3226,0x328E,0x32F6,0x335D,0x33C4,0x342B,
  0x3492,0x34F8,0x355D,0x35C2,0x3627,0x368B,0x36EF,0x3752,
  0x37B5,0x3817,0x3878,0x38D9,0x393A,0x399A,0x39F9,0x3A58,
  0x3AB6,0x3B13,0x3B70,0x3BCC,0x3C28,0x3C82,0x3CDC,0x3D35,
  0x3D8E,0x3DE5,0x3E3C,0x3E92,0x3EE7,0x3F3B,0x3F8E,0x3FE1,
  0x4032,0x4083,0x40D2,0x4121,0x416F,0x41BB,0x4207,0x4252,
  0x429B,0x42E4,0x432B,0x4372,0x43B7,0x43FB,0x443E,0x4480,
  0x44C1,0x4500,0x453F,0x457C,0x45B8,0x45F3,0x462C,0x4665,
  0x469C,0x46D2,0x4706,0x473A,0x476C,0x479C,0x47CC,0x47FA,
  0x4826,0x4852,0x487C,0x48A5,0x48CC,0x48F2,0x4917,0x493A,
  0x495C,0x497C,0x499C,0x49B9,0x49D6,0x49F1,0x4A0A,0x4A22,
  0x4A39,0x4A4F,0x4A63,0x4A75,0x4A86,0x4A96,0x4AA5,0x4AB2,
  0x4ABE,0x4AC8,0x4AD1,0x4AD9,0x4ADF,0x4AE4,0x4AE8,0x4AEA,
  0x4AEB,0x4AEA,0x4AE8,0x4AE4,0x4ADF,0x4AD9,0x4AD1,0x4AC8,
  0x4ABE,0x4AB2,0x4AA5,0x4A96,0x4A86,0x4A75,0x4A63,0x4A4F,
  0x4A39,0x4A22,0x4A0A,0x49F1,0x49D6,0x49B9,0x499C,0x497C,
  0x495C,0x493A,0x4917,0x48F2,0x48CC,0x48A5,0x487C,0x4852,
  0x4826,0x47FA,0x47CC,0x479C,0x476C,0x473A,0x4706,0x46D2,
  0x469C,0x4665,0x462C,0x45F3,0x45B8,0x457C,0x453F,0x4500,
  0x44C1,0x4480,0x443E,0x43FB,0x43B7,0x4372,0x432B,0x42E4,
  0x429B,0x4252,0x4207,0x41BB,0x416F,0x4121,0x40D2,0x4083,
  0x4032,0x3FE1,0x3F8E,0x3F3B,0x3EE7,0x3E92,0x3E3C,0x3DE5,
  0x3D8E,0x3D35,0x3CDC,0x3C82,0x3C28,0x3BCC,0x3B70,0x3B13,
  0x3AB6,0x3A58,0x39F9,0x399A,0x393A,0x38D9,0x3878,0x3817,
};

}  // namespace

Spu::Spu() : ram_(nullptr), buffer_(nullptr), engine_(nullptr) {
}

Spu::~Spu() {
}

int Spu::Initialize() {
  ram_ = new uint8_t[kRamSize];
  buffer_ = new int16_t[kBufferFrames * 2];
  memset(ram_, 0, kRamSize);
  memset(buffer_, 0, sizeof(int16_t) * kBufferFrames * 2);
  memset(voices_, 0, sizeof(voices_));
  for (int i = 0; i < kVoices; ++i)
    voices_[i].phase = kOff;

  main_volume_left_ = main_volume_right_ = 0;
  reverb_volume_left_ = reverb_volume_right_ = 0;
  key_on_ = key_off_ = pitch_modulation_ = noise_mode_ = reverb_mode_ = 0;
  endx_ = 0;
  control_ = 0;
  transfer_control_ = 0;
  status_ = 0;
  irq_address_ = 0;
  transfer_address_ = 0;
  transfer_cursor_ = 0;
  cd_volume_left_ = cd_volume_right_ = 0;
  external_volume_left_ = external_volume_right_ = 0;
  memset(reverb_registers_, 0, sizeof(reverb_registers_));

  noise_timer_ = 0;
  noise_level_ = 0;
  reverb_base_ = 0;
  reverb_cursor_ = 0;
  reverb_left_phase_ = true;

  sample_counter_ = 0;
  irq_pending_ = false;

  buffer_read_ = buffer_write_ = buffer_count_ = 0;
  cd_audio_read_ = cd_audio_write_ = cd_audio_count_ = 0;
  cd_resample_fraction_ = 0;
  cd_resample_last_[0] = cd_resample_last_[1] = 0;
  cd_resample_scratch_[0] = cd_resample_scratch_[1] = 0;
  memset(cd_audio_buffer_, 0, sizeof(cd_audio_buffer_));
  memset(&stats_, 0, sizeof(stats_));
  return 0;
}

int Spu::Deinitialize() {
  delete[] ram_;
  delete[] buffer_;
  ram_ = nullptr;
  buffer_ = nullptr;
  return 0;
}

int16_t Spu::VolumeOf(uint16_t reg) {
  // Bit 15 clear means a plain level in the low 15 bits, as a signed value
  // doubled. Bit 15 set selects a sweep, whose starting level is not stored
  // here; treating it as full volume is the usual approximation and is noted
  // in Docs/Gaps.md.
  if (reg & 0x8000)
    return 0x3FFF;
  return static_cast<int16_t>(static_cast<int16_t>(reg << 1) >> 1);
}

// ---------------------------------------------------------------------------
// ADPCM
// ---------------------------------------------------------------------------

void Spu::DecodeBlock(Voice& voice) {
  const uint32_t address = voice.current_address & (kRamSize - 1);
  CheckIrq(address);

  const uint8_t header = ram_[address];
  const uint8_t flags = ram_[address + 1];
  int shift = header & 0x0F;
  int filter = (header >> 4) & 0x0F;
  // Shifts above 12 behave as 9, and filters above 4 do not exist; both are
  // clamped rather than read off the end of the coefficient table.
  if (shift > 12)
    shift = 9;
  if (filter > 4)
    filter = 0;

  // A block flagged as the loop point records where to come back to.
  if (flags & 0x04) {
    voice.repeat_address = static_cast<uint16_t>((address / 8) & 0xFFFF);
    voice.repeat_set = true;
  }

  for (int i = 0; i < 28; ++i) {
    const uint8_t byte = ram_[address + 2 + (i / 2)];
    const int nibble = (i & 1) ? (byte >> 4) : (byte & 0x0F);
    // A 4-bit sample sits in the top of a 16-bit word, then the block's shift
    // scales it down.
    int32_t sample = static_cast<int16_t>(nibble << 12) >> shift;
    sample += (voice.previous0 * kFilterPositive[filter] +
               voice.previous1 * kFilterNegative[filter] + 32) / 64;
    sample = Clamp16(sample);
    voice.decoded[i] = static_cast<int16_t>(sample);
    voice.previous1 = voice.previous0;
    voice.previous0 = static_cast<int16_t>(sample);
  }
  ++stats_.blocks_decoded;

  // Bit 0 ends the sample; bit 1 says whether to loop back or stop.
  if (flags & 0x01) {
    voice.ended = true;
    endx_ |= (1u << (&voice - voices_));
    if (flags & 0x02) {
      voice.current_address = static_cast<uint32_t>(voice.repeat_address) * 8;
    } else {
      // No loop: release immediately and silence the voice.
      voice.phase = kRelease;
      voice.level = 0;
      voice.current_address = static_cast<uint32_t>(voice.repeat_address) * 8;
    }
  } else {
    voice.current_address = address + 16;
  }
}

// ---------------------------------------------------------------------------
// Envelope
// ---------------------------------------------------------------------------

void Spu::StepEnvelope(Voice& voice) {
  if (voice.phase == kOff)
    return;

  const uint16_t low = voice.adsr_low;
  const uint16_t high = voice.adsr_high;

  int rate = 0;
  bool exponential = false;
  bool decreasing = false;
  int32_t target = 0;

  switch (voice.phase) {
    case kAttack:
      rate = (low >> 8) & 0x7F;
      exponential = (low & 0x8000) != 0;
      decreasing = false;
      target = 0x7FFF;
      break;
    case kDecay:
      rate = ((low >> 4) & 0x0F) << 2;   // decay has only a coarse shift
      exponential = true;
      decreasing = true;
      target = std::min<int32_t>(((low & 0x0F) + 1) * 0x800, 0x7FFF);
      break;
    case kSustain:
      rate = (high >> 6) & 0x7F;
      exponential = (high & 0x8000) != 0;
      decreasing = (high & 0x4000) != 0;
      target = decreasing ? 0 : 0x7FFF;
      break;
    case kRelease:
      rate = (high & 0x1F) << 2;
      exponential = (high & 0x0020) != 0;
      decreasing = true;
      target = 0;
      break;
    default:
      return;
  }

  const int shift = (rate >> 2) & 0x1F;
  const int step_index = rate & 3;
  int32_t step = decreasing ? kDecrementStep[step_index]
                            : kIncrementStep[step_index];

  // The shift both scales the step and spreads it over several samples.
  if (shift < 11)
    step <<= (11 - shift);
  int32_t period = (shift > 11) ? (1 << (shift - 11)) : 1;

  if (exponential) {
    if (!decreasing && voice.level > 0x6000) {
      // An exponential attack slows down for the last quarter.
      period *= 4;
    }
    if (decreasing)
      step = (step * voice.level) >> 15;
  }

  // `period` is how many samples pass between steps, not something to divide
  // the step by. Dividing truncates to zero for every rate with a shift above
  // 11, which is most of them - the envelope then never moves and every voice
  // stays silent no matter what the sample data says.
  if (++voice.envelope_counter < static_cast<uint32_t>(period))
    return;
  voice.envelope_counter = 0;

  voice.level += step;
  voice.level = std::min<int32_t>(std::max<int32_t>(voice.level, 0), 0x7FFF);

  switch (voice.phase) {
    case kAttack:
      if (voice.level >= 0x7FFF)
        voice.phase = kDecay;
      break;
    case kDecay:
      if (voice.level <= target)
        voice.phase = kSustain;
      break;
    case kRelease:
      if (voice.level <= 0) {
        voice.phase = kOff;
        voice.active = false;
      }
      break;
    default:
      break;
  }
  voice.adsr_volume = static_cast<uint16_t>(voice.level);
}

void Spu::KeyOn(int index) {
  Voice& voice = voices_[index];
  voice.current_address = static_cast<uint32_t>(voice.start_address) * 8;
  voice.repeat_address = voice.start_address;
  voice.repeat_set = false;
  voice.counter = 0;
  voice.decoded_index = 28;          // force a decode on the first step
  voice.previous0 = voice.previous1 = 0;
  memset(voice.history, 0, sizeof(voice.history));
  memset(voice.decoded, 0, sizeof(voice.decoded));
  voice.phase = kAttack;
  voice.level = 0;
  voice.envelope_counter = 0;
  voice.adsr_volume = 0;
  voice.active = true;
  voice.ended = false;
  endx_ &= ~(1u << index);
  ++stats_.key_ons;
}

void Spu::KeyOff(int index) {
  Voice& voice = voices_[index];
  if (voice.phase != kOff)
    voice.phase = kRelease;
  ++stats_.key_offs;
}

// ---------------------------------------------------------------------------
// Voice stepping
// ---------------------------------------------------------------------------

void Spu::StepNoise() {
  const int frequency = (control_ >> 8) & 0x3F;
  noise_timer_ += 0x8000 >> std::max(0, 20 - (frequency >> 2));
  if (noise_timer_ >= 0x8000) {
    noise_timer_ -= 0x8000;
    // The hardware's generator is a shift register with a parity tap.
    const uint32_t bit = ((noise_level_ >> 15) ^ (noise_level_ >> 12) ^
                          (noise_level_ >> 11) ^ (noise_level_ >> 10) ^ 1) & 1;
    noise_level_ = static_cast<int16_t>((noise_level_ << 1) | bit);
  }
}

int16_t Spu::StepVoice(Voice& voice, int index, int16_t previous_output) {
  if (!voice.active && voice.phase == kOff)
    return 0;

  // Pitch, optionally modulated by the previous voice's output.
  int32_t step = voice.pitch & 0x3FFF;
  if ((pitch_modulation_ & (1u << index)) && index > 0) {
    const int32_t factor = 0x8000 + previous_output;
    step = (step * factor) >> 15;
    step &= 0xFFFF;
  }
  step = std::min<int32_t>(step, 0x3FFF);

  voice.counter += static_cast<uint32_t>(step);
  while (voice.counter >= 0x1000) {
    voice.counter -= 0x1000;
    if (voice.decoded_index >= 28) {
      DecodeBlock(voice);
      voice.decoded_index = 0;
    }
    // Shift the interpolation window along.
    voice.history[0] = voice.history[1];
    voice.history[1] = voice.history[2];
    voice.history[2] = voice.history[3];
    voice.history[3] = voice.decoded[voice.decoded_index++];
  }

  // Four-point interpolation between the samples, indexed by the fractional
  // part of the counter. Without it a pitched-down sample is audibly steppy.
  const uint32_t fraction = (voice.counter >> 4) & 0xFF;
  int32_t out = 0;
  out += (kGauss[0x0FF - fraction] * voice.history[0]) >> 15;
  out += (kGauss[0x1FF - fraction] * voice.history[1]) >> 15;
  out += (kGauss[0x100 + fraction] * voice.history[2]) >> 15;
  out += (kGauss[0x000 + fraction] * voice.history[3]) >> 15;
  out = Clamp16(out);

  if (noise_mode_ & (1u << index))
    out = noise_level_;

  StepEnvelope(voice);
  out = (out * voice.level) >> 15;
  return static_cast<int16_t>(Clamp16(out));
}

// ---------------------------------------------------------------------------
// Reverb
// ---------------------------------------------------------------------------

void Spu::ProcessReverb(int32_t input_left, int32_t input_right,
                        int32_t* output_left, int32_t* output_right) {
  *output_left = 0;
  *output_right = 0;

  // Reverb is only meaningful once software has given it a work area, and it
  // is disabled outright by the control register's reverb bit.
  if ((control_ & 0x0080) == 0 || reverb_base_ >= kRamSize)
    return;

  const uint32_t area = kRamSize - reverb_base_;
  if (area < 0x100)
    return;

  // A simple two-tap delay out of the work area. The hardware runs a much
  // larger comb-and-allpass network off the same buffer; this reproduces the
  // shape of the effect - a decaying echo at the configured depth - without
  // claiming to reproduce its exact response. Docs/Gaps.md records that.
  const uint32_t offset_a = reverb_cursor_ % area;
  const uint32_t offset_b = (reverb_cursor_ + area / 2) % area;

  const int16_t delayed_left =
      static_cast<int16_t>(RamHalf(reverb_base_ + (offset_a & ~1u)));
  const int16_t delayed_right =
      static_cast<int16_t>(RamHalf(reverb_base_ + (offset_b & ~1u)));

  // reverb_registers_[0] and [1] are the master reverb volumes.
  const int32_t feedback_left =
      (static_cast<int16_t>(reverb_registers_[0]) * delayed_left) >> 15;
  const int32_t feedback_right =
      (static_cast<int16_t>(reverb_registers_[1]) * delayed_right) >> 15;

  WriteRamHalf(reverb_base_ + (offset_a & ~1u),
               static_cast<uint16_t>(Clamp16(input_left + (feedback_left >> 1))));
  WriteRamHalf(reverb_base_ + (offset_b & ~1u),
               static_cast<uint16_t>(Clamp16(input_right + (feedback_right >> 1))));

  reverb_cursor_ = (reverb_cursor_ + 2) % area;

  *output_left = delayed_left;
  *output_right = delayed_right;
}

// ---------------------------------------------------------------------------
// Mixing
// ---------------------------------------------------------------------------

void Spu::PushFrame(int16_t left, int16_t right) {
  if (buffer_count_ >= kBufferFrames) {
    // Nobody is draining. Drop the oldest frame rather than stalling the
    // emulation, and count it - silence because the buffer overflowed and
    // silence because nothing was playing look identical otherwise.
    buffer_read_ = (buffer_read_ + 1) % kBufferFrames;
    --buffer_count_;
    ++stats_.frames_dropped;
  }
  buffer_[buffer_write_ * 2 + 0] = left;
  buffer_[buffer_write_ * 2 + 1] = right;
  buffer_write_ = (buffer_write_ + 1) % kBufferFrames;
  ++buffer_count_;
}
void Spu::PushCdFrame(int16_t left, int16_t right) {
  if (cd_audio_count_ >= kSampleRate * 2) {
    // Full. Dropping the oldest frame keeps the stream running rather than
    // stalling it, which matters when a game is streaming faster than the
    // host is draining.
    cd_audio_read_ = (cd_audio_read_ + 2) % (kSampleRate * 2);
    cd_audio_count_ -= 2;
  }
  cd_audio_buffer_[cd_audio_write_] = left;
  cd_audio_buffer_[cd_audio_write_ + 1] = right;
  cd_audio_write_ = (cd_audio_write_ + 2) % (kSampleRate * 2);
  cd_audio_count_ += 2;
}

// Interleaved stereo at some other rate, resampled onto the mixer's own.
//
// XA-ADPCM arrives at 37800 or 18900 Hz and one sector of it is about a tenth
// of a second, so the join between sectors is audible if it is not continuous:
// the fractional position and the last frame carry over, and the first output
// frame of a sector interpolates from the end of the one before it.
//
// Linear interpolation, not the hardware's seven-point filter. The difference
// is a slight softening of the top end, not a wrong pitch or a click.
void Spu::QueueCdSamples(const int16_t* stereo, int frames, int sample_rate) {
  if (stereo == nullptr || frames <= 0)
    return;

  if (sample_rate == kSampleRate) {
    for (int i = 0; i < frames; ++i)
      PushCdFrame(stereo[i * 2], stereo[i * 2 + 1]);
    cd_resample_last_[0] = stereo[(frames - 1) * 2];
    cd_resample_last_[1] = stereo[(frames - 1) * 2 + 1];
    return;
  }

  // How far to step through the source for each frame of output, 16.16.
  const uint32_t step = static_cast<uint32_t>(
      (static_cast<uint64_t>(sample_rate) << 16) / kSampleRate);

  uint32_t position = cd_resample_fraction_;
  while ((position >> 16) < static_cast<uint32_t>(frames)) {
    const uint32_t index = position >> 16;
    const int32_t weight = static_cast<int32_t>(position & 0xFFFF);

    for (int channel = 0; channel < 2; ++channel) {
      const int32_t previous =
          (index == 0) ? cd_resample_last_[channel]
                       : stereo[(index - 1) * 2 + channel];
      const int32_t current = stereo[index * 2 + channel];
      const int32_t value =
          previous + (((current - previous) * weight) >> 16);
      if (channel == 0)
        cd_resample_scratch_[0] = static_cast<int16_t>(value);
      else
        cd_resample_scratch_[1] = static_cast<int16_t>(value);
    }
    PushCdFrame(cd_resample_scratch_[0], cd_resample_scratch_[1]);
    position += step;
  }

  // Whatever is left over starts the next sector, and its frame before the
  // first is the last one of this.
  cd_resample_fraction_ = position - (static_cast<uint32_t>(frames) << 16);
  cd_resample_last_[0] = stereo[(frames - 1) * 2];
  cd_resample_last_[1] = stereo[(frames - 1) * 2 + 1];
}


void Spu::QueueCdAudio(const uint8_t* raw_sector) {
  // A raw CD-DA sector is 2352 bytes, which is exactly 588 stereo pairs
  // (16-bit left, 16-bit right, little-endian).
  const int kSamples = 588;
  for (int i = 0; i < kSamples; ++i) {
    if (cd_audio_count_ >= kSampleRate * 2) {
      // Buffer full, drop oldest
      cd_audio_read_ = (cd_audio_read_ + 2) % (kSampleRate * 2);
      cd_audio_count_ -= 2;
    }
    
    int offset = i * 4;
    int16_t left = static_cast<int16_t>(raw_sector[offset] | (raw_sector[offset + 1] << 8));
    int16_t right = static_cast<int16_t>(raw_sector[offset + 2] | (raw_sector[offset + 3] << 8));
    
    cd_audio_buffer_[cd_audio_write_] = left;
    cd_audio_buffer_[cd_audio_write_ + 1] = right;
    
    cd_audio_write_ = (cd_audio_write_ + 2) % (kSampleRate * 2);
    cd_audio_count_ += 2;
  }
}

void Spu::GenerateFrame() {
  StepNoise();

  int32_t left = 0, right = 0;
  int32_t reverb_in_left = 0, reverb_in_right = 0;
  
  if (cd_audio_count_ > 0) {
    int16_t cd_left = cd_audio_buffer_[cd_audio_read_];
    int16_t cd_right = cd_audio_buffer_[cd_audio_read_ + 1];
    cd_audio_read_ = (cd_audio_read_ + 2) % (kSampleRate * 2);
    cd_audio_count_ -= 2;

    if (control_ & 0x0001) { // CD Audio Enable
      int32_t l = (cd_left * VolumeOf(cd_volume_left_)) >> 15;
      int32_t r = (cd_right * VolumeOf(cd_volume_right_)) >> 15;
      left += l;
      right += r;
      if (control_ & 0x0004) { // CD Audio Reverb Enable
        reverb_in_left += l;
        reverb_in_right += r;
      }
    }
  }

  uint32_t active = 0;
  int16_t previous = 0;

  for (int i = 0; i < kVoices; ++i) {
    Voice& voice = voices_[i];
    const int16_t sample = StepVoice(voice, i, previous);
    previous = sample;
    if (voice.phase != kOff)
      ++active;

    const int32_t l = (sample * VolumeOf(voice.volume_left)) >> 15;
    const int32_t r = (sample * VolumeOf(voice.volume_right)) >> 15;
    left += l;
    right += r;
    if (reverb_mode_ & (1u << i)) {
      reverb_in_left += l;
      reverb_in_right += r;
    }
  }

  int32_t reverb_left = 0, reverb_right = 0;
  ProcessReverb(Clamp16(reverb_in_left), Clamp16(reverb_in_right),
                &reverb_left, &reverb_right);
  left += (reverb_left * static_cast<int16_t>(reverb_volume_left_)) >> 15;
  right += (reverb_right * static_cast<int16_t>(reverb_volume_right_)) >> 15;

  left = (Clamp16(left) * VolumeOf(main_volume_left_)) >> 15;
  right = (Clamp16(right) * VolumeOf(main_volume_right_)) >> 15;

  // The mute bit silences the output without stopping the voices.
  if ((control_ & 0x4000) == 0) {
    left = 0;
    right = 0;
  }

  const int16_t out_left = static_cast<int16_t>(Clamp16(left));
  const int16_t out_right = static_cast<int16_t>(Clamp16(right));

  if (out_left > stats_.peak_left) stats_.peak_left = out_left;
  if (out_right > stats_.peak_right) stats_.peak_right = out_right;
  stats_.voices_active = active;
  ++stats_.frames;

  PushFrame(out_left, out_right);

  if (engine_ != nullptr) {
    const int16_t frame[2] = { out_left, out_right };
    engine_->QueueAudio(frame, 2);
  }
}

void Spu::Tick(uint32_t cycles) {
  if (ram_ == nullptr)
    return;

  sample_counter_ += cycles;
  while (sample_counter_ >= kCyclesPerSample) {
    sample_counter_ -= kCyclesPerSample;
    GenerateFrame();
  }

  if (irq_pending_) {
    irq_pending_ = false;
    status_ |= 0x0040;
    ++stats_.irqs;
    system().io().SetInterrupt(kInterruptSPU);
  }
}

int Spu::ReadSamples(int16_t* out, int frames) {
  if (out == nullptr || frames <= 0)
    return 0;
  const int count = std::min(frames, buffer_count_);
  for (int i = 0; i < count; ++i) {
    out[i * 2 + 0] = buffer_[buffer_read_ * 2 + 0];
    out[i * 2 + 1] = buffer_[buffer_read_ * 2 + 1];
    buffer_read_ = (buffer_read_ + 1) % kBufferFrames;
  }
  buffer_count_ -= count;
  return count;
}

int Spu::QueuedFrames() const {
  return buffer_count_;
}

void Spu::CheckIrq(uint32_t byte_address) {
  if ((control_ & 0x0040) == 0)
    return;
  const uint32_t irq = static_cast<uint32_t>(irq_address_) * 8;
  // The address is checked against the 16-byte block being read, which is the
  // granularity the hardware compares at.
  if ((byte_address & ~0x0Fu) == (irq & ~0x0Fu))
    irq_pending_ = true;
}

// ---------------------------------------------------------------------------
// Sound RAM transfers
// ---------------------------------------------------------------------------

void Spu::WriteDataWord(uint32_t value) {
  WriteRamHalf(transfer_cursor_, static_cast<uint16_t>(value));
  transfer_cursor_ = (transfer_cursor_ + 2) & (kRamSize - 1);
  WriteRamHalf(transfer_cursor_, static_cast<uint16_t>(value >> 16));
  transfer_cursor_ = (transfer_cursor_ + 2) & (kRamSize - 1);
}

uint32_t Spu::ReadDataWord() {
  uint32_t value = RamHalf(transfer_cursor_);
  transfer_cursor_ = (transfer_cursor_ + 2) & (kRamSize - 1);
  value |= static_cast<uint32_t>(RamHalf(transfer_cursor_)) << 16;
  transfer_cursor_ = (transfer_cursor_ + 2) & (kRamSize - 1);
  return value;
}

// ---------------------------------------------------------------------------
// Registers
// ---------------------------------------------------------------------------

uint16_t Spu::Read(uint32_t address) {
  const uint32_t offset = address & 0x3FF;

  if (offset < 0x180) {
    const int index = offset / 0x10;
    const Voice& voice = voices_[index];
    switch (offset & 0x0F) {
      case 0x0: return voice.volume_left;
      case 0x2: return voice.volume_right;
      case 0x4: return voice.pitch;
      case 0x6: return voice.start_address;
      case 0x8: return voice.adsr_low;
      case 0xA: return voice.adsr_high;
      case 0xC: return voice.adsr_volume;
      default:  return voice.repeat_address;
    }
  }

  switch (offset) {
    case 0x180: return main_volume_left_;
    case 0x182: return main_volume_right_;
    case 0x184: return reverb_volume_left_;
    case 0x186: return reverb_volume_right_;
    case 0x188: return static_cast<uint16_t>(key_on_);
    case 0x18A: return static_cast<uint16_t>(key_on_ >> 16);
    case 0x18C: return static_cast<uint16_t>(key_off_);
    case 0x18E: return static_cast<uint16_t>(key_off_ >> 16);
    case 0x190: return static_cast<uint16_t>(pitch_modulation_);
    case 0x192: return static_cast<uint16_t>(pitch_modulation_ >> 16);
    case 0x194: return static_cast<uint16_t>(noise_mode_);
    case 0x196: return static_cast<uint16_t>(noise_mode_ >> 16);
    case 0x198: return static_cast<uint16_t>(reverb_mode_);
    case 0x19A: return static_cast<uint16_t>(reverb_mode_ >> 16);
    // ENDX is read-only and says which voices have reached the end of their
    // sample - software polls it to know when a one-shot has finished.
    case 0x19C: return static_cast<uint16_t>(endx_);
    case 0x19E: return static_cast<uint16_t>(endx_ >> 16);
    case 0x1A2: return static_cast<uint16_t>(reverb_base_ / 8);
    case 0x1A4: return irq_address_;
    case 0x1A6: return transfer_address_;
    case 0x1AA: return control_;
    case 0x1AC: return transfer_control_;
    case 0x1AE:
      // The status register mirrors the low bits of the control register, and
      // reports the transfer as always idle: this core completes them at once.
      return static_cast<uint16_t>((status_ & 0xFFC0) | (control_ & 0x3F));
    case 0x1B0: return cd_volume_left_;
    case 0x1B2: return cd_volume_right_;
    case 0x1B4: return external_volume_left_;
    case 0x1B6: return external_volume_right_;
    case 0x1B8: return main_volume_left_;
    case 0x1BA: return main_volume_right_;
    default:
      if (offset >= 0x1C0 && offset < 0x200)
        return reverb_registers_[(offset - 0x1C0) / 2];
      return 0;
  }
}

void Spu::Write(uint32_t address, uint16_t data) {
  const uint32_t offset = address & 0x3FF;

  if (offset < 0x180) {
    const int index = offset / 0x10;
    Voice& voice = voices_[index];
    switch (offset & 0x0F) {
      case 0x0: voice.volume_left = data; return;
      case 0x2: voice.volume_right = data; return;
      case 0x4: voice.pitch = data; return;
      case 0x6: voice.start_address = data; return;
      case 0x8: voice.adsr_low = data; return;
      case 0xA: voice.adsr_high = data; return;
      case 0xC: voice.adsr_volume = data;
                voice.level = data & 0x7FFF; return;
      default:  voice.repeat_address = data;
                voice.repeat_set = true; return;
    }
  }

  switch (offset) {
    case 0x180: main_volume_left_ = data; return;
    case 0x182: main_volume_right_ = data; return;
    case 0x184: reverb_volume_left_ = data; return;
    case 0x186: reverb_volume_right_ = data; return;

    // Key on and key off are edge-triggered: writing a one starts or stops
    // that voice there and then. Storing the value and acting on it later
    // would miss a voice that is keyed on and off inside one frame.
    case 0x188:
      key_on_ = (key_on_ & 0xFFFF0000) | data;
      for (int i = 0; i < 16; ++i)
        if (data & (1u << i)) KeyOn(i);
      return;
    case 0x18A:
      key_on_ = (key_on_ & 0x0000FFFF) | (static_cast<uint32_t>(data) << 16);
      for (int i = 0; i < 8; ++i)
        if (data & (1u << i)) KeyOn(16 + i);
      return;
    case 0x18C:
      key_off_ = (key_off_ & 0xFFFF0000) | data;
      for (int i = 0; i < 16; ++i)
        if (data & (1u << i)) KeyOff(i);
      return;
    case 0x18E:
      key_off_ = (key_off_ & 0x0000FFFF) | (static_cast<uint32_t>(data) << 16);
      for (int i = 0; i < 8; ++i)
        if (data & (1u << i)) KeyOff(16 + i);
      return;

    case 0x190: pitch_modulation_ = (pitch_modulation_ & 0xFFFF0000) | data; return;
    case 0x192: pitch_modulation_ =
                    (pitch_modulation_ & 0xFFFF) | (static_cast<uint32_t>(data) << 16);
                return;
    case 0x194: noise_mode_ = (noise_mode_ & 0xFFFF0000) | data; return;
    case 0x196: noise_mode_ =
                    (noise_mode_ & 0xFFFF) | (static_cast<uint32_t>(data) << 16);
                return;
    case 0x198: reverb_mode_ = (reverb_mode_ & 0xFFFF0000) | data; return;
    case 0x19A: reverb_mode_ =
                    (reverb_mode_ & 0xFFFF) | (static_cast<uint32_t>(data) << 16);
                return;
    case 0x19C: case 0x19E: return;      // ENDX is read-only

    case 0x1A2:
      reverb_base_ = static_cast<uint32_t>(data) * 8;
      reverb_cursor_ = 0;
      return;
    case 0x1A4: irq_address_ = data; return;
    case 0x1A6:
      transfer_address_ = data;
      transfer_cursor_ = static_cast<uint32_t>(data) * 8;
      return;
    case 0x1A8:
      // The data port: software writes samples here one halfword at a time.
      WriteRamHalf(transfer_cursor_, data);
      transfer_cursor_ = (transfer_cursor_ + 2) & (kRamSize - 1);
      return;
    case 0x1AA:
      control_ = data;
      // Acknowledging the interrupt is done by clearing the enable bit.
      if ((data & 0x0040) == 0)
        status_ &= ~0x0040;
      return;
    case 0x1AC: transfer_control_ = data; return;
    case 0x1AE: return;                  // status is read-only
    case 0x1B0: cd_volume_left_ = data; return;
    case 0x1B2: cd_volume_right_ = data; return;
    case 0x1B4: external_volume_left_ = data; return;
    case 0x1B6: external_volume_right_ = data; return;
    default:
      if (offset >= 0x1C0 && offset < 0x200)
        reverb_registers_[(offset - 0x1C0) / 2] = data;
      return;
  }
}

}
}
