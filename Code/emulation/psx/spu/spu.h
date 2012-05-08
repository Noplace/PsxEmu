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
#ifndef EMULATION_PSX_SPU_H
#define EMULATION_PSX_SPU_H

namespace emulation {
namespace psx {

class Spu : public Component {
 public:
  Spu();
  ~Spu();
  int Initialize();
  int Deinitialize();
  uint16_t  Read(uint32_t address);
  void Write(uint32_t address,uint16_t data);
 private:
  Buffer sound_buffer_;
  uint16_t effects[32];
  struct {
    bool voice_on,fm,noise,reverb;
    struct {
      union {
        struct {
          uint16_t volume:14;
          uint16_t phase:1;
          uint16_t unused:1;
        }volume;
        struct {
          uint16_t volume:7;
          uint16_t unused1:5;
          uint16_t phase:1;
          uint16_t decay:1;
          uint16_t slope:1;
          uint16_t unused2:1;
        }sweep;
        uint16_t raw;
      };
    } vol_left, vol_right; 
    union {
      struct {
        uint16_t shift:14;
        uint16_t unused:2;
      };
      uint16_t raw;
    } pitch;
    uint16_t start_address;
    union {
      struct {
        uint16_t sustain:4;
        uint16_t decay:4;
        uint16_t attack:7;
        uint16_t mode:1;
      };
      uint16_t raw;
    } ads_levels;
    union {
      struct {
        uint16_t release_rate:5;
        uint16_t decrease_mode:1;
        uint16_t sustain_rate:7;
        uint16_t unused:1;
        uint16_t sustain_rate_mode_incdec:1;
        uint16_t sustain_rate_mode_linexp:1;
      };
      uint16_t raw;
    } sr_rates;
    uint16_t current_adsr_volume;
    uint16_t repeat_address;
  } voices[24];
  union {
    struct {
      uint16_t cd_audio:1;
      uint16_t external_audio:1;
      uint16_t cd_reverb:1;
      uint16_t external_reverb:1;
      uint16_t dma:2;
      uint16_t irq:1;
      uint16_t reverb:1;
      uint16_t noise_frequency:6;
      uint16_t spu_unmute:1;
      uint16_t spu_on:1;
    };
    uint16_t raw;
  }spu_control;
  union {
    struct {
      uint16_t volume:15;
      uint16_t phase:1;
    }volume;
    uint16_t raw;
  }cd_vol_left, cd_vol_right,external_vol_left,external_vol_right; 
  union {
    struct {
      uint16_t unused1:10;
      uint16_t spu_not_ready:1;
      uint16_t decoding:1;
      uint16_t unused2:4;
    };
    uint16_t raw;
  }spu_status2;
  uint16_t main_volume_left;
  uint16_t main_volume_right;
  uint16_t reverb_depth_left;
  uint16_t reverb_depth_right;
  uint16_t reverb_workarea_start;
  uint16_t soundbuffer_irq_address1;
  uint16_t soundbuffer_irq_address2;
  uint16_t spu_data;
  uint16_t spu_control2;
  uint16_t voice_on1,voice_on2;
  uint16_t voice_off1,voice_off2;
  uint16_t channel_fm_mode1,channel_fm_mode2;
  uint16_t noise_mode1,noise_mode2;
  uint16_t reverb_mode1,reverb_mode2;

};

}
}

#endif