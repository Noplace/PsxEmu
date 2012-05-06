#ifndef EMULATION_PSX_GPU_H
#define EMULATION_PSX_GPU_H


//forward declare
namespace graphics {
class Context;
class ContextD3D9;
}

namespace emulation {
namespace psx {

struct GfxStruct;

class Gpu : public Component {
 public:
  Gpu();
  ~Gpu();
  void set_gfx(graphics::Context* gfx);
  int Initialize();
  int Deinitialize();
  uint32_t  Read(uint32_t address);
  void Write(uint32_t address,uint32_t data);
 private:
  GfxStruct* gs;
};

}
}

#endif