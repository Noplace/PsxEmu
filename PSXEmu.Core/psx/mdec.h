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

// The MDEC, the PlayStation's macroblock decoder.
//
// It is narrower than its name suggests. It does no variable-length decoding:
// software unpacks the Huffman stream itself and hands the MDEC already
// separated run/level pairs, two to a 32-bit word. What the hardware does is
// dequantise, inverse-transform, convert colour and pack pixels - one 16x16
// macroblock at a time, with no memory of a frame.
//
// Two registers:
//   0x1F801820  W: command and parameters.  R: the decoded data.
//   0x1F801824  W: control and reset.       R: status.
//
// Almost everything moves through DMA channel 0 going in and channel 1 coming
// out; the data port exists but a game moving a frame a word at a time would
// not keep up.
class Mdec : public Component {
 public:
  Mdec();
  ~Mdec();

  int Initialize();
  int Deinitialize();

  uint32_t Read(uint32_t address);
  void Write(uint32_t address, uint32_t data);

  // The DMA channels' view.
  void WriteWord(uint32_t word);
  uint32_t ReadWord();
  bool WantsData() const;      // channel 0 should run
  bool HasData() const;        // channel 1 should run

  uint32_t Status() const;

  struct Stats {
    uint64_t commands;
    uint64_t macroblocks;
    uint64_t words_in;
    uint64_t words_out;
    uint64_t unknown_commands;
    uint64_t short_blocks;     // a block whose data ended before it did
    uint64_t overflows;        // output dropped because nothing drained it
  };
  const Stats& stats() const { return stats_; }

  enum OutputDepth { kDepth4 = 0, kDepth8 = 1, kDepth24 = 2, kDepth15 = 3 };

 private:
  enum State {
    kIdle,
    kDecoding,      // command 1, collecting compressed data
    kQuantTable,    // command 2
    kScaleTable,    // command 3
  };

  State state_;
  uint32_t command_;              // the command word being served
  uint32_t words_remaining_;      // parameter words still wanted
  bool data_in_enabled_;          // control bit 30
  bool data_out_enabled_;         // control bit 29
  uint32_t current_block_;        // status bits 20-16

  uint8_t quant_luma_[64];
  uint8_t quant_chroma_[64];
  int16_t scale_table_[64];

  // Parameter words for the table commands land here before being unpacked.
  uint32_t table_words_[32];
  uint32_t table_count_;

  // The six blocks of a macroblock, decoded as their run/level pairs arrive.
  // Colour order is Cr, Cb, then the four luminance blocks.
  int16_t blocks_[6][64];
  uint32_t block_index_;          // which of the six is being filled
  int16_t coefficients_[64];      // the block being assembled, in raster order
  uint32_t coefficient_index_;    // 0 means the next code is the DC word
  uint32_t quant_scale_;

  // Pixels waiting for channel 1. One decode command carries as many
  // macroblocks as software cares to send - a whole frame is normal - and it
  // drains them afterwards, thirty-two words at a time. So this accumulates
  // across the command rather than holding one macroblock: emitting into a
  // single block buffer throws away everything but the last one, which looks
  // exactly like a video that decodes to one square of picture.
  static const int kOutputCapacity = 96 * 1024;
  uint32_t output_[kOutputCapacity];
  uint32_t output_count_;
  uint32_t output_read_;

  Stats stats_;

  void Reset();
  void StartCommand(uint32_t command);
  void FeedDecode(uint32_t word);
  bool FeedCode(uint16_t code);     // true when a block finished
  void FinishBlock();
  void EmitMacroblock();
  void EmitMonoBlock(const int16_t* luma);

  void InverseDct(const int16_t* in, int16_t* out) const;

  OutputDepth depth() const {
    return static_cast<OutputDepth>((command_ >> 27) & 3);
  }
  bool output_signed() const { return (command_ & 0x04000000) != 0; }
  bool output_bit15() const { return (command_ & 0x02000000) != 0; }
  bool monochrome() const {
    return depth() == kDepth4 || depth() == kDepth8;
  }
  const uint8_t* quant_for_block() const {
    // In colour, the first two blocks are chrominance. In monochrome there is
    // only ever the luminance table.
    if (monochrome())
      return quant_luma_;
    return (block_index_ < 2) ? quant_chroma_ : quant_luma_;
  }
};

}
}
