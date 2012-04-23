#include "../system.h"

namespace emulation {
namespace psx {

Spu::Spu() {

}

Spu::~Spu() {

}

void Spu::Initialize() {

}

uint16_t  Spu::Read(uint32_t address) {
  throw;
}

void Spu::Write(uint32_t address,uint16_t data) {
  if (address == 0x1F801D80) {
    main_volume_left = data;
    return;
  }

  if (address == 0x1F801D82) {
    main_volume_right = data;
    return;
  }

  if (address == 0x1F801D84) {
    reverb_depth_left = data;
    return;
  }

  if (address == 0x1F801D86) {
    reverb_depth_right = data;
    return;
  }


  throw;
}

}
}