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

// ai_textures_test: the AI texture upscaler's own work (graphics/upscale/texture/ai_textures.h,
// Docs/Texture-Upscaling-Plan.md, phase 3), with a model that only repeats pixels, so what comes
// out can be known exactly and no graphics card is needed - a tile made ready (holes filled from
// the nearest drawn texel, cells not read left out), its result put back together (coverage,
// semi-transparency, 2x), and the queue end to end: calls packed, images handed to the pack and
// written, and read back by the next run.

#include "graphics/upscale/texture/ai_textures.h"
#include "emucore/image/png_file.h"
#include "render/texture_pack.h"
#include "render/texture_tracker.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace {

using emulation::psx::PictureTexels;
using emulation::psx::ReplacementImage;
using emulation::psx::TexturePack;
using psxemu::AiTextures;

int failures = 0;
int checks = 0;

void Check(bool ok, const char* what) {
  ++checks;
  printf("  %-80s %s\n", what, ok ? "ok" : "FAILED");
  if (!ok)
    ++failures;
}

// A "model" that repeats each pixel four times each way.
class Repeat : public psxemu::TextureUpscaler {
 public:
  int calls = 0;
  int last_width = 0;
  int scale() const override { return 4; }
  std::string description() const override { return "repeat"; }
  bool Run(const std::vector<float>& rgb, int width, int height, std::vector<float>* out,
           int* out_width, int* out_height, std::string* error) override {
    (void)error;
    ++calls;
    last_width = width;
    *out_width = width * 4;
    *out_height = height * 4;
    out->resize(static_cast<size_t>(*out_width) * *out_height * 3);
    const size_t plane = static_cast<size_t>(width) * height;
    const size_t out_plane = static_cast<size_t>(*out_width) * *out_height;
    for (int c = 0; c < 3; ++c)
      for (int y = 0; y < *out_height; ++y)
        for (int x = 0; x < *out_width; ++x)
          (*out)[c * out_plane + static_cast<size_t>(y) * *out_width + x] =
              rgb[c * plane + static_cast<size_t>(y / 4) * width + x / 4];
    return true;
  }
};

// A picture source: `w` x `h` texels, every cell read.
std::shared_ptr<PictureTexels> Picture(uint32_t w, uint32_t h, const char* name) {
  auto p = std::make_shared<PictureTexels>();
  p->name = name;
  uint64_t image, clut;
  uint32_t mode;
  emulation::psx::TextureTracker::ParseName(name, &image, &clut, &mode);
  p->key = emulation::psx::TextureTracker::Key(image, clut, mode);
  p->width = w;
  p->height = h;
  p->rgba.assign(static_cast<size_t>(w) * h * 4, 0);
  p->cells_x = (w + 7) / 8;
  p->cells_y = (h + 7) / 8;
  p->cells.assign(static_cast<size_t>(p->cells_x) * p->cells_y, 1);
  return p;
}

void Set(PictureTexels& p, uint32_t x, uint32_t y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  uint8_t* q = &p.rgba[(static_cast<size_t>(y) * p.width + x) * 4];
  q[0] = r;
  q[1] = g;
  q[2] = b;
  q[3] = a;
}

void TestPrepare() {
  printf("a tile made ready\n");
  // 16x16: the left half red and opaque, the right half clear.
  auto p = Picture(16, 16, "0000000000000001-0000000000000001-15");
  for (uint32_t y = 0; y < 16; ++y)
    for (uint32_t x = 0; x < 8; ++x)
      Set(*p, x, y, 200, 0, 0, 255);
  const AiTextures::Prepared t = AiTextures::Prepare(*p, 0, 0, p->cells);
  const size_t n = static_cast<size_t>(AiTextures::kSide) * AiTextures::kSide;
  auto at = [&](int x, int y, int c) {
    return t.rgb[c * n + static_cast<size_t>(y + AiTextures::kPad) * AiTextures::kSide + x + AiTextures::kPad];
  };
  Check(!t.empty && t.cells.size() == 4, "a 16x16 source is one tile, giving its four cells");
  Check(t.coverage, "...with clear texels in what it gives, so its coverage goes through too");
  Check(at(12, 5, 0) > 0.78f && at(12, 5, 1) == 0.0f,
        "a clear texel takes the colour of the nearest drawn one, not black");
  Check(at(-10, 3, 0) > 0.78f && at(30, 30, 0) == at(15, 15, 0),
        "past the source's edge, its edge is repeated");
  Check(t.cover[static_cast<size_t>(AiTextures::kPad + 3) * AiTextures::kSide + AiTextures::kPad + 3] == 1.0f &&
            t.cover[static_cast<size_t>(AiTextures::kPad + 3) * AiTextures::kSide + AiTextures::kPad + 12] == 0.0f,
        "the coverage: 1 where drawn, 0 where clear");

  // A cell not read is not drawn, whatever its texels.
  auto q = Picture(16, 8, "0000000000000002-0000000000000001-15");
  for (uint32_t y = 0; y < 8; ++y)
    for (uint32_t x = 0; x < 16; ++x)
      Set(*q, x, y, x < 8 ? 10 : 250, 10, 10, 255);
  q->cells[1] = 0;
  std::vector<uint8_t> wanted = q->cells;
  const AiTextures::Prepared u = AiTextures::Prepare(*q, 0, 0, wanted);
  const size_t m = static_cast<size_t>(AiTextures::kSide) * AiTextures::kSide;
  Check(u.cells.size() == 1 &&
            u.rgb[static_cast<size_t>(AiTextures::kPad + 2) * AiTextures::kSide + AiTextures::kPad + 12] < 0.1f &&
            u.alpha[2 * AiTextures::kTile + 12] == 0,
        "a cell not read through this palette is taken as clear, and filled from what was");
  (void)m;

  auto empty = Picture(8, 8, "0000000000000003-0000000000000001-15");
  Check(AiTextures::Prepare(*empty, 0, 0, empty->cells).empty, "a tile with nothing drawn is empty");
}

void TestAssemble() {
  printf("a tile put back together\n");
  auto p = Picture(16, 16, "0000000000000004-0000000000000001-15");
  for (uint32_t y = 0; y < 16; ++y)
    for (uint32_t x = 0; x < 16; ++x)
      Set(*p, x, y, static_cast<uint8_t>(x * 16), static_cast<uint8_t>(y * 16), 7,
          (x >= 12 && y >= 12) ? 0 : (x < 4 ? 128 : 255));
  const AiTextures::Prepared t = AiTextures::Prepare(*p, 0, 0, p->cells);
  Repeat model;
  std::vector<float> rgb, cover;
  int w = 0, h = 0;
  std::string error;
  model.Run(t.rgb, AiTextures::kSide, AiTextures::kSide, &rgb, &w, &h, &error);
  model.Run(t.cover, AiTextures::kSide, AiTextures::kSide, &cover, &w, &h, &error);
  std::vector<std::pair<uint32_t, std::vector<uint8_t>>> blocks;
  AiTextures::Assemble(t, *p, rgb.data(), cover.data(), 4, 4, &blocks);
  Check(blocks.size() == 4, "every cell with anything drawn is a block");
  auto pixel = [&](uint32_t cell, int x, int y) -> const uint8_t* {
    for (const auto& [index, pixels] : blocks)
      if (index == cell)
        return &pixels[(static_cast<size_t>(y) * 32 + x) * 4];
    return nullptr;
  };
  const uint8_t* inside = pixel(0, 22, 9);   // texel (5, 2)
  Check(inside != nullptr && inside[0] == 80 && inside[1] == 32 && inside[2] == 7 && inside[3] == 255,
        "an opaque texel's pixels: its colour, drawn");
  const uint8_t* semi = pixel(0, 5, 5);      // texel (1, 1)
  Check(semi != nullptr && semi[3] == 128, "a semi-transparent texel's: alpha 128, as bit 15");
  const uint8_t* hole = pixel(3, 20, 20);    // texel (13, 13)
  const uint8_t* edge = pixel(3, 2, 20);     // texel (8, 13)
  Check(hole != nullptr && hole[3] == 0 && edge != nullptr && edge[3] == 255,
        "a clear texel's pixels are clear, a drawn one's beside it drawn");

  // At 2x: each pixel the mean of the model's two by two.
  std::vector<std::pair<uint32_t, std::vector<uint8_t>>> halves;
  AiTextures::Assemble(t, *p, rgb.data(), cover.data(), 4, 2, &halves);
  bool sixteen = !halves.empty();
  for (const auto& [index, pixels] : halves)
    sixteen = sixteen && pixels.size() == 16u * 16 * 4;
  Check(sixteen && halves[0].second[(static_cast<size_t>(4) * 16 + 10) * 4] == 80,
        "at 2x: blocks of 16x16, the model's 4x halved");
}

void TestQueue() {
  printf("the queue\n");
  namespace fs = std::filesystem;
  const fs::path folder = fs::temp_directory_path() / "psxemu_ai_textures_test";
  fs::remove_all(folder);
  const char* names[3] = { "00000000000000a1-0000000000000001-15", "00000000000000a2-0000000000000001-15",
                           "00000000000000a3-0000000000000001-4" };
  TexturePack pack(nullptr);
  std::string error;
  pack.Open("", &error, (folder / "ai").string());
  AiTextures::Options options;
  options.output = (folder / "ai").string();
  options.share = 1.0;
  auto backend = std::make_unique<Repeat>();
  Repeat* model = backend.get();
  {
    AiTextures ai(std::move(backend), options);
    ai.set_pack(&pack);
    for (const char* name : names) {
      auto p = Picture(24, 16, name);
      for (uint32_t y = 0; y < 16; ++y)
        for (uint32_t x = 0; x < 24; ++x)
          Set(*p, x, y, 100, 150, 200, 255);
      ai.Read(p);
    }
    ai.Drain();
    const AiTextures::Counts c = ai.counts();
    Check(c.tiles == 3 && c.calls == 1 && model->last_width == AiTextures::kGrid * AiTextures::kSide,
          "three sources' tiles go through in one call, nine slots wide");
    Check(c.published == 3 && pack.counts().provided == 3, "...and three images are handed to the pack");
    bool written = true;
    for (const char* name : names)
      written = written && fs::exists(folder / "ai" / (std::string(name) + ".png"));
    Check(written, "...and written to the output folder");
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;
    Check(emulation::image::ReadPngFile((folder / "ai" / (std::string(names[0]) + ".png")).string(), &w, &h, &rgba) &&
              w == 96 && h == 64 && rgba[3] == 255 && rgba[0] == 100,
          "...at 4x, as the model made them");

    // The same source read again, nothing more read: nothing to do.
    auto again = Picture(24, 16, names[0]);
    for (uint32_t y = 0; y < 16; ++y)
      for (uint32_t x = 0; x < 24; ++x)
        Set(*again, x, y, 100, 150, 200, 255);
    ai.Read(again);
    ai.Drain();
    Check(ai.counts().calls == 1, "a source read again with no more cells read costs nothing");
  }
  // The next run: what was done is read back from the folder, and not done again.
  auto second = std::make_unique<Repeat>();
  Repeat* model2 = second.get();
  AiTextures ai(std::move(second), options);
  auto p = Picture(24, 16, names[1]);
  for (uint32_t y = 0; y < 16; ++y)
    for (uint32_t x = 0; x < 24; ++x)
      Set(*p, x, y, 100, 150, 200, 255);
  ai.Read(p);
  ai.Drain();
  Check(model2->calls == 0, "the next run reads back what was done, and calls nothing");

  // A texture the pack has its own image for is the pack's.
  fs::create_directories(folder / "replace");
  std::vector<uint8_t> own(24 * 16 * 4, 255);
  emulation::image::WritePngFile((folder / "replace" / (std::string(names[2]) + ".png")).string(), 24, 16, own.data());
  TexturePack hand(nullptr);
  hand.Open((folder / "replace").string(), &error, (folder / "ai").string());
  AiTextures fresh(std::make_unique<Repeat>(), options);
  fresh.set_pack(&hand);
  auto mine = Picture(24, 16, names[2]);
  for (uint32_t y = 0; y < 16; ++y)
    for (uint32_t x = 0; x < 24; ++x)
      Set(*mine, x, y, 1, 2, 3, 255);
  fresh.Read(mine);
  fresh.Drain();
  Check(fresh.counts().calls == 0 && fresh.counts().published == 0,
        "a texture with an image of the pack's own is not upscaled");
  fresh.set_pack(nullptr);
  fs::remove_all(folder);
}

}  // namespace

int main() {
  printf("AI textures\n");
  TestPrepare();
  TestAssemble();
  TestQueue();
  printf("\n%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
