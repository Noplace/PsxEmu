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
#include "render/texture_tracker.h"

#include "render/png_file.h"

#include <algorithm>
#include <cstdio>

namespace emulation {
namespace psx {

namespace {

const uint32_t kVramW = 1024, kVramH = 512;

// More records than this tiling one draw's texels are not looked for as a composite.
const size_t kMostTiles = 64;

uint64_t Mix(uint64_t a, uint64_t b) {
  uint64_t h = (a ^ (b + 0x9E3779B97F4A7C15ull + (a << 6) + (a >> 2))) * 0xFF51AFD7ED558CCDull;
  return h ^ (h >> 33);
}

bool Overlap(int32_t ax0, int32_t ay0, int32_t ax1, int32_t ay1, int32_t bx0, int32_t by0,
             int32_t bx1, int32_t by1) {
  return ax0 <= bx1 && bx0 <= ax1 && ay0 <= by1 && by0 <= ay1;
}

uint8_t Widen(uint32_t c5) { return static_cast<uint8_t>((c5 << 3) | (c5 >> 2)); }

// The mode's three: 4-bit, 8-bit, and 15-bit (3, reserved, reads as 15-bit too).
uint32_t Mode(const RasterState& state) {
  return state.texpage_colors >= 2 ? 2 : state.texpage_colors;
}

// Texels a halfword holds in a mode.
uint32_t PerHalfword(uint32_t mode) { return mode == 0 ? 4 : mode == 1 ? 2 : 1; }

}  // namespace

const char* TextureTracker::ReasonName(Reason reason) {
  switch (reason) {
  case kNamed: return "named";
  case kTextureDrawn: return "texels drawn by the GPU";
  case kClutDrawn: return "palette drawn by the GPU";
  case kSpans: return "only partly uploaded";
  case kNotUploaded: return "never seen uploaded";
  case kClutNotUploaded: return "palette never seen uploaded";
  case kWraps: return "wraps";
  case kOutside: return "outside the drawing area";
  default: return "?";
  }
}

TextureTracker::TextureTracker(const uint16_t* vram)
    : vram_(vram), cells_(kCellsX * kCellsY), drawn_(kCellsX * kCellsY, 0) {}

TextureTracker::~TextureTracker() = default;

uint64_t TextureTracker::HashRect(const uint16_t* vram, uint32_t x, uint32_t y, uint32_t w,
                                  uint32_t h) {
  // FNV-1a, a halfword at a time, seeded with the size so an image and its transpose differ.
  uint64_t hash = 0xCBF29CE484222325ull ^ ((static_cast<uint64_t>(w) << 32) | h);
  for (uint32_t row = 0; row < h; ++row) {
    const uint16_t* line = vram + ((y + row) & (kVramH - 1)) * kVramW;
    for (uint32_t col = 0; col < w; ++col)
      hash = (hash ^ line[(x + col) & (kVramW - 1)]) * 0x100000001B3ull;
  }
  return hash;
}

// ---- Records ---------------------------------------------------------------------------------

int TextureTracker::Holding(const Rect& r, bool* overlapped) const {
  const std::vector<int>& here = cells_[(r.y0 / kCell) * kCellsX + r.x0 / kCell];
  for (int index : here) {
    const Record& u = records_[index];
    if (r.x0 >= static_cast<int32_t>(u.x) && r.y0 >= static_cast<int32_t>(u.y) &&
        r.x1 < static_cast<int32_t>(u.x + u.w) && r.y1 < static_cast<int32_t>(u.y + u.h))
      return index;
  }
  *overlapped = false;
  for (int cy = r.y0 / kCell; cy <= r.y1 / kCell && !*overlapped; ++cy)
    for (int cx = r.x0 / kCell; cx <= r.x1 / kCell && !*overlapped; ++cx)
      for (int index : cells_[cy * kCellsX + cx]) {
        const Record& u = records_[index];
        if (Overlap(r.x0, r.y0, r.x1, r.y1, u.x, u.y, u.x + u.w - 1, u.y + u.h - 1))
          *overlapped = true;
      }
  return -1;
}

bool TextureTracker::Tiling(const Rect& r, std::vector<int>* records) const {
  // Records never overlap, so they tile `r` when the parts of it they cover add up to all of it.
  records->clear();
  int64_t covered = 0;
  for (int cy = r.y0 / kCell; cy <= r.y1 / kCell; ++cy)
    for (int cx = r.x0 / kCell; cx <= r.x1 / kCell; ++cx)
      for (int index : cells_[cy * kCellsX + cx]) {
        const Record& u = records_[index];
        if (!Overlap(r.x0, r.y0, r.x1, r.y1, u.x, u.y, u.x + u.w - 1, u.y + u.h - 1) ||
            std::find(records->begin(), records->end(), index) != records->end())
          continue;
        if (records->size() == kMostTiles)
          return false;
        records->push_back(index);
        const int64_t w = std::min<int64_t>(r.x1, u.x + u.w - 1) - std::max<int64_t>(r.x0, u.x) + 1;
        const int64_t h = std::min<int64_t>(r.y1, u.y + u.h - 1) - std::max<int64_t>(r.y0, u.y) + 1;
        covered += w * h;
      }
  if (records->empty())
    return false;
  const int64_t w = r.x1 - r.x0 + 1, h = r.y1 - r.y0 + 1;
  if (covered == w * h)
    return true;
  // What they leave uncovered may still be zeros - clear texels, never drawn - which a draw can
  // sweep over around its image: Einhander's planes read whole pages, a part of each its image
  // and the rest VRAM nothing wrote, or a scrap of the BIOS's logo long gone. Looked at
  // halfword by halfword, which is why it comes last.
  std::vector<uint8_t> mine(static_cast<size_t>(w * h), 0);
  for (int index : *records) {
    const Record& u = records_[index];
    for (int32_t y = std::max(r.y0, static_cast<int32_t>(u.y));
         y <= std::min(r.y1, static_cast<int32_t>(u.y + u.h) - 1); ++y)
      for (int32_t x = std::max(r.x0, static_cast<int32_t>(u.x));
           x <= std::min(r.x1, static_cast<int32_t>(u.x + u.w) - 1); ++x)
        mine[static_cast<size_t>(y - r.y0) * w + (x - r.x0)] = 1;
  }
  for (int32_t y = r.y0; y <= r.y1; ++y)
    for (int32_t x = r.x0; x <= r.x1; ++x)
      if (!mine[static_cast<size_t>(y - r.y0) * w + (x - r.x0)] && vram_[y * kVramW + x] != 0)
        return false;
  return true;
}

bool TextureTracker::AnyDrawn(const Rect& r) const {
  for (int cy = r.y0 / kCell; cy <= r.y1 / kCell; ++cy)
    for (int cx = r.x0 / kCell; cx <= r.x1 / kCell; ++cx)
      if (drawn_[cy * kCellsX + cx])
        return true;
  return false;
}

void TextureTracker::Register(int index) {
  const Record& u = records_[index];
  for (uint32_t cy = u.y / kCell; cy <= (u.y + u.h - 1) / kCell; ++cy)
    for (uint32_t cx = u.x / kCell; cx <= (u.x + u.w - 1) / kCell; ++cx)
      cells_[cy * kCellsX + cx].push_back(index);
}

void TextureTracker::Unregister(int index) {
  const Record& u = records_[index];
  for (uint32_t cy = u.y / kCell; cy <= (u.y + u.h - 1) / kCell; ++cy)
    for (uint32_t cx = u.x / kCell; cx <= (u.x + u.w - 1) / kCell; ++cx) {
      std::vector<int>& list = cells_[cy * kCellsX + cx];
      list.erase(std::remove(list.begin(), list.end(), index), list.end());
    }
}

int TextureTracker::AddRecord(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
  int index;
  if (!free_.empty()) {
    index = free_.back();
    free_.pop_back();
  } else {
    index = static_cast<int>(records_.size());
    records_.emplace_back();
  }
  Record& u = records_[index];
  u.x = x;
  u.y = y;
  u.w = w;
  u.h = h;
  u.hash = HashRect(vram_, x, y, w, h);
  u.serial = ++serial_;
  u.live = true;
  u.pixels.reset();
  u.sources.clear();
  Register(index);
  ++counts_.records_live;
  return index;
}

void TextureTracker::Remove(int index) {
  Record& u = records_[index];
  Close(index);
  Unregister(index);
  if (last_ == index)
    last_ = -1;
  u.live = false;
  free_.push_back(index);
  --counts_.records_live;
}

void TextureTracker::Cut(int index, const Rect& r) {
  const Record u = records_[index];
  const int32_t ux0 = static_cast<int32_t>(u.x), uy0 = static_cast<int32_t>(u.y);
  const int32_t ux1 = ux0 + static_cast<int32_t>(u.w) - 1;
  const int32_t uy1 = uy0 + static_cast<int32_t>(u.h) - 1;
  Remove(index);
  // Above it and below it the whole width; beside it only its own rows.
  const int32_t my0 = std::max(uy0, r.y0), my1 = std::min(uy1, r.y1);
  const Rect pieces[4] = { { ux0, uy0, ux1, r.y0 - 1 }, { ux0, r.y1 + 1, ux1, uy1 },
                           { ux0, my0, r.x0 - 1, my1 }, { r.x1 + 1, my0, ux1, my1 } };
  for (const Rect& p : pieces) {
    const Rect q = { std::max(p.x0, ux0), std::max(p.y0, uy0), std::min(p.x1, ux1),
                     std::min(p.y1, uy1) };
    if (q.x0 > q.x1 || q.y0 > q.y1)
      continue;
    const int kept = AddRecord(static_cast<uint32_t>(q.x0), static_cast<uint32_t>(q.y0),
                               static_cast<uint32_t>(q.x1 - q.x0 + 1),
                               static_cast<uint32_t>(q.y1 - q.y0 + 1));
    const Record& k = records_[kept];
    Trace("  leaving", k.x, k.y, k.w, k.h);
  }
}

void TextureTracker::WriteOver(const Rect& r, bool by_draw) {
  std::vector<int> hit;
  for (int cy = r.y0 / kCell; cy <= r.y1 / kCell; ++cy)
    for (int cx = r.x0 / kCell; cx <= r.x1 / kCell; ++cx)
      for (int index : cells_[cy * kCellsX + cx]) {
        const Record& u = records_[index];
        if (Overlap(r.x0, r.y0, r.x1, r.y1, u.x, u.y, u.x + u.w - 1, u.y + u.h - 1) &&
            std::find(hit.begin(), hit.end(), index) == hit.end())
          hit.push_back(index);
      }
  for (int index : hit) {
    const Record& u = records_[index];
    const bool whole = r.x0 <= static_cast<int32_t>(u.x) && r.y0 <= static_cast<int32_t>(u.y) &&
                       r.x1 >= static_cast<int32_t>(u.x + u.w) - 1 &&
                       r.y1 >= static_cast<int32_t>(u.y + u.h) - 1;
    Trace(by_draw ? (whole ? "drawn over:" : "drawn over in part:")
                  : (whole ? "  uploads over" : "  uploads over part of"),
          u.x, u.y, u.w, u.h);
    if (whole) {
      Remove(index);
      ++(by_draw ? counts_.records_retired : counts_.records_replaced);
    } else {
      Cut(index, r);
      ++counts_.records_cut;
    }
  }
}

// ---- What a job writes and reads -------------------------------------------------------------

void TextureTracker::Destination(const DrawJob& job, std::vector<Rect>* out) {
  out->clear();
  auto clipped = [&](int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    x0 = std::max({ x0, job.env.area_left, 0 });
    y0 = std::max({ y0, job.env.area_top, 0 });
    x1 = std::min({ x1, job.env.area_right, static_cast<int32_t>(kVramW) - 1 });
    y1 = std::min({ y1, job.env.area_bottom, static_cast<int32_t>(kVramH) - 1 });
    if (x0 <= x1 && y0 <= y1)
      out->push_back({ x0, y0, x1, y1 });
  };
  // Fills and copies ignore the drawing area and wrap round VRAM's edges.
  auto wrapped = [&](int32_t x, int32_t y, int32_t w, int32_t h) {
    if (w <= 0 || h <= 0)
      return;
    x &= kVramW - 1;
    y &= kVramH - 1;
    w = std::min<int32_t>(w, kVramW);
    h = std::min<int32_t>(h, kVramH);
    const int32_t xs[2][2] = { { x, std::min<int32_t>(x + w, kVramW) - 1 },
                               { 0, x + w - static_cast<int32_t>(kVramW) - 1 } };
    const int32_t ys[2][2] = { { y, std::min<int32_t>(y + h, kVramH) - 1 },
                               { 0, y + h - static_cast<int32_t>(kVramH) - 1 } };
    for (const auto& yr : ys)
      for (const auto& xr : xs)
        if (xr[0] <= xr[1] && yr[0] <= yr[1])
          out->push_back({ xr[0], yr[0], xr[1], yr[1] });
  };
  switch (job.kind) {
  case DrawJob::kTriangle:
  case DrawJob::kLine: {
    const int n = job.kind == DrawJob::kTriangle ? 3 : 2;
    int32_t x0 = job.v[0].x, y0 = job.v[0].y, x1 = x0, y1 = y0;
    for (int i = 1; i < n; ++i) {
      x0 = std::min(x0, job.v[i].x);
      y0 = std::min(y0, job.v[i].y);
      x1 = std::max(x1, job.v[i].x);
      y1 = std::max(y1, job.v[i].y);
    }
    clipped(x0, y0, x1, y1);
    break;
  }
  case DrawJob::kRectangle:
    clipped(job.x, job.y, job.x + job.w - 1, job.y + job.h - 1);
    break;
  case DrawJob::kFill:
  case DrawJob::kVramCopy:
    wrapped(job.x, job.y, job.w, job.h);
    break;
  }
}

bool TextureTracker::Sampled(const DrawJob& job, bool short_by, Rect* r, Rect* texels) {
  const RasterState& state = job.state;
  int32_t u0, u1, v0, v1;
  if (job.kind == DrawJob::kRectangle) {
    // A rectangle's texels run from its base, one a pixel, backwards where flipped; only the
    // columns and rows inside the drawing area are read.
    const int32_t c0 = std::max(0, job.env.area_left - job.x);
    const int32_t c1 = std::min(job.w - 1, job.env.area_right - job.x);
    const int32_t r0 = std::max(0, job.env.area_top - job.y);
    const int32_t r1 = std::min(job.h - 1, job.env.area_bottom - job.y);
    if (c0 > c1 || r0 > r1)
      return false;
    u0 = state.flip_x ? job.base_u - c1 : job.base_u + c0;
    u1 = state.flip_x ? job.base_u - c0 : job.base_u + c1;
    v0 = state.flip_y ? job.base_v - r1 : job.base_v + r0;
    v1 = state.flip_y ? job.base_v - r0 : job.base_v + r1;
    if (u0 < 0 || v0 < 0 || u1 > 255 || v1 > 255)
      return false;   // wraps round the page's 256 texels
  } else {
    u0 = std::min({ job.v[0].u, job.v[1].u, job.v[2].u });
    u1 = std::max({ job.v[0].u, job.v[1].u, job.v[2].u });
    v0 = std::min({ job.v[0].v, job.v[1].v, job.v[2].v });
    v1 = std::max({ job.v[0].v, job.v[1].v, job.v[2].v });
    if (short_by) {
      u1 = std::max(u0, u1 - 1);
      v1 = std::max(v0, v1 - 1);
    }
  }
  // The texture window replaces some bits of each coordinate: what is read is the range of the
  // coordinates as it maps them.
  auto window = [](int32_t* lo, int32_t* hi, uint32_t mask, uint32_t offset) {
    if (mask == 0)
      return;
    const int32_t keep = ~static_cast<int32_t>(mask * 8);
    const int32_t set = static_cast<int32_t>((offset & mask) * 8);
    int32_t a = 255, b = 0;
    for (int32_t t = *lo; t <= *hi; ++t) {
      const int32_t m = ((t & keep) | set) & 0xFF;
      a = std::min(a, m);
      b = std::max(b, m);
    }
    *lo = a;
    *hi = b;
  };
  window(&u0, &u1, job.env.tw_mask_x, job.env.tw_offset_x);
  window(&v0, &v1, job.env.tw_mask_y, job.env.tw_offset_y);
  *texels = { u0, v0, u1, v1 };
  const int shift = 2 - static_cast<int>(Mode(state));   // texels to a halfword: 4, 2, 1
  r->x0 = static_cast<int32_t>(state.texpage_x) + (u0 >> shift);
  r->x1 = static_cast<int32_t>(state.texpage_x) + (u1 >> shift);
  r->y0 = static_cast<int32_t>(state.texpage_y) + v0;
  r->y1 = static_cast<int32_t>(state.texpage_y) + v1;
  return r->x1 < static_cast<int32_t>(kVramW) && r->y1 < static_cast<int32_t>(kVramH);
}

TextureTracker::Reason TextureTracker::Find(const DrawJob& job, Use* use) const {
  if (job.kind == DrawJob::kRectangle &&
      (job.x > job.env.area_right || job.x + job.w - 1 < job.env.area_left ||
       job.y > job.env.area_bottom || job.y + job.h - 1 < job.env.area_top))
    return kOutside;
  Rect s, t;
  const bool triangle = job.kind == DrawJob::kTriangle;
  // A triangle's far edge is a texel past what it reads: only that edge may run off VRAM
  // (Xenogears' 64-texel page at x 960).
  const bool whole = Sampled(job, false, &s, &t);
  if (!whole && !(triangle && Sampled(job, true, &s, &t)))
    return kWraps;
  // One record holding it all; or several tiling it; and for a triangle, the same a texel short
  // at its far edges.
  bool overlapped = true;
  int record = Holding(s, &overlapped);
  bool tiled = record < 0 && Tiling(s, &use->records);
  if (whole && record < 0 && !tiled && triangle) {
    Rect s2, t2;
    bool overlapped2 = true;
    if (Sampled(job, true, &s2, &t2)) {
      record = Holding(s2, &overlapped2);
      tiled = record < 0 && Tiling(s2, &use->records);
      if (record >= 0 || tiled) {
        s = s2;
        t = t2;
      }
    }
  }
  if (record < 0 && !tiled)
    return AnyDrawn(s) ? kTextureDrawn : overlapped ? kSpans : kNotUploaded;
  use->sampled = s;
  use->texels = t;
  if (record >= 0) {
    use->records.assign(1, record);
    use->image_hash = records_[record].hash;
  } else {
    // A composite's hash: its records', and where each lies from their common corner, in the
    // order they lie in.
    std::vector<int>& parts = use->records;
    std::sort(parts.begin(), parts.end(), [&](int a, int b) {
      const Record &ra = records_[a], &rb = records_[b];
      return ra.y != rb.y ? ra.y < rb.y : ra.x < rb.x;
    });
    uint32_t x0 = kVramW, y0 = kVramH;
    for (int index : parts) {
      x0 = std::min(x0, records_[index].x);
      y0 = std::min(y0, records_[index].y);
    }
    uint64_t hash = 0xC0C0C0C0C0C0C0C0ull;
    for (int index : parts) {
      const Record& u = records_[index];
      hash = Mix(Mix(hash, u.hash), (static_cast<uint64_t>(u.x - x0) << 32) | (u.y - y0));
    }
    use->image_hash = hash;
  }
  const uint32_t mode = Mode(job.state);
  use->clut_hash = 0;
  if (mode < 2) {
    const uint32_t entries = mode == 0 ? 16 : 256;
    // A palette running past VRAM's right edge is read only as far as the edge: games index only
    // the entries that fit (Metal Gear Solid's 8-bit palettes start at x 928).
    const uint32_t fits = std::min(entries, kVramW - job.state.clut_x);
    const Rect c = { static_cast<int32_t>(job.state.clut_x), static_cast<int32_t>(job.state.clut_y),
                     static_cast<int32_t>(job.state.clut_x + fits - 1),
                     static_cast<int32_t>(job.state.clut_y) };
    // In one live record, or several tiling it (Castlevania's 256-entry palettes span two).
    bool clut_overlapped = true;
    const int palette = Holding(c, &clut_overlapped);
    std::vector<int> palette_tiles;
    if (palette < 0 && !Tiling(c, &palette_tiles))
      return AnyDrawn(c) ? kClutDrawn : kClutNotUploaded;
    // Live records have not changed since they came, so their palettes' hashes are kept by them.
    uint64_t holder = palette >= 0 ? records_[palette].serial : 0;
    for (int index : palette_tiles)
      holder = Mix(holder, records_[index].serial);
    const uint64_t key = Mix(Mix(holder, job.state.clut_x),
                             (static_cast<uint64_t>(job.state.clut_y) << 16) | fits);
    auto found = clut_hashes_.find(key);
    if (found == clut_hashes_.end()) {
      if (clut_hashes_.size() > (1u << 20))
        clut_hashes_.clear();
      found = clut_hashes_.emplace(key, HashRect(vram_, job.state.clut_x, job.state.clut_y,
                                                 fits, 1)).first;
    }
    use->clut_hash = found->second;
  }
  return kNamed;
}

TextureTracker::Reason TextureTracker::Classify(const DrawJob& job, std::string* name) const {
  Use use;
  const Reason reason = Find(job, &use);
  if (reason == kNamed && name != nullptr)
    *name = Name(use.image_hash, use.clut_hash, Mode(job.state));
  return reason;
}

std::string TextureTracker::Name(uint64_t image_hash, uint64_t clut_hash, uint32_t mode) {
  char text[64];
  snprintf(text, sizeof(text), "%016llx-%016llx-%s", static_cast<unsigned long long>(image_hash),
           static_cast<unsigned long long>(clut_hash), mode == 0 ? "4" : mode == 1 ? "8" : "15");
  return text;
}

// ---- Observing -------------------------------------------------------------------------------

void TextureTracker::Drawing(const DrawJob& job) {
  // A copy of uploaded texels - a palette moved into place (Silent Hill's, into the framebuffer's
  // corner), a texture out of a staging area - carries them along: the destination is recorded
  // too, once copied. Looked at before the copy, which may land over its own source.
  pending_copy_ = false;
  if (job.kind == DrawJob::kVramCopy && job.w > 0 && job.h > 0 &&
      job.src_x + job.w <= static_cast<int32_t>(kVramW) &&
      job.src_y + job.h <= static_cast<int32_t>(kVramH) &&
      job.x + job.w <= static_cast<int32_t>(kVramW) && job.y + job.h <= static_cast<int32_t>(kVramH)) {
    const Rect source = { job.src_x, job.src_y, job.src_x + job.w - 1, job.src_y + job.h - 1 };
    bool overlapped = true;
    std::vector<int> tiles;
    pending_copy_ = Holding(source, &overlapped) >= 0 || Tiling(source, &tiles);
  }
  pending_textured_ = (job.kind == DrawJob::kTriangle || job.kind == DrawJob::kRectangle) &&
                      job.state.textured;
  if (!pending_textured_)
    return;
  Use use;
  pending_ = Find(job, &use);
  if (pending_ != kNamed) {
    if (log_ != nullptr && log_left_[pending_] > 0) {
      --log_left_[pending_];
      Log(job, pending_);
    }
    return;
  }
  if (use.records.size() > 1)
    ++counts_.composite_jobs;
  const uint32_t mode = Mode(job.state);
  const uint64_t key = Mix(Mix(use.image_hash, use.clut_hash), mode);
  if (seen_.insert(key).second)
    ++counts_.sources;
  if (!dump_folder_.empty())
    Gather(job, use, key, Name(use.image_hash, use.clut_hash, mode));
}

void TextureTracker::Drawn(const DrawJob& job, uint64_t pixels) {
  if (pending_textured_) {
    ++counts_.textured_jobs;
    counts_.textured_pixels += pixels;
    ++counts_.jobs[pending_];
    counts_.pixels[pending_] += pixels;
    pending_textured_ = false;
  }
  // A job that put nothing down changed nothing - but fills and copies are not counted in pixels
  // by every rasteriser, so they always count as drawn.
  if (pixels == 0 && job.kind != DrawJob::kFill && job.kind != DrawJob::kVramCopy)
    return;
  Destination(job, &destination_);
  for (const Rect& r : destination_) {
    for (int cy = r.y0 / kCell; cy <= r.y1 / kCell; ++cy)
      for (int cx = r.x0 / kCell; cx <= r.x1 / kCell; ++cx)
        drawn_[cy * kCellsX + cx] = 1;
    WriteOver(r, true);
  }
  if (pending_copy_ && destination_.size() == 1) {
    const Rect& r = destination_[0];
    const int copied = AddRecord(static_cast<uint32_t>(r.x0), static_cast<uint32_t>(r.y0),
                                 static_cast<uint32_t>(r.x1 - r.x0 + 1),
                                 static_cast<uint32_t>(r.y1 - r.y0 + 1));
    const Record& c = records_[copied];
    Trace("copied from uploads, making", c.x, c.y, c.w, c.h);
    ++counts_.records_copied;
  }
  pending_copy_ = false;
}

void TextureTracker::Written(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
  DrawJob as_fill = {};
  as_fill.kind = DrawJob::kFill;
  as_fill.x = static_cast<int32_t>(x);
  as_fill.y = static_cast<int32_t>(y);
  as_fill.w = static_cast<int32_t>(w);
  as_fill.h = static_cast<int32_t>(h);
  Trace("upload", x, y, w, h);
  Destination(as_fill, &destination_);
  // Inside one live record, it is a patch to that image - a few glyphs of a font sheet, say: the
  // image stays, with what it holds now.
  if (destination_.size() == 1) {
    bool overlapped = false;
    const int patched = Holding(destination_[0], &overlapped);
    if (patched >= 0) {
      Record& u = records_[patched];
      Trace("  patches", u.x, u.y, u.w, u.h);
      Close(patched);
      u.hash = HashRect(vram_, u.x, u.y, u.w, u.h);
      u.serial = ++serial_;
      last_ = patched;
      ++counts_.uploads_patched;
      return;
    }
  }
  const std::vector<Rect> pieces = destination_;
  for (const Rect& r : pieces) {
    // What was there before is written over...
    WriteOver(r, false);
    // ...and a cell it covers whole holds nothing the GPU drew any more.
    for (int cy = (r.y0 + kCell - 1) / kCell; (cy + 1) * kCell - 1 <= r.y1; ++cy)
      for (int cx = (r.x0 + kCell - 1) / kCell; (cx + 1) * kCell - 1 <= r.x1; ++cx)
        drawn_[cy * kCellsX + cx] = 0;
  }
  // One that wraps round VRAM's edge is not an image anyone authored; it is not recorded.
  if (pieces.size() != 1) {
    Trace("  wraps, not recorded:", x, y, w, h);
    return;
  }
  // Carrying straight on from the last one - the next strip of the same image - it joins it, in
  // whichever direction: Ridge Racer loads one image a row at a time from the bottom up.
  if (last_ >= 0 && records_[last_].live) {
    const Record before = records_[last_];
    const bool column = before.x == x && before.w == w;
    const bool row = before.y == y && before.h == h;
    const bool below = column && before.y + before.h == y, above = column && y + h == before.y;
    const bool right = row && before.x + before.w == x, left = row && x + w == before.x;
    if (below || above || right || left) {
      Remove(last_);
      const int joined = AddRecord(left ? x : before.x, above ? y : before.y,
                                   below || above ? w : before.w + w,
                                   below || above ? before.h + h : h);
      last_ = joined;
      ++counts_.uploads_joined;
      const Record& j = records_[joined];
      Trace("  joins, making", j.x, j.y, j.w, j.h);
      return;
    }
  }
  last_ = AddRecord(x, y, w, h);
  ++counts_.uploads;
}

void TextureTracker::Reloaded() {
  // Whatever was uploaded before is not known to be anything now; what was gathered from it is
  // written first.
  Flush();
  records_.clear();
  free_.clear();
  last_ = -1;
  for (std::vector<int>& list : cells_)
    list.clear();
  drawn_.assign(drawn_.size(), 0);
  clut_hashes_.clear();
  counts_.records_live = 0;
}

void TextureTracker::Flush() {
  for (size_t i = 0; i < records_.size(); ++i)
    if (records_[i].live)
      Close(static_cast<int>(i));
}

// ---- Dumping ---------------------------------------------------------------------------------

void TextureTracker::Gather(const DrawJob& job, const Use& use, uint64_t key,
                            const std::string& name) {
  const uint32_t mode = Mode(job.state);
  const uint32_t per = PerHalfword(mode);
  auto found = open_.find(key);
  if (found == open_.end()) {
    Source source;
    source.name = name;
    source.mode = mode;
    uint32_t x0 = kVramW, y0 = kVramH, x1 = 0, y1 = 0;
    for (int index : use.records) {
      const Record& u = records_[index];
      x0 = std::min(x0, u.x);
      y0 = std::min(y0, u.y);
      x1 = std::max(x1, u.x + u.w);
      y1 = std::max(y1, u.y + u.h);
    }
    source.x = x0;
    source.y = y0;
    source.halfwords = x1 - x0;
    source.rows = y1 - y0;
    for (int index : use.records) {
      // Each record's halfwords, copied once while it is still what was uploaded, for every
      // source read from it.
      Record& u = records_[index];
      if (!u.pixels) {
        auto copy = std::make_shared<std::vector<uint16_t>>(static_cast<size_t>(u.w) * u.h);
        for (uint32_t row = 0; row < u.h; ++row) {
          const uint16_t* line = vram_ + (u.y + row) * kVramW + u.x;
          std::copy(line, line + u.w, copy->begin() + static_cast<size_t>(row) * u.w);
        }
        u.pixels = copy;
      }
      source.parts.push_back({ u.pixels, u.x - x0, u.y - y0, u.w, u.h });
      u.sources.push_back(key);
    }
    source.width = source.halfwords * per;
    source.height = source.rows;
    if (mode < 2) {
      // As far as VRAM's edge, as Find hashed it; the entries past it read as clear.
      const uint32_t entries = mode == 0 ? 16 : 256;
      const uint32_t fits = std::min(entries, kVramW - job.state.clut_x);
      const uint16_t* clut = vram_ + job.state.clut_y * kVramW + job.state.clut_x;
      source.clut.assign(clut, clut + fits);
      source.clut.resize(entries, 0);
    }
    source.cells_x = (source.width + 7) / 8;
    source.cells_y = (source.height + 7) / 8;
    source.used.assign(static_cast<size_t>(source.cells_x) * source.cells_y, 0);
    found = open_.emplace(key, std::move(source)).first;
  }
  // The texels it reads, in the source's own texel coordinates: the texture page's left edge may
  // lie before or after the source's.
  Source& source = found->second;
  const int32_t page_x = (static_cast<int32_t>(job.state.texpage_x) - static_cast<int32_t>(source.x)) *
                         static_cast<int32_t>(per);
  const int32_t page_y = static_cast<int32_t>(job.state.texpage_y) - static_cast<int32_t>(source.y);
  const int32_t cx0 = std::max(0, (page_x + use.texels.x0) / 8);
  const int32_t cy0 = std::max(0, (page_y + use.texels.y0) / 8);
  const int32_t cx1 = std::min(static_cast<int32_t>(source.cells_x) - 1, (page_x + use.texels.x1) / 8);
  const int32_t cy1 = std::min(static_cast<int32_t>(source.cells_y) - 1, (page_y + use.texels.y1) / 8);
  for (int32_t cy = cy0; cy <= cy1; ++cy)
    for (int32_t cx = cx0; cx <= cx1; ++cx)
      source.used[static_cast<size_t>(cy) * source.cells_x + cx] = 1;
}

void TextureTracker::Close(int record) {
  Record& u = records_[record];
  for (uint64_t key : u.sources) {
    auto found = open_.find(key);
    if (found == open_.end())
      continue;
    Write(found->second);
    open_.erase(found);
  }
  u.sources.clear();
  u.pixels.reset();
}

void TextureTracker::Write(const Source& source) {
  const uint32_t least = static_cast<uint32_t>(dump_min_);
  if (source.width < least || source.height < least)
    return;
  const std::string path = dump_folder_ + "/" + source.name + ".png";
  if (FILE* there = fopen(path.c_str(), "rb")) {
    fclose(there);
    return;   // a run before wrote it
  }
  // Its records laid out as they were in VRAM; a corner none of them covers holds 0000h, clear.
  std::vector<uint16_t> image(static_cast<size_t>(source.halfwords) * source.rows, 0);
  for (const Part& part : source.parts)
    for (uint32_t row = 0; row < part.h; ++row)
      std::copy(part.pixels->begin() + static_cast<size_t>(row) * part.w,
                part.pixels->begin() + static_cast<size_t>(row + 1) * part.w,
                image.begin() + static_cast<size_t>(part.y + row) * source.halfwords + part.x);
  const uint32_t per = PerHalfword(source.mode);
  const uint32_t bits = 16 / per;
  std::vector<uint8_t> rgba(static_cast<size_t>(source.width) * source.height * 4, 0);
  uint64_t used = 0;
  for (uint32_t cy = 0; cy < source.cells_y; ++cy)
    for (uint32_t cx = 0; cx < source.cells_x; ++cx) {
      if (!source.used[static_cast<size_t>(cy) * source.cells_x + cx])
        continue;
      for (uint32_t row = cy * 8; row < std::min(source.height, cy * 8 + 8); ++row)
        for (uint32_t col = cx * 8; col < std::min(source.width, cx * 8 + 8); ++col) {
          ++used;
          const uint16_t block = image[static_cast<size_t>(row) * source.halfwords + col / per];
          const uint32_t index = (block >> ((col % per) * bits)) & ((1u << bits) - 1);
          const uint16_t c = per == 1 ? block : source.clut[index];
          uint8_t* p = &rgba[(static_cast<size_t>(row) * source.width + col) * 4];
          if (c == 0)
            continue;   // not drawn: left clear
          p[0] = Widen(c & 31);
          p[1] = Widen((c >> 5) & 31);
          p[2] = Widen((c >> 10) & 31);
          p[3] = (c & 0x8000) != 0 ? 128 : 255;
        }
    }
  if (used == 0)
    return;
  const int w = static_cast<int>(source.width), h = static_cast<int>(source.height);
  if (WritePngFile(path, w, h, rgba.data())) {
    ++counts_.dumped;
    counts_.dumped_texels += static_cast<uint64_t>(source.width) * source.height;
    counts_.dumped_used += used;
  } else {
    ++counts_.dump_failures;
  }
}

// ---- Logs ------------------------------------------------------------------------------------

void TextureTracker::Trace(const char* what, uint32_t x, uint32_t y, uint32_t w,
                           uint32_t h) const {
  const int32_t x0 = static_cast<int32_t>(x), y0 = static_cast<int32_t>(y);
  if (!tracing_ || log_ == nullptr || w == 0 || h == 0 ||
      !Overlap(trace_.x0, trace_.y0, trace_.x1, trace_.y1, x0, y0,
               x0 + static_cast<int32_t>(w) - 1, y0 + static_cast<int32_t>(h) - 1))
    return;
  fprintf(log_, "texture trace: %s %u,%u %ux%u\n", what, x, y, w, h);
}

void TextureTracker::Log(const DrawJob& job, Reason reason) const {
  Rect s = {}, t = {};
  const bool inside = Sampled(job, false, &s, &t);
  fprintf(log_, "texture miss: %s; %s %d-bit page %u,%u clut %u,%u", ReasonName(reason),
          job.kind == DrawJob::kRectangle ? "rectangle" : "triangle",
          Mode(job.state) == 0 ? 4 : Mode(job.state) == 1 ? 8 : 15, job.state.texpage_x,
          job.state.texpage_y, job.state.clut_x, job.state.clut_y);
  if (job.kind == DrawJob::kRectangle)
    fprintf(log_, " uv %u,%u %dx%d%s%s", job.base_u, job.base_v, job.w, job.h,
            job.state.flip_x ? " flipped x" : "", job.state.flip_y ? " flipped y" : "");
  else
    fprintf(log_, " uv %u,%u %u,%u %u,%u", job.v[0].u, job.v[0].v, job.v[1].u, job.v[1].v,
            job.v[2].u, job.v[2].v);
  if (job.env.tw_mask_x != 0 || job.env.tw_mask_y != 0)
    fprintf(log_, " window %u,%u/%u,%u", job.env.tw_mask_x, job.env.tw_mask_y,
            job.env.tw_offset_x, job.env.tw_offset_y);
  if (!inside) {
    fprintf(log_, "\n");
    return;
  }
  fprintf(log_, " reads %d,%d-%d,%d; records there:", s.x0, s.y0, s.x1, s.y1);
  std::vector<int> listed;
  for (int cy = s.y0 / kCell; cy <= s.y1 / kCell; ++cy)
    for (int cx = s.x0 / kCell; cx <= s.x1 / kCell; ++cx)
      for (int index : cells_[cy * kCellsX + cx]) {
        const Record& u = records_[index];
        if (std::find(listed.begin(), listed.end(), index) != listed.end() ||
            !Overlap(s.x0, s.y0, s.x1, s.y1, u.x, u.y, u.x + u.w - 1, u.y + u.h - 1))
          continue;
        listed.push_back(index);
        fprintf(log_, " %u,%u %ux%u", u.x, u.y, u.w, u.h);
      }
  fprintf(log_, "\n");
}

}  // namespace psx
}  // namespace emulation
