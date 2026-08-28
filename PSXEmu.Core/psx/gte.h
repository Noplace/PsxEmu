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
  Geometry Transformation Engine - coprocessor 2.

  A fixed-point vector and matrix unit. Everything 3D on this machine goes
  through it: vertices are rotated, translated and perspective-divided here,
  and lighting and depth cueing are computed here too. It has no analogue on
  any other console this project's practices came from, and nothing about it is
  visible from the outside except through 64 registers and a command word.

  Two things make it awkward, and both are load-bearing rather than
  incidental:

  - **Everything saturates, and the saturation is recorded.** Each result is
    clamped to its own register's range, and clamping sets a bit in FLAG.
    Games read FLAG - it is how back-face culling and near-plane clipping are
    done - so a saturation that is silently skipped does not produce a slightly
    wrong picture, it produces polygons that should have been discarded.
  - **The perspective divide is not a divide.** It is a Newton-Raphson
    reciprocal off a 257-entry table, and its exact result, including how it
    overflows, is what software depends on.
*/
class Gte : public Component {
 public:
  Gte();
  ~Gte();

  int Initialize();
  int Deinitialize();

  // The four register-move instructions, and the command word.
  uint32_t ReadData(uint32_t index);                  // MFC2
  void WriteData(uint32_t index, uint32_t value);     // MTC2
  uint32_t ReadControl(uint32_t index);               // CFC2
  void WriteControl(uint32_t index, uint32_t value);  // CTC2
  void Execute(uint32_t command);

  // Kept for the aggregate header's older call site.
  void ExecuteCommand(uint32_t command) { Execute(command); }

  // How many commands ran, and how many were not recognised. An unimplemented
  // command that quietly does nothing is the failure mode worth counting.
  struct Stats {
    uint64_t commands;
    uint64_t unknown_commands;
    uint32_t executed[64];
  };
  const Stats& stats() const { return stats_; }

 private:
  // ---- register file -----------------------------------------------------
  int16_t v_[3][3];        // V0, V1, V2 as (x, y, z)
  uint8_t rgbc_[4];        // R, G, B, CODE
  uint16_t otz_;
  int16_t ir_[4];          // IR0, IR1, IR2, IR3
  int16_t sxy_[3][2];      // the screen-coordinate FIFO, oldest first
  uint16_t sz_[4];         // the depth FIFO, oldest first
  uint8_t rgb_fifo_[3][4]; // the colour FIFO, oldest first
  uint32_t res1_;
  int32_t mac_[4];         // MAC0, MAC1, MAC2, MAC3
  uint32_t lzcs_;
  int32_t lzcr_;

  // Three 3x3 matrices: rotation, light direction, light colour.
  int16_t matrix_[3][3][3];
  // Three translation vectors: translation, background colour, far colour.
  int32_t translation_[3][3];

  int32_t ofx_, ofy_;
  uint16_t h_;
  int16_t dqa_;
  int32_t dqb_;
  int16_t zsf3_, zsf4_;
  uint32_t flag_;

  // Set from the command word for the duration of one command.
  bool lm_;                // saturate IR to 0..7FFF rather than -8000..7FFF
  int sf_;                 // 0 or 12, the fractional shift

  Stats stats_;

  // ---- saturation and overflow ------------------------------------------
  // Each of these both clamps and records. They are named after the sections
  // of the hardware description they come from, so the two can be read side
  // by side.
  int64_t CheckMac(int index, int64_t value);        // A1/A2/A3, 44-bit
  int32_t CheckMac0(int64_t value);                  // F, 32-bit
  int16_t SaturateIr(int index, int32_t value, bool lm);   // Lm_B1/B2/B3
  int16_t SaturateIr0(int32_t value);                // Lm_H
  uint8_t SaturateColour(int index, int32_t value);  // Lm_C1/C2/C3
  uint16_t SaturateSz3(int32_t value);               // Lm_D
  int32_t SaturateScreenX(int32_t value);            // Lm_G1
  int32_t SaturateScreenY(int32_t value);            // Lm_G2

  // The reciprocal used by the perspective divide.
  uint32_t Divide(uint16_t numerator, uint16_t denominator);

  // ---- shared steps ------------------------------------------------------
  void SetMacAndIr(int64_t x, int64_t y, int64_t z, bool lm);
  void PushScreenXy(int32_t x, int32_t y);
  void PushScreenZ(uint16_t z);
  void PushColour(uint8_t r, uint8_t g, uint8_t b);
  void PushColourFromMac();
  // Multiplies one of the three matrices by a vector and adds a translation.
  void MultiplyMatrixByVector(int matrix, const int16_t vector[3],
                              int translation, bool lm);
  void InterpolateFarColour(int32_t x, int32_t y, int32_t z);
  void NormalColour(int vector_index);
  void NormalColourColour(int vector_index, bool with_colour);
  void DepthCue(uint8_t r, uint8_t g, uint8_t b);

  // ---- commands ----------------------------------------------------------
  void Rtps(int vector_index, bool compute_ir0);
  void Rtpt();
  void Nclip();
  void Avsz3();
  void Avsz4();
  void Mvmva(uint32_t command);
  void Op();
  void Gpf();
  void Gpl();
  void Sqr();
  void Dcpl();
  void Dpcs(bool use_rgb_fifo);
  void Dpct();
  void Intpl();
  void Ncds(int vector_index);
  void Ncdt();
  void Nccs(int vector_index);
  void Ncct();
  void Ncs(int vector_index);
  void Nct();
  void Cc();
  void Cdp();
};

// Kept so existing code that spelled it the old way still compiles.
typedef Gte GTE;

}
}
