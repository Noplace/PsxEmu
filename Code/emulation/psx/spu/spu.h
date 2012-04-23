/******************************************************************************
* Copyright Khalid Al-Kooheji 2010
* Filename    : spu.h
* Description : 
* 
*
* 
* 
* 
*******************************************************************************/
#ifndef EMULATION_PSX_SPU_H
#define EMULATION_PSX_SPU_H

namespace emulation {
namespace psx {

class Spu : public Component {
 public:
  Spu();
  ~Spu();
  void Initialize();
  uint16_t  Read(uint32_t address);
  void Write(uint32_t address,uint16_t data);
 private:
  uint8_t sound_buffer_[512*1024];
  int main_volume_left;
  int main_volume_right;
  int reverb_depth_left;
  int reverb_depth_right;
  struct {
    int vol_left;
    int vol_right;
  }voices[24];
};

}
}

#endif