/*****************************************************************************************************************
* Copyright (c) 2014 Khalid Ali Al-Kooheji                                                                       *
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

// The AI texture upscaler (Docs/Texture-Upscaling-Plan.md, phase 3): the textures a game draws, made
// larger by a model on the graphics card, as they are drawn, and drawn in their place.
//
// - **Fed by the texture tracker** (render/texture_upscale.h): each time draws read more of a
//   source, its texels and the 8x8-texel cells read so far. Only the cells read are upscaled - a
//   sheet read through 91 palettes, a corner each, is 91 corners, not 91 sheets - and only once
//   each: what is done is kept, in memory and in a PNG in the output folder, and read back from
//   there by the next run.
// - **In tiles of 64x64 texels**, each with 16 texels of what is round it - the source's own
//   texels where there are any, its edge repeated past them - so the model, which looks about that
//   far, sees each tile's edges in place. Up to nine tiles go through the model as one picture,
//   three by three: small textures share a call rather than paying for one each.
// - **Before**: texels not drawn - clear, or in cells not read - take the colour of the nearest
//   texel that is drawn, so no dark fringe is drawn into a cut-out's edge.
// - **After**: a tile with clear texels has its coverage go through the model too, and a pixel is
//   drawn where that is a half or more - smooth edges for cut-outs; semi-transparency (bit 15)
//   follows the nearest texel. The model's 4x is halved, a 2x2 average, for 2x.
// - **On its own thread and its own Direct3D 12 device** on the card the rasteriser draws on,
//   through any TextureUpscaler backend: an ONNX model on DirectML (onnx_runtime.h), or RTX Video
//   in phase 4. Held to a share of the time - after a call that took t, it rests t (1 - share) /
//   share - so the game keeps its speed while the queue runs.
// - **Upscale All** feeds it a folder of dumps instead: each a source with every cell it has
//   filled read.

#include "graphics/upscale/texture/texture_upscaler.h"
#include "render/raster.h"
#include "render/texture_upscale.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace psxemu {

    class AiTextures : public emulation::psx::TextureUpscaleQueue {
     public:
        struct Options {
            int scale = 4;            // 2 or 4: the model's 4x, or half it
            std::string output;       // the folder the upscaled sources are written to
            double share = 0.25;      // of the time it may work; 1 for all of it
            // Tiles to a call, each way, 1 to kGrid: on the card the game draws on, one, so a
            // call is over between two frames rather than holding the card across them.
            int grid = kGrid;
        };
        struct Counts {
            uint64_t sources = 0;     // sources told of
            uint64_t tiles = 0;       // tiles upscaled
            uint64_t empty_tiles = 0; // ...found to have nothing drawn, and not
            uint64_t calls = 0;       // calls to the model
            uint64_t published = 0;   // images handed to the pack
            uint64_t failures = 0;    // calls that failed
            uint64_t queued = 0;      // tiles waiting now
            uint64_t palettes_cycling = 0;   // images read through more than eight palettes
            double call_ms = 0.0;     // the model's time, all calls
            std::string last_error;
        };

        // Upscales with `backend`, which is the worker's alone from here.
        AiTextures(std::unique_ptr<TextureUpscaler> backend, const Options& options);
        ~AiTextures() override;

        // TextureUpscaleQueue.
        void Read(std::shared_ptr<const emulation::psx::SourceTexels> source) override;
        void set_pack(emulation::psx::TexturePack* pack) override;

        // Upscale All: every dump in `folder` (a PNG named as a source), each with its filled cells
        // read. Returns how many were queued.
        size_t QueueFolder(const std::string& folder);
        // Waits until nothing is waiting or being worked on.
        void Drain();
        Counts counts() const;

        // The pieces, for the tests: a tile's model input made ready, and its result put together.
        static constexpr int kTile = 64;     // texels a tile is, each way
        static constexpr int kPad = 16;      // ...and round it
        static constexpr int kSide = kTile + 2 * kPad;
        static constexpr int kGrid = 3;      // tiles to a call, each way
        struct Prepared {
            uint64_t key = 0;
            uint32_t tx = 0, ty = 0;              // which tile, in tiles
            bool empty = false;                    // nothing drawn in it
            bool coverage = false;                 // clear texels among the cells it gives
            std::vector<float> rgb, cover;         // kSide squared, planar 3 channels
            std::vector<uint8_t> alpha;            // kTile squared: 0, 128 or 255, the texels'
            std::vector<uint32_t> cells;           // the source's cells it gives: index
        };
        // A tile of `source` made ready: decoded, unread cells cleared, holes filled from the nearest
        // drawn texel. `wanted` is the source's cells to give, by index.
        static Prepared Prepare(const emulation::psx::SourceTexels& source, uint32_t tx, uint32_t ty,
                                const std::vector<uint8_t>& wanted);
        // Its blocks at `scale` from the model's output for it - `k` times kSide square, planar -
        // and its coverage's, if any: each given cell's pixels, (8 * scale) squared RGBA8, by cell
        // index. A block with nothing drawn is left out.
        static void Assemble(const Prepared& tile, const emulation::psx::SourceTexels& source,
                             const float* rgb, const float* cover, int k, int scale,
                             std::vector<std::pair<uint32_t, std::vector<uint8_t>>>* blocks);

     private:
        // What is known of each source: its newest texels, the cells done and their blocks.
        struct Output {
            std::string name;
            uint32_t width = 0, height = 0, cells_x = 0, cells_y = 0;
            std::shared_ptr<const emulation::psx::SourceTexels> latest;
            std::vector<uint8_t> done;            // cells upscaled (or found empty)
            std::set<uint32_t> queued;            // tiles waiting: tx + ty * 65536
            std::unordered_map<uint32_t, std::vector<uint8_t>> blocks;
            int pending = 0;                      // tiles waiting or in a call
            bool changed = false;                 // blocks added since last published
        };

        void Worker();
        // A source told of: what of it is new queued as tiles.
        void Absorb(std::shared_ptr<const emulation::psx::SourceTexels> source);
        // A source's done cells from the output folder, the first time it is met.
        void Seed(Output* output);
        // One call: up to kGrid squared tiles' inputs side by side, the result cut back into them.
        void RunBatch();
        // A source's blocks written to its PNG and handed to the pack.
        void Publish(uint64_t key, Output* output);

        std::unique_ptr<TextureUpscaler> backend_;
        Options options_;
        std::thread thread_;
        mutable std::mutex mutex_;
        std::condition_variable wake_, idle_;
        std::deque<std::shared_ptr<const emulation::psx::SourceTexels>> inbox_;
        std::deque<std::string> files_;   // Upscale All's dumps, read one at a time
        bool stopping_ = false;
        bool busy_ = false;
        Counts counts_;
        std::mutex pack_mutex_;
        emulation::psx::TexturePack* pack_ = nullptr;

        // The worker's own.
        std::unordered_map<uint64_t, Output> outputs_;
        std::deque<std::pair<uint64_t, uint32_t>> tiles_;   // key, tx + ty * 65536
        std::unordered_map<std::string, std::set<std::string>> palettes_;   // by image hash
    };

    // An ONNX model on DirectML (onnx_runtime.h) on a device of its own, on the card with `adapter`
    // (0: Windows' default): ONNX Runtime loaded from `runtime_folder` first. Null, with `error`.
    std::unique_ptr<TextureUpscaler> MakeOnnxBackend(const std::wstring& runtime_folder,
                                                     const std::wstring& model, uint64_t adapter,
                                                     std::string* error);

}   // namespace psxemu
