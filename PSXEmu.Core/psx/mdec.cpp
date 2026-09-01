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

#include <cstring>

namespace emulation {
namespace psx {

namespace {

// Coefficients arrive along the diagonals of the block, so this maps the order
// they come in to the position they belong in.
const uint8_t kZigzag[64] = {
   0,  1,  8, 16,  9,  2,  3, 10,
  17, 24, 32, 25, 18, 11,  4,  5,
  12, 19, 26, 33, 40, 48, 41, 34,
  27, 20, 13,  6,  7, 14, 21, 28,
  35, 42, 49, 56, 57, 50, 43, 36,
  29, 22, 15, 23, 30, 37, 44, 51,
  58, 59, 52, 45, 38, 31, 39, 46,
  53, 60, 61, 54, 47, 55, 62, 63,
};

// The value software pads with once it has sent a block's last coefficient.
// Read as a code it is a run of 63 with a level of zero, which walks off the
// end of the block and would end it anyway - but naming it makes the places
// that check for it readable.
const uint16_t kEndOfBlock = 0xFE00;

int32_t SignExtend10(uint32_t value) {
  return static_cast<int32_t>(static_cast<int16_t>(value << 6)) >> 6;
}

int32_t Clamp(int32_t value, int32_t low, int32_t high) {
  if (value < low)
    return low;
  if (value > high)
    return high;
  return value;
}

uint8_t ClampToByte(int32_t value) {
  return static_cast<uint8_t>(Clamp(value, 0, 255));
}

}  // namespace

Mdec::Mdec() {
  memset(quant_luma_, 0, sizeof(quant_luma_));
  memset(quant_chroma_, 0, sizeof(quant_chroma_));
  memset(scale_table_, 0, sizeof(scale_table_));
  memset(&stats_, 0, sizeof(stats_));
  Reset();
}

Mdec::~Mdec() {
}

int Mdec::Initialize() {
  memset(quant_luma_, 0, sizeof(quant_luma_));
  memset(quant_chroma_, 0, sizeof(quant_chroma_));
  memset(scale_table_, 0, sizeof(scale_table_));
  memset(&stats_, 0, sizeof(stats_));
  Reset();
  return S_OK;
}

int Mdec::Deinitialize() {
  return S_OK;
}

void Mdec::Reset() {
  state_ = kIdle;
  command_ = 0;
  words_remaining_ = 0;
  data_in_enabled_ = false;
  data_out_enabled_ = false;
  current_block_ = 0;
  table_count_ = 0;
  block_index_ = 0;
  coefficient_index_ = 0;
  quant_scale_ = 0;
  output_count_ = 0;
  output_read_ = 0;
  memset(blocks_, 0, sizeof(blocks_));
  memset(coefficients_, 0, sizeof(coefficients_));
}

// ---------------------------------------------------------------------------
// Registers
// ---------------------------------------------------------------------------

uint32_t Mdec::Status() const {
  uint32_t status = 0;
  if (output_read_ >= output_count_)
    status |= 0x80000000;                       // 31: data-out fifo empty
  if (state_ != kIdle && words_remaining_ == 0)
    status |= 0x40000000;                       // 30: data-in fifo full
  if (state_ != kIdle)
    status |= 0x20000000;                       // 29: command busy
  if (data_in_enabled_ && WantsData())
    status |= 0x10000000;                       // 28: data-in request
  if (data_out_enabled_ && HasData())
    status |= 0x08000000;                       // 27: data-out request

  status |= (static_cast<uint32_t>(depth()) & 3) << 25;
  if (output_signed())
    status |= 0x01000000;
  if (output_bit15())
    status |= 0x00800000;
  status |= (current_block_ & 0x1F) << 16;

  // Words remaining minus one, so "none left" reads as FFFFh.
  status |= (words_remaining_ - 1) & 0xFFFF;
  return status;
}

uint32_t Mdec::Read(uint32_t address) {
  if ((address & 4) != 0)
    return Status();
  return ReadWord();
}

void Mdec::Write(uint32_t address, uint32_t data) {
  if ((address & 4) != 0) {
    if (data & 0x80000000) {
      Reset();
      return;
    }
    data_in_enabled_ = (data & 0x40000000) != 0;
    data_out_enabled_ = (data & 0x20000000) != 0;
    return;
  }
  WriteWord(data);
}

bool Mdec::WantsData() const {
  return state_ != kIdle && words_remaining_ > 0;
}

bool Mdec::HasData() const {
  return output_read_ < output_count_;
}

uint32_t Mdec::ReadWord() {
  // Draining everything does not reset the buffer: more macroblocks of the
  // same command may still be on their way, and they append behind these.
  // Only a new command starts it over.
  if (output_read_ >= output_count_)
    return 0;
  ++stats_.words_out;
  return output_[output_read_++];
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

void Mdec::StartCommand(uint32_t command) {
  command_ = command;
  ++stats_.commands;

  switch ((command >> 29) & 7) {
    case 1:                                    // decode macroblock(s)
      state_ = kDecoding;
      words_remaining_ = command & 0xFFFF;
      // A monochrome stream carries luminance only, so it starts where the
      // luminance blocks would be rather than at the chrominance pair.
      block_index_ = monochrome() ? 2 : 0;
      current_block_ = monochrome() ? 0 : 4;
      coefficient_index_ = 0;
      output_count_ = 0;
      output_read_ = 0;
      break;

    case 2:                                    // set quant table(s)
      state_ = kQuantTable;
      // Bit 0 says whether the colour table follows the luminance one.
      words_remaining_ = (command & 1) ? 32 : 16;
      table_count_ = 0;
      break;

    case 3:                                    // set scale table
      state_ = kScaleTable;
      words_remaining_ = 32;
      table_count_ = 0;
      break;

    default:
      ++stats_.unknown_commands;
      state_ = kIdle;
      words_remaining_ = 0;
      break;
  }
}

void Mdec::WriteWord(uint32_t word) {
  if (state_ == kIdle) {
    StartCommand(word);
    return;
  }

  ++stats_.words_in;

  switch (state_) {
    case kQuantTable:
    case kScaleTable:
      if (table_count_ < 32)
        table_words_[table_count_++] = word;
      break;

    case kDecoding:
      FeedDecode(word);
      break;

    default:
      break;
  }

  if (words_remaining_ > 0)
    --words_remaining_;
  if (words_remaining_ != 0)
    return;

  // The last parameter word has arrived, so the command is done.
  if (state_ == kQuantTable) {
    // Four entries to a word, luminance first, colour after it if asked for.
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(table_words_);
    memcpy(quant_luma_, bytes, 64);
    if (table_count_ > 16)
      memcpy(quant_chroma_, bytes + 64, 64);
  } else if (state_ == kScaleTable) {
    // Two signed halfwords to a word.
    memcpy(scale_table_, table_words_, sizeof(scale_table_));
  }

  state_ = kIdle;
}

// ---------------------------------------------------------------------------
// Decoding
// ---------------------------------------------------------------------------

void Mdec::FeedDecode(uint32_t word) {
  FeedCode(static_cast<uint16_t>(word & 0xFFFF));
  FeedCode(static_cast<uint16_t>(word >> 16));
}

// One run/level code. The first code of a block carries the quantisation
// factor and the DC coefficient instead; every code after it is a run of
// zeroes followed by the value that ends the run.
bool Mdec::FeedCode(uint16_t code) {
  if (coefficient_index_ == 0) {
    // Software pads the tail of its data with end-of-block markers. One
    // arriving where a block should start is padding, not a block.
    if (code == kEndOfBlock)
      return false;

    memset(coefficients_, 0, sizeof(coefficients_));
    quant_scale_ = (code >> 10) & 0x3F;
    const int32_t dc = SignExtend10(code & 0x3FF);
    const uint8_t* quant = quant_for_block();
    const int32_t value = (quant_scale_ == 0) ? (dc * 2) : (dc * quant[0]);
    coefficients_[0] = static_cast<int16_t>(Clamp(value, -0x400, 0x3FF));
    coefficient_index_ = 1;
    return false;
  }

  if (code == kEndOfBlock) {
    FinishBlock();
    return true;
  }

  const uint32_t run = (code >> 10) & 0x3F;
  const int32_t level = SignExtend10(code & 0x3FF);

  coefficient_index_ += run;
  if (coefficient_index_ > 63) {
    // Ran off the end of the block. The hardware stops here rather than
    // writing past it.
    ++stats_.short_blocks;
    FinishBlock();
    return true;
  }

  const uint8_t* quant = quant_for_block();
  int32_t value;
  if (quant_scale_ == 0) {
    value = level * 2;
  } else {
    value = (level * static_cast<int32_t>(quant[coefficient_index_]) *
             static_cast<int32_t>(quant_scale_) + 4) / 8;
  }
  coefficients_[kZigzag[coefficient_index_]] =
      static_cast<int16_t>(Clamp(value, -0x400, 0x3FF));
  ++coefficient_index_;

  if (coefficient_index_ > 63) {
    FinishBlock();
    return true;
  }
  return false;
}

void Mdec::FinishBlock() {
  InverseDct(coefficients_, blocks_[block_index_]);
  coefficient_index_ = 0;

  if (monochrome()) {
    EmitMonoBlock(blocks_[block_index_]);
    return;
  }

  ++block_index_;
  // The status register numbers the blocks 0..3 for luminance and 4,5 for
  // chrominance, which is not the order they arrive in.
  current_block_ = (block_index_ < 2) ? (block_index_ + 4) : (block_index_ - 2);
  if (block_index_ == 6) {
    EmitMacroblock();
    block_index_ = 0;
    current_block_ = 4;
  }
}

// ---------------------------------------------------------------------------
// The inverse transform
// ---------------------------------------------------------------------------

// Separable 8x8, using the cosine table the scale-table command supplied. Two
// passes of a straightforward matrix multiply: correctness first, and this is
// nowhere near the hot path - a 320x240 frame is 300 macroblocks.
void Mdec::InverseDct(const int16_t* in, int16_t* out) const {
  int32_t pass[64];

  // Columns.
  for (int x = 0; x < 8; ++x) {
    for (int y = 0; y < 8; ++y) {
      int32_t sum = 0;
      for (int u = 0; u < 8; ++u)
        sum += static_cast<int32_t>(in[u * 8 + x]) * scale_table_[u * 8 + y];
      // Rounded, not truncated: an arithmetic shift biases negative values
      // downwards, and half the coefficients of a real block are negative.
      pass[y * 8 + x] = (sum + (1 << 14)) >> 15;
    }
  }

  // Rows.
  for (int y = 0; y < 8; ++y) {
    for (int x = 0; x < 8; ++x) {
      int32_t sum = 0;
      for (int u = 0; u < 8; ++u)
        sum += pass[y * 8 + u] * scale_table_[u * 8 + x];
      // Rounded, then saturated to the range the hardware carries.
      const int32_t value = (sum + (1 << 14)) >> 15;
      out[y * 8 + x] = static_cast<int16_t>(Clamp(value, -128, 127));
    }
  }
}

// ---------------------------------------------------------------------------
// Colour conversion and output
// ---------------------------------------------------------------------------

namespace {

// Packs one pixel into the output buffer in whichever depth was asked for.
// Kept apart from the conversion itself so the four depths are in one place.
struct PixelWriter {
  uint32_t* words;
  uint32_t capacity;
  uint32_t count;          // words used
  uint32_t partial;        // bits waiting for a whole word
  uint32_t partial_bits;

  void Push(uint32_t value, uint32_t bits) {
    partial |= (value & ((bits == 32) ? 0xFFFFFFFFu : ((1u << bits) - 1)))
               << partial_bits;
    partial_bits += bits;
    while (partial_bits >= 32) {
      if (count < capacity)
        words[count++] = partial;
      partial = 0;
      partial_bits = 0;
    }
  }

  void Flush() {
    if (partial_bits != 0 && count < capacity) {
      words[count++] = partial;
      partial = 0;
      partial_bits = 0;
    }
  }
};

}  // namespace

// One 16x16 macroblock: two chrominance blocks at half resolution and four
// luminance blocks, arranged top-left, top-right, bottom-left, bottom-right.
void Mdec::EmitMacroblock() {
  const int16_t* cr = blocks_[0];
  const int16_t* cb = blocks_[1];
  const int16_t* const luma[4] = { blocks_[2], blocks_[3], blocks_[4],
                                   blocks_[5] };

  PixelWriter writer = { output_, kOutputCapacity, output_count_, 0, 0 };
  const int32_t offset = output_signed() ? 0 : 128;
  const uint32_t bit15 = output_bit15() ? 0x8000u : 0u;

  for (int y = 0; y < 16; ++y) {
    for (int x = 0; x < 16; ++x) {
      // Chrominance is one sample per 2x2 luminance pixels.
      const int index = (y >> 1) * 8 + (x >> 1);
      const int32_t cr_value = cr[index];
      const int32_t cb_value = cb[index];

      // The luminance block this pixel falls in, and where in it.
      const int block = (y >> 3) * 2 + (x >> 3);
      const int32_t y_value = luma[block][(y & 7) * 8 + (x & 7)];

      // The hardware's fixed-point form of the usual YCbCr matrix:
      //   R = Y + 1.402 Cr
      //   G = Y - 0.3437 Cb - 0.7143 Cr
      //   B = Y + 1.772 Cb
      const int32_t r = y_value + ((91881 * cr_value) >> 16);
      const int32_t g =
          y_value - ((22554 * cb_value + 46802 * cr_value) >> 16);
      const int32_t b = y_value + ((116130 * cb_value) >> 16);

      switch (depth()) {
        case kDepth24: {
          writer.Push(ClampToByte(r + offset), 8);
          writer.Push(ClampToByte(g + offset), 8);
          writer.Push(ClampToByte(b + offset), 8);
          break;
        }
        case kDepth15:
        default: {
          const uint32_t r5 = ClampToByte(r + offset) >> 3;
          const uint32_t g5 = ClampToByte(g + offset) >> 3;
          const uint32_t b5 = ClampToByte(b + offset) >> 3;
          writer.Push(bit15 | (b5 << 10) | (g5 << 5) | r5, 16);
          break;
        }
      }
    }
  }

  writer.Flush();
  if (writer.count == kOutputCapacity && output_count_ != writer.count)
    ++stats_.overflows;
  output_count_ = writer.count;
  ++stats_.macroblocks;
}

// 4-bit and 8-bit output are luminance only, one 8x8 block at a time.
void Mdec::EmitMonoBlock(const int16_t* luma) {
  PixelWriter writer = { output_, kOutputCapacity, output_count_, 0, 0 };
  const int32_t offset = output_signed() ? 0 : 128;

  for (int i = 0; i < 64; ++i) {
    const int32_t value = ClampToByte(luma[i] + offset);
    if (depth() == kDepth4)
      writer.Push(static_cast<uint32_t>(value) >> 4, 4);
    else
      writer.Push(static_cast<uint32_t>(value), 8);
  }

  writer.Flush();
  if (writer.count == kOutputCapacity && output_count_ != writer.count)
    ++stats_.overflows;
  output_count_ = writer.count;
  ++stats_.macroblocks;
}

}
}
