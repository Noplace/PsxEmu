#include "../system.h"

namespace emulation {
namespace psx {

Spu::Spu() {

}

Spu::~Spu() {

}

int Spu::Initialize() {
  sound_buffer_.Alloc(512*1024);
  memset(&voices,0,sizeof(voices));
  spu_control.raw = 0;
  spu_status2.raw = 0;
  cd_vol_left.raw = 0;
  cd_vol_right.raw = 0;
  external_vol_left.raw = 0;
  external_vol_right.raw = 0;
  spu_control2 = 0;
  voice_on1 = 0;
  voice_on2 = 0;
  voice_off1 = 0;
  voice_off2 = 0;
  channel_fm_mode1 = 0;
  channel_fm_mode2 = 0;
  noise_mode1 = 0;
  noise_mode2 = 0;
  reverb_mode1 = 0;
  reverb_mode2 = 0;
  main_volume_left = 0;
  main_volume_right = 0;
  reverb_depth_left = 0;
  reverb_depth_right = 0;
  reverb_workarea_start = 0;
  soundbuffer_irq_address1 = 0;
  soundbuffer_irq_address2 = 0;
  spu_data = 0;
  return 0;
}

int Spu::Deinitialize() {
  sound_buffer_.Dealloc();
  return 0;
}

uint16_t  Spu::Read(uint32_t address) {

  if (address >= 0x1F801C00 && address <= 0x1F801D7F) {
    int voice_index = ((address & 0xFF0) >> 4) - 0xC0;
    auto& voice = voices[voice_index];
    switch (address & 0xF ) {
      case 0x0: return voice.vol_left.raw;
      case 0x2: return voice.vol_right.raw;
      case 0x4: return voice.pitch.raw;
      case 0x6: return voice.start_address;
      case 0x8: return voice.ads_levels.raw;
      case 0xA: return voice.sr_rates.raw;
      case 0xC: return voice.current_adsr_volume;
      case 0xE: return voice.repeat_address;
    }
  }
  switch (address) {
    case 0x1F801D80: return main_volume_left;
    case 0x1F801D82: return main_volume_right;
    case 0x1F801D84: return reverb_depth_left;
    case 0x1F801D86: return reverb_depth_right;
    case 0x1F801D88: return voice_on1;
    case 0x1F801D8A: return voice_on2;
    case 0x1F801D8C: return voice_off1;
    case 0x1F801D8E: return voice_off2;
    case 0x1F801D90: return channel_fm_mode1;
    case 0x1F801D92: return channel_fm_mode2;
    case 0x1F801D94: return noise_mode1;
    case 0x1F801D96: return noise_mode2;
    case 0x1F801D98: return reverb_mode1;
    case 0x1F801D9A: return reverb_mode2;
    case 0x1F801DAA: return spu_control.raw;
    case 0x1F801DAC: return spu_control2;
    case 0x1F801DAE: return spu_status2.raw;
  }
  throw;
}

void Spu::Write(uint32_t address,uint16_t data) {

  if (address >= 0x1F801C00 && address <= 0x1F801D7F) {
    int voice_index = ((address & 0xFF0) >> 4) - 0xC0;
    auto& voice = voices[voice_index];
    switch (address & 0xF ) {
      case 0x0:voice.vol_left.raw = data; return;
      case 0x2:voice.vol_right.raw = data; return;
      case 0x4:voice.pitch.raw = data; return;
      case 0x6:voice.start_address = data; return;
      case 0x8:voice.ads_levels.raw = data; return;
      case 0xA:voice.sr_rates.raw = data; return;
      case 0xC:voice.current_adsr_volume = data; return;
      case 0xE:voice.repeat_address = data; return;
    }
  }

  switch (address) {
    case 0x1F801D80: main_volume_left = data; return;
    case 0x1F801D82: main_volume_right = data; return;
    case 0x1F801D84: reverb_depth_left = data; return;
    case 0x1F801D86: reverb_depth_right = data; return;
      //voice on
    case 0x1F801D88: 
      voice_on1 = data;
      for (int i=0;i<16;++i) {
        voices[i].voice_on = (data & (1 << i)) == 1;
      }
      return;
    case 0x1F801D8A: 
      voice_on2 = data;
      for (int i=0;i<8;++i) {
        voices[16+i].voice_on = (data & (1 << i)) == 1;
      }
      return;
      //voice off
    case 0x1F801D8C: 
      voice_off1 = data;
      for (int i=0;i<16;++i) {
        voices[i].voice_on = (data & (1 << i)) == 0;
      }
      return;
    case 0x1F801D8E: 
      voice_off2 = data;
      for (int i=0;i<8;++i) {
        voices[16+i].voice_on = (data & (1 << i)) == 0;
      }
      return;
      //channel fm
    case 0x1F801D90: 
      channel_fm_mode1 = data;
      for (int i=0;i<16;++i) {
        voices[i].fm = (data & (1 << i)) == 1;
      }
      return;
    case 0x1F801D92: 
      channel_fm_mode2 = data;
      for (int i=0;i<8;++i) {
        voices[16+i].fm = (data & (1 << i)) == 1;
      }
      return;
      //noise
    case 0x1F801D94: 
      noise_mode1 = data;
      for (int i=0;i<16;++i) {
        voices[i].noise = (data & (1 << i)) == 1;
      }
      return;
    case 0x1F801D96: 
      noise_mode2 = data;
      for (int i=0;i<8;++i) {
        voices[16+i].noise = (data & (1 << i)) == 1;
      }
      return;
      //reverb
    case 0x1F801D98: 
      reverb_mode1 = data;
      for (int i=0;i<16;++i) {
        voices[i].reverb = (data & (1 << i)) == 1;
      }
      return;
    case 0x1F801D9A: 
      reverb_mode2 = data;
      for (int i=0;i<8;++i) {
        voices[16+i].reverb = (data & (1 << i)) == 1;
      }
      return;
    case 0x1F801DA2: reverb_workarea_start = data; return;
    case 0x1F801DA4: soundbuffer_irq_address1 = data; return;
    case 0x1F801DA6: soundbuffer_irq_address2 = data; return;
    case 0x1F801DA8: spu_data = data; return;
    case 0x1F801DAA: spu_control.raw = data; return;
    case 0x1F801DAC: spu_control2 = data; return;
    case 0x1F801DB0: cd_vol_left.raw = data; return;
    case 0x1F801DB2: cd_vol_right.raw = data; return;
    case 0x1F801DB4: external_vol_left.raw = data; return;
    case 0x1F801DB6: external_vol_right.raw = data; return;
  }

  if (address >= 0x1F801DC0 && address <= 0X1F801DFE) {
    effects[((address&0xFF)>>1)-0x60] = data;
    return;
  }


  throw;
}

}
}