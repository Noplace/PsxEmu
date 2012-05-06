#include <VisualEssence/Code/ve.h>
#include "../system.h"

namespace emulation {
namespace psx {

struct GfxStruct {
  graphics::ContextD3D9* gfx;
  graphics::Camera camera;
  graphics::InputLayout input_layout;
};

Gpu::Gpu() {
  gs = new GfxStruct();
}

Gpu::~Gpu() {
  delete gs;
}

void Gpu::set_gfx(graphics::Context* gfx) {
  this->gs->gfx = (graphics::ContextD3D9*)gfx;
}

int Gpu::Initialize() {
  graphics::InputElement gielements[] = 
  {
      {0, 0 , D3DDECLTYPE_FLOAT3  , D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
      {0, 12, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR   , 0},
      {0, 16, D3DDECLTYPE_FLOAT2  , D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
      D3DDECL_END()
  };

  gs->gfx->CreateInputLayout(gielements,gs->input_layout);
  gs->gfx->SetInputLayout(gs->input_layout);
  gs->camera.Initialize(gs->gfx);
  gs->camera.Ortho2D();
  gs->gfx->device()->SetTransform(D3DTS_VIEW,(D3DMATRIX*)&gs->camera.view());
  gs->gfx->device()->SetTransform(D3DTS_PROJECTION,(D3DMATRIX*)&gs->camera.projection());

  return 0;
}

int Gpu::Deinitialize() {
  gs->gfx->DestoryInputLayout(gs->input_layout);
  return 0;
}

uint32_t Gpu::Read(uint32_t address) {
  throw;
  return 0;
}

void Gpu::Write(uint32_t address,uint32_t data) {
  throw;
}

}
}