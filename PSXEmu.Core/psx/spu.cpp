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

// The interpolation the hardware applies between ADPCM samples: psx-spx's
// table, half a Gaussian rising to 59B3h. The four weights an index picks
// out - 0FFh-i, 1FFh-i, 100h+i and i - always add up to just under 8000h, so
// interpolating never changes a sample's level.
//
// This was a different table, a full bell peaking at 4AEBh, whose four
// weights summed to anything from 0.80 to 1.13 depending on where the pitch
// counter sat between samples. Any pitch but a multiple of 1000h swept
// through those, modulating the voice's level at a rate tied to its pitch -
// a buzzy roughness on nearly every note - and at the native rate the voice
// played at 80%, smeared over three samples (bug 154).
const int16_t kGauss[512] = {
 -0x0001,-0x0001,-0x0001,-0x0001,-0x0001,-0x0001,-0x0001,-0x0001,
 -0x0001,-0x0001,-0x0001,-0x0001,-0x0001,-0x0001,-0x0001,-0x0001,
  0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0001,
  0x0001, 0x0001, 0x0001, 0x0002, 0x0002, 0x0002, 0x0003, 0x0003,
  0x0003, 0x0004, 0x0004, 0x0005, 0x0005, 0x0006, 0x0007, 0x0007,
  0x0008, 0x0009, 0x0009, 0x000A, 0x000B, 0x000C, 0x000D, 0x000E,
  0x000F, 0x0010, 0x0011, 0x0012, 0x0013, 0x0015, 0x0016, 0x0018,
  0x0019, 0x001B, 0x001C, 0x001E, 0x0020, 0x0021, 0x0023, 0x0025,
  0x0027, 0x0029, 0x002C, 0x002E, 0x0030, 0x0033, 0x0035, 0x0038,
  0x003A, 0x003D, 0x0040, 0x0043, 0x0046, 0x0049, 0x004D, 0x0050,
  0x0054, 0x0057, 0x005B, 0x005F, 0x0063, 0x0067, 0x006B, 0x006F,
  0x0074, 0x0078, 0x007D, 0x0082, 0x0087, 0x008C, 0x0091, 0x0096,
  0x009C, 0x00A1, 0x00A7, 0x00AD, 0x00B3, 0x00BA, 0x00C0, 0x00C7,
  0x00CD, 0x00D4, 0x00DB, 0x00E3, 0x00EA, 0x00F2, 0x00FA, 0x0101,
  0x010A, 0x0112, 0x011B, 0x0123, 0x012C, 0x0135, 0x013F, 0x0148,
  0x0152, 0x015C, 0x0166, 0x0171, 0x017B, 0x0186, 0x0191, 0x019C,
  0x01A8, 0x01B4, 0x01C0, 0x01CC, 0x01D9, 0x01E5, 0x01F2, 0x0200,
  0x020D, 0x021B, 0x0229, 0x0237, 0x0246, 0x0255, 0x0264, 0x0273,
  0x0283, 0x0293, 0x02A3, 0x02B4, 0x02C4, 0x02D6, 0x02E7, 0x02F9,
  0x030B, 0x031D, 0x0330, 0x0343, 0x0356, 0x036A, 0x037E, 0x0392,
  0x03A7, 0x03BC, 0x03D1, 0x03E7, 0x03FC, 0x0413, 0x042A, 0x0441,
  0x0458, 0x0470, 0x0488, 0x04A0, 0x04B9, 0x04D2, 0x04EC, 0x0506,
  0x0520, 0x053B, 0x0556, 0x0572, 0x058E, 0x05AA, 0x05C7, 0x05E4,
  0x0601, 0x061F, 0x063E, 0x065C, 0x067C, 0x069B, 0x06BB, 0x06DC,
  0x06FD, 0x071E, 0x0740, 0x0762, 0x0784, 0x07A7, 0x07CB, 0x07EF,
  0x0813, 0x0838, 0x085D, 0x0883, 0x08A9, 0x08D0, 0x08F7, 0x091E,
  0x0946, 0x096F, 0x0998, 0x09C1, 0x09EB, 0x0A16, 0x0A40, 0x0A6C,
  0x0A98, 0x0AC4, 0x0AF1, 0x0B1E, 0x0B4C, 0x0B7A, 0x0BA9, 0x0BD8,
  0x0C07, 0x0C38, 0x0C68, 0x0C99, 0x0CCB, 0x0CFD, 0x0D30, 0x0D63,
  0x0D97, 0x0DCB, 0x0E00, 0x0E35, 0x0E6B, 0x0EA1, 0x0ED7, 0x0F0F,
  0x0F46, 0x0F7F, 0x0FB7, 0x0FF1, 0x102A, 0x1065, 0x109F, 0x10DB,
  0x1116, 0x1153, 0x118F, 0x11CD, 0x120B, 0x1249, 0x1288, 0x12C7,
  0x1307, 0x1347, 0x1388, 0x13C9, 0x140B, 0x144D, 0x1490, 0x14D4,
  0x1517, 0x155C, 0x15A0, 0x15E6, 0x162C, 0x1672, 0x16B9, 0x1700,
  0x1747, 0x1790, 0x17D8, 0x1821, 0x186B, 0x18B5, 0x1900, 0x194B,
  0x1996, 0x19E2, 0x1A2E, 0x1A7B, 0x1AC8, 0x1B16, 0x1B64, 0x1BB3,
  0x1C02, 0x1C51, 0x1CA1, 0x1CF1, 0x1D42, 0x1D93, 0x1DE5, 0x1E37,
  0x1E89, 0x1EDC, 0x1F2F, 0x1F82, 0x1FD6, 0x202A, 0x207F, 0x20D4,
  0x2129, 0x217F, 0x21D5, 0x222C, 0x2282, 0x22DA, 0x2331, 0x2389,
  0x23E1, 0x2439, 0x2492, 0x24EB, 0x2545, 0x259E, 0x25F8, 0x2653,
  0x26AD, 0x2708, 0x2763, 0x27BE, 0x281A, 0x2876, 0x28D2, 0x292E,
  0x298B, 0x29E7, 0x2A44, 0x2AA1, 0x2AFF, 0x2B5C, 0x2BBA, 0x2C18,
  0x2C76, 0x2CD4, 0x2D33, 0x2D91, 0x2DF0, 0x2E4F, 0x2EAE, 0x2F0D,
  0x2F6C, 0x2FCC, 0x302B, 0x308B, 0x30EA, 0x314A, 0x31AA, 0x3209,
  0x3269, 0x32C9, 0x3329, 0x3389, 0x33E9, 0x3449, 0x34A9, 0x3509,
  0x3569, 0x35C9, 0x3629, 0x3689, 0x36E8, 0x3748, 0x37A8, 0x3807,
  0x3867, 0x38C6, 0x3926, 0x3985, 0x39E4, 0x3A43, 0x3AA2, 0x3B00,
  0x3B5F, 0x3BBD, 0x3C1B, 0x3C79, 0x3CD7, 0x3D35, 0x3D92, 0x3DEF,
  0x3E4C, 0x3EA9, 0x3F05, 0x3F62, 0x3FBD, 0x4019, 0x4074, 0x40D0,
  0x412A, 0x4185, 0x41DF, 0x4239, 0x4292, 0x42EB, 0x4344, 0x439C,
  0x43F4, 0x444C, 0x44A3, 0x44FA, 0x4550, 0x45A6, 0x45FC, 0x4651,
  0x46A6, 0x46FA, 0x474E, 0x47A1, 0x47F4, 0x4846, 0x4898, 0x48E9,
  0x493A, 0x498A, 0x49D9, 0x4A29, 0x4A77, 0x4AC5, 0x4B13, 0x4B5F,
  0x4BAC, 0x4BF7, 0x4C42, 0x4C8D, 0x4CD7, 0x4D20, 0x4D68, 0x4DB0,
  0x4DF7, 0x4E3E, 0x4E84, 0x4EC9, 0x4F0E, 0x4F52, 0x4F95, 0x4FD7,
  0x5019, 0x505A, 0x509A, 0x50DA, 0x5118, 0x5156, 0x5194, 0x51D0,
  0x520C, 0x5247, 0x5281, 0x52BA, 0x52F3, 0x532A, 0x5361, 0x5397,
  0x53CC, 0x5401, 0x5434, 0x5467, 0x5499, 0x54CA, 0x54FA, 0x5529,
  0x5558, 0x5585, 0x55B2, 0x55DE, 0x5609, 0x5632, 0x565B, 0x5684,
  0x56AB, 0x56D1, 0x56F6, 0x571B, 0x573E, 0x5761, 0x5782, 0x57A3,
  0x57C3, 0x57E2, 0x57FF, 0x581C, 0x5838, 0x5853, 0x586D, 0x5886,
  0x589E, 0x58B5, 0x58CB, 0x58E0, 0x58F4, 0x5907, 0x5919, 0x592A,
  0x593A, 0x5949, 0x5958, 0x5965, 0x5971, 0x597C, 0x5986, 0x598F,
  0x5997, 0x599E, 0x59A4, 0x59A9, 0x59AD, 0x59B0, 0x59B2, 0x59B3,
};

}  // namespace

Spu::Spu()
    : ram_(nullptr),
      buffer_(nullptr),
      trace_voices_(false),
      voice_events_(nullptr),
      voice_event_count_(0) {
}

Spu::~Spu() {
}

// Allocated on demand: a front end never asks for this, and 16384 events is
// 320 KB nobody else should pay for.
void Spu::set_trace_voices(bool on) {
  if (on && voice_events_ == nullptr)
    voice_events_ = new VoiceEvent[kMaxVoiceEvents];
  voice_event_count_ = 0;
  trace_voices_ = on;
}

int Spu::Initialize() {
  ram_ = new uint8_t[kRamSize];
  buffer_ = new int16_t[kBufferFrames * 2];
  memset(ram_, 0, kRamSize);
  memset(buffer_, 0, sizeof(int16_t) * kBufferFrames * 2);
  memset(voices_, 0, sizeof(voices_));
  for (int i = 0; i < kVoices; ++i)
    voices_[i].phase = kOff;

  main_volume_left_ = VolumeSweep{0, 0, 0};
  main_volume_right_ = VolumeSweep{0, 0, 0};
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
  reverb_left_phase_ = false;
  memset(reverb_downsample_, 0, sizeof(reverb_downsample_));
  memset(reverb_upsample_, 0, sizeof(reverb_upsample_));
  reverb_resample_position_ = 0;

  sample_counter_ = 0;
  irq_pending_ = false;

  buffer_read_ = buffer_write_ = buffer_count_ = 0;
  cd_audio_read_ = cd_audio_write_ = cd_audio_count_ = 0;
  memset(xa_ring_, 0, sizeof(xa_ring_));
  xa_ring_position_ = 0;
  xa_sixstep_ = 6;
  memset(cd_audio_buffer_, 0, sizeof(cd_audio_buffer_));
  memset(&stats_, 0, sizeof(stats_));
  return 0;
}

void Spu::Serialise(StateIO& io) {
  io.Plain(voices_);
  io.Bytes(ram_, kRamSize);
  io.Plain(main_volume_left_);
  io.Plain(main_volume_right_);
  io.Plain(reverb_volume_left_);
  io.Plain(reverb_volume_right_);
  io.Plain(key_on_);
  io.Plain(key_off_);
  io.Plain(pitch_modulation_);
  io.Plain(noise_mode_);
  io.Plain(reverb_mode_);
  io.Plain(endx_);
  io.Plain(control_);
  io.Plain(transfer_control_);
  io.Plain(status_);
  io.Plain(irq_address_);
  io.Plain(transfer_address_);
  io.Plain(transfer_cursor_);
  io.Plain(cd_volume_left_);
  io.Plain(cd_volume_right_);
  io.Plain(external_volume_left_);
  io.Plain(external_volume_right_);
  io.Plain(reverb_registers_);
  io.Plain(noise_timer_);
  io.Plain(noise_level_);
  io.Plain(reverb_base_);
  io.Plain(reverb_cursor_);
  // Only the resampler's phase is kept - its history is not, see
  // reverb_downsample_ - so a loaded state steps the reverb on the same
  // samples it would have.
  reverb_left_phase_ = (reverb_resample_position_ & 1) != 0;
  io.Plain(reverb_left_phase_);
  if (!io.saving()) {
    memset(reverb_downsample_, 0, sizeof(reverb_downsample_));
    memset(reverb_upsample_, 0, sizeof(reverb_upsample_));
    reverb_resample_position_ = reverb_left_phase_ ? 1 : 0;
    // An older state could hold a cursor below the work area.
    if (reverb_cursor_ < reverb_base_)
      reverb_cursor_ = reverb_base_;
  }
  io.Plain(sample_counter_);
  io.Plain(irq_pending_);
  io.Plain(cd_audio_buffer_);
  io.Plain(cd_audio_read_);
  io.Plain(cd_audio_write_);
  io.Plain(cd_audio_count_);
  // The XA resampler, in the twelve bytes the linear one it replaced kept - so states made before
  // still load, without a new state version: where it is in its ring and its pattern, marked
  // "XA" so that an older state's fraction is not read as one. Not its history, as with the
  // reverb's above: a loaded state's first XA frames are filtered from silence, less than a
  // millisecond of it.
  uint32_t xa_state = 0x58410000u | ((xa_sixstep_ & 0xFF) << 8) | (xa_ring_position_ & 31);
  int16_t xa_unused[4] = {};
  io.Plain(xa_state);
  io.Plain(xa_unused);
  if (!io.saving()) {
    memset(xa_ring_, 0, sizeof(xa_ring_));
    xa_ring_position_ = 0;
    xa_sixstep_ = 6;
    if ((xa_state >> 16) == 0x5841u) {
      const uint32_t sixstep = (xa_state >> 8) & 0xFF;
      xa_ring_position_ = xa_state & 31;
      if (sixstep <= 9)
        xa_sixstep_ = sixstep;
    }
  }
}

int Spu::Deinitialize() {
  delete[] ram_;
  delete[] buffer_;
  delete[] voice_events_;
  ram_ = nullptr;
  buffer_ = nullptr;
  voice_events_ = nullptr;
  voice_event_count_ = 0;
  return 0;
}

/*
// [OLD IMPLEMENTATION]
int16_t Spu::VolumeOf(uint16_t reg) {
  // Bit 15 clear means a plain level in the low 15 bits, as a signed value
  // doubled. Bit 15 set selects a sweep, whose starting level is not stored
  // here; treating it as full volume is the usual approximation and is noted
  // in Docs/Gaps.md.
  if (reg & 0x8000)
    return 0x3FFF;
  return static_cast<int16_t>(static_cast<int16_t>(reg << 1) >> 1);
}
*/

// [NEW IMPLEMENTATION]
int16_t Spu::VolumeOf(const VolumeSweep& sweep) {
  // If bit 15 is set, it's a sweep, and we return the current running level.
  //
  // Otherwise bits 14-0 are the level *halved* - -4000h..+3FFFh standing for
  // -100%..+100% - so the register is doubled into the -8000h..+7FFEh range
  // every mix site multiplies by and shifts 15 back out of. This used to
  // shift the doubling straight back out again ((reg << 1) >> 1), which
  // cancels: a game asking for unity, 3FFFh, mixed at half. Both the voice
  // and the main volume went through it, so everything came out at a quarter
  // of amplitude - consistently, which is why nothing sounded wrong relative
  // to anything else and the front end just carried a 2x master gain.
  if (sweep.reg & 0x8000)
    return static_cast<int16_t>(sweep.level);
  return static_cast<int16_t>(sweep.reg << 1);
}

// The CD and external input volumes are not sweep registers - they are plain
// signed 16-bit levels, -8000h..+7FFFh, and go straight into the multiply.
// Running them through VolumeOf, which is for the sweep-capable voice and
// main volumes, turned the maximum the BIOS CD player writes (7FFFh) into -1:
// bit 15 read as the sweep flag, and the doubling trick underneath it wraps
// the top half of the range negative. That silenced every CD-DA and XA track
// while the counters still said audio was flowing.
int16_t Spu::InputVolumeOf(uint16_t reg) {
  return static_cast<int16_t>(reg);
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

  // A block flagged as the loop point records where to come back to - unless
  // software has set the loop point by register since the last key-on, which
  // takes precedence over the flag until the voice is keyed on again.
  if ((flags & 0x04) && !voice.ignore_loop_start)
    voice.repeat_address = static_cast<uint16_t>((address / 8) & 0xFFFF);

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

  // The end and repeat flags wait until the block has played: LeaveBlock.
  voice.block = static_cast<uint8_t>((voice.block & kBlockFirst) | kBlockLoaded |
                                     (flags & (kBlockEnd | kBlockRepeat)));
}

// Off at once, envelope and all - what an end without repeat does, and what
// clearing the SPU's enable bit does to every voice.
void Spu::ForceOff(Voice& voice) {
  voice.phase = kOff;
  voice.level = 0;
  voice.adsr_volume = 0;
  voice.active = false;
}

// The voice has played the 28 samples of the block it holds and needs the
// next. Bit 0 of the block's flags ends the sample - ENDX, and back to the
// repeat address - and bit 1 says whether it loops there or stops, unless the
// voice is playing noise, whose sample data plays no part and which an end
// does not stop. Otherwise the next block is the one after.
//
// The flags used to act as the block was decoded, before it played: a sample
// that ends without looping lost its whole last block, up to 28 samples, and
// ENDX said it was over a block early. And a noise voice was stopped by flags
// in data it was not playing (bug 158).
void Spu::LeaveBlock(Voice& voice, int index) {
  const uint8_t block = voice.block;
  voice.block = 0;                     // not loaded, and no longer the first
  if ((block & kBlockLoaded) == 0) {
    // Nothing played yet since a key-on (or a state saved before blocks were
    // kept): current_address is already the block to decode.
    voice.block = block & kBlockFirst;
    return;
  }
  if ((block & kBlockEnd) == 0) {
    voice.current_address = (voice.current_address & (kRamSize - 1)) + 16;
    return;
  }
  endx_ |= 1u << index;
  voice.current_address = static_cast<uint32_t>(voice.repeat_address) * 8;
  if ((block & kBlockRepeat) == 0 && (noise_mode_ & (1u << index)) == 0)
    ForceOff(voice);
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

// One sample of a volume register. A fixed level is just that; a sweep runs
// the same envelope as ADSR, off the register's rate (bits 0-6), direction
// (13), curve (14) and phase (12). The model is DuckStation's, which is
// psx-spx's plus hardware measurements psx-spx does not give:
//
//  - A decrease stops at zero. This used to carry on past it to full volume
//    with the phase inverted, so a linear fade-out ended at full volume.
//  - The phase bit turns a sweep round - an increase runs toward -8000h, a
//    decrease rises from below toward zero and stops there - except in an
//    exponential decrease, where psx-spx says it does nothing. It used to
//    flip the step instead, which sent both kinds of decrease the wrong way.
//  - An exponential increase slows to a quarter above 6000h, by a smaller
//    step at fast rates and a longer wait at slow ones.
//
// `counter` accumulates 8000h a sample at rates up to 47 and less above, and
// the envelope steps when it reaches 8000h - so rate 7Fh, which accumulates
// nothing, never moves.
void Spu::StepSweep(VolumeSweep& sweep) {
  if ((sweep.reg & 0x8000) == 0) {
    // A fixed level, doubled into the full range the sweep below ramps over -
    // see VolumeOf for why the doubling is not shifted back out.
    sweep.level = static_cast<int16_t>(sweep.reg << 1);
    return;
  }

  const bool exponential = (sweep.reg & 0x4000) != 0;
  const bool decreasing = (sweep.reg & 0x2000) != 0;
  const bool inverted = (sweep.reg & 0x1000) != 0;
  const int rate = sweep.reg & 0x7F;
  const int shift = rate >> 2;

  // +7..+4 or, bit-inverted, -8..-5.
  int32_t step = 7 - (rate & 3);
  if ((decreasing != inverted) || (decreasing && exponential))
    step = ~step;
  uint32_t increment = 0x8000;
  if (rate < 44) {
    step <<= (11 - shift);
  } else if (rate >= 48) {
    increment >>= (shift - 11);
    if (rate != 0x7F && increment == 0)
      increment = 1;
  }

  if (exponential) {
    if (decreasing) {
      step = (step * sweep.level) >> 15;
    } else if (sweep.level >= 0x6000) {
      if (rate < 40) {
        step >>= 2;
      } else if (rate >= 44) {
        increment >>= 2;
      } else {
        step >>= 1;
        increment >>= 1;
      }
    }
  }

  sweep.counter += increment;
  if ((sweep.counter & 0x8000) == 0)
    return;
  sweep.counter = 0;

  const int32_t next = sweep.level + step;
  if (!decreasing)
    sweep.level = std::min<int32_t>(std::max<int32_t>(next, -0x8000), 0x7FFF);
  else if (inverted && !exponential)
    sweep.level = std::min<int32_t>(std::max<int32_t>(next, -0x8000), 0);
  else
    sweep.level = std::max<int32_t>(next, 0);
}

// A write to a volume register starts it afresh: a sweep from wherever the
// level already is, with its counter empty; a fixed level at once.
void Spu::WriteSweep(VolumeSweep& sweep, uint16_t data) {
  sweep.reg = data;
  sweep.counter = 0;
  if ((data & 0x8000) == 0)
    sweep.level = static_cast<int16_t>(data << 1);
}

void Spu::KeyOn(int index) {
  Voice& voice = voices_[index];
  const uint16_t repeat_before = voice.repeat_address;
  voice.current_address = static_cast<uint32_t>(voice.start_address) * 8;
  // The repeat address is deliberately left alone. Key-on sets where the
  // voice starts, not where it loops - software is free to set a loop point
  // by register before keying on, and the hardware keeps it.
  //
  // Resetting it here to the start address is what made Final Fantasy VII's
  // prelude play a twelfth flat. It writes repeat=186Eh and then keys on with
  // start=186Ah for every note; 186Ah is two blocks of silence followed by a
  // 28-sample triangle at 186Eh, so throwing the repeat address away turned a
  // 28-sample loop into an 84-sample one - a third of the frequency, on every
  // note, and a buzzing pulse train instead of a continuous tone. See bug 39.
  voice.ignore_loop_start = false;
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
  voice.block = kBlockFirst;         // nothing loaded; the start is next
  endx_ &= ~(1u << index);
  ++stats_.key_ons;

  if (trace_voices_ && voice_event_count_ < kMaxVoiceEvents) {
    VoiceEvent& event = voice_events_[voice_event_count_++];
    event.frame = stats_.frames;
    event.kind = kEventKeyOn;
    event.voice = static_cast<uint16_t>(index);
    event.pitch = voice.pitch;
    event.start_address = voice.start_address;
    event.repeat_address = repeat_before;
    event.adsr_low = voice.adsr_low;
    event.adsr_high = voice.adsr_high;
    event.volume_left = voice.volume_left.reg;
    event.volume_right = voice.volume_right.reg;
  }
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

// psx-spx's generator: a timer counts down by the step (SPUCNT bits 8-9, as
// 4..7) every sample, and each time it goes below zero the shift register
// moves once and the timer reloads with 20000h >> shift (bits 10-13) - twice
// if once is not enough, so at the top rates it moves every sample. Shift 15
// with step 4 is a new level every sample; shift 0 one every 32768.
//
// This used to add 8000h >> (20 - shift) to a counter and move at 8000h:
// a new level every 2^(20 - shift) samples, 32 to 56 times too slow, so hiss
// came out as a low crackle, and at shifts 0-4 the increment was zero and the
// level never moved at all. It ignored the step bits entirely (bug 155).
void Spu::StepNoise() {
  const int step = 4 + ((control_ >> 8) & 3);
  const int shift = (control_ >> 10) & 0x0F;
  noise_timer_ -= step;
  if (noise_timer_ >= 0)
    return;
  // The hardware's generator is a shift register with a parity tap.
  const uint32_t bit = ((noise_level_ >> 15) ^ (noise_level_ >> 12) ^
                        (noise_level_ >> 11) ^ (noise_level_ >> 10) ^ 1) & 1;
  noise_level_ = static_cast<int16_t>((noise_level_ << 1) | bit);
  noise_timer_ += 0x20000 >> shift;
  if (noise_timer_ < 0)
    noise_timer_ += 0x20000 >> shift;
}

int16_t Spu::StepVoice(Voice& voice, int index, int16_t previous_output) {
  // A voice that is off makes no sound, but the hardware still runs it: its
  // address keeps moving through sample RAM, through the flags' jumps, and a
  // block it reads at the IRQ address raises the interrupt. Games use exactly
  // that to hear when a sample has finished - point the voice's repeat
  // address at a silent looping block, put the IRQ address in it, and the
  // interrupt arrives when the voice parks there. Thousand Arms' opening
  // waits for it before its first dialogue; skipping off voices left the game
  // walking Meis on the spot for ever (bug 144). Only while the IRQ is
  // enabled, as DuckStation does - with it off nothing can tell, and stepping
  // 24 silent voices would be time for nothing.
  if (!voice.active && voice.phase == kOff && (control_ & 0x0040) == 0)
    return 0;

  // Pitch, optionally modulated by the previous voice's output, then clipped
  // to 4000h (176.4 kHz) - psx-spx's order. The modulation works on the whole
  // register, so 4000h-7FFFh scaled down by a quiet neighbour still count; the
  // register is taken as signed, which is the hardware's glitch above 7FFFh.
  //
  // This used to mask the register to 3FFFh before anything else, so pitch
  // 4000h became 0 and stalled the voice, 5000h played at 1000h, and a
  // modulated pitch above 3FFFh started from the wrong number (bug 156).
  int32_t step = voice.pitch;
  if ((pitch_modulation_ & (1u << index)) && index > 0) {
    const int32_t factor = 0x8000 + previous_output;
    step = (static_cast<int16_t>(voice.pitch) * factor) >> 15;
    step &= 0xFFFF;
  }
  step = std::min<int32_t>(step, 0x4000);

  voice.counter += static_cast<uint32_t>(step);
  while (voice.counter >= 0x1000) {
    voice.counter -= 0x1000;
    if (voice.decoded_index >= 28) {
      LeaveBlock(voice, index);
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

/*
// [OLD IMPLEMENTATION]
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
*/

// psx-spx's resampling filter, 44.1 kHz to the reverb's 22.05 kHz and back:
// 39 taps, every other one zero bar the centre (4000h), so only these twenty
// are multiplied.
namespace {
const int32_t kReverbFir[20] = {
    -0x0001, 0x0002, -0x000A, 0x0023, -0x0067, 0x010A, -0x0268, 0x0534,
    -0x0B90, 0x2806, 0x2806, -0x0B90, 0x0534, -0x0268, 0x010A, -0x0067,
    0x0023, -0x000A, 0x0002, -0x0001 };
}  // namespace

// Where a reverb register points: `units` is its value (8-byte units), which
// the hardware adds to the current address in halfwords and wraps back into
// the work area - mBASE to the end of sound RAM - when it runs off the end.
uint32_t Spu::ReverbAddress(uint32_t units, int32_t halfwords) const {
  const uint32_t kMask = 0x3FFFF;   // sound RAM in halfwords
  const uint32_t base = reverb_base_ / 2;
  uint32_t offset = reverb_cursor_ / 2 + (((units << 2) + halfwords) & kMask);
  if (offset & 0x40000)
    offset += base;
  return (offset & kMask) * 2;
}

int32_t Spu::ReverbRead(uint32_t units, int32_t halfwords) const {
  return static_cast<int16_t>(RamHalf(ReverbAddress(units, halfwords)));
}

void Spu::ReverbWrite(uint32_t units, int32_t value) {
  WriteRamHalf(ReverbAddress(units, 0), static_cast<uint16_t>(value));
}

// The reverb network psx-spx documents - same-side and cross reflections, four
// comb taps, two all-pass stages - in the arrangement Mednafen and DuckStation
// use, which halves each sum rather than scaling each product by 8000h, and
// saturates to 16 bits at every stage as the hardware does. It runs at
// 22,050 Hz: every sample goes into the resampler, and every other one the
// network steps once for both sides.
//
// With the master enable (SPUCNT bit 7) off, nothing is written to the work
// area but the network still reads it and plays what is there - the enable
// stops the echo being fed, not heard - and the address still moves.
//
// This replaced a network that stepped on every other input sample without
// filtering it, held each output for two samples, saturated only what it
// wrote, and went silent with the enable off.
void Spu::ProcessReverb(int32_t input_left, int32_t input_right,
                        int32_t* output_left, int32_t* output_right) {
  const uint32_t position = reverb_resample_position_;
  reverb_downsample_[0][position] = reverb_downsample_[0][position | 0x40] =
      static_cast<int16_t>(input_left);
  reverb_downsample_[1][position] = reverb_downsample_[1][position | 0x40] =
      static_cast<int16_t>(input_right);

  const uint16_t* r = reverb_registers_;
  auto vol = [r](int reg) { return static_cast<int32_t>(static_cast<int16_t>(r[reg])); };
  // What the all-pass stages multiply by: minus the coefficient, with -8000h
  // (whose negation does not fit) taken as +7FFFh.
  auto neg = [](int32_t v) { return v == -0x8000 ? 0x7FFF : -v; };
  const int32_t iir = vol(vIIR);
  // The IIR's other half, (8000h - vIIR) times the old sample, with the one
  // coefficient that value cannot represent handled the way the hardware does.
  auto iir_other = [iir](int32_t sample) {
    if (iir == -0x8000)
      return sample == -0x8000 ? 0 : sample * -0x10000;
    return sample * (0x8000 - iir);
  };

  int32_t out[2];
  if (position & 1) {
    int32_t down[2];
    for (int side = 0; side < 2; ++side) {
      const int16_t* src = &reverb_downsample_[side][(position - 38) & 0x3F];
      int32_t sum = 0;
      for (int tap = 0; tap < 20; ++tap)
        sum += kReverbFir[tap] * src[tap * 2];
      down[side] = Clamp16((sum + 0x4000 * src[19]) >> 15);
    }

    const bool enabled = (control_ & 0x0080) != 0;
    const int same_dest[2] = { mLSAME, mRSAME };
    const int same_src[2] = { dLSAME, dRSAME };
    const int diff_dest[2] = { mLDIFF, mRDIFF };
    const int diff_src[2] = { dLDIFF, dRDIFF };
    const int comb[4][2] = { { mLCOMB1, mRCOMB1 }, { mLCOMB2, mRCOMB2 },
                             { mLCOMB3, mRCOMB3 }, { mLCOMB4, mRCOMB4 } };
    const int comb_vol[4] = { vCOMB1, vCOMB2, vCOMB3, vCOMB4 };
    const int apf1[2] = { mLAPF1, mRAPF1 };
    const int apf2[2] = { mLAPF2, mRAPF2 };
    const int input_vol[2] = { vLIN, vRIN };

    for (int side = 0; side < 2; ++side) {
      if (enabled) {
        // Same-side and cross reflections, into the IIR stages. The cross
        // one takes the other side's delayed sample.
        const int32_t in = (down[side] * vol(input_vol[side])) >> 14;
        const int32_t input_same = Clamp16(
            (((ReverbRead(r[same_src[side]]) * vol(vWALL)) >> 14) + in) >> 1);
        const int32_t input_diff = Clamp16(
            (((ReverbRead(r[diff_src[side ^ 1]]) * vol(vWALL)) >> 14) + in) >> 1);
        const int32_t same = Clamp16(
            (((input_same * iir) >> 14) +
             (iir_other(ReverbRead(r[same_dest[side]], -1)) >> 14)) >> 1);
        const int32_t diff = Clamp16(
            (((input_diff * iir) >> 14) +
             (iir_other(ReverbRead(r[diff_dest[side]], -1)) >> 14)) >> 1);
        ReverbWrite(r[same_dest[side]], same);
        ReverbWrite(r[diff_dest[side]], diff);
      }

      // Early echo: the four comb taps.
      int32_t acc = 0;
      for (int tap = 0; tap < 4; ++tap)
        acc += (ReverbRead(r[comb[tap][side]]) * vol(comb_vol[tap])) >> 14;

      // Late reverb: two all-pass stages, each reading its own output from
      // dAPF1/dAPF2 earlier.
      const int32_t fb_a = ReverbRead(static_cast<uint32_t>(r[apf1[side]] - r[dAPF1]));
      const int32_t fb_b = ReverbRead(static_cast<uint32_t>(r[apf2[side]] - r[dAPF2]));
      const int32_t mda = Clamp16((acc + ((fb_a * neg(vol(vAPF1))) >> 14)) >> 1);
      const int32_t mdb = Clamp16(
          fb_a + ((((mda * vol(vAPF1)) >> 14) + ((fb_b * neg(vol(vAPF2))) >> 14)) >> 1));
      const int16_t sample = static_cast<int16_t>(Clamp16(fb_b + ((mdb * vol(vAPF2)) >> 15)));
      reverb_upsample_[side][(position >> 1) | 0x20] =
          reverb_upsample_[side][position >> 1] = sample;

      if (enabled) {
        ReverbWrite(r[apf1[side]], mda);
        ReverbWrite(r[apf2[side]], mdb);
      }
    }

    // One halfword on, back to mBASE past the end of sound RAM.
    uint32_t cursor = (reverb_cursor_ / 2 + 1) & 0x3FFFF;
    if (cursor == 0)
      cursor = reverb_base_ / 2;
    reverb_cursor_ = cursor * 2;

    for (int side = 0; side < 2; ++side) {
      const int16_t* src = &reverb_upsample_[side][((position >> 1) - 19) & 0x1F];
      int32_t sum = 0;
      for (int tap = 0; tap < 20; ++tap)
        sum += kReverbFir[tap] * src[tap];
      out[side] = Clamp16(sum >> 14);
    }
  } else {
    // Between the reverb's own samples the filter's centre tap is the only
    // one that is not zero.
    const uint32_t index = (((position >> 1) - 19) & 0x1F) + 9;
    out[0] = reverb_upsample_[0][index];
    out[1] = reverb_upsample_[1][index];
  }

  reverb_resample_position_ = (position + 1) & 0x3F;
  *output_left = out[0];
  *output_right = out[1];
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
    ++stats_.cd_samples_dropped;
  }
  cd_audio_buffer_[cd_audio_write_] = left;
  cd_audio_buffer_[cd_audio_write_ + 1] = right;
  cd_audio_write_ = (cd_audio_write_ + 2) % (kSampleRate * 2);
  cd_audio_count_ += 2;
}

int Spu::PeekCdFrames(int16_t* out, int max_frames) const {
  int frames = cd_audio_count_ / 2;
  if (frames > max_frames)
    frames = max_frames;
  for (int i = 0; i < frames; ++i) {
    const int at = (cd_audio_read_ + i * 2) % (kSampleRate * 2);
    out[i * 2] = cd_audio_buffer_[at];
    out[i * 2 + 1] = cd_audio_buffer_[at + 1];
  }
  return frames;
}

namespace {

// The CD-ROM decoder's resampling filter, 37800 Hz to 44100: psx-spx's "25-point Zigzag
// Interpolation", seven tables of 29 taps, one for each of the seven output frames made from
// every six input frames. Tap i weights the input frame i+1 back from the newest (psx-spx's
// ringbuf[(p-i) AND 1Fh] for i = 1..29). The same numbers as DuckStation's; it applies them a
// frame later than psx-spx says - tap 0 to the oldest frame in its ring - which is a phase of
// one 37800 Hz frame and changes nothing that can be heard. psx-spx says the table is "nearly
// correct": a console's output differs from it by small roundings.
const int16_t kXaZigzag[7][29] = {
  { 0, 0, 0, 0, 0, -0x0002, 0x000A, -0x0022, 0x0041, -0x0054, 0x0034, 0x0009, -0x010A, 0x0400,
    -0x0A78, 0x234C, 0x6794, -0x1780, 0x0BCD, -0x0623, 0x0350, -0x016D, 0x006B, 0x000A, -0x0010,
    0x0011, -0x0008, 0x0003, -0x0001 },
  { 0, 0, 0, -0x0002, 0, 0x0003, -0x0013, 0x003C, -0x004B, 0x00A2, -0x00E3, 0x0132, -0x0043,
    -0x0267, 0x0C9D, 0x74BB, -0x11B4, 0x09B8, -0x05BF, 0x0372, -0x01A8, 0x00A6, -0x001B, 0x0005,
    0x0006, -0x0008, 0x0003, -0x0001, 0 },
  { 0, 0, -0x0001, 0x0003, -0x0002, -0x0005, 0x001F, -0x004A, 0x00B3, -0x0192, 0x02B1, -0x039E,
    0x04F8, -0x05A6, 0x7939, -0x05A6, 0x04F8, -0x039E, 0x02B1, -0x0192, 0x00B3, -0x004A, 0x001F,
    -0x0005, -0x0002, 0x0003, -0x0001, 0, 0 },
  { 0, -0x0001, 0x0003, -0x0008, 0x0006, 0x0005, -0x001B, 0x00A6, -0x01A8, 0x0372, -0x05BF,
    0x09B8, -0x11B4, 0x74BB, 0x0C9D, -0x0267, -0x0043, 0x0132, -0x00E3, 0x00A2, -0x004B, 0x003C,
    -0x0013, 0x0003, 0, -0x0002, 0, 0, 0 },
  { -0x0001, 0x0003, -0x0008, 0x0011, -0x0010, 0x000A, 0x006B, -0x016D, 0x0350, -0x0623, 0x0BCD,
    -0x1780, 0x6794, 0x234C, -0x0A78, 0x0400, -0x010A, 0x0009, 0x0034, -0x0054, 0x0041, -0x0022,
    0x000A, -0x0001, 0, 0x0001, 0, 0, 0 },
  { 0x0002, -0x0008, 0x0010, -0x0023, 0x002B, 0x001A, -0x00EB, 0x027B, -0x0548, 0x0AFA, -0x16FA,
    0x53E0, 0x3C07, -0x1249, 0x080E, -0x0347, 0x015B, -0x0044, -0x0017, 0x0046, -0x0023, 0x0011,
    -0x0005, 0, 0, 0, 0, 0, 0 },
  { -0x0005, 0x0011, -0x0023, 0x0046, -0x0017, -0x0044, 0x015B, -0x0347, 0x080E, -0x1249, 0x3C07,
    0x53E0, -0x16FA, 0x0AFA, -0x0548, 0x027B, -0x00EB, 0x001A, 0x002B, -0x0023, 0x0010, -0x0008,
    0x0002, 0, 0, 0, 0, 0, 0 },
};

// 18900 Hz to 44100, seven frames out for every three in. psx-spx has no table for it (it guesses
// the hardware zigzags first and interpolates after); these are Mednafen's weights, applied as
// DuckStation applies them, to the 25 frames before the newest. Duplicating each frame into the
// 37800 Hz filter
// instead is what DuckStation measured to sound worse, with a spike near 16 kHz. Few games use it.
const int16_t kXa18900[7][25] = {
  { 0x0, -0x5, 0x11, -0x23, 0x46, -0x17, -0x44, 0x15b, -0x347, 0x80e, -0x1249, 0x3c07, 0x53e0,
    -0x16fa, 0xafa, -0x548, 0x27b, -0xeb, 0x1a, 0x2b, -0x23, 0x10, -0x8, 0x2, 0x0 },
  { 0x0, -0x2, 0xa, -0x22, 0x41, -0x54, 0x34, 0x9, -0x10a, 0x400, -0xa78, 0x234c, 0x6794,
    -0x1780, 0xbcd, -0x623, 0x350, -0x16d, 0x6b, 0xa, -0x10, 0x11, -0x8, 0x3, -0x1 },
  { -0x2, 0x0, 0x3, -0x13, 0x3c, -0x4b, 0xa2, -0xe3, 0x132, -0x43, -0x267, 0xc9d, 0x74bb,
    -0x11b4, 0x9b8, -0x5bf, 0x372, -0x1a8, 0xa6, -0x1b, 0x5, 0x6, -0x8, 0x3, -0x1 },
  { -0x1, 0x3, -0x2, -0x5, 0x1f, -0x4a, 0xb3, -0x192, 0x2b1, -0x39e, 0x4f8, -0x5a6, 0x7939,
    -0x5a6, 0x4f8, -0x39e, 0x2b1, -0x192, 0xb3, -0x4a, 0x1f, -0x5, -0x2, 0x3, -0x1 },
  { -0x1, 0x3, -0x8, 0x6, 0x5, -0x1b, 0xa6, -0x1a8, 0x372, -0x5bf, 0x9b8, -0x11b4, 0x74bb,
    0xc9d, -0x267, -0x43, 0x132, -0xe3, 0xa2, -0x4b, 0x3c, -0x13, 0x3, 0x0, -0x2 },
  { -0x1, 0x3, -0x8, 0x11, -0x10, 0xa, 0x6b, -0x16d, 0x350, -0x623, 0xbcd, -0x1780, 0x6794,
    0x234c, -0xa78, 0x400, -0x10a, 0x9, 0x34, -0x54, 0x41, -0x22, 0xa, -0x2, 0x0 },
  { 0x0, 0x2, -0x8, 0x10, -0x23, 0x2b, 0x1a, -0xeb, 0x27b, -0x548, 0xafa, -0x16fa, 0x53e0,
    0x3c07, -0x1249, 0x80e, -0x347, 0x15b, -0x44, -0x17, 0x46, -0x23, 0x11, -0x5, 0x0 },
};

int16_t ClampSample(int32_t value) {
  return static_cast<int16_t>(value < -0x8000 ? -0x8000 : (value > 0x7FFF ? 0x7FFF : value));
}

}  // namespace

// 37800 Hz: each frame goes into the ring, and after every sixth, seven frames come out, one
// from each table. `xa_ring_position_` is where the next frame goes, so the newest is the one
// before it.
void Spu::ResampleXa37800(const int16_t* stereo, int frames) {
  uint32_t p = xa_ring_position_ & 31;
  // The 18900 Hz pattern counts 0 to 9; coming from it, this one starts over.
  if (xa_sixstep_ < 1 || xa_sixstep_ > 6)
    xa_sixstep_ = 6;
  for (int i = 0; i < frames; ++i) {
    xa_ring_[0][p] = stereo[i * 2];
    xa_ring_[1][p] = stereo[i * 2 + 1];
    p = (p + 1) & 31;
    if (--xa_sixstep_ != 0)
      continue;
    xa_sixstep_ = 6;
    for (int table = 0; table < 7; ++table) {
      int32_t sum[2] = { 0, 0 };
      for (int tap = 0; tap < 29; ++tap) {
        const uint32_t at = (p - 1 - static_cast<uint32_t>(tap)) & 31;
        sum[0] += (static_cast<int32_t>(xa_ring_[0][at]) * kXaZigzag[table][tap]) >> 15;
        sum[1] += (static_cast<int32_t>(xa_ring_[1][at]) * kXaZigzag[table][tap]) >> 15;
      }
      PushCdFrame(ClampSample(sum[0]), ClampSample(sum[1]));
    }
  }
  xa_ring_position_ = p;
}

// 18900 Hz: a frame out every step of three sevenths of an input frame. Here `xa_ring_position_`
// is the newest frame itself, as in DuckStation, which this follows exactly.
void Spu::ResampleXa18900(const int16_t* stereo, int frames) {
  uint32_t p = xa_ring_position_ & 31;
  for (int i = 0; i < frames;) {
    if (xa_sixstep_ >= 7) {
      xa_sixstep_ -= 7;
      p = (p + 1) & 31;
      xa_ring_[0][p] = stereo[i * 2];
      xa_ring_[1][p] = stereo[i * 2 + 1];
      ++i;
    }
    int32_t sum[2] = { 0, 0 };
    for (int tap = 0; tap < 25; ++tap) {
      const uint32_t at = (p + 32 - 25 + static_cast<uint32_t>(tap)) & 31;
      sum[0] += static_cast<int32_t>(xa_ring_[0][at]) * kXa18900[xa_sixstep_][tap];
      sum[1] += static_cast<int32_t>(xa_ring_[1][at]) * kXa18900[xa_sixstep_][tap];
    }
    PushCdFrame(ClampSample(sum[0] >> 15), ClampSample(sum[1] >> 15));
    xa_sixstep_ += 3;
  }
  xa_ring_position_ = p;
}

// Interleaved stereo at some other rate, resampled onto the mixer's own: XA-ADPCM, at 37800 or
// 18900 Hz, through the CD-ROM decoder's own filters above - one sector of it is about a tenth of a
// second, and the ring carries over, so a sector's first frames are filtered with the end of the
// one before. A mono stream arrives with each frame in both channels and comes out so.
void Spu::QueueCdSamples(const int16_t* stereo, int frames, int sample_rate) {
  if (stereo == nullptr || frames <= 0)
    return;
  if (sample_rate == kSampleRate) {
    for (int i = 0; i < frames; ++i)
      PushCdFrame(stereo[i * 2], stereo[i * 2 + 1]);
    return;
  }
  if (sample_rate == 18900)
    ResampleXa18900(stereo, frames);
  else
    ResampleXa37800(stereo, frames);
}


void Spu::QueueCdAudio(const uint8_t* raw_sector) {
  // A raw CD-DA sector is 2352 bytes, which is exactly 588 stereo pairs
  // (16-bit left, 16-bit right, little-endian). It goes into the same queue
  // XA-ADPCM uses - the hardware has one CD input and mixes whichever of the
  // two the drive is producing.
  const int kSamples = 588;
  for (int i = 0; i < kSamples; ++i) {
    const int offset = i * 4;
    const int16_t left = static_cast<int16_t>(
        raw_sector[offset] | (raw_sector[offset + 1] << 8));
    const int16_t right = static_cast<int16_t>(
        raw_sector[offset + 2] | (raw_sector[offset + 3] << 8));
    PushCdFrame(left, right);

    ++stats_.cd_samples_in;
    const int16_t loud_left = static_cast<int16_t>(left < 0 ? -left : left);
    const int16_t loud_right = static_cast<int16_t>(right < 0 ? -right : right);
    const int16_t loudest = loud_left > loud_right ? loud_left : loud_right;
    if (loudest > stats_.cd_peak)
      stats_.cd_peak = loudest;
  }
}

void Spu::GenerateFrame() {
  StepNoise();

  int32_t left = 0, right = 0;
  int32_t reverb_in_left = 0, reverb_in_right = 0;

  uint32_t active = 0;
  int16_t previous = 0;

  for (int i = 0; i < kVoices; ++i) {
    Voice& voice = voices_[i];
    const int16_t sample = StepVoice(voice, i, previous);
    previous = sample;
    if (voice.phase != kOff)
      ++active;

    // A volume is applied and then steps, so a sweep's first sample plays at
    // the level it started from.
    const int32_t l = (sample * VolumeOf(voice.volume_left)) >> 15;
    const int32_t r = (sample * VolumeOf(voice.volume_right)) >> 15;
    StepSweep(voice.volume_left);
    StepSweep(voice.volume_right);
    left += l;
    right += r;
    if (reverb_mode_ & (1u << i)) {
      reverb_in_left += l;
      reverb_in_right += r;
    }
  }

  // SPUCNT bit 14 clear mutes the voices and what they send the reverb, and
  // nothing else: psx-spx says it does not touch CD audio, and the reverb
  // still plays out what it holds. It used to silence the whole output, so a
  // game streaming XA or CD audio with the voices muted played nothing.
  if ((control_ & 0x4000) == 0) {
    left = right = 0;
    reverb_in_left = reverb_in_right = 0;
  }

  if (cd_audio_count_ > 0) {
    int16_t cd_left = cd_audio_buffer_[cd_audio_read_];
    int16_t cd_right = cd_audio_buffer_[cd_audio_read_ + 1];
    cd_audio_read_ = (cd_audio_read_ + 2) % (kSampleRate * 2);
    cd_audio_count_ -= 2;
    // Through the drive's own volume matrix first, as it comes out of the
    // drive: left and right each a mix of both (bug 157). Here rather than as
    // the drive queues it, so a change takes effect on the next frame, not
    // after the tenth of a second of XA already queued has played.
    system().cdrom().MixAudioFrame(&cd_left, &cd_right);

    if (control_ & 0x0001) { // CD Audio Enable
      ++stats_.cd_samples_out;
      int32_t l = (cd_left * InputVolumeOf(cd_volume_left_)) >> 15;
      int32_t r = (cd_right * InputVolumeOf(cd_volume_right_)) >> 15;
      left += l;
      right += r;
      const int32_t cl = l < 0 ? -l : l;
      const int32_t cr = r < 0 ? -r : r;
      const int32_t cpeak = cl > cr ? cl : cr;
      if (cpeak > stats_.cd_out_peak)
        stats_.cd_out_peak = static_cast<int16_t>(cpeak > 32767 ? 32767 : cpeak);
      if (control_ & 0x0004) { // CD Audio Reverb Enable
        reverb_in_left += l;
        reverb_in_right += r;
      }
    } else {
      ++stats_.cd_frames_muted;
    }
  }

  int32_t reverb_left = 0, reverb_right = 0;
  ProcessReverb(Clamp16(reverb_in_left), Clamp16(reverb_in_right),
                &reverb_left, &reverb_right);
  left += (reverb_left * static_cast<int16_t>(reverb_volume_left_)) >> 15;
  right += (reverb_right * static_cast<int16_t>(reverb_volume_right_)) >> 15;

  left = (Clamp16(left) * VolumeOf(main_volume_left_)) >> 15;
  right = (Clamp16(right) * VolumeOf(main_volume_right_)) >> 15;
  StepSweep(main_volume_left_);
  StepSweep(main_volume_right_);

  // The user's gain, on top of the hardware's own main volume so it lifts the
  // voices, the reverb and CD audio together. Fixed point rather than float so
  // the mix stays integer, and clamped after, so turning it up distorts the
  // loud parts instead of wrapping them round.
  const int32_t gain = static_cast<int32_t>(system().config().audio_volume *
                                            256.0f + 0.5f);
  if (gain != 256) {
    left = (left * gain) >> 8;
    right = (right * gain) >> 8;
  }

  const int16_t out_left = static_cast<int16_t>(Clamp16(left));
  const int16_t out_right = static_cast<int16_t>(Clamp16(right));

  if (out_left > stats_.peak_left) stats_.peak_left = out_left;
  if (out_right > stats_.peak_right) stats_.peak_right = out_right;
  stats_.voices_active = active;
  ++stats_.frames;

  PushFrame(out_left, out_right);
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
  // Raised once, then held until software acknowledges it by clearing the
  // enable bit: the flag is what interrupts, and a flag already set makes no
  // new edge. With off voices now stepping (bug 144), one parked on a looping
  // block at the IRQ address would otherwise raise it again every block.
  if ((control_ & 0x0040) == 0 || (status_ & 0x0040) != 0)
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
      case 0x0: return voice.volume_left.reg;
      case 0x2: return voice.volume_right.reg;
      case 0x4: return voice.pitch;
      case 0x6: return voice.start_address;
      case 0x8: return voice.adsr_low;
      case 0xA: return voice.adsr_high;
      case 0xC: return voice.adsr_volume;
      default:  return voice.repeat_address;
    }
  }

  switch (offset) {
    case 0x180: return main_volume_left_.reg;
    case 0x182: return main_volume_right_.reg;
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
      // The status register mirrors the low bits of the control register.
      // Bit 10 is the transfer busy flag: set while a request-mode DMA is
      // still feeding the SPU its blocks (bug 145, Dma::Dma4) - software
      // that waits on it before starting the next transfer must not find the
      // last one still running. Manual transfers complete at once.
      return static_cast<uint16_t>(
          (status_ & 0xFBC0) | (control_ & 0x3F) |
          (system().io().dma.spu_transfer_running() ? 0x0400 : 0));
    case 0x1B0: return cd_volume_left_;
    case 0x1B2: return cd_volume_right_;
    case 0x1B4: return external_volume_left_;
    case 0x1B6: return external_volume_right_;
    case 0x1B8: return static_cast<uint16_t>(main_volume_left_.level);
    case 0x1BA: return static_cast<uint16_t>(main_volume_right_.level);
    default:
      // Each voice's current volume, left then right, where a sweep has got to.
      if (offset >= 0x200 && offset < 0x260) {
        const Voice& voice = voices_[(offset - 0x200) / 4];
        return static_cast<uint16_t>((offset & 2) ? voice.volume_right.level
                                                  : voice.volume_left.level);
      }
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
      // Bit 15 of a volume register selects a sweep rather than a level.
      case 0x0: WriteSweep(voice.volume_left, data);
                if (data & 0x8000) ++stats_.volume_sweeps;
                else ++stats_.volume_levels;
                return;
      case 0x2: WriteSweep(voice.volume_right, data);
                if (data & 0x8000) ++stats_.volume_sweeps;
                else ++stats_.volume_levels;
                return;
      case 0x4: voice.pitch = data;
                if (data > stats_.max_pitch) stats_.max_pitch = data;
                if (data > 0x3FFF) ++stats_.pitch_writes_over_3fff;
                return;
      case 0x6: voice.start_address = data; return;
      case 0x8: voice.adsr_low = data; return;
      case 0xA: voice.adsr_high = data; return;
      case 0xC: voice.adsr_volume = data;
                voice.level = data & 0x7FFF; return;
      // A repeat address written while a voice is still in the first block
      // after its key-on does not outrank the loop-start flag: a block
      // carrying it can still set the address over what was written. Written
      // to a voice that is off, or once it is past its first block, it does.
      // DuckStation found this on hardware, naming The Misadventures of Tron
      // Bonne, Valkyrie Profile and Re-Loaded; it used to outrank the flag
      // always (bug 158).
      default:  voice.repeat_address = data;
                if (voice.phase == kOff || (voice.block & kBlockFirst) == 0)
                  voice.ignore_loop_start = true;
                if (trace_voices_ && voice_event_count_ < kMaxVoiceEvents) {
                  VoiceEvent& event = voice_events_[voice_event_count_++];
                  memset(&event, 0, sizeof(event));
                  event.frame = stats_.frames;
                  event.kind = kEventRepeatWrite;
                  event.voice = static_cast<uint16_t>(index);
                  event.repeat_address = data;
                  event.start_address = voice.start_address;
                }
                return;
    }
  }

  switch (offset) {
    case 0x180: WriteSweep(main_volume_left_, data); return;
    case 0x182: WriteSweep(main_volume_right_, data); return;
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

    // The three per-voice mode masks. Each is remembered as "ever set" as
    // well as stored: whether a game uses pitch modulation, noise or reverb
    // at all decides whether the approximations in those paths can be the
    // reason it sounds wrong, and reading the live register cannot say,
    // because software clears them again between notes.
    case 0x190: pitch_modulation_ = (pitch_modulation_ & 0xFFFF0000) | data;
                stats_.pitch_modulation_seen |= data;
                return;
    case 0x192: pitch_modulation_ =
                    (pitch_modulation_ & 0xFFFF) | (static_cast<uint32_t>(data) << 16);
                stats_.pitch_modulation_seen |= static_cast<uint32_t>(data) << 16;
                return;
    case 0x194: noise_mode_ = (noise_mode_ & 0xFFFF0000) | data;
                stats_.noise_seen |= data;
                return;
    case 0x196: noise_mode_ =
                    (noise_mode_ & 0xFFFF) | (static_cast<uint32_t>(data) << 16);
                stats_.noise_seen |= static_cast<uint32_t>(data) << 16;
                return;
    case 0x198: reverb_mode_ = (reverb_mode_ & 0xFFFF0000) | data;
                stats_.reverb_seen |= data;
                return;
    case 0x19A: reverb_mode_ =
                    (reverb_mode_ & 0xFFFF) | (static_cast<uint32_t>(data) << 16);
                stats_.reverb_seen |= static_cast<uint32_t>(data) << 16;
                return;
    case 0x19C: case 0x19E: return;      // ENDX is read-only

    case 0x1A2:
      reverb_base_ = (static_cast<uint32_t>(data) * 8) & (kRamSize - 1);
      reverb_cursor_ = reverb_base_;
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
      // Clearing the enable bit (15) stops every voice there and then, as
      // DuckStation measured - not at the next sample. It used to change
      // nothing, so voices played on with the SPU disabled (bug 158).
      if ((control_ & 0x8000) != 0 && (data & 0x8000) == 0)
        for (int i = 0; i < kVoices; ++i)
          ForceOff(voices_[i]);
      control_ = data;
      stats_.control_seen |= data;
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
