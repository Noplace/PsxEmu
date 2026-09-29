// hw_raster_test - the hardware rasteriser against the software one (Docs/Hardware-Renderer-Plan.md).
//
// Two machines, no BIOS: one draws with SoftwareRaster, the other with the front end's
// D3D11Raster on WARP - Windows' own software Direct3D, so this needs no graphics card and gives
// the same answer every run. Each scene writes the same GP0 words to both - random primitives of
// one kind, from a fixed seed - and then compares all of VRAM, pixel for pixel.
//
// The software rasteriser is the reference: it has been checked against real hardware through
// bugs 58-105. At native size the hardware one does the same integer arithmetic, so every scene
// must match to the pixel. A scene that does not says how many pixels differ, and how many by
// more than one 5-bit step in a channel (or in the mask bit), which tells rounding from a wrong
// texel or a wrong pixel covered.
//
// Nothing here samples a texture from the pixels the same primitive is drawing: the software
// rasteriser sees its own writes as it goes, the card sees VRAM as it was before, and the console
// has a texture cache - there is no right answer to compare against (RandomTexture).
//
//   hw_raster_test [--seed n] [--scale n] [--verbose] [--bisect]
//
// --scale draws the hardware side at n times the resolution, without true colour. Native VRAM
// is taken from each console pixel's own sub-pixel, so it must still match to the pixel.
//
// --bisect compares after every primitive and stops a scene at the first one to differ, printing
// its GP0 words.
//
// Built only by build_tools.bat, with PSXEmu.Win32 on the include path.

#include "psx/psx.h"
#include "graphics/hw_raster/d3d11_raster.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using emulation::psx::System;

namespace {

int g_checks = 0;
int g_failures = 0;
bool g_verbose = false;
bool g_bisect = false;

void Check(bool condition, const std::string& what) {
  ++g_checks;
  if (!condition) {
    ++g_failures;
    printf("  FAIL  %s\n", what.c_str());
  }
}

// xorshift32: the same primitives every run, and on every machine.
struct Random {
  uint32_t state;
  uint32_t Next() {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
  }
  int32_t Range(int32_t lo, int32_t hi) {   // inclusive
    return lo + static_cast<int32_t>(Next() % static_cast<uint32_t>(hi - lo + 1));
  }
  bool Chance(int percent) { return static_cast<int>(Next() % 100) < percent; }
};

struct Machines {
  System* software;
  System* hardware;
  std::vector<uint32_t> recent;   // --bisect: the words since the last primitive began

  void Gp0(uint32_t word) {
    recent.push_back(word);
    software->gpu().WriteData(word);
    hardware->gpu().WriteData(word);
  }
  void Gp1(uint32_t word) {
    software->gpu().WriteStatus(word);
    hardware->gpu().WriteStatus(word);
  }
  // Past what any scene owes, so everything queued has been drawn.
  void Run() {
    for (int i = 0; i < 64; ++i) {
      software->gpu().Tick(4096);
      hardware->gpu().Tick(4096);
    }
  }
};

uint32_t Xy(int32_t x, int32_t y) {
  return (static_cast<uint32_t>(y & 0x7FF) << 16) | static_cast<uint32_t>(x & 0x7FF);
}

// Everything back to a known start: the GPU reset, VRAM black, the drawing area the top-left
// 512x256 with no offset, and no mask rules.
void Reset(Machines& m) {
  m.Gp1(0x00000000);
  m.Gp0(0xE1000000);
  m.Gp0(0xE2000000);
  m.Gp0(0xE3000000);
  m.Gp0(0xE4000000 | (255u << 10) | 511u);
  m.Gp0(0xE5000000);
  m.Gp0(0xE6000000);
  for (uint32_t x = 0; x < 1024; x += 512) {
    for (uint32_t y = 0; y < 512; y += 256) {
      m.Gp0(0x02000000);
      m.Gp0(Xy(x, y));
      m.Gp0((256u << 16) | 512u);
    }
  }
  m.Run();
}

// Random texture data across the right half of VRAM - x 512-1023, where the texture pages 8-15
// are, and the CLUTs the scenes use - uploaded as a game uploads one. Some texels are zero, which
// a texture draws as transparent, and some have bit 15, which makes them blend and sets the mask.
void UploadTextures(Machines& m, Random& random) {
  m.Gp0(0xA0000000);
  m.Gp0(Xy(512, 0));
  m.Gp0((512u << 16) | 512u);
  for (int i = 0; i < 512 * 512 / 2; ++i) {
    uint32_t word = 0;
    for (int half = 0; half < 2; ++half) {
      uint32_t pixel = random.Next() & 0xFFFF;
      if (random.Chance(10))
        pixel = 0;
      else if (random.Chance(70))
        pixel &= 0x7FFF;
      word |= pixel << (half * 16);
    }
    m.Gp0(word);
  }
  m.Run();
}

// A texture page and a CLUT that the primitive using them cannot be drawing into. One that samples
// the pixels it is drawing gets them from the software rasteriser in the order it draws them,
// and from the card all as they were before it - and from the console's texture cache, something
// else again - so no answer is the right one. So a page never runs past VRAM's right edge and
// back round into the drawing area, as a 15-bit page from x=768 up would, and a CLUT never does.
//
// The page is the texpage word a polygon's second vertex carries: bits 0-3 page x, 4 page y, 5-6
// the blend mode, 7-8 the depth. `half` < 0: pages 8-15, in the right half of VRAM, where the
// textures were uploaded. Otherwise pages 0-7 in that half of VRAM's height - what has been drawn
// there, while the drawing area is the other half.
struct Texture {
  uint32_t page, clut;
};

Texture RandomTexture(Random& random, int half) {
  const uint32_t depth = static_cast<uint32_t>(random.Range(0, 2));
  const int32_t last = depth == 0 ? 15 : depth == 1 ? 14 : 12;   // the last page that fits
  const uint32_t x = static_cast<uint32_t>(half < 0 ? random.Range(8, last) : random.Range(0, 7));
  const uint32_t y = static_cast<uint32_t>(half < 0 ? random.Range(0, 1) : half);
  // 16 or 256 entries from x=512 on, in 16-pixel units.
  const uint32_t clut_x = static_cast<uint32_t>(random.Range(32, depth == 1 ? 48 : 63));
  return { x | (y << 4) | (static_cast<uint32_t>(random.Range(0, 3)) << 5) | (depth << 7),
           clut_x | (static_cast<uint32_t>(random.Range(0, 511)) << 6) };
}

uint32_t RandomColour(Random& random) {
  return random.Next() & 0xFFFFFF;
}

// Compares all of VRAM: every pixel must be the same.
void Compare(Machines& m, const char* scene) {
  m.Run();
  const uint16_t* a = m.software->gpu().vram();
  const uint16_t* b = m.hardware->gpu().vram();
  int differ = 0, beyond = 0, drawn = 0;
  int first = -1;
  for (int i = 0; i < 1024 * 512; ++i) {
    if (a[i] != 0)
      ++drawn;
    if (a[i] == b[i])
      continue;
    ++differ;
    bool distant = (a[i] & 0x8000) != (b[i] & 0x8000);
    for (int shift = 0; shift < 15; shift += 5) {
      const int d = static_cast<int>((a[i] >> shift) & 31) - static_cast<int>((b[i] >> shift) & 31);
      if (d > 1 || d < -1)
        distant = true;
    }
    if (distant) {
      ++beyond;
      if (first < 0)
        first = i;
    }
    if (g_verbose && differ <= 8)
      printf("    (%d,%d) software %04X hardware %04X\n", i % 1024, i / 1024, a[i], b[i]);
  }
  printf("%-48s %7d drawn  %6d differ  %5d beyond one step\n", scene, drawn, differ, beyond);
  if (first >= 0)
    printf("    first beyond one step at (%d,%d): software %04X hardware %04X\n", first % 1024,
           first / 1024, a[first], b[first]);
  Check(drawn > 0, std::string(scene) + ": the software rasteriser drew something");
  Check(differ == 0, std::string(scene) + ": every pixel the same");
}

// --bisect: after every primitive, whether the two now differ - and if so, the words that made
// it and the first pixels it got wrong, so a scene stops at the first primitive to go astray.
bool Bisected(Machines& m) {
  if (!g_bisect)
    return false;
  m.Run();
  const uint16_t* a = m.software->gpu().vram();
  const uint16_t* b = m.hardware->gpu().vram();
  int shown = 0;
  for (int i = 0; i < 1024 * 512; ++i) {
    if (a[i] == b[i])
      continue;
    if (shown == 0) {
      printf("  the first primitive to differ:");
      for (size_t w = 0; w < m.recent.size(); ++w)
        printf("%s%08X", w % 8 == 0 ? "\n    " : " ", m.recent[w]);
      printf("\n");
    }
    printf("    (%d,%d) software %04X hardware %04X\n", i % 1024, i / 1024, a[i], b[i]);
    if (++shown == 12)
      break;
  }
  return shown > 0;
}

// A polygon: `command` 20h-3Fh, with its words written out in the order GP0 takes them.
void Polygon(Machines& m, Random& random, uint32_t command, int32_t cx, int32_t cy, int32_t size,
             int texture_half) {
  const bool gouraud = (command & 0x10) != 0;
  const bool quad = (command & 0x08) != 0;
  const bool textured = (command & 0x04) != 0;
  const int verts = quad ? 4 : 3;
  m.Gp0((command << 24) | RandomColour(random));
  const Texture texture = RandomTexture(random, texture_half);
  const uint32_t clut = texture.clut;
  const uint32_t page = texture.page;
  const int32_t u0 = random.Range(0, 255), v0 = random.Range(0, 255);
  for (int i = 0; i < verts; ++i) {
    if (gouraud && i > 0)
      m.Gp0(RandomColour(random));
    m.Gp0(Xy(cx + random.Range(-size, size), cy + random.Range(-size, size)));
    if (textured) {
      const uint32_t u = static_cast<uint32_t>(std::min(255, std::max(0, u0 + random.Range(-64, 64))));
      const uint32_t v = static_cast<uint32_t>(std::min(255, std::max(0, v0 + random.Range(-64, 64))));
      const uint32_t high = (i == 0) ? clut : (i == 1) ? page : 0;
      m.Gp0((high << 16) | (v << 8) | u);
    }
  }
}

void ScenePolygons(Machines& m, Random& random, const char* name, uint32_t mask_bits,
                   int count, int max_size, bool texture_anywhere,
                   bool dither_randomly = true, bool mask_randomly = false,
                   bool window_randomly = false) {
  // Sampling what was drawn: the drawing area moves between the top and bottom halves of the
  // left of VRAM every hundred primitives, and textures come from the other half.
  int half = 0;
  for (int i = 0; i < count; ++i) {
    m.recent.clear();
    if (texture_anywhere && i % 100 == 0) {
      half = (i / 100) & 1;
      m.Gp0(0xE3000000 | (static_cast<uint32_t>(half * 256) << 10));
      m.Gp0(0xE4000000 | (static_cast<uint32_t>(half * 256 + 255) << 10) | 511u);
      m.Gp0(0xE5000000 | (static_cast<uint32_t>(half * 256) << 11));
    }
    if (dither_randomly && random.Chance(5))
      m.Gp0(0xE1000000 | (random.Range(0, 1) << 9));
    if (mask_randomly && random.Chance(10))
      m.Gp0(0xE6000000 | random.Range(0, 3));
    if (window_randomly && random.Chance(10))
      m.Gp0(0xE2000000 | (random.Next() & 0xFFFFF));
    const uint32_t command = 0x20 | (random.Next() & 0x1F & mask_bits);
    Polygon(m, random, command, random.Range(0, 511), random.Range(0, 255),
            random.Range(1, max_size), texture_anywhere ? 1 - half : -1);
    if (Bisected(m))
      break;
  }
  Compare(m, name);
}

// A rectangle, 60h-7Fh. Its texture page comes from GP0(E1h), which is set to a random one first
// when it is textured - with the flip bits.
void Rectangle(Machines& m, Random& random, uint32_t command) {
  const bool textured = (command & 0x04) != 0;
  const Texture texture = RandomTexture(random, -1);
  if (textured)
    m.Gp0(0xE1000000 | texture.page | (random.Range(0, 3) << 12));
  m.Gp0((command << 24) | RandomColour(random));
  m.Gp0(Xy(random.Range(-40, 520), random.Range(-40, 270)));
  if (textured)
    m.Gp0((texture.clut << 16) | (random.Next() & 0xFFFF));
  if (((command >> 3) & 3) == 0)
    m.Gp0((random.Range(1, 120) << 16) | random.Range(1, 120));
}

void SceneRectangles(Machines& m, Random& random, const char* name, uint32_t mask_bits,
                     int count, bool mask_randomly = false,
                     bool window_randomly = false) {
  for (int i = 0; i < count; ++i) {
    m.recent.clear();
    if (mask_randomly && random.Chance(10))
      m.Gp0(0xE6000000 | random.Range(0, 3));
    if (window_randomly && random.Chance(10))
      m.Gp0(0xE2000000 | (random.Next() & 0xFFFFF));
    Rectangle(m, random, 0x60 | (random.Next() & 0x1F & mask_bits));
    if (Bisected(m))
      break;
  }
  Compare(m, name);
}

void SceneLines(Machines& m, Random& random, int count) {
  for (int i = 0; i < count; ++i) {
    m.recent.clear();
    if (random.Chance(5))
      m.Gp0(0xE1000000 | (random.Range(0, 1) << 9) | (random.Range(0, 3) << 5));
    if (random.Chance(5))
      m.Gp0(0xE6000000 | random.Range(0, 3));
    const bool gouraud = random.Chance(50);
    const bool poly = random.Chance(30);
    const bool semi = random.Chance(40);
    const uint32_t command = 0x40 | (gouraud ? 0x10 : 0) | (poly ? 0x08 : 0) | (semi ? 0x02 : 0);
    m.Gp0((command << 24) | RandomColour(random));
    const int points = poly ? random.Range(2, 6) : 2;
    for (int p = 0; p < points; ++p) {
      if (gouraud && p > 0)
        m.Gp0(RandomColour(random));
      m.Gp0(Xy(random.Range(-20, 530), random.Range(-20, 275)));
    }
    if (poly)
      m.Gp0(0x55555555);
  }
  Compare(m, "lines and polylines, every blend, dithered, masked");
}

// VRAM kept in step: draws mixed with CPU-to-VRAM uploads into what is being drawn and sampled,
// VRAM-to-CPU reads - every word compared between the two - and copies, onto themselves too,
// which the console does pixel by pixel and so smears.
void SceneCoherence(Machines& m, Random& random, int count) {
  int reads = 0, words_differing = 0;
  for (int i = 0; i < count; ++i) {
    m.recent.clear();
    if (random.Chance(5))
      m.Gp0(0xE6000000 | random.Range(0, 3));
    const int what = random.Range(0, 9);
    if (what < 5) {
      Polygon(m, random, 0x20 | (random.Next() & 0x1F), random.Range(0, 511),
              random.Range(0, 255), random.Range(1, 60), -1);
    }
    else if (what < 7) {
      const int32_t w = random.Range(1, 64), h = random.Range(1, 64);
      m.Gp0(0xA0000000);
      m.Gp0(Xy(random.Range(0, 1023), random.Range(0, 511)) & 0x01FF03FF);
      m.Gp0((static_cast<uint32_t>(h) << 16) | static_cast<uint32_t>(w));
      for (int32_t word = 0; word < (w * h + 1) / 2; ++word)
        m.Gp0(random.Next());
    }
    else if (what < 8) {
      // Anywhere, so often onto itself: a short shift, in any direction.
      const int32_t sx = random.Range(0, 1023), sy = random.Range(0, 511);
      const int32_t dx = random.Chance(50) ? sx + random.Range(-8, 8) : random.Range(0, 1023);
      const int32_t dy = random.Chance(50) ? sy + random.Range(-8, 8) : random.Range(0, 511);
      m.Gp0(0x80000000);
      m.Gp0(Xy(sx, sy) & 0x01FF03FF);
      m.Gp0(Xy(dx, dy) & 0x01FF03FF);
      m.Gp0((static_cast<uint32_t>(random.Range(1, 96)) << 16) | random.Range(1, 96));
    }
    else {
      const int32_t w = random.Range(1, 64), h = random.Range(1, 64);
      m.Gp0(0xC0000000);
      m.Gp0(Xy(random.Range(0, 1023), random.Range(0, 511)) & 0x01FF03FF);
      m.Gp0((static_cast<uint32_t>(h) << 16) | static_cast<uint32_t>(w));
      ++reads;
      for (int32_t word = 0; word < (w * h + 1) / 2; ++word)
        if (m.software->gpu().ReadData() != m.hardware->gpu().ReadData())
          ++words_differing;
    }
    if (Bisected(m))
      break;
  }
  printf("%-48s %7d reads  %6d words differ\n", "  (VRAM-to-CPU reads)", reads, words_differing);
  Check(reads > 0 && words_differing == 0, "every word read back from VRAM the same");
  Compare(m, "draws, uploads, reads and copies onto themselves");
}

void SceneFillsAndCopies(Machines& m, Random& random, int count) {
  for (int i = 0; i < count; ++i) {
    m.recent.clear();
    if (random.Chance(10))
      m.Gp0(0xE6000000 | random.Range(0, 3));
    if (random.Chance(40)) {
      m.Gp0(0x02000000 | RandomColour(random));
      m.Gp0(Xy(random.Range(0, 1023), random.Range(0, 511)));
      m.Gp0((random.Range(0, 300) << 16) | random.Range(0, 300));
    }
    else {
      // Never onto itself, which the two do differently until phase 3: the source and the
      // destination, wrapped, share no pixel.
      int32_t sx, sy, dx, dy, w, h;
      bool overlap;
      do {
        sx = random.Range(0, 1023); sy = random.Range(0, 511);
        dx = random.Range(0, 1023); dy = random.Range(0, 511);
        w = random.Range(1, 200); h = random.Range(1, 200);
        const int32_t across = (dx - sx + 1024) % 1024, down = (dy - sy + 512) % 512;
        overlap = (across < w || 1024 - across < w) && (down < h || 512 - down < h);
      } while (overlap);
      m.Gp0(0x80000000);
      m.Gp0(Xy(sx, sy) & 0x01FF03FF);
      m.Gp0(Xy(dx, dy) & 0x01FF03FF);
      m.Gp0((static_cast<uint32_t>(h) << 16) | static_cast<uint32_t>(w));
    }
  }
  Compare(m, "fills and copies, wrapping, with the mask rules");
}

}  // namespace

// A shared picture read back as a screenshot reads it - through a device of its own - against
// pixels.
bool SharedMatches(const emulation::psx::SharedPicture& picture,
                   const std::vector<uint32_t>& pixels) {
  std::vector<uint32_t> read;
  return psxemu::D3D11Raster::ReadSharedPicture(picture, &read) && read == pixels;
}

// Phase 6 (psx/shared_picture.h): the picture left on the card, against the one read back. Two
// rasterisers at 2x over the same native VRAM, one made to share its pictures and one not, each
// resolving the display area; the shared one is read back the way a screenshot reads it.
void SceneSharedPicture(Random& random) {
  printf("the picture left on the card\n");
  std::vector<uint16_t> vram(1024 * 512);
  for (uint16_t& pixel : vram)
    pixel = static_cast<uint16_t>(random.Next());
  emulation::psx::RasterOptions options;
  options.scale = 2;
  std::string error;
  std::unique_ptr<psxemu::D3D11Raster> reading =
      psxemu::D3D11Raster::Create(vram.data(), options, true, &error);
  options.shared_picture = true;
  std::unique_ptr<psxemu::D3D11Raster> sharing =
      psxemu::D3D11Raster::Create(vram.data(), options, true, &error);
  if (!reading || !sharing || !sharing->sharing_pictures()) {
    Check(false, "WARP makes a rasteriser that shares its pictures: " + error);
    return;
  }
  reading->Written(0, 0, 1024, 512);
  sharing->Written(0, 0, 1024, 512);

  // The first read-back has nothing behind it and waits, so it is this picture too.
  std::vector<uint32_t> pixels, unused;
  emulation::psx::SharedPicture none, shared;
  int scale = 0;
  reading->ResolveDisplay(16, 8, 320, 240, &pixels, &none, &scale);
  const bool made = sharing->ResolveDisplay(16, 8, 320, 240, &unused, &shared, &scale);
  Check(made && shared && scale == 2 && shared.width == 640 && shared.height == 480 &&
            unused.empty() && !none,
        "made to, it hands the picture over on the card, 640x480, and nothing in memory");
  Check(shared && SharedMatches(shared, pixels),
        "the picture on the card is the read-back one to the pixel");

  // A fixed number of textures: with every picture still held, the next comes back as pixels.
  const int kTextures = psxemu::D3D11Raster::kSharedPictures;
  std::vector<emulation::psx::SharedPicture> held = { shared };
  bool all_shared = true;
  for (int i = 1; i < kTextures; ++i) {
    emulation::psx::SharedPicture next;
    all_shared = all_shared && sharing->ResolveDisplay(16, 8, 320, 240, &unused, &next, &scale) &&
                 next && next.serial == held.back().serial + 1;
    held.push_back(next);
  }
  std::vector<uint32_t> over_pixels;
  emulation::psx::SharedPicture over;
  const bool resolved = sharing->ResolveDisplay(16, 8, 320, 240, &over_pixels, &over, &scale);
  Check(all_shared && resolved && !over && over_pixels == pixels,
        "every texture held, the next picture is read back instead - and is the same picture");

  // A frame the presenter never took gives its texture back at once.
  shared.source->Dropped(held[2].serial);
  emulation::psx::SharedPicture again;
  sharing->ResolveDisplay(16, 8, 320, 240, &unused, &again, &scale);
  Check(again && again.texture_id == held[2].texture_id,
        "a dropped frame's texture is drawn into again at once");
  // And the presenter's card finishing with pictures gives theirs back.
  shared.source->Release(held[4].serial);
  emulation::psx::SharedPicture after;
  sharing->ResolveDisplay(16, 8, 320, 240, &unused, &after, &scale);
  Check(after && after.texture_id != held[4].texture_id && after.texture_id != again.texture_id,
        "one the presenter has moved past is drawn into again, and the one it is on is not");

  // The rasteriser goes - the rasteriser switched, say - with a frame still in flight: the
  // frame's picture is still there to show.
  sharing.reset();
  Check(SharedMatches(held[4], pixels),
        "a picture outlives the rasteriser that drew it");
}

int main(int argc, char** argv) {
  uint32_t seed = 1;
  int scale = 1;
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc)
      seed = static_cast<uint32_t>(strtoul(argv[++i], nullptr, 0));
    else if (strcmp(argv[i], "--verbose") == 0)
      g_verbose = true;
    else if (strcmp(argv[i], "--bisect") == 0)
      g_bisect = true;
    else if (strcmp(argv[i], "--scale") == 0 && i + 1 < argc)
      scale = atoi(argv[++i]);
  }

  Machines m;
  m.software = new System();
  m.software->InitializeWithoutBios();
  m.hardware = new System();
  static psxemu::D3D11Raster* made = nullptr;   // for the lost-card scene
  m.hardware->set_hardware_raster([](uint16_t* vram, const emulation::psx::RasterOptions& options,
                                     std::string* error)
                                      -> std::unique_ptr<emulation::psx::RasterBackend> {
    std::unique_ptr<psxemu::D3D11Raster> raster =
        psxemu::D3D11Raster::Create(vram, options, true, error);
    made = raster.get();
    return raster;
  });
  m.hardware->config().gpu_rasteriser = "hardware";
  // Above 1x without true colour, the hardware rasteriser still leaves native VRAM exactly as
  // the software one does: every scene must match at every scale.
  m.hardware->config().resolution_scale = scale;
  m.hardware->config().true_color = false;
  m.hardware->InitializeWithoutBios();
  if (!m.hardware->gpu().hardware_raster()) {
    printf("no hardware rasteriser: %s\n", m.hardware->gpu().raster_error().c_str());
    return 1;
  }
  printf("seed %u, the hardware rasteriser at %dx\n\n", seed, scale);
  Random random{seed ? seed : 1};

  // Polygons: flat 20h/22h, Gouraud 30h/32h - only the untextured bits.
  Reset(m);
  ScenePolygons(m, random, "untextured triangles and quads, opaque", 0x18, 3000, 60, false);
  Reset(m);
  ScenePolygons(m, random, "untextured, every blend mode", 0x1A, 3000, 60, false);
  Reset(m);
  ScenePolygons(m, random, "untextured, large", 0x1A, 300, 400, false);

  // Every scene from here starts from new textures, since Reset clears all of VRAM.
  Reset(m);
  UploadTextures(m, random);
  ScenePolygons(m, random, "textured, raw and modulated, opaque", 0x1D, 3000, 60, false);
  Reset(m);
  UploadTextures(m, random);
  ScenePolygons(m, random, "textured, every blend mode", 0x1F, 3000, 60, false);
  Reset(m);
  UploadTextures(m, random);
  ScenePolygons(m, random, "textured, sampling what was drawn", 0x1F, 3000, 60, true);
  Reset(m);
  UploadTextures(m, random);
  ScenePolygons(m, random, "textured, texture window", 0x1F, 2000, 60, false, true, false,
                true);
  Reset(m);
  UploadTextures(m, random);
  ScenePolygons(m, random, "everything, with the mask rules", 0x1F, 3000, 60, true, true,
                true);

  Reset(m);
  UploadTextures(m, random);
  SceneRectangles(m, random, "rectangles, every size, flipped, every blend", 0x1F, 3000);
  Reset(m);
  UploadTextures(m, random);
  SceneRectangles(m, random, "rectangles, with the mask rules and window", 0x1F, 3000, true,
                  true);

  Reset(m);
  SceneLines(m, random, 3000);

  Reset(m);
  UploadTextures(m, random);
  SceneFillsAndCopies(m, random, 400);

  Reset(m);
  UploadTextures(m, random);
  SceneCoherence(m, random, 3000);

  // Interlaced 480i with drawing to the displayed field off: every draw skips its rows.
  Reset(m);
  UploadTextures(m, random);
  m.Gp1(0x08000024);
  m.Gp0(0xE1000000);
  ScenePolygons(m, random, "480i, the displayed field left alone", 0x1F, 1000, 60, false);

  // The graphics card lost - a driver reset, say: at the next frame the machine carries on with
  // the software rasteriser, from native VRAM as it was last brought up to date, and says why.
  m.Gp1(0x08000000);
  Reset(m);
  UploadTextures(m, random);
  ScenePolygons(m, random, "before the card is lost", 0x1F, 500, 60, false);
  // Everything queued drawn and read back first: what the card draws after the last read-back
  // is lost with it, as it would be.
  for (int i = 0; i < 400; ++i) {
    m.software->gpu().Tick(4096);
    m.hardware->gpu().Tick(4096);
  }
  Compare(m, "before the card is lost, all drawn");
  made->SimulateLoss();
  made = nullptr;
  for (int i = 0; i < 400; ++i) {   // a few frames
    m.software->gpu().Tick(4096);
    m.hardware->gpu().Tick(4096);
  }
  Check(!m.hardware->gpu().hardware_raster(),
        "a lost card leaves the software rasteriser drawing, at the next frame");
  Check(m.hardware->gpu().raster_error().find("simulated") != std::string::npos,
        "and the machine says why");
  Compare(m, "the moment after, nothing lost");
  ScenePolygons(m, random, "after, in software, from where it was", 0x1F, 500, 60, false);

  SceneSharedPicture(random);

  printf("\n%d checks, %d failures\n", g_checks, g_failures);
  delete m.hardware;
  delete m.software;
  return g_failures == 0 ? 0 : 1;
}
