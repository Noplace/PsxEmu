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

// A replacement pack (Docs/Texture-Upscaling-Plan.md, phase 2): PNGs named as the texture tracker
// names sources - `<image>-<clut>-<mode>.png`, as `--texture-dump` writes them - drawn by the
// hardware rasteriser in place of the texels they were named from.
//
// - **A folder**, its subfolders too, read once when opened: a map from name to file. An image is
//   read when a job first wants it, on a loader thread of the pack's own; until it is ready the
//   job draws the game's own texels, so a replacement can arrive a few frames late but the game
//   never waits for it. Read in a tool, it can be read there and then instead (set_synchronous),
//   so a run's pictures do not depend on the loader's speed.
// - **Any whole multiple of the source's size**, 1x to 8x. Alpha is rounded to the three the
//   PlayStation has: 0 (not drawn), 128 (bit 15 set: semi-transparent) or 255.
// - **pack.ini**, optional, beside the images:
//
//       [pack]
//       filter = nearest            ; or bilinear: how every image is sampled
//       [filter]
//       <name> = bilinear           ; ...or one of them
//       [alias]
//       <name> = <another name>     ; draw <name> with the other's image: palette swaps
//       [never]
//       <name>                      ; never replace it: fonts, a HUD
//
//   Names with or without ".png"; `;` and `#` start comments.
// - **Memory**: images decoded are kept up to a budget, longest unused let go first and read
//   again if wanted; a rasteriser keeps its own copy on the card while it uses one.
//
// Never machine state: nothing here is saved, and a pack changed on disk is read again by opening
// it again.

#include "render/raster.h"
#include "render/texture_tracker.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace emulation {
namespace psx {

  class TexturePack : public TextureReplacer {
   public:
    struct Counts {
      uint64_t files = 0;            // names the pack has an image for, aliases included
      uint64_t jobs_replaced = 0;    // textured jobs drawn with a replacement...
      uint64_t jobs_ai = 0;          // ...the AI upscaler's
      uint64_t provided = 0;         // images the upscaler handed over this run
      uint64_t jobs_waiting = 0;     // ...named one the pack has, still loading
      uint64_t jobs_unmatched = 0;   // ...named one the pack has nothing for
      uint64_t jobs_never = 0;       // ...named one pack.ini says to leave alone
      uint64_t loaded = 0;           // images read
      uint64_t bad = 0;              // ...unreadable, or not a whole multiple of their source
      uint64_t dropped = 0;          // images let go to stay within the budget
      uint64_t bytes = 0;            // decoded images held now
    };

    // Names jobs as `tracker` - the observer `Gpu` tells of each job - names them.
    explicit TexturePack(const TextureTracker* tracker);
    ~TexturePack() override;

    // Reads `folder`'s names and its pack.ini, letting go of whatever was open - and `ai_folder`'s
    // names, if one is given, the AI upscaler's own (phase 3), for any texture `folder` has none
    // for. Either may be empty. False, with `error` saying why, if a folder given cannot be read.
    bool Open(const std::string& folder, std::string* error, const std::string& ai_folder = "");

    // The AI upscaler's (render/texture_upscale.h), from its thread: whether a texture has an
    // image of the pack's own or is never to be replaced - nothing for the upscaler to do; and an
    // image it has made, written at `path`, drawn in place of the texture from the next job on.
    bool HandMade(uint64_t key) const;
    void Provide(uint64_t key, const std::string& name, const std::string& path,
                 std::shared_ptr<const ReplacementImage> image);
    // Lets go of everything; nothing is replaced until Open is called again.
    void Close();
    // Reads each image on the thread that first wants it, rather than the loader's.
    void set_synchronous(bool on) { synchronous_ = on; }
    // Decoded images beyond this many bytes are let go, longest unused first.
    void set_memory_budget(uint64_t bytes) { budget_ = bytes; }
    // Each image made `times` as large as it is read, nearest - the identity check's packs at 2x
    // to 8x, made from a dump at 1x (Docs/Texture-Upscaling-Plan.md, phase 2).
    void set_test_scale(int times) { test_scale_ = times; }

    Counts counts() const;
    // What pack.ini said that made no sense, and images that could not be used, a line each.
    std::vector<std::string> warnings() const;

    // TextureReplacer, on the rasteriser's thread.
    bool Replace(const DrawJob& job, TextureReplacement* replacement) override;

    // An image as the pack keeps it, from a PNG's `w` x `h` RGBA pixels, for a source `width` x
    // `height` texels: cut into blocks, the empty ones left out, alpha rounded. False, with
    // `error` saying why, if it is not 1x to 8x the source's size each way, the same both ways.
    static bool MakeImage(const std::vector<uint8_t>& rgba, int w, int h, uint32_t width,
                          uint32_t height, bool bilinear, ReplacementImage* image,
                          std::string* error);
    // A number for an image made elsewhere (ReplacementImage::id): never one given before.
    static uint64_t NewImageId();

   private:
    enum class State { kUnloaded, kLoading, kReady, kBad };
    struct Entry {
      std::string path;
      std::string name;
      bool ai = false;      // the upscaler's, not the pack's own
      bool never = false;
      bool bilinear = false;
      State state = State::kUnloaded;
      std::shared_ptr<const ReplacementImage> image;
      uint64_t used = 0;               // serial_ when last asked for
      uint32_t width = 0, height = 0;  // the source's, as first asked for
    };

    void Loader();
    // Reads an entry's image - with no lock held - and hands it over, if the pack it was asked
    // of is still the open one.
    void Load(uint64_t key, uint64_t generation);
    // Images let go, under the lock, until those held fit the budget.
    void Trim();

    const TextureTracker* tracker_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::unordered_map<uint64_t, Entry> entries_;   // by TextureTracker::Key
    std::deque<std::pair<uint64_t, uint64_t>> queue_;   // keys to load, and for which pack
    std::thread thread_;
    bool stopping_ = false;
    bool synchronous_ = false;
    int test_scale_ = 1;
    uint64_t generation_ = 0;   // moved on by every Open and Close
    uint64_t budget_ = 1ull << 30;
    uint64_t serial_ = 0;
    Counts counts_;
    std::vector<std::string> warnings_;
  };

}  // namespace psx
}  // namespace emulation
