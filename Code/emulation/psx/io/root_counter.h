#ifndef EMULATION_PSX_ROOT_COUNTER_H
#define EMULATION_PSX_ROOT_COUNTER_H

namespace emulation {
namespace psx {

class RootCounter {
 public:
  uint32_t counter;
  uint32_t target;
  union {
    struct {
      unsigned en:1;
      unsigned _unused1:2;
      unsigned tar:1;
      unsigned irq1:1;
      unsigned _unused2:1;
      unsigned irq2:1;
      unsigned _unused3:1;
      unsigned clc:1;
      unsigned div:1;
      unsigned _unused4:21;
    };
    uint32_t raw;
  }mode;

  uint32_t ReadCounter() {
    return counter;
  }

  uint32_t ReadMode() {
    return this->mode.raw;
  }

  uint32_t ReadTarget() {
    return target;
  }

  void WriteMode(uint32_t mode) {
    this->mode.raw = mode;
  }

  void WriteTarget(uint32_t target) {
    this->target = target & 0xFFFF;
  }

  void Tick() {
    if (mode.en == 0) {
      ++counter;
      auto limit = mode.tar == 0? 0xffff:target;
      
      if (counter >= limit) {
        //boom
      }
    }
  }

};

}
}

#endif