/*****************************************************************************************************************
* Copyright (c) 2015 Khalid Ali Al-Kooheji                                                                       *
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
  Geometry Transformation Engine (GTE)
*/

struct GTEMatrix {
  int16_t _11,_12,_13;
  int16_t _21,_22,_23;
  int16_t _31,_32,_33;
  int16_t _unused;
};

struct GTEContext {
  union {
    struct {
      struct {
        int16_t X,Y,Z,_unused;
      } V0;
      struct {
        int16_t X,Y,Z,_unused;
      } V1;
      struct {
        int16_t X,Y,Z,_unused;
      } V2;
      struct {
        uint8_t R,G,B,C;
      } RGBC;
 
      uint16_t OTZ;
      uint16_t _unused1;

      int16_t IR0, _unused2;
      int16_t IR1, _unused3;
      int16_t IR2, _unused4;
      int16_t IR3, _unused5;

      int16_t SX0,SY0,SX1,SY1,SX2,SY2,SXP,SYP;

      int16_t SZ0, _unused6;
      int16_t SZ1, _unused7;
      int16_t SZ2, _unused8;
      int16_t SZ3, _unused9;

      struct {
        uint8_t R,G,B,C;
      } RGB0,RGB1,RGB2;

      uint32_t RES1;

      int32_t MAC0,MAC1,MAC2,MAC3;
      uint32_t IRGB,ORGB;
      int32_t LZCS,LZCR;

      GTEMatrix RT;

      struct {
        int32_t X,Y,Z;
      } TR;

      GTEMatrix LLM;

      struct {
        int32_t R,G,B;
      } BK;

      GTEMatrix LCM;
      
      struct {
        int32_t R,G,B;
      } FK;

      uint32_t OFX,OFY;
      uint16_t H,_unused10;
      int16_t DQA,_unused11;
      int32_t DQB;
      int16_t ZSF3,_unused12;
      int16_t ZSF4,_unused13;
      uint32_t FLAG;
    };
    uint32_t reg[64];
  };
};

class GTE : public Component {
 public:
  int Initialize();
  int Deinitialize();
  void ExecuteCommand(uint32_t code);
 private:
  GTEContext context_;
  uint8_t sf;
  void RTPS();
};

}
}

