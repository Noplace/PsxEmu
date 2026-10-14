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
#pragma once

// Where things were in the last picture (Docs/DLSS-Plan.md, phase 2) - what DLSS needs to know of
// every pixel, and what the PlayStation never says: each frame is a fresh list of polygons, with
// nothing to tell which of last frame's this one is.
//
// So each thing drawn is given a key that ought to be the same from one picture to the next - a
// vertex's coordinates in its model, a sprite's texture and size - and remembered under it with
// where it was. Next picture, the same key finds it again. A key several things share - the same
// model drawn twice, a tile repeated across a map - is settled by distance: the nearest of the
// last picture's positions under it, if it is near enough to be the same thing moved rather than
// another one. Nothing found is "not known", never a guess.
//
// Two tables: this picture's, being filled, and the last's, being searched. NewPicture turns one
// into the other. Nothing here is machine state: it changes nothing the machine does, and is not
// saved.

#include <algorithm>
#include <cstdint>
#include <vector>

namespace emulation {
namespace psx {

class VertexMotion {
 public:
  // How far a thing may move between two pictures and still be taken for the same one, in
  // screen pixels. Beyond it the nearest is more likely another thing than this one moved.
  // Measured on Ridge Racer's attract mode, whose camera swings fast: 64 lost a tenth of the
  // vertices and half again as many pictures to fresh starts as 128.
  static constexpr float kReach = 128.0f;
  void set_reach(float reach) { reach_ = reach; }

  VertexMotion() {
    for (int i = 0; i < 2; ++i) {
      heads_[i].assign(kBuckets, 0);
      seen_[i].reserve(4096);
    }
  }

  // Whether the last picture had something under `key` within `reach` of (x, y) - by default
  // set_reach's - and if so (*dx, *dy) is the nearest's position minus this one.
  bool Find(uint64_t key, float x, float y, float* dx, float* dy, float reach = 0.0f) const {
    const uint32_t bucket = Bucket(key);
    const std::vector<Seen>& last = seen_[last_];
    const float limit = reach > 0.0f ? reach : reach_;
    float best = limit * limit;
    const Seen* nearest = nullptr;
    for (uint32_t at = heads_[last_][bucket]; at != 0; at = last[at - 1].next) {
      const Seen& seen = last[at - 1];
      if (seen.key != key)
        continue;
      const float ex = seen.x - x, ey = seen.y - y;
      const float d = ex * ex + ey * ey;
      if (d <= best) {
        best = d;
        nearest = &seen;
      }
    }
    if (nearest == nullptr)
      return false;
    *dx = nearest->x - x;
    *dy = nearest->y - y;
    return true;
  }

  // `key` is at (x, y) in this picture, for the next to find.
  void Remember(uint64_t key, float x, float y) {
    std::vector<Seen>& now = seen_[last_ ^ 1];
    if (now.size() >= kMostPerPicture)
      return;
    std::vector<uint32_t>& heads = heads_[last_ ^ 1];
    const uint32_t bucket = Bucket(key);
    now.push_back({ key, x, y, heads[bucket] });
    heads[bucket] = static_cast<uint32_t>(now.size());
  }

  // Find, then Remember - the whole of it for one key.
  bool FindAndRemember(uint64_t key, float x, float y, float* dx, float* dy) {
    const bool found = Find(key, x, y, dx, dy);
    Count(found);
    Remember(key, x, y);
    return found;
  }

  // A picture has ended: what was remembered in it is what the next one searches.
  void NewPicture() {
    last_ ^= 1;
    std::vector<Seen>& now = seen_[last_ ^ 1];
    std::vector<uint32_t>& heads = heads_[last_ ^ 1];
    for (const Seen& seen : now)
      heads[Bucket(seen.key)] = 0;
    now.clear();
  }

  // Forgets both pictures: nothing drawn before is the same as anything after - a state loaded,
  // the machine reset.
  void Forget() {
    NewPicture();
    NewPicture();
  }

  // Things looked for since the counts were last taken, and found.
  void Count(bool found) {
    ++looked_;
    if (found)
      ++found_;
  }
  uint64_t looked() const { return looked_; }
  uint64_t found() const { return found_; }
  void ClearCounts() { looked_ = found_ = 0; }

 private:
  static constexpr uint32_t kBuckets = 1u << 16;
  // A picture with more than this many things in it forgets the rest rather than growing.
  static constexpr size_t kMostPerPicture = 1u << 20;

  struct Seen {
    uint64_t key;
    float x, y;
    uint32_t next;   // the one before it in the same bucket, 1-based; 0 ends the chain
  };

  static uint32_t Bucket(uint64_t key) {
    key ^= key >> 29;
    key *= 0xBF58476D1CE4E5B9ull;
    key ^= key >> 32;
    return static_cast<uint32_t>(key) & (kBuckets - 1);
  }

  std::vector<Seen> seen_[2];
  std::vector<uint32_t> heads_[2];
  int last_ = 0;   // which of the two is the last picture's
  float reach_ = kReach;
  uint64_t looked_ = 0, found_ = 0;
};

}  // namespace psx
}  // namespace emulation
