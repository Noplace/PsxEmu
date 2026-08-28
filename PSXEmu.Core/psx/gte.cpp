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
#include "psx/psx.h"

#include <algorithm>
#include <cstring>

namespace emulation {
namespace psx {

namespace {

// FLAG bits. Games read this register - it is how near-plane clipping and
// back-face culling are decided - so every one of these has to be set at the
// same moment the hardware sets it.
const uint32_t kFlagMac1Positive = 1u << 30;
const uint32_t kFlagMac1Negative = 1u << 27;
const uint32_t kFlagIr1Saturated = 1u << 24;
const uint32_t kFlagColourR      = 1u << 21;
const uint32_t kFlagSz3Saturated = 1u << 18;
const uint32_t kFlagDivideOverflow = 1u << 17;
const uint32_t kFlagMac0Positive = 1u << 16;
const uint32_t kFlagMac0Negative = 1u << 15;
const uint32_t kFlagSx2Saturated = 1u << 14;
const uint32_t kFlagSy2Saturated = 1u << 13;
const uint32_t kFlagIr0Saturated = 1u << 12;

// Bit 31 is not written directly: it is the OR of the bits that count as
// errors, which is bits 30..23 and 18..13.
const uint32_t kFlagErrorMask = 0x7F87E000u;

// MAC1-3 accumulate in 44 bits before the fractional shift.
const int64_t kMacMax =  (int64_t(1) << 43) - 1;
const int64_t kMacMin = -(int64_t(1) << 43);

// The reciprocal table behind the perspective divide. The divide is a
// Newton-Raphson refinement seeded from here, not a division, and software
// depends on its exact result.
const uint8_t kUnrTable[257] = {
  0xFF,0xFD,0xFB,0xF9,0xF7,0xF5,0xF3,0xF1,0xEF,0xEE,0xEC,0xEA,0xE8,0xE6,0xE4,0xE3,
  0xE1,0xDF,0xDD,0xDC,0xDA,0xD8,0xD6,0xD5,0xD3,0xD1,0xD0,0xCE,0xCD,0xCB,0xC9,0xC8,
  0xC6,0xC5,0xC3,0xC1,0xC0,0xBE,0xBD,0xBB,0xBA,0xB8,0xB7,0xB5,0xB4,0xB2,0xB1,0xB0,
  0xAE,0xAD,0xAB,0xAA,0xA9,0xA7,0xA6,0xA4,0xA3,0xA2,0xA0,0x9F,0x9E,0x9C,0x9B,0x9A,
  0x99,0x97,0x96,0x95,0x94,0x92,0x91,0x90,0x8F,0x8D,0x8C,0x8B,0x8A,0x89,0x87,0x86,
  0x85,0x84,0x83,0x82,0x81,0x7F,0x7E,0x7D,0x7C,0x7B,0x7A,0x79,0x78,0x77,0x75,0x74,
  0x73,0x72,0x71,0x70,0x6F,0x6E,0x6D,0x6C,0x6B,0x6A,0x69,0x68,0x67,0x66,0x65,0x64,
  0x63,0x62,0x61,0x60,0x5F,0x5E,0x5D,0x5D,0x5C,0x5B,0x5A,0x59,0x58,0x57,0x56,0x55,
  0x54,0x53,0x53,0x52,0x51,0x50,0x4F,0x4E,0x4D,0x4D,0x4C,0x4B,0x4A,0x49,0x48,0x48,
  0x47,0x46,0x45,0x44,0x43,0x43,0x42,0x41,0x40,0x3F,0x3F,0x3E,0x3D,0x3C,0x3C,0x3B,
  0x3A,0x39,0x39,0x38,0x37,0x36,0x36,0x35,0x34,0x33,0x33,0x32,0x31,0x31,0x30,0x2F,
  0x2E,0x2E,0x2D,0x2C,0x2C,0x2B,0x2A,0x2A,0x29,0x28,0x28,0x27,0x26,0x26,0x25,0x24,
  0x24,0x23,0x22,0x22,0x21,0x20,0x20,0x1F,0x1E,0x1E,0x1D,0x1D,0x1C,0x1B,0x1B,0x1A,
  0x19,0x19,0x18,0x18,0x17,0x16,0x16,0x15,0x15,0x14,0x14,0x13,0x12,0x12,0x11,0x11,
  0x10,0x0F,0x0F,0x0E,0x0E,0x0D,0x0D,0x0C,0x0C,0x0B,0x0A,0x0A,0x09,0x09,0x08,0x08,
  0x07,0x07,0x06,0x06,0x05,0x05,0x04,0x04,0x03,0x03,0x02,0x02,0x01,0x01,0x00,0x00,
  0x00
};

int CountLeadingZeros16(uint16_t value) {
  int count = 0;
  while (count < 16 && (value & 0x8000) == 0) {
    value <<= 1;
    ++count;
  }
  return count;
}

int32_t ComputeLeadingCount(uint32_t value) {
  // Leading zeroes of a positive value, leading ones of a negative one.
  const uint32_t bits = (value & 0x80000000u) ? ~value : value;
  int count = 0;
  for (int bit = 31; bit >= 0; --bit) {
    if (bits & (1u << bit))
      break;
    ++count;
  }
  return count;
}

int32_t SignExtend16(uint16_t value) {
  return static_cast<int16_t>(value);
}

}  // namespace

Gte::Gte() {
}

Gte::~Gte() {
}

int Gte::Initialize() {
  memset(v_, 0, sizeof(v_));
  memset(rgbc_, 0, sizeof(rgbc_));
  otz_ = 0;
  memset(ir_, 0, sizeof(ir_));
  memset(sxy_, 0, sizeof(sxy_));
  memset(sz_, 0, sizeof(sz_));
  memset(rgb_fifo_, 0, sizeof(rgb_fifo_));
  res1_ = 0;
  memset(mac_, 0, sizeof(mac_));
  lzcs_ = 0;
  lzcr_ = 32;

  memset(matrix_, 0, sizeof(matrix_));
  memset(translation_, 0, sizeof(translation_));
  ofx_ = ofy_ = 0;
  h_ = 0;
  dqa_ = 0;
  dqb_ = 0;
  zsf3_ = zsf4_ = 0;
  flag_ = 0;

  lm_ = false;
  sf_ = 0;
  memset(&stats_, 0, sizeof(stats_));
  return S_OK;
}

int Gte::Deinitialize() {
  return S_OK;
}

// ---------------------------------------------------------------------------
// Saturation and overflow
// ---------------------------------------------------------------------------

int64_t Gte::CheckMac(int index, int64_t value) {
  if (value > kMacMax)
    flag_ |= kFlagMac1Positive >> (index - 1);
  else if (value < kMacMin)
    flag_ |= kFlagMac1Negative >> (index - 1);
  // The accumulator really is 44 bits wide, so an overflow wraps rather than
  // clamping. Sign-extending from bit 43 is what reproduces that.
  return (value << 20) >> 20;
}

int32_t Gte::CheckMac0(int64_t value) {
  if (value > 2147483647LL)
    flag_ |= kFlagMac0Positive;
  else if (value < -2147483648LL)
    flag_ |= kFlagMac0Negative;
  return static_cast<int32_t>(value);
}

int16_t Gte::SaturateIr(int index, int32_t value, bool lm) {
  const int32_t minimum = lm ? 0 : -0x8000;
  if (value > 0x7FFF) {
    flag_ |= kFlagIr1Saturated >> (index - 1);
    return 0x7FFF;
  }
  if (value < minimum) {
    flag_ |= kFlagIr1Saturated >> (index - 1);
    return static_cast<int16_t>(minimum);
  }
  return static_cast<int16_t>(value);
}

int16_t Gte::SaturateIr0(int32_t value) {
  if (value < 0) {
    flag_ |= kFlagIr0Saturated;
    return 0;
  }
  if (value > 0x1000) {
    flag_ |= kFlagIr0Saturated;
    return 0x1000;
  }
  return static_cast<int16_t>(value);
}

uint8_t Gte::SaturateColour(int index, int32_t value) {
  if (value < 0) {
    flag_ |= kFlagColourR >> (index - 1);
    return 0;
  }
  if (value > 0xFF) {
    flag_ |= kFlagColourR >> (index - 1);
    return 0xFF;
  }
  return static_cast<uint8_t>(value);
}

uint16_t Gte::SaturateSz3(int32_t value) {
  if (value < 0) {
    flag_ |= kFlagSz3Saturated;
    return 0;
  }
  if (value > 0xFFFF) {
    flag_ |= kFlagSz3Saturated;
    return 0xFFFF;
  }
  return static_cast<uint16_t>(value);
}

int32_t Gte::SaturateScreenX(int32_t value) {
  if (value < -0x400) { flag_ |= kFlagSx2Saturated; return -0x400; }
  if (value > 0x3FF)  { flag_ |= kFlagSx2Saturated; return 0x3FF; }
  return value;
}

int32_t Gte::SaturateScreenY(int32_t value) {
  if (value < -0x400) { flag_ |= kFlagSy2Saturated; return -0x400; }
  if (value > 0x3FF)  { flag_ |= kFlagSy2Saturated; return 0x3FF; }
  return value;
}

// The perspective divide. Not a division: a Newton-Raphson reciprocal seeded
// from kUnrTable, with a documented overflow at exactly h >= sz3*2.
uint32_t Gte::Divide(uint16_t numerator, uint16_t denominator) {
  if (numerator >= denominator * 2) {
    flag_ |= kFlagDivideOverflow;
    return 0x1FFFF;
  }

  const int shift = CountLeadingZeros16(denominator);
  uint32_t n = static_cast<uint32_t>(numerator) << shift;
  uint32_t d = static_cast<uint32_t>(denominator) << shift;
  const uint32_t u = kUnrTable[(d - 0x7FC0) >> 7] + 0x101;

  d = ((0x2000080 - (d * u)) >> 8);
  d = ((0x0000080 + (d * u)) >> 8);

  const uint64_t result = ((static_cast<uint64_t>(n) * d) + 0x8000) >> 16;
  return static_cast<uint32_t>(std::min<uint64_t>(0x1FFFF, result));
}

// ---------------------------------------------------------------------------
// Shared steps
// ---------------------------------------------------------------------------

void Gte::SetMacAndIr(int64_t x, int64_t y, int64_t z, bool lm) {
  mac_[1] = static_cast<int32_t>(CheckMac(1, x) >> sf_);
  mac_[2] = static_cast<int32_t>(CheckMac(2, y) >> sf_);
  mac_[3] = static_cast<int32_t>(CheckMac(3, z) >> sf_);
  ir_[1] = SaturateIr(1, mac_[1], lm);
  ir_[2] = SaturateIr(2, mac_[2], lm);
  ir_[3] = SaturateIr(3, mac_[3], lm);
}

void Gte::PushScreenXy(int32_t x, int32_t y) {
  sxy_[0][0] = sxy_[1][0];  sxy_[0][1] = sxy_[1][1];
  sxy_[1][0] = sxy_[2][0];  sxy_[1][1] = sxy_[2][1];
  sxy_[2][0] = static_cast<int16_t>(x);
  sxy_[2][1] = static_cast<int16_t>(y);
}

void Gte::PushScreenZ(uint16_t z) {
  sz_[0] = sz_[1];
  sz_[1] = sz_[2];
  sz_[2] = sz_[3];
  sz_[3] = z;
}

void Gte::PushColour(uint8_t r, uint8_t g, uint8_t b) {
  memcpy(rgb_fifo_[0], rgb_fifo_[1], 4);
  memcpy(rgb_fifo_[1], rgb_fifo_[2], 4);
  rgb_fifo_[2][0] = r;
  rgb_fifo_[2][1] = g;
  rgb_fifo_[2][2] = b;
  rgb_fifo_[2][3] = rgbc_[3];        // CODE passes through untouched
}

void Gte::PushColourFromMac() {
  PushColour(SaturateColour(1, mac_[1] >> 4),
             SaturateColour(2, mac_[2] >> 4),
             SaturateColour(3, mac_[3] >> 4));
  ir_[1] = SaturateIr(1, mac_[1], lm_);
  ir_[2] = SaturateIr(2, mac_[2], lm_);
  ir_[3] = SaturateIr(3, mac_[3], lm_);
}

// matrix: 0 rotation, 1 light direction, 2 light colour.
// translation: 0 TR, 1 BK, 2 FC, 3 none.
void Gte::MultiplyMatrixByVector(int matrix, const int16_t vector[3],
                                 int translation, bool lm) {
  const int16_t (*m)[3] = matrix_[matrix];
  int64_t result[3];

  for (int row = 0; row < 3; ++row) {
    int64_t sum = 0;
    if (translation < 3)
      sum = static_cast<int64_t>(translation_[translation][row]) << 12;
    sum += static_cast<int64_t>(m[row][0]) * vector[0];
    sum += static_cast<int64_t>(m[row][1]) * vector[1];
    sum += static_cast<int64_t>(m[row][2]) * vector[2];
    result[row] = sum;
  }
  SetMacAndIr(result[0], result[1], result[2], lm);
}

// The step shared by every depth-cued command: move the colour a fraction IR0
// of the way toward the far colour.
void Gte::InterpolateFarColour(int32_t x, int32_t y, int32_t z) {
  const int64_t dx = (static_cast<int64_t>(translation_[2][0]) << 12) - x;
  const int64_t dy = (static_cast<int64_t>(translation_[2][1]) << 12) - y;
  const int64_t dz = (static_cast<int64_t>(translation_[2][2]) << 12) - z;

  // The difference is saturated with lm forced off, whatever the command said.
  const int16_t sx = SaturateIr(1, static_cast<int32_t>(CheckMac(1, dx) >> sf_), false);
  const int16_t sy = SaturateIr(2, static_cast<int32_t>(CheckMac(2, dy) >> sf_), false);
  const int16_t sz = SaturateIr(3, static_cast<int32_t>(CheckMac(3, dz) >> sf_), false);

  mac_[1] = static_cast<int32_t>(
      CheckMac(1, static_cast<int64_t>(sx) * ir_[0] + x) >> sf_);
  mac_[2] = static_cast<int32_t>(
      CheckMac(2, static_cast<int64_t>(sy) * ir_[0] + y) >> sf_);
  mac_[3] = static_cast<int32_t>(
      CheckMac(3, static_cast<int64_t>(sz) * ir_[0] + z) >> sf_);
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

void Gte::Rtps(int vector_index, bool compute_ir0) {
  const int16_t* v = v_[vector_index];
  const int16_t (*rt)[3] = matrix_[0];

  int64_t x = (static_cast<int64_t>(translation_[0][0]) << 12) +
              static_cast<int64_t>(rt[0][0]) * v[0] +
              static_cast<int64_t>(rt[0][1]) * v[1] +
              static_cast<int64_t>(rt[0][2]) * v[2];
  int64_t y = (static_cast<int64_t>(translation_[0][1]) << 12) +
              static_cast<int64_t>(rt[1][0]) * v[0] +
              static_cast<int64_t>(rt[1][1]) * v[1] +
              static_cast<int64_t>(rt[1][2]) * v[2];
  int64_t z = (static_cast<int64_t>(translation_[0][2]) << 12) +
              static_cast<int64_t>(rt[2][0]) * v[0] +
              static_cast<int64_t>(rt[2][1]) * v[1] +
              static_cast<int64_t>(rt[2][2]) * v[2];

  x = CheckMac(1, x);
  y = CheckMac(2, y);
  z = CheckMac(3, z);
  mac_[1] = static_cast<int32_t>(x >> sf_);
  mac_[2] = static_cast<int32_t>(y >> sf_);
  mac_[3] = static_cast<int32_t>(z >> sf_);

  ir_[1] = SaturateIr(1, mac_[1], lm_);
  ir_[2] = SaturateIr(2, mac_[2], lm_);

  // IR3's saturation flag is judged on the value shifted by 12 whatever the
  // command's shift was, while the value stored is the ordinary one. That is a
  // real hardware quirk, not a simplification.
  const int32_t z_shifted = static_cast<int32_t>(z >> 12);
  if (z_shifted < -0x8000 || z_shifted > 0x7FFF)
    flag_ |= kFlagIr1Saturated >> 2;
  ir_[3] = static_cast<int16_t>(
      std::min(std::max(mac_[3], lm_ ? 0 : -0x8000), 0x7FFF));

  PushScreenZ(SaturateSz3(z_shifted));

  const uint32_t divided = Divide(h_, sz_[3]);

  int64_t sx = static_cast<int64_t>(divided) * ir_[1] + ofx_;
  int64_t sy = static_cast<int64_t>(divided) * ir_[2] + ofy_;
  CheckMac0(sx);
  CheckMac0(sy);
  PushScreenXy(SaturateScreenX(static_cast<int32_t>(sx >> 16)),
               SaturateScreenY(static_cast<int32_t>(sy >> 16)));

  if (compute_ir0) {
    const int64_t depth = static_cast<int64_t>(divided) * dqa_ + dqb_;
    mac_[0] = CheckMac0(depth);
    ir_[0] = SaturateIr0(mac_[0] >> 12);
  }
}

void Gte::Rtpt() {
  Rtps(0, false);
  Rtps(1, false);
  Rtps(2, true);
}

void Gte::Nclip() {
  const int64_t area =
      static_cast<int64_t>(sxy_[0][0]) * sxy_[1][1] +
      static_cast<int64_t>(sxy_[1][0]) * sxy_[2][1] +
      static_cast<int64_t>(sxy_[2][0]) * sxy_[0][1] -
      static_cast<int64_t>(sxy_[0][0]) * sxy_[2][1] -
      static_cast<int64_t>(sxy_[1][0]) * sxy_[0][1] -
      static_cast<int64_t>(sxy_[2][0]) * sxy_[1][1];
  mac_[0] = CheckMac0(area);
}

void Gte::Avsz3() {
  const int64_t sum = static_cast<int64_t>(zsf3_) *
                      (static_cast<int32_t>(sz_[1]) + sz_[2] + sz_[3]);
  mac_[0] = CheckMac0(sum);
  otz_ = SaturateSz3(static_cast<int32_t>(sum >> 12));
}

void Gte::Avsz4() {
  const int64_t sum = static_cast<int64_t>(zsf4_) *
                      (static_cast<int32_t>(sz_[0]) + sz_[1] + sz_[2] + sz_[3]);
  mac_[0] = CheckMac0(sum);
  otz_ = SaturateSz3(static_cast<int32_t>(sum >> 12));
}

void Gte::Mvmva(uint32_t command) {
  const int matrix_index = (command >> 17) & 3;
  const int vector_index = (command >> 15) & 3;
  const int translation_index = (command >> 13) & 3;

  int16_t vector[3];
  if (vector_index < 3) {
    vector[0] = v_[vector_index][0];
    vector[1] = v_[vector_index][1];
    vector[2] = v_[vector_index][2];
  } else {
    vector[0] = ir_[1];
    vector[1] = ir_[2];
    vector[2] = ir_[3];
  }

  // Matrix 3 is not a matrix. The hardware reads a garbage one built out of
  // the colour register and a constant; software does not use it deliberately.
  int16_t garbage[3][3];
  const int16_t (*m)[3];
  if (matrix_index < 3) {
    m = matrix_[matrix_index];
  } else {
    garbage[0][0] = static_cast<int16_t>(-static_cast<int16_t>(rgbc_[0]) * 16);
    garbage[0][1] = static_cast<int16_t>(static_cast<int16_t>(rgbc_[0]) * 16);
    garbage[0][2] = ir_[0];
    garbage[1][0] = garbage[1][1] = garbage[1][2] = matrix_[0][0][2];
    garbage[2][0] = garbage[2][1] = garbage[2][2] = matrix_[0][1][1];
    m = garbage;
  }

  if (translation_index == 2) {
    // Translating by the far colour is documented as buggy: the translation
    // and the first product are computed, used only to set the saturation
    // flags, and then thrown away.
    for (int row = 0; row < 3; ++row) {
      const int64_t discarded =
          (static_cast<int64_t>(translation_[2][row]) << 12) +
          static_cast<int64_t>(m[row][0]) * vector[0];
      SaturateIr(row + 1, static_cast<int32_t>(CheckMac(row + 1, discarded) >> sf_),
                 false);
    }
    int64_t result[3];
    for (int row = 0; row < 3; ++row) {
      result[row] = static_cast<int64_t>(m[row][1]) * vector[1] +
                    static_cast<int64_t>(m[row][2]) * vector[2];
    }
    SetMacAndIr(result[0], result[1], result[2], lm_);
    return;
  }

  int64_t result[3];
  for (int row = 0; row < 3; ++row) {
    int64_t sum = 0;
    if (translation_index < 3)
      sum = static_cast<int64_t>(translation_[translation_index][row]) << 12;
    sum += static_cast<int64_t>(m[row][0]) * vector[0];
    sum += static_cast<int64_t>(m[row][1]) * vector[1];
    sum += static_cast<int64_t>(m[row][2]) * vector[2];
    result[row] = sum;
  }
  SetMacAndIr(result[0], result[1], result[2], lm_);
}

void Gte::Op() {
  const int16_t d1 = matrix_[0][0][0];
  const int16_t d2 = matrix_[0][1][1];
  const int16_t d3 = matrix_[0][2][2];
  SetMacAndIr(static_cast<int64_t>(d2) * ir_[3] - static_cast<int64_t>(d3) * ir_[2],
              static_cast<int64_t>(d3) * ir_[1] - static_cast<int64_t>(d1) * ir_[3],
              static_cast<int64_t>(d1) * ir_[2] - static_cast<int64_t>(d2) * ir_[1],
              lm_);
}

void Gte::Gpf() {
  SetMacAndIr(static_cast<int64_t>(ir_[0]) * ir_[1],
              static_cast<int64_t>(ir_[0]) * ir_[2],
              static_cast<int64_t>(ir_[0]) * ir_[3], lm_);
  PushColourFromMac();
}

void Gte::Gpl() {
  // The only command that shifts the accumulator *up* before adding.
  const int64_t x = (static_cast<int64_t>(mac_[1]) << sf_) +
                    static_cast<int64_t>(ir_[0]) * ir_[1];
  const int64_t y = (static_cast<int64_t>(mac_[2]) << sf_) +
                    static_cast<int64_t>(ir_[0]) * ir_[2];
  const int64_t z = (static_cast<int64_t>(mac_[3]) << sf_) +
                    static_cast<int64_t>(ir_[0]) * ir_[3];
  SetMacAndIr(x, y, z, lm_);
  PushColourFromMac();
}

void Gte::Sqr() {
  SetMacAndIr(static_cast<int64_t>(ir_[1]) * ir_[1],
              static_cast<int64_t>(ir_[2]) * ir_[2],
              static_cast<int64_t>(ir_[3]) * ir_[3], lm_);
}

void Gte::DepthCue(uint8_t r, uint8_t g, uint8_t b) {
  InterpolateFarColour(static_cast<int32_t>(r) << 16,
                       static_cast<int32_t>(g) << 16,
                       static_cast<int32_t>(b) << 16);
  PushColourFromMac();
}

void Gte::Dpcs(bool use_rgb_fifo) {
  const uint8_t* source = use_rgb_fifo ? rgb_fifo_[0] : rgbc_;
  DepthCue(source[0], source[1], source[2]);
}

void Gte::Dpct() {
  // Three passes, each reading the oldest colour - which the push then
  // replaces, so the FIFO walks forward on its own.
  Dpcs(true);
  Dpcs(true);
  Dpcs(true);
}

void Gte::Intpl() {
  InterpolateFarColour(static_cast<int32_t>(ir_[1]) << 12,
                       static_cast<int32_t>(ir_[2]) << 12,
                       static_cast<int32_t>(ir_[3]) << 12);
  PushColourFromMac();
}

void Gte::Dcpl() {
  const int32_t r = (static_cast<int32_t>(rgbc_[0]) * ir_[1]) << 4;
  const int32_t g = (static_cast<int32_t>(rgbc_[1]) * ir_[2]) << 4;
  const int32_t b = (static_cast<int32_t>(rgbc_[2]) * ir_[3]) << 4;
  InterpolateFarColour(r, g, b);
  PushColourFromMac();
}

// The lighting chain, shared by the NC* family: a light direction, then a
// light colour, then optionally the surface colour, then optionally depth cue.
void Gte::NormalColour(int vector_index) {
  MultiplyMatrixByVector(1, v_[vector_index], 3, lm_);
  const int16_t light[3] = { ir_[1], ir_[2], ir_[3] };
  MultiplyMatrixByVector(2, light, 1, lm_);
  PushColourFromMac();
}

void Gte::Ncs(int vector_index) {
  NormalColour(vector_index);
}

void Gte::Nct() {
  Ncs(0);
  Ncs(1);
  Ncs(2);
}

void Gte::Nccs(int vector_index) {
  MultiplyMatrixByVector(1, v_[vector_index], 3, lm_);
  const int16_t light[3] = { ir_[1], ir_[2], ir_[3] };
  MultiplyMatrixByVector(2, light, 1, lm_);
  SetMacAndIr((static_cast<int64_t>(rgbc_[0]) * ir_[1]) << 4,
              (static_cast<int64_t>(rgbc_[1]) * ir_[2]) << 4,
              (static_cast<int64_t>(rgbc_[2]) * ir_[3]) << 4, lm_);
  PushColourFromMac();
}

void Gte::Ncct() {
  Nccs(0);
  Nccs(1);
  Nccs(2);
}

void Gte::Ncds(int vector_index) {
  MultiplyMatrixByVector(1, v_[vector_index], 3, lm_);
  const int16_t light[3] = { ir_[1], ir_[2], ir_[3] };
  MultiplyMatrixByVector(2, light, 1, lm_);
  const int32_t r = (static_cast<int32_t>(rgbc_[0]) * ir_[1]) << 4;
  const int32_t g = (static_cast<int32_t>(rgbc_[1]) * ir_[2]) << 4;
  const int32_t b = (static_cast<int32_t>(rgbc_[2]) * ir_[3]) << 4;
  InterpolateFarColour(r, g, b);
  PushColourFromMac();
}

void Gte::Ncdt() {
  Ncds(0);
  Ncds(1);
  Ncds(2);
}

void Gte::Cc() {
  const int16_t source[3] = { ir_[1], ir_[2], ir_[3] };
  MultiplyMatrixByVector(2, source, 1, lm_);
  SetMacAndIr((static_cast<int64_t>(rgbc_[0]) * ir_[1]) << 4,
              (static_cast<int64_t>(rgbc_[1]) * ir_[2]) << 4,
              (static_cast<int64_t>(rgbc_[2]) * ir_[3]) << 4, lm_);
  PushColourFromMac();
}

void Gte::Cdp() {
  const int16_t source[3] = { ir_[1], ir_[2], ir_[3] };
  MultiplyMatrixByVector(2, source, 1, lm_);
  const int32_t r = (static_cast<int32_t>(rgbc_[0]) * ir_[1]) << 4;
  const int32_t g = (static_cast<int32_t>(rgbc_[1]) * ir_[2]) << 4;
  const int32_t b = (static_cast<int32_t>(rgbc_[2]) * ir_[3]) << 4;
  InterpolateFarColour(r, g, b);
  PushColourFromMac();
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------

void Gte::Execute(uint32_t command) {
  const uint32_t opcode = command & 0x3F;

  // Every command starts with FLAG clear: it reports what *this* command did.
  flag_ = 0;
  sf_ = (command & (1u << 19)) ? 12 : 0;
  lm_ = (command & (1u << 10)) != 0;

  ++stats_.commands;
  ++stats_.executed[opcode];

  switch (opcode) {
    case 0x01: Rtps(0, true); break;
    case 0x06: Nclip(); break;
    case 0x0C: Op(); break;
    case 0x10: Dpcs(false); break;
    case 0x11: Intpl(); break;
    case 0x12: Mvmva(command); break;
    case 0x13: Ncds(0); break;
    case 0x14: Cdp(); break;
    case 0x16: Ncdt(); break;
    case 0x1B: Nccs(0); break;
    case 0x1C: Cc(); break;
    case 0x1E: Ncs(0); break;
    case 0x20: Nct(); break;
    case 0x28: Sqr(); break;
    case 0x29: Dcpl(); break;
    case 0x2A: Dpct(); break;
    case 0x2D: Avsz3(); break;
    case 0x2E: Avsz4(); break;
    case 0x30: Rtpt(); break;
    case 0x3D: Gpf(); break;
    case 0x3E: Gpl(); break;
    case 0x3F: Ncct(); break;
    default:
      ++stats_.unknown_commands;
      break;
  }

  if (flag_ & kFlagErrorMask)
    flag_ |= 0x80000000u;
}

// ---------------------------------------------------------------------------
// Register file
// ---------------------------------------------------------------------------

uint32_t Gte::ReadData(uint32_t index) {
  switch (index & 31) {
    case 0:  return (static_cast<uint16_t>(v_[0][0])) |
                    (static_cast<uint32_t>(static_cast<uint16_t>(v_[0][1])) << 16);
    case 1:  return static_cast<uint32_t>(static_cast<int32_t>(v_[0][2]));
    case 2:  return (static_cast<uint16_t>(v_[1][0])) |
                    (static_cast<uint32_t>(static_cast<uint16_t>(v_[1][1])) << 16);
    case 3:  return static_cast<uint32_t>(static_cast<int32_t>(v_[1][2]));
    case 4:  return (static_cast<uint16_t>(v_[2][0])) |
                    (static_cast<uint32_t>(static_cast<uint16_t>(v_[2][1])) << 16);
    case 5:  return static_cast<uint32_t>(static_cast<int32_t>(v_[2][2]));
    case 6:  return rgbc_[0] | (rgbc_[1] << 8) | (rgbc_[2] << 16) |
                    (static_cast<uint32_t>(rgbc_[3]) << 24);
    case 7:  return otz_;
    case 8:  return static_cast<uint32_t>(static_cast<int32_t>(ir_[0]));
    case 9:  return static_cast<uint32_t>(static_cast<int32_t>(ir_[1]));
    case 10: return static_cast<uint32_t>(static_cast<int32_t>(ir_[2]));
    case 11: return static_cast<uint32_t>(static_cast<int32_t>(ir_[3]));
    case 12: case 13: case 14: {
      const int slot = (index & 31) - 12;
      return (static_cast<uint16_t>(sxy_[slot][0])) |
             (static_cast<uint32_t>(static_cast<uint16_t>(sxy_[slot][1])) << 16);
    }
    // SXYP mirrors the newest entry rather than being a register of its own.
    case 15: return (static_cast<uint16_t>(sxy_[2][0])) |
                    (static_cast<uint32_t>(static_cast<uint16_t>(sxy_[2][1])) << 16);
    case 16: return sz_[0];
    case 17: return sz_[1];
    case 18: return sz_[2];
    case 19: return sz_[3];
    case 20: case 21: case 22: {
      const int slot = (index & 31) - 20;
      return rgb_fifo_[slot][0] | (rgb_fifo_[slot][1] << 8) |
             (rgb_fifo_[slot][2] << 16) |
             (static_cast<uint32_t>(rgb_fifo_[slot][3]) << 24);
    }
    case 23: return res1_;
    case 24: return static_cast<uint32_t>(mac_[0]);
    case 25: return static_cast<uint32_t>(mac_[1]);
    case 26: return static_cast<uint32_t>(mac_[2]);
    case 27: return static_cast<uint32_t>(mac_[3]);
    case 28: case 29: {
      // IRGB and ORGB both read the same thing: IR1-3 squeezed into 5 bits.
      const int32_t r = std::min(std::max(ir_[1] / 128, 0), 0x1F);
      const int32_t g = std::min(std::max(ir_[2] / 128, 0), 0x1F);
      const int32_t b = std::min(std::max(ir_[3] / 128, 0), 0x1F);
      return static_cast<uint32_t>(r | (g << 5) | (b << 10));
    }
    case 30: return lzcs_;
    default: return static_cast<uint32_t>(lzcr_);
  }
}

void Gte::WriteData(uint32_t index, uint32_t value) {
  switch (index & 31) {
    case 0:  v_[0][0] = static_cast<int16_t>(value);
             v_[0][1] = static_cast<int16_t>(value >> 16); break;
    case 1:  v_[0][2] = static_cast<int16_t>(value); break;
    case 2:  v_[1][0] = static_cast<int16_t>(value);
             v_[1][1] = static_cast<int16_t>(value >> 16); break;
    case 3:  v_[1][2] = static_cast<int16_t>(value); break;
    case 4:  v_[2][0] = static_cast<int16_t>(value);
             v_[2][1] = static_cast<int16_t>(value >> 16); break;
    case 5:  v_[2][2] = static_cast<int16_t>(value); break;
    case 6:  rgbc_[0] = static_cast<uint8_t>(value);
             rgbc_[1] = static_cast<uint8_t>(value >> 8);
             rgbc_[2] = static_cast<uint8_t>(value >> 16);
             rgbc_[3] = static_cast<uint8_t>(value >> 24); break;
    case 7:  otz_ = static_cast<uint16_t>(value); break;
    case 8:  ir_[0] = static_cast<int16_t>(value); break;
    case 9:  ir_[1] = static_cast<int16_t>(value); break;
    case 10: ir_[2] = static_cast<int16_t>(value); break;
    case 11: ir_[3] = static_cast<int16_t>(value); break;
    case 12: case 13: case 14: {
      const int slot = (index & 31) - 12;
      sxy_[slot][0] = static_cast<int16_t>(value);
      sxy_[slot][1] = static_cast<int16_t>(value >> 16);
      break;
    }
    case 15:
      // Writing SXYP pushes the FIFO rather than writing a register.
      PushScreenXy(static_cast<int16_t>(value),
                   static_cast<int16_t>(value >> 16));
      break;
    case 16: sz_[0] = static_cast<uint16_t>(value); break;
    case 17: sz_[1] = static_cast<uint16_t>(value); break;
    case 18: sz_[2] = static_cast<uint16_t>(value); break;
    case 19: sz_[3] = static_cast<uint16_t>(value); break;
    case 20: case 21: case 22: {
      const int slot = (index & 31) - 20;
      rgb_fifo_[slot][0] = static_cast<uint8_t>(value);
      rgb_fifo_[slot][1] = static_cast<uint8_t>(value >> 8);
      rgb_fifo_[slot][2] = static_cast<uint8_t>(value >> 16);
      rgb_fifo_[slot][3] = static_cast<uint8_t>(value >> 24);
      break;
    }
    case 23: res1_ = value; break;
    case 24: mac_[0] = static_cast<int32_t>(value); break;
    case 25: mac_[1] = static_cast<int32_t>(value); break;
    case 26: mac_[2] = static_cast<int32_t>(value); break;
    case 27: mac_[3] = static_cast<int32_t>(value); break;
    case 28:
      // Writing IRGB expands 5-bit components back into IR1-3.
      ir_[1] = static_cast<int16_t>((value & 0x1F) * 128);
      ir_[2] = static_cast<int16_t>(((value >> 5) & 0x1F) * 128);
      ir_[3] = static_cast<int16_t>(((value >> 10) & 0x1F) * 128);
      break;
    case 29: break;                       // ORGB is read-only
    case 30:
      lzcs_ = value;
      lzcr_ = ComputeLeadingCount(value);
      break;
    default: break;                       // LZCR is read-only
  }
}

uint32_t Gte::ReadControl(uint32_t index) {
  switch (index & 31) {
    case 0:  return (static_cast<uint16_t>(matrix_[0][0][0])) |
                    (static_cast<uint32_t>(static_cast<uint16_t>(matrix_[0][0][1])) << 16);
    case 1:  return (static_cast<uint16_t>(matrix_[0][0][2])) |
                    (static_cast<uint32_t>(static_cast<uint16_t>(matrix_[0][1][0])) << 16);
    case 2:  return (static_cast<uint16_t>(matrix_[0][1][1])) |
                    (static_cast<uint32_t>(static_cast<uint16_t>(matrix_[0][1][2])) << 16);
    case 3:  return (static_cast<uint16_t>(matrix_[0][2][0])) |
                    (static_cast<uint32_t>(static_cast<uint16_t>(matrix_[0][2][1])) << 16);
    case 4:  return static_cast<uint32_t>(static_cast<int32_t>(matrix_[0][2][2]));
    case 5:  return static_cast<uint32_t>(translation_[0][0]);
    case 6:  return static_cast<uint32_t>(translation_[0][1]);
    case 7:  return static_cast<uint32_t>(translation_[0][2]);
    case 8:  return (static_cast<uint16_t>(matrix_[1][0][0])) |
                    (static_cast<uint32_t>(static_cast<uint16_t>(matrix_[1][0][1])) << 16);
    case 9:  return (static_cast<uint16_t>(matrix_[1][0][2])) |
                    (static_cast<uint32_t>(static_cast<uint16_t>(matrix_[1][1][0])) << 16);
    case 10: return (static_cast<uint16_t>(matrix_[1][1][1])) |
                    (static_cast<uint32_t>(static_cast<uint16_t>(matrix_[1][1][2])) << 16);
    case 11: return (static_cast<uint16_t>(matrix_[1][2][0])) |
                    (static_cast<uint32_t>(static_cast<uint16_t>(matrix_[1][2][1])) << 16);
    case 12: return static_cast<uint32_t>(static_cast<int32_t>(matrix_[1][2][2]));
    case 13: return static_cast<uint32_t>(translation_[1][0]);
    case 14: return static_cast<uint32_t>(translation_[1][1]);
    case 15: return static_cast<uint32_t>(translation_[1][2]);
    case 16: return (static_cast<uint16_t>(matrix_[2][0][0])) |
                    (static_cast<uint32_t>(static_cast<uint16_t>(matrix_[2][0][1])) << 16);
    case 17: return (static_cast<uint16_t>(matrix_[2][0][2])) |
                    (static_cast<uint32_t>(static_cast<uint16_t>(matrix_[2][1][0])) << 16);
    case 18: return (static_cast<uint16_t>(matrix_[2][1][1])) |
                    (static_cast<uint32_t>(static_cast<uint16_t>(matrix_[2][1][2])) << 16);
    case 19: return (static_cast<uint16_t>(matrix_[2][2][0])) |
                    (static_cast<uint32_t>(static_cast<uint16_t>(matrix_[2][2][1])) << 16);
    case 20: return static_cast<uint32_t>(static_cast<int32_t>(matrix_[2][2][2]));
    case 21: return static_cast<uint32_t>(translation_[2][0]);
    case 22: return static_cast<uint32_t>(translation_[2][1]);
    case 23: return static_cast<uint32_t>(translation_[2][2]);
    case 24: return static_cast<uint32_t>(ofx_);
    case 25: return static_cast<uint32_t>(ofy_);
    // H is stored unsigned and used unsigned, but reads back sign-extended.
    // That is a documented hardware quirk and software has been seen to rely
    // on it, so it is not tidied away here.
    case 26: return static_cast<uint32_t>(SignExtend16(h_));
    case 27: return static_cast<uint32_t>(static_cast<int32_t>(dqa_));
    case 28: return static_cast<uint32_t>(dqb_);
    case 29: return static_cast<uint32_t>(static_cast<int32_t>(zsf3_));
    case 30: return static_cast<uint32_t>(static_cast<int32_t>(zsf4_));
    default: return flag_;
  }
}

void Gte::WriteControl(uint32_t index, uint32_t value) {
  switch (index & 31) {
    case 0:  matrix_[0][0][0] = static_cast<int16_t>(value);
             matrix_[0][0][1] = static_cast<int16_t>(value >> 16); break;
    case 1:  matrix_[0][0][2] = static_cast<int16_t>(value);
             matrix_[0][1][0] = static_cast<int16_t>(value >> 16); break;
    case 2:  matrix_[0][1][1] = static_cast<int16_t>(value);
             matrix_[0][1][2] = static_cast<int16_t>(value >> 16); break;
    case 3:  matrix_[0][2][0] = static_cast<int16_t>(value);
             matrix_[0][2][1] = static_cast<int16_t>(value >> 16); break;
    case 4:  matrix_[0][2][2] = static_cast<int16_t>(value); break;
    case 5:  translation_[0][0] = static_cast<int32_t>(value); break;
    case 6:  translation_[0][1] = static_cast<int32_t>(value); break;
    case 7:  translation_[0][2] = static_cast<int32_t>(value); break;
    case 8:  matrix_[1][0][0] = static_cast<int16_t>(value);
             matrix_[1][0][1] = static_cast<int16_t>(value >> 16); break;
    case 9:  matrix_[1][0][2] = static_cast<int16_t>(value);
             matrix_[1][1][0] = static_cast<int16_t>(value >> 16); break;
    case 10: matrix_[1][1][1] = static_cast<int16_t>(value);
             matrix_[1][1][2] = static_cast<int16_t>(value >> 16); break;
    case 11: matrix_[1][2][0] = static_cast<int16_t>(value);
             matrix_[1][2][1] = static_cast<int16_t>(value >> 16); break;
    case 12: matrix_[1][2][2] = static_cast<int16_t>(value); break;
    case 13: translation_[1][0] = static_cast<int32_t>(value); break;
    case 14: translation_[1][1] = static_cast<int32_t>(value); break;
    case 15: translation_[1][2] = static_cast<int32_t>(value); break;
    case 16: matrix_[2][0][0] = static_cast<int16_t>(value);
             matrix_[2][0][1] = static_cast<int16_t>(value >> 16); break;
    case 17: matrix_[2][0][2] = static_cast<int16_t>(value);
             matrix_[2][1][0] = static_cast<int16_t>(value >> 16); break;
    case 18: matrix_[2][1][1] = static_cast<int16_t>(value);
             matrix_[2][1][2] = static_cast<int16_t>(value >> 16); break;
    case 19: matrix_[2][2][0] = static_cast<int16_t>(value);
             matrix_[2][2][1] = static_cast<int16_t>(value >> 16); break;
    case 20: matrix_[2][2][2] = static_cast<int16_t>(value); break;
    case 21: translation_[2][0] = static_cast<int32_t>(value); break;
    case 22: translation_[2][1] = static_cast<int32_t>(value); break;
    case 23: translation_[2][2] = static_cast<int32_t>(value); break;
    case 24: ofx_ = static_cast<int32_t>(value); break;
    case 25: ofy_ = static_cast<int32_t>(value); break;
    case 26: h_ = static_cast<uint16_t>(value); break;
    case 27: dqa_ = static_cast<int16_t>(value); break;
    case 28: dqb_ = static_cast<int32_t>(value); break;
    case 29: zsf3_ = static_cast<int16_t>(value); break;
    case 30: zsf4_ = static_cast<int16_t>(value); break;
    default:
      // Bit 31 is not writable; it is recomputed from the error bits.
      flag_ = value & 0x7FFFF000u;
      if (flag_ & kFlagErrorMask)
        flag_ |= 0x80000000u;
      break;
  }
}

}
}
