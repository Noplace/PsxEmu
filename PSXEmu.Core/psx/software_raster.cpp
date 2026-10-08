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
#include "psx/psx.h"
#include "psx/software_raster.h"

#include <algorithm>
#include <cstdlib>

// Moved here from gpu.cpp unchanged (phase 0 of Docs/Hardware-Renderer-Plan.md): the pixel code
// is the code the GPU always had, and every checksum in Test-Suite.md says so.

namespace emulation {
    namespace psx {

        namespace {

            // Dither matrix, applied to the 8-bit components before they are truncated to
            // the 5 bits VRAM stores. Without it, Gouraud shading bands visibly.
            const int8_t kDitherTable[4][4] = {
              { -4,  0, -3,  1 },
              {  2, -2,  3, -1 },
              { -3,  1, -4,  0 },
              {  3, -1,  2, -2 },
            };

            inline uint8_t Clamp8(int32_t v) {
                return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
            }

            inline uint16_t To15Bit(uint8_t r, uint8_t g, uint8_t b) {
                return static_cast<uint16_t>(((b >> 3) << 10) | ((g >> 3) << 5) | (r >> 3));
            }

            // 5-bit VRAM components are widened by replicating the top bits, so 0x1F maps
            // to 0xFF rather than 0xF8 and white stays white.
            inline uint8_t From5Bit(uint32_t c) {
                return static_cast<uint8_t>((c << 3) | (c >> 2));
            }

            // Top-left fill rule for a triangle edge, given as (dx, dy) of that edge in
            // the same a->b->c winding RasterTriangle normalises every triangle to
            // (positive signed area). Without this, a plain w >= 0 test accepts a pixel
            // sitting exactly on a shared edge for BOTH triangles that touch it - a
            // quad's own two halves along their shared diagonal, and any two adjacent
            // primitives that happen to share a screen-space edge. That is invisible for
            // opaque draws (the second one repaints the same colour) but for additive
            // semi-transparent draws it blends twice, leaving a bright seam exactly on
            // every such edge - which for a surface built from many small quads (the
            // ground, a creature's segmented body) shows up as a fine diagonal hatching
            // over the whole thing rather than one obviously-wrong line. Biasing a
            // non-top-left edge's test by -1 (integer coordinates only, so w is always a
            // whole number) makes exactly one of the two triangles that share an edge
            // claim it, matching the rule real GPUs use for the same reason.
            // Which way round "left" is depends on the winding, and
            // RasterTriangle normalises every triangle to a positive signed area
            // with y growing downwards. Under that, work the edge function out for a
            // vertical edge: an edge running *up* the screen (dy < 0) has the interior
            // to its right, which is a left edge and is kept; one running down
            // (dy > 0) is a right edge and is dropped. Horizontal edges are the
            // familiar way round - a top edge runs right (dx > 0).
            //
            // This had the vertical test the wrong way round, so it kept right edges
            // and dropped left ones. On its own that only moved which of two
            // neighbours owned a shared column. Once the raster loops became
            // half-open (`x < right`), though, the left-hand primitive could no
            // longer draw its rightmost column at all, and the right-hand one was
            // refusing the same column as "not a left edge" - so a shared column
            // between two semi-transparent primitives was drawn by neither and came
            // out as an unblended gap.
            inline int32_t EdgeBias(int32_t dx, int32_t dy) {
                const bool top_left = (dy < 0) || (dy == 0 && dx > 0);
                return top_left ? 0 : -1;
            }

        }  // namespace

        SoftwareRaster::SoftwareRaster(uint16_t* vram) : vram_(vram) {}

        void SoftwareRaster::Apply(const DrawJob& job) {
            env_ = job.env;
            command_ = job.command;
            switch (job.kind) {
            case DrawJob::kTriangle:
                RasterTriangle(job.v[0], job.v[1], job.v[2], job.state);
                break;
            case DrawJob::kLine:
                DrawLineSegment(job.v[0], job.v[1], job.state);
                break;
            case DrawJob::kRectangle:
                RasterRectangle(job);
                break;
            case DrawJob::kFill:
                RasterFill(job);
                break;
            case DrawJob::kVramCopy:
                RasterVramCopy(job);
                break;
            }
        }

        // Records a write into the watched rectangle against the command doing it.
        //
        // Wrapped to VRAM first, exactly as VramAt does, because that is the cell
        // actually written: a fill, transfer or copy that runs off the right or
        // bottom edge comes back round, and passing the unwrapped coordinate here
        // reported it against a rectangle that does not exist. A write landing
        // somewhere unexpected is precisely what this is for, so the one class of
        // write most worth catching was the one it could not see.
        void SoftwareRaster::NoteWatchWrite(uint32_t x, uint32_t y) {
            if (watch_.w == 0)
                return;
            x &= (kVramWidth - 1);
            y &= (kVramHeight - 1);
            if ((x - watch_.x) < watch_.w && (y - watch_.y) < watch_.h) {
                ++counters_.watch_writers[command_];
                ++counters_.watch_writes;
            }
        }

        // A rectangle lying wholly inside the drawing area, on a frame that skips no field: nothing is
        // clipped and no row is left alone, so a pixel is its colour or its texel and the write, and
        // the loop has nothing in it for the checks that settled that. An untextured, opaque one with
        // no mask to respect is a run of one value a row. Counts as RasterRectangle's generic loop
        // makes them, kept in locals.
        template <bool kTextured>
        void SoftwareRaster::ShadeRectangle(const DrawJob& job) {
            const RasterState& state = job.state;
            const bool check_mask = env_.check_mask;
            const bool force_mask = env_.force_set_mask;
            const bool semi = state.semi_transparent;
            const bool raw = state.raw_texture;
            const uint32_t mode = state.semi_mode;
            const bool watching = watch_.w != 0;
            const uint32_t u_keep = ~(env_.tw_mask_x * 8);
            const uint32_t u_set = (env_.tw_offset_x & env_.tw_mask_x) * 8;
            const uint32_t v_keep = ~(env_.tw_mask_y * 8);
            const uint32_t v_set = (env_.tw_offset_y & env_.tw_mask_y) * 8;
            const uint32_t page_x = state.texpage_x, page_y = state.texpage_y;
            const uint32_t clut_x = state.clut_x, clut_y = state.clut_y;
            const uint32_t depth = state.texpage_colors;
            const uint8_t r = job.r, g = job.g, b = job.b;
            const uint16_t flat = static_cast<uint16_t>(To15Bit(r, g, b) | (force_mask ? 0x8000 : 0));

            uint64_t pixels = 0, rejected = 0, transparent = 0, texels = 0;

            for (int32_t row = 0; row < job.h; ++row) {
                const uint32_t vy = static_cast<uint32_t>(job.y + row);
                uint16_t* const vram_row = &VramAt(0, vy);
                const uint32_t x0 = static_cast<uint32_t>(job.x);

                if (!kTextured && !semi && !check_mask) {
                    // The whole row is the one value, which the mask bit is in already.
                    std::fill(vram_row + x0, vram_row + x0 + job.w, flat);
                    pixels += static_cast<uint64_t>(job.w);
                    if (watching)
                        for (int32_t col = 0; col < job.w; ++col)
                            NoteWatchWrite(x0 + col, vy);
                    continue;
                }

                for (int32_t col = 0; col < job.w; ++col) {
                    uint8_t pr = r, pg = g, pb = b;
                    bool from_texture = false, texture_mask = false;
                    if (kTextured) {
                        // A flipped rectangle walks its texture backwards from the base.
                        const int32_t tu = state.flip_x ? (job.base_u - col) : (job.base_u + col);
                        const int32_t tv = state.flip_y ? (job.base_v - row) : (job.base_v + row);
                        uint32_t u = static_cast<uint8_t>(tu);
                        uint32_t v = static_cast<uint8_t>(tv);
                        u = ((u & u_keep) | u_set) & 0xFF;
                        v = ((v & v_keep) | v_set) & 0xFF;
                        ++texels;
                        uint16_t texel;
                        switch (depth) {
                        case 0: {   // 4 bits per texel, via CLUT
                            const uint16_t block = VramAt(page_x + (u / 4), page_y + v);
                            texel = VramAt(clut_x + ((block >> ((u & 3) * 4)) & 0x0F), clut_y);
                            break;
                        }
                        case 1: {   // 8 bits per texel, via CLUT
                            const uint16_t block = VramAt(page_x + (u / 2), page_y + v);
                            texel = VramAt(clut_x + ((block >> ((u & 1) * 8)) & 0xFF), clut_y);
                            break;
                        }
                        default:    // 15 bits per texel, direct
                            texel = VramAt(page_x + u, page_y + v);
                            break;
                        }
                        if (texel == 0) {   // fully transparent texel
                            ++transparent;
                            continue;
                        }
                        pr = From5Bit(texel & 0x1F);
                        pg = From5Bit((texel >> 5) & 0x1F);
                        pb = From5Bit((texel >> 10) & 0x1F);
                        if (!raw) {
                            pr = Clamp8((pr * r) >> 7);
                            pg = Clamp8((pg * g) >> 7);
                            pb = Clamp8((pb * b) >> 7);
                        }
                        from_texture = true;
                        texture_mask = (texel & 0x8000) != 0;
                    }

                    uint16_t& target = vram_row[(x0 + static_cast<uint32_t>(col)) & (kVramWidth - 1)];
                    if (check_mask && (target & 0x8000)) {
                        ++rejected;
                        continue;
                    }
                    if (semi && (!from_texture || texture_mask))
                        BlendSemiTransparent(&target, pr, pg, pb, mode);
                    else
                        target = To15Bit(pr, pg, pb);
                    const bool set_mask = force_mask || (from_texture && texture_mask);
                    target = static_cast<uint16_t>((target & 0x7FFF) | (set_mask ? 0x8000 : 0));
                    ++pixels;
                    if (watching)
                        NoteWatchWrite(x0 + static_cast<uint32_t>(col), vy);
                }
            }

            counters_.pixels += pixels;
            counters_.mask_rejected += rejected;
            counters_.transparent_texels += transparent;
            counters_.texels_by_depth[depth & 3] += texels;
        }

        // A rectangle's pixels, from the job the command left behind.
        void SoftwareRaster::RasterRectangle(const DrawJob& job) {
            if (!env_.skip_field && job.w > 0 && job.h > 0 && job.x >= env_.area_left &&
                job.x + job.w - 1 <= env_.area_right && job.y >= env_.area_top &&
                job.y + job.h - 1 <= env_.area_bottom) {
                if (job.state.textured)
                    ShadeRectangle<true>(job);
                else
                    ShadeRectangle<false>(job);
                return;
            }
            const RasterState& state = job.state;
            const int32_t x = job.x, y = job.y, w = job.w, h = job.h;
            const uint8_t r = job.r, g = job.g, b = job.b;
            const uint8_t base_u = job.base_u, base_v = job.base_v;
            for (int32_t row = 0; row < h; ++row) {
                for (int32_t col = 0; col < w; ++col) {
                    if (!state.textured) {
                        PlotPixel(x + col, y + row, r, g, b, state, false, false);
                        continue;
                    }
                    // A flipped rectangle walks its texture backwards from the base.
                    const int32_t tu = state.flip_x ? (base_u - col) : (base_u + col);
                    const int32_t tv = state.flip_y ? (base_v - row) : (base_v + row);
                    const uint16_t texel = SampleTexture(
                        static_cast<uint8_t>(tu), static_cast<uint8_t>(tv), state);
                    if (texel == 0) {  // fully transparent texel
                        ++counters_.transparent_texels;
                        continue;
                    }
                    uint8_t tr = From5Bit(texel & 0x1F);
                    uint8_t tg = From5Bit((texel >> 5) & 0x1F);
                    uint8_t tb = From5Bit((texel >> 10) & 0x1F);
                    if (!state.raw_texture) {
                        tr = Clamp8((tr * r) >> 7);
                        tg = Clamp8((tg * g) >> 7);
                        tb = Clamp8((tb * b) >> 7);
                    }
                    PlotPixel(x + col, y + row, tr, tg, tb, state, true,
                        (texel & 0x8000) != 0);
                }
            }
        }

        // A fill's rows. It ignores the drawing area and the mask bits - see
        // CmdFillRectangle for why it clips at the VRAM edge rather than wrapping -
        // but it does skip the displayed field.
        void SoftwareRaster::RasterFill(const DrawJob& job) {
            for (int32_t row = 0; row < job.h; ++row) {
                const int32_t vy = job.y + row;
                if (vy >= kVramHeight)
                    break;
                if (SkipsVramRow(vy)) {
                    ++counters_.field_skipped;
                    continue;
                }
                for (int32_t col = 0; col < job.w; ++col) {
                    const int32_t vx = job.x + col;
                    if (vx >= kVramWidth)
                        break;
                    VramAt(static_cast<uint32_t>(vx), static_cast<uint32_t>(vy)) =
                        job.fill_colour;
                    NoteWatchWrite(static_cast<uint32_t>(vx), static_cast<uint32_t>(vy));
                }
            }
        }

        // A VRAM-to-VRAM copy: read a pixel, write it, honouring the mask bits.
        void SoftwareRaster::RasterVramCopy(const DrawJob& job) {
            for (int32_t row = 0; row < job.h; ++row) {
                for (int32_t col = 0; col < job.w; ++col) {
                    const uint32_t sx = static_cast<uint32_t>(job.src_x + col);
                    const uint32_t sy = static_cast<uint32_t>(job.src_y + row);
                    const uint32_t dx = static_cast<uint32_t>(job.x + col);
                    const uint32_t dy = static_cast<uint32_t>(job.y + row);
                    const uint16_t pixel = VramAt(sx, sy);
                    if (env_.check_mask && (VramAt(dx, dy) & 0x8000))
                        continue;
                    VramAt(dx, dy) =
                        env_.force_set_mask ? (pixel | 0x8000) : pixel;
                    NoteWatchWrite(dx, dy);
                }
            }
        }

        uint16_t SoftwareRaster::SampleTexture(uint32_t u, uint32_t v, const RasterState& state) {
            ++counters_.texels_by_depth[state.texpage_colors & 3];

            // The texture window folds the coordinates before they index the page.
            u = (u & ~(env_.tw_mask_x * 8)) |
                ((env_.tw_offset_x & env_.tw_mask_x) * 8);
            v = (v & ~(env_.tw_mask_y * 8)) |
                ((env_.tw_offset_y & env_.tw_mask_y) * 8);
            u &= 0xFF;
            v &= 0xFF;

            switch (state.texpage_colors) {
            case 0: {  // 4 bits per texel, via CLUT
                const uint16_t block = VramAt(state.texpage_x + (u / 4), state.texpage_y + v);
                const uint32_t index = (block >> ((u & 3) * 4)) & 0x0F;
                return VramAt(state.clut_x + index, state.clut_y);
            }
            case 1: {  // 8 bits per texel, via CLUT
                const uint16_t block = VramAt(state.texpage_x + (u / 2), state.texpage_y + v);
                const uint32_t index = (block >> ((u & 1) * 8)) & 0xFF;
                return VramAt(state.clut_x + index, state.clut_y);
            }
            default:   // 15 bits per texel, direct
                return VramAt(state.texpage_x + u, state.texpage_y + v);
            }
        }

        void SoftwareRaster::BlendSemiTransparent(uint16_t* dst, uint8_t r, uint8_t g, uint8_t b,
            uint32_t mode) const {
            const uint16_t back = *dst;
            const int32_t br = From5Bit(back & 0x1F);
            const int32_t bg = From5Bit((back >> 5) & 0x1F);
            const int32_t bb = From5Bit((back >> 10) & 0x1F);

            int32_t nr, ng, nb;
            switch (mode) {
            case 0:  // B/2 + F/2
                nr = (br + r) / 2; ng = (bg + g) / 2; nb = (bb + b) / 2;
                break;
            case 1:  // B + F
                nr = br + r; ng = bg + g; nb = bb + b;
                break;
            case 2:  // B - F
                nr = br - r; ng = bg - g; nb = bb - b;
                break;
            default: // B + F/4
                nr = br + r / 4; ng = bg + g / 4; nb = bb + b / 4;
                break;
            }
            // The mask bit is not this function's to decide - PlotPixel sets it from the
            // texel and GP0(E6h) after this returns - so it is left clear here rather
            // than carried over from the pixel underneath.
            *dst = To15Bit(Clamp8(nr), Clamp8(ng), Clamp8(nb));
        }

        void SoftwareRaster::PlotPixel(int32_t x, int32_t y, uint8_t r, uint8_t g, uint8_t b,
            const RasterState& state, bool from_texture,
            bool texture_mask) {
            if (x < env_.area_left || x > env_.area_right ||
                y < env_.area_top || y > env_.area_bottom) {
                ++counters_.clipped;
                return;
            }
            // The field being displayed is left alone (bug 89). Every primitive
            // but a triangle comes through here; RasterTriangle settles it a
            // whole row at a time.
            if (SkipsVramRow(y)) {
                ++counters_.field_skipped;
                return;
            }

            WritePixel(x, y, r, g, b, state, from_texture, texture_mask);
        }

        // A pixel already known to be inside the drawing area and on a row that is
        // drawn: the mask check, the blend and the write.
        void SoftwareRaster::WritePixel(int32_t x, int32_t y, uint8_t r, uint8_t g, uint8_t b,
            const RasterState& state, bool from_texture,
            bool texture_mask) {
            uint16_t& target = VramAt(static_cast<uint32_t>(x), static_cast<uint32_t>(y));
            if (env_.check_mask && (target & 0x8000)) {
                ++counters_.mask_rejected;
                return;
            }

            // A textured pixel is only blended when its own mask bit says so; an
            // untextured one follows the primitive's semi-transparency flag.
            const bool blend = state.semi_transparent &&
                (!from_texture || texture_mask);

            if (blend) {
                BlendSemiTransparent(&target, r, g, b, state.semi_mode);
            }
            else {
                target = To15Bit(r, g, b);
            }

            // The mask bit written is GP0(E6h) bit 0: "0=TextureBit15, 1=ForceBit15=1"
            // (psx-spx). Forced, it is always set; otherwise a *textured* draw hands the
            // texel's own bit 15 straight through to the framebuffer, and an untextured
            // one writes zero. It is not the bit that was already there - the pixel is
            // being replaced, mask bit included.
            //
            // Only the forced half of that was modelled, so a texture's bit 15 reached
            // the blend decision above and then vanished. Silent Hill is what found it:
            // it draws its scene with textures whose bit 15 is set, which on hardware
            // marks those pixels, and then lays a flat semi-transparent quad over the
            // player with mask-checking on. Every pixel it covers should be rejected.
            // With nothing marked, the quad drew in full - a pale rectangle around the
            // character, exactly the size of the quad (bug 83).
            const bool set_mask =
                env_.force_set_mask || (from_texture && texture_mask);
            target = static_cast<uint16_t>((target & 0x7FFF) | (set_mask ? 0x8000 : 0));

            ++counters_.pixels;

            NoteWatchWrite(static_cast<uint32_t>(x), static_cast<uint32_t>(y));
        }

        void SoftwareRaster::RasterTriangle(const RasterVertex& v0, const RasterVertex& v1, const RasterVertex& v2,
            const RasterState& state) {
            // Hardware rejects any primitive spanning more than 1023x511.
            const int32_t min_x = std::min(v0.x, std::min(v1.x, v2.x));
            const int32_t max_x = std::max(v0.x, std::max(v1.x, v2.x));
            const int32_t min_y = std::min(v0.y, std::min(v1.y, v2.y));
            const int32_t max_y = std::max(v0.y, std::max(v1.y, v2.y));
            if (max_x - min_x >= 1024 || max_y - min_y >= 512)
                return;

            // Two different kinds of bound, and treating them alike loses a column and a row.
            //
            // A primitive's own extent is half-open: a quad given x=0 and x=640 covers columns 0 to
            // 639, which is why max_x and max_y give up their last pixel here rather than in the
            // loop. The drawing area is not - GP0(E4) states an *inclusive* bottom-right corner,
            // which is how the per-pixel clip in Plot() has always read it (`x > draw_area_right_`
            // rejects, so right itself is inside).
            //
            // Clipping with an exclusive bound against an inclusive limit threw away the last
            // column and row of the drawing area whenever a primitive reached them - which the
            // BIOS's own background does, drawn as (0,0)-(640,480) against an area of 639x479. The
            // whole of column 639 and row 479 went unpainted, 1,117 pixels of a 640x478 screen.
            const int32_t left = std::max(min_x, env_.area_left);
            const int32_t right = std::min(max_x - 1, env_.area_right);
            const int32_t top = std::max(min_y, env_.area_top);
            const int32_t bottom = std::min(max_y - 1, env_.area_bottom);
            if (left > right || top > bottom)
                return;

            const int32_t area = (v1.x - v0.x) * (v2.y - v0.y) -
                (v2.x - v0.x) * (v1.y - v0.y);
            if (area == 0)
                return;

            // Work in a consistent winding so the edge functions share a sign test.
            const RasterVertex& a = v0;
            const RasterVertex& b = (area > 0) ? v1 : v2;
            const RasterVertex& c = (area > 0) ? v2 : v1;
            const int32_t double_area = (area > 0) ? area : -area;

            // The fill rule, for every triangle: a pixel exactly on an edge belongs
            // to the triangle for which that edge is a top or left one, so of two
            // triangles sharing an edge exactly one draws it, and a triangle alone
            // leaves its right and bottom edges undrawn. The hardware leaves them
            // out of every polygon, whatever the blending (psx-spx).
            //
            // It used to apply to semi-transparent triangles only (bug 105). Silent
            // Hill's hatching - an additive blend applied twice on a shared edge -
            // was where it came in, and applying it to everything put seams through
            // Wild Arms' field. But the rule was upside down then (bug 59), keeping
            // right edges and dropping left ones; with it the right way round, the
            // same field scene shows no seams, and opaque triangles no longer come
            // out a pixel fatter on their right and bottom diagonals than the
            // hardware draws them, or depend on draw order for who owns a shared
            // edge.
            const int32_t bias0 = EdgeBias(b.x - a.x, b.y - a.y);
            const int32_t bias1 = EdgeBias(c.x - b.x, c.y - b.y);
            const int32_t bias2 = EdgeBias(a.x - c.x, a.y - c.y);

            // The three edge functions, and every quantity interpolated between the corners, are
            // linear in x and y: each is its value at the first pixel of a row plus a fixed step
            // per pixel, and per row. Stepped in integers that is exact - the numbers each pixel
            // used to work out from scratch, with all its multiplications, every time.
            struct Linear {
                int32_t at;   // at (left, top), and then at the start of each row
                int32_t dx;   // what one pixel to the right adds
                int32_t dy;   // what one row down adds
            };
            Linear e0{ (b.x - a.x) * (top - a.y) - (b.y - a.y) * (left - a.x), a.y - b.y, b.x - a.x };
            Linear e1{ (c.x - b.x) * (top - b.y) - (c.y - b.y) * (left - b.x), b.y - c.y, c.x - b.x };
            Linear e2{ (a.x - c.x) * (top - c.y) - (a.y - c.y) * (left - c.x), c.y - a.y, a.x - c.x };

            // A quantity's barycentric sum - w1 belongs to a, w2 to b, w0 to c - which divided
            // by the doubled area is its value at the pixel.
            auto weighted = [&](int32_t at_a, int32_t at_b, int32_t at_c) {
                return Linear{ e1.at * at_a + e2.at * at_b + e0.at * at_c,
                               e1.dx * at_a + e2.dx * at_b + e0.dx * at_c,
                               e1.dy * at_a + e2.dy * at_b + e0.dy * at_c };
            };
            Linear red{}, green{}, blue{}, tex_u{}, tex_v{};
            if (state.gouraud) {
                red = weighted(a.r, b.r, c.r);
                green = weighted(a.g, b.g, c.g);
                blue = weighted(a.b, b.b, c.b);
            }
            if (state.textured) {
                tex_u = weighted(a.u, b.u, c.u);
                tex_v = weighted(a.v, b.v, c.v);
            }

            // Everything the rows need, handed to the loop that is built for this triangle's kind.
            TriSetup setup;
            setup.left = left; setup.right = right; setup.top = top; setup.bottom = bottom;
            setup.double_area = double_area;
            setup.bias[0] = bias0; setup.bias[1] = bias1; setup.bias[2] = bias2;
            setup.edge[0] = { e0.at, e0.dx, e0.dy };
            setup.edge[1] = { e1.at, e1.dx, e1.dy };
            setup.edge[2] = { e2.at, e2.dx, e2.dy };
            setup.red = { red.at, red.dx, red.dy };
            setup.green = { green.at, green.dx, green.dy };
            setup.blue = { blue.at, blue.dx, blue.dy };
            setup.tex_u = { tex_u.at, tex_u.dx, tex_u.dy };
            setup.tex_v = { tex_v.at, tex_v.dx, tex_v.dy };
            setup.flat_r = a.r; setup.flat_g = a.g; setup.flat_b = a.b;

            const int kind = (state.gouraud ? 4 : 0) | (state.textured ? 2 : 0) | (state.dither ? 1 : 0);
            switch (kind) {
            case 0: ShadeRows<false, false, false>(setup, state); break;
            case 1: ShadeRows<false, false, true>(setup, state); break;
            case 2: ShadeRows<false, true, false>(setup, state); break;
            case 3: ShadeRows<false, true, true>(setup, state); break;
            case 4: ShadeRows<true, false, false>(setup, state); break;
            case 5: ShadeRows<true, false, true>(setup, state); break;
            case 6: ShadeRows<true, true, false>(setup, state); break;
            default: ShadeRows<true, true, true>(setup, state); break;
            }
        }

        // The triangle's rows, for one kind of triangle - whether it is Gouraud-shaded, textured and
        // dithered being known as it is compiled rather than asked of every pixel - and so a loop with
        // nothing in it but what its pixels do.
        //
        // The covered pixels of a row are one run: each of the three edge functions is a line in x and a
        // pixel is covered when all three are not negative, so each edge cuts the row at a place a
        // division finds, and only what is between the cuts is visited - not every pixel of the
        // bounding box tested three times. The same pixels, the same colour and texel each, and the same
        // counts, which are kept in locals and added to the rasteriser's at the end.
        template <bool kGouraud, bool kTextured, bool kDither>
        void SoftwareRaster::ShadeRows(const TriSetup& t, const RasterState& state) {
            const bool check_mask = env_.check_mask;
            const bool force_mask = env_.force_set_mask;
            const bool semi = state.semi_transparent;
            const bool raw = state.raw_texture;
            const uint32_t mode = state.semi_mode;
            const bool watching = watch_.w != 0;
            const int32_t area = t.double_area;
            // The texture window folds the coordinates before they index the page.
            const uint32_t u_keep = ~(env_.tw_mask_x * 8);
            const uint32_t u_set = (env_.tw_offset_x & env_.tw_mask_x) * 8;
            const uint32_t v_keep = ~(env_.tw_mask_y * 8);
            const uint32_t v_set = (env_.tw_offset_y & env_.tw_mask_y) * 8;
            const uint32_t page_x = state.texpage_x, page_y = state.texpage_y;
            const uint32_t clut_x = state.clut_x, clut_y = state.clut_y;
            const uint32_t depth = state.texpage_colors;

            uint64_t pixels = 0, rejected = 0, transparent = 0, texels = 0, skipped = 0;

            int32_t w[3] = { t.edge[0].at, t.edge[1].at, t.edge[2].at };
            // Each is a uint32_t that wraps as the int32_t sums it replaces did.
            uint32_t row_r = static_cast<uint32_t>(t.red.at), row_g = static_cast<uint32_t>(t.green.at);
            uint32_t row_b = static_cast<uint32_t>(t.blue.at);
            uint32_t row_u = static_cast<uint32_t>(t.tex_u.at), row_v = static_cast<uint32_t>(t.tex_v.at);

            for (int32_t y = t.top; y <= t.bottom; ++y) {
                // Where the three edges let this row start and stop, as offsets from `left`.
                int32_t lo = 0, hi = t.right - t.left;
                bool empty = false;
                for (int i = 0; i < 3; ++i) {
                    const int32_t c = w[i] + t.bias[i];     // the edge function at `left`, biased
                    const int32_t dx = t.edge[i].dx;
                    if (dx == 0) {
                        if (c < 0)
                            empty = true;
                    }
                    else if (dx > 0) {
                        if (c < 0)
                            lo = std::max(lo, (-c + dx - 1) / dx);   // c + offset * dx >= 0
                    }
                    else {
                        if (c < 0)
                            empty = true;
                        else
                            hi = std::min(hi, c / -dx);
                    }
                }

                if (!empty && lo <= hi) {
                    if (SkipsVramRow(y)) {
                        // The displayed field's row, which hardware leaves alone (bug 89): what the
                        // triangle covers of it is counted, and nothing is worked out for it.
                        skipped += static_cast<uint64_t>(hi - lo + 1);
                    }
                    else {
                        uint16_t* const vram_row = &VramAt(0, static_cast<uint32_t>(y));
                        const uint32_t offset = static_cast<uint32_t>(lo);
                        uint32_t sum_r = row_r + offset * static_cast<uint32_t>(t.red.dx);
                        uint32_t sum_g = row_g + offset * static_cast<uint32_t>(t.green.dx);
                        uint32_t sum_b = row_b + offset * static_cast<uint32_t>(t.blue.dx);
                        uint32_t sum_u = row_u + offset * static_cast<uint32_t>(t.tex_u.dx);
                        uint32_t sum_v = row_v + offset * static_cast<uint32_t>(t.tex_v.dx);
                        const int32_t x_end = t.left + hi;
                        for (int32_t x = t.left + lo; x <= x_end; ++x) {
                            uint8_t r, g, bl;
                            if (kGouraud) {
                                r = Clamp8(static_cast<int32_t>(sum_r) / area);
                                g = Clamp8(static_cast<int32_t>(sum_g) / area);
                                bl = Clamp8(static_cast<int32_t>(sum_b) / area);
                                sum_r += static_cast<uint32_t>(t.red.dx);
                                sum_g += static_cast<uint32_t>(t.green.dx);
                                sum_b += static_cast<uint32_t>(t.blue.dx);
                            }
                            else {
                                r = t.flat_r; g = t.flat_g; bl = t.flat_b;
                            }
                            if (kDither) {
                                const int8_t dither = kDitherTable[y & 3][x & 3];
                                r = Clamp8(r + dither);
                                g = Clamp8(g + dither);
                                bl = Clamp8(bl + dither);
                            }

                            bool from_texture = false, texture_mask = false;
                            if (kTextured) {
                                uint32_t u = static_cast<uint32_t>(static_cast<int32_t>(sum_u) / area);
                                uint32_t v = static_cast<uint32_t>(static_cast<int32_t>(sum_v) / area);
                                sum_u += static_cast<uint32_t>(t.tex_u.dx);
                                sum_v += static_cast<uint32_t>(t.tex_v.dx);
                                u = ((u & u_keep) | u_set) & 0xFF;
                                v = ((v & v_keep) | v_set) & 0xFF;
                                ++texels;
                                uint16_t texel;
                                switch (depth) {
                                case 0: {   // 4 bits per texel, via CLUT
                                    const uint16_t block = VramAt(page_x + (u / 4), page_y + v);
                                    texel = VramAt(clut_x + ((block >> ((u & 3) * 4)) & 0x0F), clut_y);
                                    break;
                                }
                                case 1: {   // 8 bits per texel, via CLUT
                                    const uint16_t block = VramAt(page_x + (u / 2), page_y + v);
                                    texel = VramAt(clut_x + ((block >> ((u & 1) * 8)) & 0xFF), clut_y);
                                    break;
                                }
                                default:    // 15 bits per texel, direct
                                    texel = VramAt(page_x + u, page_y + v);
                                    break;
                                }
                                if (texel == 0) {   // fully transparent texel
                                    ++transparent;
                                    continue;
                                }
                                uint8_t tr = From5Bit(texel & 0x1F);
                                uint8_t tg = From5Bit((texel >> 5) & 0x1F);
                                uint8_t tb = From5Bit((texel >> 10) & 0x1F);
                                if (!raw) {
                                    tr = Clamp8((tr * r) >> 7);
                                    tg = Clamp8((tg * g) >> 7);
                                    tb = Clamp8((tb * bl) >> 7);
                                }
                                r = tr; g = tg; bl = tb;
                                from_texture = true;
                                texture_mask = (texel & 0x8000) != 0;
                            }

                            // WritePixel: the mask check, the blend, and the mask bit written.
                            uint16_t& target = vram_row[static_cast<uint32_t>(x) & (kVramWidth - 1)];
                            if (check_mask && (target & 0x8000)) {
                                ++rejected;
                                continue;
                            }
                            if (semi && (!from_texture || texture_mask))
                                BlendSemiTransparent(&target, r, g, bl, mode);
                            else
                                target = To15Bit(r, g, bl);
                            const bool set_mask = force_mask || (from_texture && texture_mask);
                            target = static_cast<uint16_t>((target & 0x7FFF) | (set_mask ? 0x8000 : 0));
                            ++pixels;
                            if (watching)
                                NoteWatchWrite(static_cast<uint32_t>(x), static_cast<uint32_t>(y));
                        }
                    }
                }

                for (int i = 0; i < 3; ++i)
                    w[i] += t.edge[i].dy;
                row_r += static_cast<uint32_t>(t.red.dy); row_g += static_cast<uint32_t>(t.green.dy);
                row_b += static_cast<uint32_t>(t.blue.dy);
                row_u += static_cast<uint32_t>(t.tex_u.dy); row_v += static_cast<uint32_t>(t.tex_v.dy);
            }

            counters_.pixels += pixels;
            counters_.mask_rejected += rejected;
            counters_.transparent_texels += transparent;
            counters_.texels_by_depth[depth & 3] += texels;
            counters_.field_skipped += skipped;
        }

        void SoftwareRaster::DrawLineSegment(const RasterVertex& v0, const RasterVertex& v1,
            const RasterState& state) {
            int32_t x = v0.x;
            int32_t y = v0.y;
            const int32_t dx = std::abs(v1.x - v0.x);
            const int32_t dy = -std::abs(v1.y - v0.y);
            const int32_t step_x = (v0.x < v1.x) ? 1 : -1;
            const int32_t step_y = (v0.y < v1.y) ? 1 : -1;
            int32_t error = dx + dy;
            const int32_t steps = std::max(dx, -dy);

            for (int32_t i = 0; ; ++i) {
                uint8_t r = v0.r, g = v0.g, b = v0.b;
                if (state.gouraud && steps > 0) {
                    r = Clamp8(v0.r + ((v1.r - v0.r) * i) / steps);
                    g = Clamp8(v0.g + ((v1.g - v0.g) * i) / steps);
                    b = Clamp8(v0.b + ((v1.b - v0.b) * i) / steps);
                }
                if (state.dither) {
                    const int8_t offset = kDitherTable[y & 3][x & 3];
                    r = Clamp8(r + offset);
                    g = Clamp8(g + offset);
                    b = Clamp8(b + offset);
                }
                PlotPixel(x, y, r, g, b, state, false, false);

                if (x == v1.x && y == v1.y)
                    break;
                const int32_t error2 = 2 * error;
                if (error2 >= dy) { error += dy; x += step_x; }
                if (error2 <= dx) { error += dx; y += step_y; }
            }
        }

    }  // namespace psx
}  // namespace emulation
