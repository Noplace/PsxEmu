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

// texture_tracker_test: the texture tracker's rules (render/texture_tracker.h,
// Docs/Texture-Upscaling-Plan.md, phase 1) on a VRAM of its own - which upload a draw names, the
// name the same wherever the image is, strips joined, patches and partial uploads, every reason a
// draw names nothing, and a dumped PNG read back texel by texel.

#include "render/png_file.h"
#include "render/texture_tracker.h"

#include "zlib.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace {

using emulation::psx::DrawJob;
using emulation::psx::TextureTracker;

int failures = 0;
int checks = 0;

void Check(bool ok, const char* what) {
  ++checks;
  printf("  %-80s %s\n", what, ok ? "ok" : "FAILED");
  if (!ok)
    ++failures;
}

struct Vram {
  std::vector<uint16_t> words = std::vector<uint16_t>(1024 * 512, 0);
  uint16_t& at(uint32_t x, uint32_t y) { return words[(y & 511) * 1024 + (x & 1023)]; }
};

// The test image: `w` x `h` halfwords whose values depend only on where in the image they are and
// `seed` - so the same image can be put anywhere.
uint16_t Texel(uint32_t col, uint32_t row, uint32_t seed) {
  return static_cast<uint16_t>(((col * 7 + row * 13 + seed * 101) * 2654435761u) >> 16) | 1;
}

void Upload(Vram& vram, TextureTracker& tracker, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
            uint32_t seed, uint32_t col0 = 0, uint32_t row0 = 0) {
  for (uint32_t row = 0; row < h; ++row)
    for (uint32_t col = 0; col < w; ++col)
      vram.at(x + col, y + row) = Texel(col0 + col, row0 + row, seed);
  tracker.Written(x, y, w, h);
}

void UploadClut(Vram& vram, TextureTracker& tracker, uint32_t x, uint32_t y, uint32_t entries) {
  for (uint32_t i = 0; i < entries; ++i)
    vram.at(x + i, y) = static_cast<uint16_t>(i == 0 ? 0 : (i * 1057) | (i == 3 ? 0x8000 : 0));
  tracker.Written(x, y, entries, 1);
}

DrawJob Sprite(uint32_t page_x, uint32_t page_y, uint32_t mode, uint32_t clut_x, uint32_t clut_y,
               int32_t x, int32_t y, int32_t w, int32_t h, uint8_t u, uint8_t v) {
  DrawJob job = {};
  job.kind = DrawJob::kRectangle;
  job.env.area_left = 0;
  job.env.area_top = 0;
  job.env.area_right = 639;
  job.env.area_bottom = 479;
  job.state.textured = true;
  job.state.texpage_x = page_x;
  job.state.texpage_y = page_y;
  job.state.texpage_colors = mode;
  job.state.clut_x = clut_x;
  job.state.clut_y = clut_y;
  job.x = x;
  job.y = y;
  job.w = w;
  job.h = h;
  job.base_u = u;
  job.base_v = v;
  return job;
}

TextureTracker::Reason Classify(const TextureTracker& tracker, const DrawJob& job,
                                std::string* name = nullptr) {
  return tracker.Classify(job, name);
}

void Draw(TextureTracker& tracker, const DrawJob& job, uint64_t pixels) {
  tracker.Drawing(job);
  tracker.Drawn(job, pixels);
}

// A 4-bit image of 16x32 halfwords - 64x32 texels - and its palette.
const uint32_t kW = 16, kH = 32;

void TestNaming() {
  printf("naming\n");
  Vram vram;
  TextureTracker tracker(vram.words.data());
  Upload(vram, tracker, 640, 0, kW, kH, 1);
  UploadClut(vram, tracker, 0, 480, 16);
  std::string name;
  const DrawJob sprite = Sprite(640, 0, 0, 0, 480, 10, 10, 16, 16, 0, 0);
  Check(Classify(tracker, sprite, &name) == TextureTracker::kNamed,
        "a sprite reading a 4-bit upload through an uploaded palette names it");
  Check(name.size() == 16 + 1 + 16 + 2 && name.substr(name.size() - 2) == "-4",
        "its name: the upload's hash, the palette's, and the mode");

  Vram other;
  TextureTracker elsewhere(other.words.data());
  Upload(other, elsewhere, 704, 256, kW, kH, 1);
  UploadClut(other, elsewhere, 32, 490, 16);
  std::string there;
  Classify(elsewhere, Sprite(704, 256, 0, 32, 490, 10, 10, 16, 16, 0, 0), &there);
  Check(there == name, "the same image and palette elsewhere in VRAM have the same name");

  Vram tinted;
  TextureTracker paletted(tinted.words.data());
  Upload(tinted, paletted, 640, 0, kW, kH, 1);
  UploadClut(tinted, paletted, 0, 480, 16);
  tinted.at(5, 480) ^= 0x1F;
  paletted.Written(5, 480, 1, 1);
  std::string recoloured;
  Classify(paletted, Sprite(640, 0, 0, 0, 480, 10, 10, 16, 16, 0, 0), &recoloured);
  Check(!recoloured.empty() && recoloured != name, "...and with another palette, another name");

  DrawJob triangle = sprite;
  triangle.kind = DrawJob::kTriangle;
  triangle.v[0].u = 0, triangle.v[0].v = 0;
  triangle.v[1].u = 63, triangle.v[1].v = 0;
  triangle.v[2].u = 0, triangle.v[2].v = 31;
  triangle.v[0].x = 0, triangle.v[1].x = 64, triangle.v[2].x = 0;
  triangle.v[0].y = 0, triangle.v[1].y = 0, triangle.v[2].y = 32;
  Check(Classify(tracker, triangle) == TextureTracker::kNamed,
        "a triangle whose texture coordinates stay inside the upload names it");
  triangle.v[1].u = 64;
  Check(Classify(tracker, triangle) == TextureTracker::kNamed,
        "...one whose far edge is a texel past it still names it: that edge is not read");
  // Past the image, VRAM a state load filled: not zeros, and nobody's upload.
  Vram loaded;
  TextureTracker after_load(loaded.words.data());
  for (uint32_t y = 0; y < 64; ++y)
    for (uint32_t x = 640; x < 704; ++x)
      loaded.at(x, y) = 0x1234;
  after_load.Reloaded();
  Upload(loaded, after_load, 640, 0, kW, kH, 1);
  UploadClut(loaded, after_load, 0, 480, 16);
  triangle.v[1].u = 66;
  Check(Classify(after_load, triangle) == TextureTracker::kSpans,
        "...one reaching further, into VRAM a state load filled, does not: only partly uploaded");

  Vram direct;
  TextureTracker fifteen(direct.words.data());
  Upload(direct, fifteen, 512, 0, 32, 32, 3);
  std::string fifteen_name;
  Check(Classify(fifteen, Sprite(512, 0, 2, 0, 0, 0, 0, 32, 32, 0, 0), &fifteen_name) ==
                TextureTracker::kNamed &&
            fifteen_name.find("-0000000000000000-15") != std::string::npos,
        "a 15-bit texture needs no palette, and its name says so");
}

void TestStrips() {
  printf("strips, patches and partial uploads\n");
  Vram whole;
  TextureTracker one(whole.words.data());
  Upload(whole, one, 640, 0, kW, kH, 1);
  UploadClut(whole, one, 0, 480, 16);
  std::string name;
  const DrawJob all = Sprite(640, 0, 0, 0, 480, 0, 0, 64, 32, 0, 0);
  Classify(one, all, &name);

  Vram strips;
  TextureTracker joined(strips.words.data());
  UploadClut(strips, joined, 0, 480, 16);
  Upload(strips, joined, 640, 0, kW, 16, 1);
  Upload(strips, joined, 640, 16, kW, 16, 1, 0, 16);
  std::string strip_name;
  Check(Classify(joined, all, &strip_name) == TextureTracker::kNamed && strip_name == name,
        "two strips one after the other are one image, named as if uploaded whole");

  Vram rows;
  TextureTracker upwards(rows.words.data());
  UploadClut(rows, upwards, 0, 480, 16);
  for (int row = static_cast<int>(kH) - 1; row >= 0; --row)
    Upload(rows, upwards, 640, static_cast<uint32_t>(row), kW, 1, 1, 0, static_cast<uint32_t>(row));
  std::string rows_name;
  Check(Classify(upwards, all, &rows_name) == TextureTracker::kNamed && rows_name == name,
        "...and rows one at a time from the bottom up, too (Ridge Racer)");

  Vram apart;
  TextureTracker parted(apart.words.data());
  for (uint32_t y = 0; y < 64; ++y)   // what a state load left around them: not zeros
    for (uint32_t x = 640; x < 704; ++x)
      apart.at(x, y) = 0x1234;
  parted.Reloaded();
  UploadClut(apart, parted, 0, 480, 16);
  Upload(apart, parted, 640, 0, kW, 16, 1);
  Upload(apart, parted, 800, 0, 8, 8, 9);
  Upload(apart, parted, 640, 16, kW, 16, 1, 0, 16);
  std::string tiles_name;
  Check(Classify(parted, all, &tiles_name) == TextureTracker::kNamed && tiles_name != name,
        "strips with another upload between them stay two, and a draw over both names them "
        "together: a composite (Castlevania)");
  Check(Classify(parted, Sprite(640, 0, 0, 0, 480, 0, 0, 64, 40, 0, 0)) ==
            TextureTracker::kSpans,
        "...but not one reading past them: only partly uploaded");

  Vram patch;
  TextureTracker patched(patch.words.data());
  Upload(patch, patched, 640, 0, kW, kH, 1);
  UploadClut(patch, patched, 0, 480, 16);
  Upload(patch, patched, 644, 4, 2, 4, 7);
  std::string patched_name;
  Check(Classify(patched, all, &patched_name) == TextureTracker::kNamed && patched_name != name,
        "an upload inside one patches it: still named, by what it holds now");

  Vram part;
  TextureTracker trimmed(part.words.data());
  Upload(part, trimmed, 640, 0, kW, kH, 1);
  UploadClut(part, trimmed, 0, 480, 16);
  Upload(part, trimmed, 640, 16, 32, 32, 5);
  Check(Classify(trimmed, Sprite(640, 0, 0, 0, 480, 0, 0, 64, 16, 0, 0)) ==
            TextureTracker::kNamed,
        "one uploaded over its bottom rows keeps its top rows named");
  Check(Classify(trimmed, Sprite(640, 0, 0, 0, 480, 0, 0, 64, 32, 0, 0)) ==
            TextureTracker::kNamed,
        "...and a draw reading old rows and new names the two together");
  Check(trimmed.counts().records_cut == 1, "...counted as cut");

  // Gran Turismo 2: all of VRAM at once, then the framebuffer drawn into it.
  Vram all_vram;
  TextureTracker everything(all_vram.words.data());
  for (uint32_t row = 0; row < 512; row += 16)
    Upload(all_vram, everything, 0, row, 1024, 16, 2, 0, row);
  DrawJob framebuffer = Sprite(0, 0, 2, 0, 0, 0, 0, 320, 240, 0, 0);
  framebuffer.state.textured = false;
  Draw(everything, framebuffer, 320 * 240);
  Check(everything.counts().uploads_joined == 31 && everything.counts().records_cut == 1,
        "a VRAM loaded whole in strips is one record, and the framebuffer cuts it");
  Check(Classify(everything, Sprite(640, 0, 2, 0, 0, 0, 0, 32, 32, 0, 0)) ==
            TextureTracker::kNamed,
        "...leaving what it did not touch named");
  Check(Classify(everything, Sprite(0, 0, 2, 0, 0, 0, 0, 32, 32, 0, 0)) ==
            TextureTracker::kTextureDrawn,
        "...and what it drew the GPU's");
}

void TestReasons() {
  printf("why not\n");
  Vram vram;
  TextureTracker tracker(vram.words.data());
  Upload(vram, tracker, 640, 0, kW, kH, 1);
  UploadClut(vram, tracker, 0, 480, 16);
  const DrawJob sprite = Sprite(640, 0, 0, 0, 480, 10, 10, 16, 16, 0, 0);

  Check(Classify(tracker, Sprite(640, 0, 0, 64, 480, 10, 10, 16, 16, 0, 0)) ==
            TextureTracker::kClutNotUploaded,
        "a palette nobody uploaded");
  UploadClut(vram, tracker, 960, 500, 64);
  Check(Classify(tracker, Sprite(640, 0, 1, 960, 500, 10, 10, 16, 16, 0, 0)) ==
            TextureTracker::kNamed,
        "an 8-bit palette running past VRAM's edge is read as far as the edge (Metal Gear Solid)");
  Check(Classify(tracker, Sprite(704, 0, 0, 0, 480, 10, 10, 16, 16, 0, 0)) ==
            TextureTracker::kNotUploaded,
        "texels nobody uploaded");
  DrawJob off = sprite;
  off.x = 700;
  Check(Classify(tracker, off) == TextureTracker::kOutside,
        "a sprite wholly outside the drawing area reads nothing");
  Check(Classify(tracker, Sprite(640, 0, 0, 0, 480, 10, 10, 16, 16, 250, 0)) ==
            TextureTracker::kWraps,
        "one whose texels run past the page's 256 wraps");

  // VRAM nothing has written is zeros - clear texels - and a draw may sweep over it.
  DrawJob wide = Sprite(640, 0, 0, 0, 480, 0, 0, 256, 8, 0, 0);
  Check(Classify(tracker, wide) == TextureTracker::kNamed,
        "a sprite reading past an image into VRAM never written still names it (Einhander)");

  // The texture window: a 256-texel sprite whose window keeps it inside eight texels.
  DrawJob scribble = Sprite(0, 0, 2, 0, 0, 656, 0, 48, 8, 0, 0);
  scribble.state.textured = false;
  scribble.env.area_right = 1023;
  scribble.r = scribble.g = scribble.b = 255;   // white, as it puts down
  for (uint32_t y = 0; y < 8; ++y)   // what it puts down
    for (uint32_t x = 656; x < 704; ++x)
      vram.at(x, y) = 0x7FFF;
  Draw(tracker, scribble, 48 * 8);
  DrawJob windowed = wide;
  Check(Classify(tracker, windowed) == TextureTracker::kTextureDrawn,
        "a 256-texel sprite reads past a 64-texel image into what the GPU drew...");
  windowed.env.tw_mask_x = 0x1F;
  windowed.env.tw_offset_x = 2;
  Check(Classify(tracker, windowed) == TextureTracker::kNamed,
        "...unless its texture window keeps it to eight texels inside");

  DrawJob fill = {};
  fill.kind = DrawJob::kFill;
  fill.x = 0;
  fill.y = 480;
  fill.w = 16;
  fill.h = 1;
  Draw(tracker, fill, 16);
  Check(Classify(tracker, sprite) == TextureTracker::kClutDrawn,
        "once the GPU draws over its palette, the palette is the GPU's");
  UploadClut(vram, tracker, 0, 480, 16);
  Check(Classify(tracker, sprite) == TextureTracker::kNamed, "...until it is uploaded again");

  DrawJob over = Sprite(0, 0, 2, 0, 0, 640, 0, 4, 4, 0, 0);
  over.state.textured = false;
  over.env.area_right = 1023;
  over.r = 200;   // not black, which would leave clear texels
  Draw(tracker, over, 16);
  Check(Classify(tracker, sprite) == TextureTracker::kTextureDrawn,
        "a sprite drawn into the image: the texels it drew over are the GPU's");
  Check(tracker.counts().records_retired == 1 && tracker.counts().records_cut == 1,
        "...the image cut, the palette under the fill gone whole");

  DrawJob nothing = over;
  nothing.x = 656;
  nothing.env.area_left = 700;   // wholly outside the drawing area
  Upload(vram, tracker, 640, 0, kW, kH, 1);
  Draw(tracker, nothing, 0);
  Check(Classify(tracker, sprite) == TextureTracker::kNamed,
        "a draw that can put nothing down retires nothing");
  DrawJob counted_none = over;
  counted_none.x = 640;   // over the texels the sprite reads
  Draw(tracker, counted_none, 0);
  Check(Classify(tracker, sprite) == TextureTracker::kTextureDrawn,
        "...but one that can is a draw, whatever pixels the rasteriser counted (a hardware one counts none)");
  Upload(vram, tracker, 640, 0, kW, kH, 1);

  // A palette copied into place by the GPU: what it copied was uploaded, so it is named too.
  DrawJob copy = {};
  copy.kind = DrawJob::kVramCopy;
  copy.src_x = 0;
  copy.src_y = 480;
  copy.x = 0;
  copy.y = 0;
  copy.w = 16;
  copy.h = 1;
  tracker.Drawing(copy);
  for (int i = 0; i < 16; ++i)
    vram.at(i, 0) = vram.at(i, 480);
  tracker.Drawn(copy, 16);
  Check(Classify(tracker, Sprite(640, 0, 0, 0, 0, 10, 10, 16, 16, 0, 0)) ==
                TextureTracker::kNamed &&
            tracker.counts().records_copied == 1,
        "a palette copied from an uploaded one names it, as Silent Hill's are");
  DrawJob copy_drawn = copy;
  copy_drawn.src_x = 660;   // where the scribble above drew
  copy_drawn.src_y = 0;
  copy_drawn.y = 1;
  tracker.Drawing(copy_drawn);
  tracker.Drawn(copy_drawn, 16);
  Check(tracker.counts().records_copied == 1, "...but a copy of what the GPU drew is not recorded");

  // A hardware rasteriser copies on the card and leaves native VRAM as it was: the copy names the
  // same all the same, from the texels its record keeps.
  for (int i = 0; i < 16; ++i)
    vram.at(32 + i, 0) = 0x1234;
  DrawJob card_copy = copy;
  card_copy.x = 32;
  Draw(tracker, card_copy, 16);
  std::string by_software, by_card;
  Classify(tracker, Sprite(640, 0, 0, 0, 0, 10, 10, 16, 16, 0, 0), &by_software);
  Classify(tracker, Sprite(640, 0, 0, 32, 0, 10, 10, 16, 16, 0, 0), &by_card);
  Check(!by_card.empty() && by_card == by_software,
        "...and names the same where only the card has the copy");

  // What no record covers is clear where native VRAM holds zeros - or, where the GPU drew, where
  // a fill of zeros drew last, whatever native VRAM still says.
  {
    Vram v2;
    TextureTracker t2(v2.words.data());
    Upload(v2, t2, 64, 0, 16, 32, 3);
    const DrawJob wide15 = Sprite(64, 0, 2, 0, 0, 10, 10, 32, 8, 0, 0);
    Check(Classify(t2, wide15) == TextureTracker::kNamed,
          "15-bit texels beside an image in VRAM nothing wrote are clear");
    for (uint32_t y = 0; y < 32; ++y)
      for (uint32_t x = 80; x < 96; ++x)
        v2.at(x, y) = 0x4321;   // on the card, cleared below; native VRAM never told
    DrawJob zero = {};
    zero.kind = DrawJob::kFill;
    zero.x = 80;
    zero.w = 16;
    zero.h = 32;
    Draw(t2, zero, 16 * 32);
    Check(Classify(t2, wide15) == TextureTracker::kNamed,
          "...and beside it where a fill of zeros drew, whatever native VRAM says");
    DrawJob grey = zero;
    grey.fill_colour = 0x4210;
    Draw(t2, grey, 16 * 32);
    Check(Classify(t2, wide15) != TextureTracker::kNamed,
          "...but not where anything else drew");
    DrawJob zeros = {};
    zeros.kind = DrawJob::kVramCopy;
    zeros.src_x = 200;   // VRAM nothing wrote
    zeros.src_y = 300;
    zeros.x = 80;
    zeros.w = 16;
    zeros.h = 32;
    Draw(t2, zeros, 16 * 32);   // on the card; native VRAM still says 0x4210
    Check(Classify(t2, wide15) == TextureTracker::kNamed,
          "...or where a copy of what is known to be clear landed");
    Draw(t2, grey, 16 * 32);
    DrawJob black = Sprite(0, 0, 0, 0, 0, 80, 0, 16, 32, 0, 0);
    black.state.textured = false;
    black.env.area_right = 1023;
    black.r = black.g = black.b = 7;   // cut to five bits, 0
    Draw(t2, black, 16 * 32);
    Check(Classify(t2, wide15) == TextureTracker::kNamed,
          "...or where a black rectangle drew, as Metal Gear Solid clears VRAM");

    // A triangle drew only the pixels it covers, not its bounding box: beside its apex, inside
    // the box, VRAM is as it was.
    Vram v3;
    TextureTracker t3(v3.words.data());
    Upload(v3, t3, 64, 0, 16, 32, 3);
    DrawJob tri = {};
    tri.kind = DrawJob::kTriangle;
    tri.env.area_right = 1023;
    tri.env.area_bottom = 511;
    tri.v[0].x = 88; tri.v[0].y = 8;
    tri.v[1].x = 95; tri.v[1].y = 31;
    tri.v[2].x = 80; tri.v[2].y = 31;
    Draw(t3, tri, 100);
    const DrawJob beside = Sprite(64, 0, 2, 0, 0, 10, 10, 24, 2, 0, 8);   // 64..87, rows 8 and 9
    Check(Classify(t3, beside) == TextureTracker::kNamed,
          "a triangle's bounding box beside its apex is not drawn");
    const DrawJob under = Sprite(64, 0, 2, 0, 0, 10, 10, 24, 2, 0, 28);   // rows 28 and 29
    Check(Classify(t3, under) != TextureTracker::kNamed, "...what it covers is");
  }

  // Run-ahead: held as the state is saved, the frames ahead drawing over the image, and the load
  // back bringing the records back as they were held.
  {
    Upload(vram, tracker, 640, 0, kW, kH, 1);
    UploadClut(vram, tracker, 0, 480, 16);
    std::string before;
    Classify(tracker, sprite, &before);
    tracker.Hold();
    DrawJob ahead = over;
    ahead.x = 640;
    Draw(tracker, ahead, 16);
    const bool drawn_ahead = Classify(tracker, sprite) == TextureTracker::kTextureDrawn;
    tracker.Reloaded();
    tracker.Release();
    std::string after;
    Check(drawn_ahead && Classify(tracker, sprite, &after) == TextureTracker::kNamed &&
              after == before,
          "run-ahead's load back brings back the records held as its state was saved");
  }

  tracker.Reloaded();
  Check(Classify(tracker, sprite) == TextureTracker::kNotUploaded,
        "after a state load nothing is named until the game uploads again");

  Draw(tracker, sprite, 256);
  const TextureTracker::Counts& c = tracker.counts();
  Check(c.textured_jobs == 1 && c.pixels[TextureTracker::kNotUploaded] == 256,
        "a draw's pixels are counted against its reason");
}

// The RGBA of a PNG EncodePng wrote: IDAT inflated, a filter byte of 0 before each row.
bool ReadPng(const std::string& path, int* w, int* h, std::vector<uint8_t>* rgba) {
  FILE* f = fopen(path.c_str(), "rb");
  if (f == nullptr)
    return false;
  std::vector<uint8_t> file;
  uint8_t buffer[4096];
  size_t got;
  while ((got = fread(buffer, 1, sizeof(buffer), f)) > 0)
    file.insert(file.end(), buffer, buffer + got);
  fclose(f);
  auto be32 = [&](size_t at) {
    return (uint32_t(file[at]) << 24) | (uint32_t(file[at + 1]) << 16) |
           (uint32_t(file[at + 2]) << 8) | file[at + 3];
  };
  std::vector<uint8_t> idat;
  for (size_t at = 8; at + 8 <= file.size();) {
    const uint32_t length = be32(at);
    const std::string type(reinterpret_cast<const char*>(&file[at + 4]), 4);
    if (type == "IHDR") {
      *w = static_cast<int>(be32(at + 8));
      *h = static_cast<int>(be32(at + 12));
    } else if (type == "IDAT") {
      idat.insert(idat.end(), file.begin() + at + 8, file.begin() + at + 8 + length);
    }
    at += 12 + length;
  }
  std::vector<uint8_t> raw(static_cast<size_t>(*w) * 4 * *h + *h);
  uLongf size = static_cast<uLongf>(raw.size());
  z_stream z = {};
  inflateInit(&z);
  z.next_in = idat.data();
  z.avail_in = static_cast<uInt>(idat.size());
  z.next_out = raw.data();
  z.avail_out = static_cast<uInt>(size);
  const bool ok = inflate(&z, Z_FINISH) == Z_STREAM_END && z.total_out == size;
  inflateEnd(&z);
  if (!ok)
    return false;
  rgba->clear();
  for (int y = 0; y < *h; ++y) {
    const uint8_t* row = &raw[static_cast<size_t>(y) * (*w * 4 + 1)];
    if (row[0] != 0)
      return false;
    rgba->insert(rgba->end(), row + 1, row + 1 + *w * 4);
  }
  return true;
}

void TestDumping() {
  printf("dumping\n");
  namespace fs = std::filesystem;
  const fs::path folder = fs::temp_directory_path() / "psxemu_texture_tracker_test";
  fs::remove_all(folder);
  fs::create_directories(folder);

  Vram vram;
  TextureTracker tracker(vram.words.data());
  tracker.set_dump_folder(folder.string());
  Upload(vram, tracker, 640, 0, kW, kH, 1);
  UploadClut(vram, tracker, 0, 480, 16);
  // One texel of each kind in the first 8x8 cell: index 0 (colour 0000h, not drawn), index 3
  // (semi-transparent), and the rest opaque.
  vram.at(640, 0) = 0x3030;   // texels 0..3 index 0, 3, 0, 3
  tracker.Written(640, 0, 1, 1);   // a patch: the image holds this now
  const DrawJob sprite = Sprite(640, 0, 0, 0, 480, 10, 10, 8, 8, 0, 0);
  std::string name;
  Classify(tracker, sprite, &name);
  Draw(tracker, sprite, 64);
  tracker.Flush();
  const std::string path = (folder / (name + ".png")).string();
  int w = 0, h = 0;
  std::vector<uint8_t> rgba;
  Check(ReadPng(path, &w, &h, &rgba), "a source read is written as a PNG named after it");
  Check(w == 64 && h == 32, "...the upload's size in texels: 16 halfwords of 4-bit, 64 wide");
  auto texel = [&](int x, int y) { return &rgba[(static_cast<size_t>(y) * w + x) * 4]; };
  Check(rgba.size() == 64u * 32 * 4 && texel(0, 0)[3] == 0,
        "a texel whose colour is 0000h is clear: not drawn");
  const uint16_t c3 = vram.at(3, 480);
  Check(texel(1, 0)[3] == 128 && texel(1, 0)[0] == (((c3 & 31) << 3) | ((c3 & 31) >> 2)),
        "a semi-transparent one has alpha 128, its colour widened from five bits");
  Check(texel(5, 5)[3] == 255, "an opaque one, 255");
  Check(texel(20, 20)[3] == 0 && texel(9, 0)[3] == 0,
        "texels no draw read through that palette are left clear, by 8x8 cell");
  const TextureTracker::Counts& c = tracker.counts();
  Check(c.dumped == 1 && c.dumped_texels == 64u * 32 && c.dumped_used == 64,
        "counted: one written, 2048 texels, 64 of them filled in");

  // A run after that reads only what the file has leaves it as it is...
  {
    TextureTracker same(vram.words.data());
    same.set_dump_folder(folder.string());
    same.Written(640, 0, kW, kH);
    same.Written(0, 480, 16, 1);
    Draw(same, sprite, 64);
    same.Flush();
    Check(same.counts().dumped == 0 && same.counts().dumped_merged == 0,
          "a file already holding every cell read is left as it is");
  }
  // ...and one reading more fills the new cells in over it.
  TextureTracker again(vram.words.data());
  again.set_dump_folder(folder.string());
  again.Written(640, 0, kW, kH);
  again.Written(0, 480, 16, 1);
  Draw(again, Sprite(640, 0, 0, 0, 480, 10, 10, 32, 32, 0, 0), 1024);
  again.Flush();
  std::vector<uint8_t> merged;
  Check(again.counts().dumped == 0 && again.counts().dumped_merged == 1 &&
            ReadPng(path, &w, &h, &merged) && w == 64 && h == 32,
        "a file already there has the cells read now added to it");
  bool same_first = true, more = false, rest_clear = true;
  for (int y = 0; y < 32; ++y)
    for (int x = 0; x < 64; ++x) {
      const uint8_t* m = &merged[(static_cast<size_t>(y) * 64 + x) * 4];
      if (x < 8 && y < 8)
        same_first = same_first && memcmp(m, texel(x, y), 4) == 0;
      else if (x < 32)
        more = more || m[3] != 0;
      else
        rest_clear = rest_clear && m[3] == 0;
    }
  Check(same_first && more && rest_clear,
        "...keeping the cells it had, filling the 15 new ones, and nothing else");
  // Written in the background, as the front end does, the same file comes out once Flush returns.
  {
    const fs::path behind = folder / "background";
    fs::create_directories(behind);
    TextureTracker background(vram.words.data());
    background.set_dump_folder(behind.string(), 8, true);
    background.Written(640, 0, kW, kH);
    background.Written(0, 480, 16, 1);
    Draw(background, Sprite(640, 0, 0, 0, 480, 10, 10, 32, 32, 0, 0), 1024);
    background.Flush();
    std::vector<uint8_t> written;
    int bw = 0, bh = 0;
    const bool read = ReadPng((behind / (name + ".png")).string(), &bw, &bh, &written);
    // The first cell differs from the merged file's only where the patch left it.
    Check(background.counts().dumped == 1 && read && bw == 64 && bh == 32 &&
              memcmp(written.data() + 8 * 64 * 4, merged.data() + 8 * 64 * 4, 24 * 64 * 4) == 0,
          "written in the background, it is there once Flush returns");
  }

  // The tracker's own reader agrees with this test's on every byte.
  int dw = 0, dh = 0;
  std::vector<uint8_t> decoded;
  Check(emulation::psx::ReadPngFile(path, &dw, &dh, &decoded) && dw == 64 && dh == 32 &&
            decoded == merged,
        "ReadPngFile reads a dump back byte for byte");

  const std::vector<uint8_t> tiny = { 1, 2, 3, 4 };
  Check(emulation::psx::EncodePng(1, 1, tiny.data()).size() > 40,
        "EncodePng makes a whole file of one pixel");
  fs::remove_all(folder);
}

// A PNG of any colour type, depth and interlacing, its rows filtered with each of the five filters
// in turn, written here independently of png_file: what ReadPngFile is checked against. `sample`
// gives channel `c` of pixel (x, y), as stored at `depth`.
template <typename Sample>
std::vector<uint8_t> MakePng(uint32_t w, uint32_t h, int colour, int depth, bool interlaced,
                             Sample sample, const std::vector<uint8_t>& plte,
                             const std::vector<uint8_t>& trns) {
  const int channels = colour == 2 ? 3 : colour == 4 ? 2 : colour == 6 ? 4 : 1;
  const uint32_t bits = static_cast<uint32_t>(channels * depth);
  const size_t step = bits >= 8 ? bits / 8 : 1;
  struct P { uint32_t x0, y0, dx, dy; };
  const P whole[1] = { { 0, 0, 1, 1 } };
  const P adam7[7] = { { 0, 0, 8, 8 }, { 4, 0, 8, 8 }, { 0, 4, 4, 8 }, { 2, 0, 4, 4 },
                       { 0, 2, 2, 4 }, { 1, 0, 2, 2 }, { 0, 1, 1, 2 } };
  std::vector<uint8_t> raw;
  int row_number = 0;
  for (int i = 0; i < (interlaced ? 7 : 1); ++i) {
    const P& p = interlaced ? adam7[i] : whole[0];
    const uint32_t pw = w > p.x0 ? (w - p.x0 + p.dx - 1) / p.dx : 0;
    const uint32_t ph = h > p.y0 ? (h - p.y0 + p.dy - 1) / p.dy : 0;
    if (pw == 0 || ph == 0)
      continue;
    const size_t stride = (static_cast<size_t>(pw) * bits + 7) / 8;
    std::vector<uint8_t> above(stride, 0);
    for (uint32_t y = 0; y < ph; ++y) {
      std::vector<uint8_t> line(stride, 0);
      for (uint32_t x = 0; x < pw; ++x)
        for (int c = 0; c < channels; ++c) {
          const uint32_t s = sample(p.x0 + x * p.dx, p.y0 + y * p.dy, c);
          const size_t index = static_cast<size_t>(x) * channels + c;
          if (depth == 16) {
            line[index * 2] = static_cast<uint8_t>(s >> 8);
            line[index * 2 + 1] = static_cast<uint8_t>(s);
          } else if (depth == 8) {
            line[index] = static_cast<uint8_t>(s);
          } else {
            const size_t bit = index * static_cast<size_t>(depth);
            line[bit / 8] |= static_cast<uint8_t>(s << (8 - depth - static_cast<int>(bit % 8)));
          }
        }
      const int filter = row_number++ % 5;
      raw.push_back(static_cast<uint8_t>(filter));
      for (size_t x = 0; x < stride; ++x) {
        const int a = x >= step ? line[x - step] : 0, b = y > 0 ? above[x] : 0;
        const int c = (y > 0 && x >= step) ? above[x - step] : 0;
        const int pp = a + b - c;
        const int pa = abs(pp - a), pb = abs(pp - b), pc = abs(pp - c);
        const int paeth = (pa <= pb && pa <= pc) ? a : pb <= pc ? b : c;
        const int predicted = filter == 0 ? 0 : filter == 1 ? a : filter == 2 ? b
                              : filter == 3 ? (a + b) / 2 : paeth;
        raw.push_back(static_cast<uint8_t>(line[x] - predicted));
      }
      above = line;
    }
  }
  z_stream z = {};
  deflateInit(&z, 9);
  std::vector<uint8_t> packed(deflateBound(&z, static_cast<uLong>(raw.size())));
  z.next_in = raw.data();
  z.avail_in = static_cast<uInt>(raw.size());
  z.next_out = packed.data();
  z.avail_out = static_cast<uInt>(packed.size());
  deflate(&z, Z_FINISH);
  packed.resize(z.total_out);
  deflateEnd(&z);
  std::vector<uint8_t> png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
  auto put32 = [](std::vector<uint8_t>* out, uint32_t v) {
    for (int s = 24; s >= 0; s -= 8)
      out->push_back(static_cast<uint8_t>(v >> s));
  };
  auto chunk = [&](const char* type, const std::vector<uint8_t>& data) {
    put32(&png, static_cast<uint32_t>(data.size()));
    const size_t start = png.size();
    png.insert(png.end(), type, type + 4);
    png.insert(png.end(), data.begin(), data.end());
    put32(&png, static_cast<uint32_t>(crc32(0, png.data() + start, static_cast<uInt>(png.size() - start))));
  };
  std::vector<uint8_t> header;
  put32(&header, w);
  put32(&header, h);
  header.insert(header.end(), { static_cast<uint8_t>(depth), static_cast<uint8_t>(colour), 0, 0,
                                static_cast<uint8_t>(interlaced ? 1 : 0) });
  chunk("IHDR", header);
  if (!plte.empty())
    chunk("PLTE", plte);
  if (!trns.empty())
    chunk("tRNS", trns);
  // Two IDATs, which a reader must join.
  const size_t half = packed.size() / 2;
  chunk("IDAT", std::vector<uint8_t>(packed.begin(), packed.begin() + static_cast<ptrdiff_t>(half)));
  chunk("IDAT", std::vector<uint8_t>(packed.begin() + static_cast<ptrdiff_t>(half), packed.end()));
  chunk("IEND", {});
  return png;
}

void TestPngReading() {
  printf("reading PNGs\n");
  // Samples from where the pixel is, so every pixel of every pass differs.
  auto value = [](uint32_t x, uint32_t y, int c, uint32_t most) {
    return ((x * 37 + y * 101 + static_cast<uint32_t>(c) * 59) * 2654435761u >> 7) % (most + 1);
  };
  struct Case {
    const char* what;
    int colour, depth;
    bool interlaced;
    uint32_t w, h;
  };
  const Case cases[] = {
    { "RGBA, 8 bits, each filter", 6, 8, false, 23, 9 },
    { "RGBA, 16 bits, interlaced", 6, 16, true, 17, 13 },
    { "RGB, 8 bits, interlaced, a colour key", 2, 8, true, 9, 11 },
    { "RGB, 16 bits, a colour key", 2, 16, false, 5, 6 },
    { "grey, 1 bit", 0, 1, false, 19, 3 },
    { "grey, 4 bits, interlaced, a key", 0, 4, true, 7, 7 },
    { "grey and alpha, 16 bits", 4, 16, false, 4, 5 },
    { "palette, 2 bits, interlaced, alpha", 3, 2, true, 10, 10 },
    { "palette, 8 bits, alpha for some entries", 3, 8, false, 6, 4 },
    { "one pixel, interlaced", 6, 8, true, 1, 1 },
  };
  for (const Case& t : cases) {
    const uint32_t most = (1u << t.depth) - 1;
    const int channels = t.colour == 2 ? 3 : t.colour == 4 ? 2 : t.colour == 6 ? 4 : 1;
    std::vector<uint8_t> plte, trns;
    uint32_t key[3] = { 0, 0, 0 };
    if (t.colour == 3) {
      for (uint32_t i = 0; i <= most; ++i)
        plte.insert(plte.end(), { static_cast<uint8_t>(i * 40), static_cast<uint8_t>(255 - i),
                                  static_cast<uint8_t>(i * 7) });
      trns = { 0, 77, 200 };   // entries past these are opaque
    } else if (t.colour == 0 || t.colour == 2) {
      // The key is the colour of pixel (1, 0), so it is met.
      for (int c = 0; c < channels; ++c) {
        key[c] = value(1, 0, c, most);
        trns.push_back(static_cast<uint8_t>(key[c] >> 8));
        trns.push_back(static_cast<uint8_t>(key[c]));
      }
      if (t.depth == 1)
        trns.clear();
    }
    auto sample = [&](uint32_t x, uint32_t y, int c) { return value(x, y, c, most); };
    const std::vector<uint8_t> png = MakePng(t.w, t.h, t.colour, t.depth, t.interlaced, sample,
                                             plte, trns);
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;
    bool ok = emulation::psx::DecodePng(png.data(), png.size(), &w, &h, &rgba) &&
              w == static_cast<int>(t.w) && h == static_cast<int>(t.h);
    auto eight = [&](uint32_t s) {
      return static_cast<uint8_t>(t.depth == 16 ? s >> 8 : t.depth == 8 ? s : s * 255 / most);
    };
    for (uint32_t y = 0; ok && y < t.h; ++y)
      for (uint32_t x = 0; ok && x < t.w; ++x) {
        uint8_t want[4];
        uint32_t s[4] = {};
        for (int c = 0; c < channels; ++c)
          s[c] = sample(x, y, c);
        if (t.colour == 3) {
          want[0] = plte[s[0] * 3];
          want[1] = plte[s[0] * 3 + 1];
          want[2] = plte[s[0] * 3 + 2];
          want[3] = s[0] < trns.size() ? trns[s[0]] : 255;
        } else if (t.colour == 0 || t.colour == 4) {
          want[0] = want[1] = want[2] = eight(s[0]);
          want[3] = t.colour == 4 ? eight(s[1]) : (!trns.empty() && s[0] == key[0]) ? 0 : 255;
        } else {
          for (int c = 0; c < 3; ++c)
            want[c] = eight(s[c]);
          want[3] = t.colour == 6 ? eight(s[3])
                    : (!trns.empty() && s[0] == key[0] && s[1] == key[1] && s[2] == key[2]) ? 0 : 255;
        }
        ok = memcmp(&rgba[(static_cast<size_t>(y) * t.w + x) * 4], want, 4) == 0;
      }
    Check(ok, t.what);
  }
  const std::vector<uint8_t> junk = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n', 0, 0, 0, 1 };
  int w = 0, h = 0;
  std::vector<uint8_t> rgba;
  Check(!emulation::psx::DecodePng(junk.data(), junk.size(), &w, &h, &rgba),
        "a cut-off file is not read");
}

}  // namespace

int main() {
  printf("texture tracker\n");
  TestNaming();
  TestStrips();
  TestReasons();
  TestDumping();
  TestPngReading();
  printf("\n%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
