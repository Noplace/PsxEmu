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

// Which texture each draw uses, by a name that is the same from one run to the next
// (Docs/Texture-Upscaling-Plan.md, phase 1). The PlayStation has no texture objects: a draw names
// a texture page, a colour mode and a CLUT, and reads VRAM texel by texel. A game loads a texture
// as it was authored, though - CPU-to-VRAM transfers of the image and of its palette - and those
// transfers are what is named here.
//
// - **Uploads.** Each transfer is recorded with a hash of its pixels. Records never overlap.
//   - One that carries straight on from the last one recorded - the same columns from the row it
//     ended or began at, or the same rows from the column - joins it: games load an image in
//     strips (Ridge Racer: 32 rows at a time, and another a row at a time from the bottom up).
//   - One that lands inside a live record patches it - a font sheet with a few glyphs changed -
//     and the record's hash is taken again.
//   - Anything written over part of a record - an upload, or a draw: any job whose destination
//     overlaps it - cuts that rectangle out of it, and what is left stays as up to four records,
//     each hashed afresh: Gran Turismo 2 loads the whole of VRAM at once, then draws its
//     framebuffer into it. Written over whole, a record is gone.
//   - A VRAM-to-VRAM copy of uploaded texels carries them along: the copy is recorded too
//     (Silent Hill copies its palettes into the framebuffer's corner).
// - **Sources.** A textured job names a source when the texels it can sample lie inside one live
//   record - or are tiled exactly by several, a composite (Castlevania's backdrops, uploaded as
//   64x128 tiles a row at a time), with whatever they leave uncovered all zeros, the clear texel
//   (Einhander's planes read whole pages around quarter-page images) - and, for 4- and 8-bit
//   textures, its CLUT lies inside one or is tiled the same way, as far as VRAM's edge. The
//   name is the record's hash (a composite's, its records' hashes and places), the CLUT's (its 16
//   or 256 entries) and the mode - never where in VRAM it is, so a pack works wherever the game
//   puts the image. A triangle's furthest texture coordinates are usually not read - the fill
//   rule leaves its far edges out - so one that names nothing is tried again a texel short.
// - **Why not.** Every other textured job is counted with the reason - the texels or the CLUT drawn
//   by the GPU, only partly uploaded, never seen uploaded (before a state load, say), wrapping
//   round VRAM or the texture page, or outside the drawing area - and the pixels it drew, so a run
//   says what share of a game's textured pixels can be named at all.
// - **Dumping.** With a folder set, each source is written there as `<image>-<clut>-<mode>.png`:
//   its records' extent in texels, with only the 8x8-texel cells draws read through that palette
//   filled in and the rest left clear - a sheet that a game reads with 91 palettes, a corner each
//   (Ridge Racer), is otherwise 91 copies of the whole sheet in 91 wrong colours. Alpha is 0 where
//   a texel is 0000h (not drawn), 128 where bit 15 is set (semi-transparent), 255 elsewhere. A
//   source is written once one of its records stops being what it was - written over, patched,
//   joined - or at a state load, or Flush; from copies of the records taken while they were whole.
//   A file already there is left as it is.
//
// An observer of `Gpu` (render/raster.h): it reads native VRAM and changes nothing the machine
// sees. Upload records are not machine state either; after a state load nothing is named until
// the game uploads again.

#include "render/raster.h"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace emulation {
namespace psx {

  class TextureTracker : public RasterObserver {
   public:
    // Why a textured job did, or did not, name a source.
    enum Reason {
      kNamed,
      kTextureDrawn,       // its texels were drawn by the GPU since they were uploaded
      kClutDrawn,          // ...its palette was
      kSpans,              // its texels are only partly in live uploads
      kNotUploaded,        // no upload of them was seen (before a state load, or the BIOS's)
      kClutNotUploaded,
      kWraps,              // its texels wrap round the texture page or VRAM's edge
      kOutside,            // a rectangle wholly outside the drawing area: it reads nothing
      kReasons
    };
    static const char* ReasonName(Reason reason);

    struct Counts {
      uint64_t uploads = 0;           // transfers recorded
      uint64_t uploads_joined = 0;    // ...carrying on from the one before, and joined to it
      uint64_t uploads_patched = 0;   // ...inside a live record, which then holds them
      uint64_t records_replaced = 0;  // records uploaded over whole
      uint64_t records_retired = 0;   // records drawn over whole
      uint64_t records_cut = 0;       // records written over in part, cut into what was left
      uint64_t records_copied = 0;    // records made by VRAM-to-VRAM copies of uploaded texels
      uint64_t records_live = 0;      // records alive now
      uint64_t textured_jobs = 0;
      uint64_t textured_pixels = 0;
      uint64_t jobs[kReasons] = {};
      uint64_t pixels[kReasons] = {};
      uint64_t composite_jobs = 0;    // named jobs whose texels several records tile
      uint64_t sources = 0;           // distinct names seen
      uint64_t dumped = 0;            // ...written as PNGs
      uint64_t dumped_texels = 0;     // ...the area of those PNGs
      uint64_t dumped_used = 0;       // ...the texels of it filled in, in 8x8 cells
      uint64_t dump_failures = 0;
    };

    // Reads `vram`, Gpu's native 1024x512.
    explicit TextureTracker(const uint16_t* vram);
    ~TextureTracker() override;

    // Where to write sources; empty for nowhere. Sources smaller than `min_texels` in either
    // direction are named and counted but not written.
    void set_dump_folder(const std::string& folder, int min_texels = 8) {
      dump_folder_ = folder;
      dump_min_ = min_texels;
    }
    // Writes every source not yet written - the end of a run. Call with nothing being drawn.
    void Flush();

    const Counts& counts() const { return counts_; }

    // A line into `log` for the first `per_reason` jobs of each reason that names nothing: the
    // texels it reads and the uploads they touch. Null for none.
    void set_log(FILE* log, int per_reason) {
      log_ = log;
      log_left_.assign(kReasons, per_reason);
    }
    // ...and a line for everything that happens to a record touching this rectangle of VRAM:
    // recorded, joined, patched, written over. Needs a log.
    void set_trace(int32_t x, int32_t y, int32_t w, int32_t h) {
      trace_ = { x, y, x + w - 1, y + h - 1 };
      tracing_ = w > 0 && h > 0;
    }

    // What a textured job names, and why if nothing: what Drawing works out. `name` is the
    // source's file name without ".png".
    Reason Classify(const DrawJob& job, std::string* name) const;

    // RasterObserver.
    void Drawing(const DrawJob& job) override;
    void Drawn(const DrawJob& job, uint64_t pixels) override;
    void Written(uint32_t x, uint32_t y, uint32_t w, uint32_t h) override;
    void Reloaded() override;

    // A hash of a rectangle of VRAM - rows of halfwords, wrapping as VRAM does - and of its size.
    static uint64_t HashRect(const uint16_t* vram, uint32_t x, uint32_t y, uint32_t w, uint32_t h);

   private:
    struct Rect {
      int32_t x0, y0, x1, y1;   // inclusive
    };
    struct Record {
      uint32_t x = 0, y = 0, w = 0, h = 0;   // inside VRAM: x + w <= 1024, y + h <= 512
      uint64_t hash = 0;
      uint64_t serial = 0;                    // which record this is, never reused
      bool live = false;
      // While dumping: its halfwords, copied the first time a source is named from it, and the
      // sources being gathered from it.
      std::shared_ptr<const std::vector<uint16_t>> pixels;
      std::vector<uint64_t> sources;
    };
    // What a named job reads: its records - one, or a composite's - and its palette.
    struct Use {
      std::vector<int> records;
      uint64_t image_hash = 0;   // the record's hash, or the composite's
      uint64_t clut_hash = 0;    // 0 for 15-bit
      Rect sampled = {};         // in VRAM, as named
      Rect texels = {};          // as texture coordinates
    };
    // A source being dumped: its records as they were, its palette, and the cells read through it.
    struct Part {
      std::shared_ptr<const std::vector<uint16_t>> pixels;
      uint32_t x, y, w, h;   // halfwords, from the source's corner
    };
    struct Source {
      std::string name;
      uint32_t mode = 0;
      uint32_t x = 0, y = 0;                 // its corner in VRAM, halfwords
      uint32_t halfwords = 0, rows = 0;      // its extent
      uint32_t width = 0, height = 0;        // in texels
      std::vector<Part> parts;
      std::vector<uint16_t> clut;
      uint32_t cells_x = 0, cells_y = 0;
      std::vector<uint8_t> used;             // each 8x8-texel cell read
    };

    // The cells a rectangle touches, 16x16 halfwords each.
    static const int kCell = 16;
    static const int kCellsX = 1024 / kCell, kCellsY = 512 / kCell;

    Reason Find(const DrawJob& job, Use* use) const;
    static std::string Name(uint64_t image_hash, uint64_t clut_hash, uint32_t mode);
    // The live record holding all of `r`, or -1 - and then whether any live one overlaps it.
    int Holding(const Rect& r, bool* overlapped) const;
    // Whether live records tile all of `r` between them, and which.
    bool Tiling(const Rect& r, std::vector<int>* records) const;
    bool AnyDrawn(const Rect& r) const;
    int AddRecord(uint32_t x, uint32_t y, uint32_t w, uint32_t h);
    void Register(int index);
    void Unregister(int index);
    void Remove(int index);
    // `r` cut out of a record, what is left kept as up to four records.
    void Cut(int index, const Rect& r);
    // Every live record `r` overlaps written over: gone if whole, cut if in part.
    void WriteOver(const Rect& r, bool by_draw);
    // The rectangles of VRAM a job writes - more than one where it wraps - or none.
    static void Destination(const DrawJob& job, std::vector<Rect>* out);
    // The texels a textured job can sample: as a VRAM rectangle (`r`) and as texture coordinates
    // (`texels`, after the texture window) - a texel short at a triangle's far edges if `short_by`.
    // False if they wrap.
    static bool Sampled(const DrawJob& job, bool short_by, Rect* r, Rect* texels);
    // A named job's texels marked read in its source, the source started if it is new.
    void Gather(const DrawJob& job, const Use& use, uint64_t key, const std::string& name);
    // A record's sources written, as it stops being what they were read from.
    void Close(int record);
    void Write(const Source& source);
    void Log(const DrawJob& job, Reason reason) const;
    // A trace line, if `what` touches the traced rectangle.
    void Trace(const char* what, uint32_t x, uint32_t y, uint32_t w, uint32_t h) const;

    const uint16_t* vram_;
    std::vector<Record> records_;
    std::vector<int> free_;
    std::vector<std::vector<int>> cells_;   // the records overlapping each cell
    std::vector<uint8_t> drawn_;            // each cell drawn by the GPU since last uploaded whole
    uint64_t serial_ = 0;
    int last_ = -1;   // the upload recorded last, which the next may carry on from
    // Palettes' hashes by the record holding them and where: a live record never changes.
    mutable std::unordered_map<uint64_t, uint64_t> clut_hashes_;
    std::unordered_set<uint64_t> seen_;     // the names met, hashed
    std::unordered_map<uint64_t, Source> open_;   // sources being gathered, by that hash
    std::vector<Rect> destination_;
    FILE* log_ = nullptr;
    std::vector<int> log_left_;
    Rect trace_ = {};
    bool tracing_ = false;
    std::string dump_folder_;
    int dump_min_ = 8;
    Counts counts_;
    // Between Drawing and Drawn: what the job was found to name.
    Reason pending_ = kNamed;
    bool pending_textured_ = false;
    bool pending_copy_ = false;   // a VRAM-to-VRAM copy of uploaded texels
  };

}  // namespace psx
}  // namespace emulation
