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

// PGXP: the GTE's vertices as it worked them out, before rounding (Docs/Hardware-Renderer-Plan.md,
// phase 5).
//
// The GTE projects a vertex to a screen position with a fraction, and rounds it into SXY - two
// int16s in a 32-bit word. The game then moves that word about: MFC2 or SWC2 it out, store it
// into a display list, load it back, copy it between registers; DMA hands the list to the GPU.
// The rounding is the wobble of PlayStation 3D: a vertex jumps a whole pixel at a time.
//
// So beside every register and every word of RAM and the scratchpad there is a *shadow*: the
// unrounded position, and the 32-bit word it was made for. The instructions that move an SXY
// word about move its shadow with it; nothing else touches the shadows at all. Instead, a shadow
// is only ever used while its word still holds the value it was made for - `Matches` - so any
// other write to the word (an ALU result, a byte store, a DMA from the CD) has made it stale by
// changing the value, with no cost to find out. A word that happens to be written with exactly
// the value again keeps its shadow, which describes the same whole-pixel position anyway.
//
// When a polygon's vertex word reaches the GPU - by DMA from RAM, or stored to GP0 from a register
// - the shadow comes with it if it matches, and the hardware rasteriser draws the vertex where the
// GTE put it rather than where it was rounded to. The machine itself never sees any of this: it
// runs on the rounded words, exactly as without.

#include <cstdint>
#include <vector>

namespace emulation {
namespace psx {

  struct PreciseVertex {
    uint32_t value = 0;   // the SXY word this describes
    float x = 0.0f, y = 0.0f;   // screen position, unrounded
    float w = 0.0f;       // the depth it was projected from (SZ3); 0 when unknown
    // Where it was in the last picture minus where it is, in screen pixels, when the GTE found
    // it there (psx/vertex_motion.h, Docs/DLSS-Plan.md) - it goes wherever the position goes.
    float mx = 0.0f, my = 0.0f;
    bool moved = false;
    bool valid = false;

    bool Matches(uint32_t word) const { return valid && value == word; }
  };

  class Pgxp {
   public:
    // On while the setting asks for it and the hardware rasteriser is drawing. Turning it on
    // clears every shadow: whatever the words hold now was not tracked.
    bool enabled() const { return enabled_; }
    void set_enabled(bool on) {
      if (on == enabled_)
        return;
      enabled_ = on;
      if (on) {
        ram_.assign(kRamWords, PreciseVertex());
        for (PreciseVertex& shadow : registers_)
          shadow = PreciseVertex();
        for (PreciseVertex& shadow : scratchpad_)
          shadow = PreciseVertex();
      }
      else {
        std::vector<PreciseVertex>().swap(ram_);
      }
    }

    // A general-purpose register's shadow. $zero's is never valid.
    PreciseVertex& reg(uint32_t index) { return registers_[index & 31]; }

    // The shadow of the word at a physical address, or null if it is not RAM or the scratchpad.
    // RAM's 2 MB are mirrored four times over the first 8 MB, as the bus does.
    PreciseVertex* word(uint32_t physical) {
      if (physical < 0x00800000)
        return &ram_[(physical & 0x1FFFFC) >> 2];
      if ((physical & 0xFFFFFC00) == 0x1F800000)
        return &scratchpad_[(physical & 0x3FC) >> 2];
      return nullptr;
    }

    // A register moved to another - ADDU/OR with $zero, ADDIU/ORI with 0 - takes its shadow.
    void Move(uint32_t to, uint32_t from) {
      if (to != 0) {
        registers_[to] = registers_[from];
        sources_[to] = sources_[from];
      }
    }

    // Where a register's word was loaded from, for motion's address key (psx/vertex_motion.h,
    // Gte::MotionKey::kAddress): a vertex's place in its model's list stays put while its
    // coordinates are worked out afresh each frame. Kept by LW and register moves, and good
    // only while the register still holds the word loaded - checked by value, as shadows are.
    void set_source(uint32_t index, uint32_t value, uint32_t physical) {
      if (index != 0)
        sources_[index & 31] = { value, physical | 1u };
    }
    // The physical address, with bit 0 set, that `value` in register `index` was loaded from;
    // 0 if it was not.
    uint32_t source(uint32_t index, uint32_t value) const {
      const Source& source = sources_[index & 31];
      return source.value == value ? source.address : 0;
    }

    // A store to the GPU's GP0 port hands its shadow over this way: the CPU sets it before the
    // store and the port takes it if it matches the word stored.
    const PreciseVertex* store() const { return store_; }
    void set_store(const PreciseVertex* shadow) { store_ = shadow; }

   private:
    static const uint32_t kRamWords = 0x200000 / 4;

    struct Source {
      uint32_t value = 0;
      uint32_t address = 0;   // bit 0 set when known
    };

    bool enabled_ = false;
    PreciseVertex registers_[32];
    Source sources_[32];
    std::vector<PreciseVertex> ram_;   // 8 MB while on, nothing while off
    PreciseVertex scratchpad_[256];
    const PreciseVertex* store_ = nullptr;
  };

}  // namespace psx
}  // namespace emulation
