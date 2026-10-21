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
#include "graphics/upscale/texture/ai_textures.h"

#include "graphics/adapters.h"
#include "graphics/upscale/texture/onnx_runtime.h"
#include "graphics/upscale/texture/rtx_video.h"
#include "emucore/image/png_file.h"
#include "render/texture_pack.h"
#include "render/texture_tracker.h"

#include <d3d12.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>

#pragma comment(lib, "d3d12.lib")

namespace psxemu {

    using emulation::psx::PictureTexels;
    using emulation::psx::ReplacementImage;
    using emulation::psx::SourceTexels;
    using emulation::psx::TexturePack;

    AiTextures::AiTextures(std::unique_ptr<TextureUpscaler> backend, const Options& options)
        : backend_(std::move(backend)), options_(options) {
        thread_ = std::thread(&AiTextures::Worker, this);
    }

    AiTextures::~AiTextures() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        wake_.notify_all();
        if (thread_.joinable())
            thread_.join();
    }

    void AiTextures::Read(std::shared_ptr<const SourceTexels> source) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            inbox_.push_back(std::move(source));
            ++counts_.sources;
        }
        wake_.notify_one();
    }

    void AiTextures::set_pack(TexturePack* pack) {
        std::lock_guard<std::mutex> lock(pack_mutex_);
        pack_ = pack;
    }

    size_t AiTextures::QueueFolder(const std::string& folder, bool only_new) {
        namespace fs = std::filesystem;
        std::error_code failed;
        size_t queued = 0;
        std::lock_guard<std::mutex> lock(mutex_);
        for (fs::directory_iterator it(fs::path(folder), failed), end; !failed && it != end;
             it.increment(failed)) {
            const fs::path& path = it->path();
            uint64_t image, clut;
            uint32_t mode;
            std::string ext = path.extension().string();
            for (char& c : ext)
                c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
            if (ext != ".png" ||
                !emulation::psx::TextureTracker::ParseName(path.stem().string(), &image, &clut, &mode))
                continue;
            if (only_new) {
                // Done already: its image was written after the dump last grew.
                std::error_code missing;
                const fs::path done = fs::path(options_.output) / path.filename();
                const auto done_at = fs::last_write_time(done, missing);
                if (!missing && done_at >= fs::last_write_time(path, missing) && !missing)
                    continue;
            }
            files_.push_back(path.string());
            ++queued;
        }
        counts_.queued += queued;
        wake_.notify_one();
        return queued;
    }

    void AiTextures::Drain() {
        std::unique_lock<std::mutex> lock(mutex_);
        idle_.wait(lock, [this] {
            return inbox_.empty() && files_.empty() && counts_.queued == 0 && !busy_;
        });
    }

    AiTextures::Counts AiTextures::counts() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return counts_;
    }

    // ---- The thread ------------------------------------------------------------------------

    void AiTextures::Worker() {
        std::unique_lock<std::mutex> lock(mutex_);
        for (;;) {
            wake_.wait(lock, [this] {
                return stopping_ || !inbox_.empty() || !files_.empty() || !tiles_.empty();
            });
            if (stopping_)
                return;
            busy_ = true;
            std::deque<std::shared_ptr<const SourceTexels>> inbox;
            inbox.swap(inbox_);
            lock.unlock();

            for (auto& source : inbox)
                Absorb(std::move(source));
            // Dumps read until there are tiles enough for a few calls - a whole folder read at once
            // could be gigabytes, and one at a time leaves calls half empty.
            for (;;) {
                std::string file;
                {
                    std::lock_guard<std::mutex> files(mutex_);
                    if (tiles_.size() >= static_cast<size_t>(2 * options_.grid * options_.grid) || files_.empty())
                        break;
                    file = files_.front();
                    files_.pop_front();
                }
                auto picture = std::make_shared<PictureTexels>();
                int w = 0, h = 0;
                uint64_t image, clut;
                uint32_t mode;
                const std::string name = std::filesystem::path(file).stem().string();
                if (emulation::image::ReadPngFile(file, &w, &h, &picture->rgba) &&
                    emulation::psx::TextureTracker::ParseName(name, &image, &clut, &mode)) {
                    picture->name = name;
                    picture->key = emulation::psx::TextureTracker::Key(image, clut, mode);
                    picture->width = static_cast<uint32_t>(w);
                    picture->height = static_cast<uint32_t>(h);
                    picture->cells_x = (picture->width + 7) / 8;
                    picture->cells_y = (picture->height + 7) / 8;
                    picture->cells.assign(static_cast<size_t>(picture->cells_x) * picture->cells_y, 0);
                    // A dump's read cells are the ones it filled.
                    for (uint32_t y = 0; y < picture->height; ++y)
                        for (uint32_t x = 0; x < picture->width; ++x)
                            if (picture->rgba[(static_cast<size_t>(y) * picture->width + x) * 4 + 3] != 0)
                                picture->cells[(y / 8) * picture->cells_x + x / 8] = 1;
                    Absorb(picture);
                }
            }
            if (!tiles_.empty())
                RunBatch();

            lock.lock();
            counts_.queued = tiles_.size() + files_.size();
            busy_ = false;
            if (inbox_.empty() && files_.empty() && tiles_.empty())
                idle_.notify_all();
        }
    }

    void AiTextures::Absorb(std::shared_ptr<const SourceTexels> source) {
        // Palette animation: one image read through many palettes is that many upscales.
        if (source->name.size() >= 33) {
            std::set<std::string>& palettes = palettes_[source->name.substr(0, 16)];
            palettes.insert(source->name.substr(17, 16));
            if (palettes.size() == 9) {
                std::lock_guard<std::mutex> lock(mutex_);
                ++counts_.palettes_cycling;
            }
        }
        {
            std::lock_guard<std::mutex> lock(pack_mutex_);
            if (pack_ != nullptr && pack_->HandMade(source->key))
                return;   // the pack has its own, or is never to replace it
        }
        Output& output = outputs_[source->key];
        if (output.name.empty()) {
            output.name = source->name;
            output.width = source->width;
            output.height = source->height;
            output.cells_x = source->cells_x;
            output.cells_y = source->cells_y;
            output.done.assign(source->cells.size(), 0);
            Seed(&output);
        }
        if (output.width != source->width || output.height != source->height)
            return;   // another source that hashed the same
        output.latest = source;
        for (uint32_t cy = 0; cy < output.cells_y; ++cy)
            for (uint32_t cx = 0; cx < output.cells_x; ++cx) {
                const uint32_t cell = cy * output.cells_x + cx;
                if (!source->cells[cell] || output.done[cell])
                    continue;
                const uint32_t tile = (cx / 8) + (cy / 8) * 65536;
                if (output.queued.insert(tile).second) {
                    tiles_.emplace_back(source->key, tile);
                    ++output.pending;
                }
            }
    }

    void AiTextures::Seed(Output* output) {
        if (options_.output.empty())
            return;
        const std::string path = options_.output + "/" + output->name + ".png";
        int w = 0, h = 0;
        std::vector<uint8_t> rgba;
        const int s = options_.scale;
        if (!emulation::image::ReadPngFile(path, &w, &h, &rgba) ||
            w != static_cast<int>(output->width) * s || h != static_cast<int>(output->height) * s)
            return;
        const int side = 8 * s;
        for (uint32_t cy = 0; cy < output->cells_y; ++cy)
            for (uint32_t cx = 0; cx < output->cells_x; ++cx) {
                std::vector<uint8_t> block(static_cast<size_t>(side) * side * 4, 0);
                bool any = false;
                for (int y = 0; y < side; ++y) {
                    const int py = static_cast<int>(cy) * side + y;
                    if (py >= h)
                        break;
                    for (int x = 0; x < side; ++x) {
                        const int px = static_cast<int>(cx) * side + x;
                        if (px >= w)
                            break;
                        const uint8_t* from = &rgba[(static_cast<size_t>(py) * w + px) * 4];
                        memcpy(&block[(static_cast<size_t>(y) * side + x) * 4], from, 4);
                        any = any || from[3] != 0;
                    }
                }
                if (!any)
                    continue;
                const uint32_t cell = cy * output->cells_x + cx;
                output->blocks[cell] = std::move(block);
                output->done[cell] = 1;
            }
    }

    // ---- A tile's way in and out -----------------------------------------------------------

    AiTextures::Prepared AiTextures::Prepare(const SourceTexels& source, uint32_t tx, uint32_t ty,
                                             const std::vector<uint8_t>& wanted) {
        Prepared tile;
        tile.key = source.key;
        tile.tx = tx;
        tile.ty = ty;
        // The cells it gives.
        for (uint32_t cy = ty * 8; cy < std::min(source.cells_y, ty * 8 + 8); ++cy)
            for (uint32_t cx = tx * 8; cx < std::min(source.cells_x, tx * 8 + 8); ++cx)
                if (wanted[cy * source.cells_x + cx])
                    tile.cells.push_back(cy * source.cells_x + cx);
        if (tile.cells.empty())
            return tile;

        // What is round it decoded once; past the source's edge, its edge repeated.
        const int x0 = static_cast<int>(tx) * kTile - kPad, y0 = static_cast<int>(ty) * kTile - kPad;
        const int w = static_cast<int>(source.width), h = static_cast<int>(source.height);
        const int dx0 = std::max(0, x0), dy0 = std::max(0, y0);
        const int dx1 = std::min(w, x0 + kSide), dy1 = std::min(h, y0 + kSide);
        std::vector<uint8_t> decoded(static_cast<size_t>(dx1 - dx0) * (dy1 - dy0) * 4);
        source.Decode(static_cast<uint32_t>(dx0), static_cast<uint32_t>(dy0),
                      static_cast<uint32_t>(dx1 - dx0), static_cast<uint32_t>(dy1 - dy0),
                      decoded.data());
        const size_t n = static_cast<size_t>(kSide) * kSide;
        std::vector<uint8_t> colour(n * 3, 0), alpha(n, 0);
        for (int ry = 0; ry < kSide; ++ry)
            for (int rx = 0; rx < kSide; ++rx) {
                const int sx = std::min(std::max(x0 + rx, 0), w - 1);
                const int sy = std::min(std::max(y0 + ry, 0), h - 1);
                const uint8_t* p = &decoded[(static_cast<size_t>(sy - dy0) * (dx1 - dx0) + (sx - dx0)) * 4];
                const size_t i = static_cast<size_t>(ry) * kSide + rx;
                // A cell no draw has read through this palette may be anything; it is not drawn.
                const bool read = source.cells[(sy / 8) * source.cells_x + sx / 8] != 0;
                alpha[i] = read ? p[3] : 0;
                memcpy(&colour[i * 3], p, 3);
            }
        // Holes take the nearest drawn texel's colour, outward a step at a time.
        std::vector<int> from(n, -1);
        std::vector<size_t> frontier;
        for (size_t i = 0; i < n; ++i)
            if (alpha[i] != 0) {
                from[i] = static_cast<int>(i);
                frontier.push_back(i);
            }
        // Empty unless a cell it gives has a drawn texel: cells read but clear, beside drawn ones
        // done before, give nothing - and leave nothing in the image to show the next run they
        // were done, so they would go through the model every run.
        tile.empty = true;
        for (uint32_t cell : tile.cells) {
            const int cx = static_cast<int>(cell % source.cells_x) * 8 - static_cast<int>(tx) * kTile;
            const int cy = static_cast<int>(cell / source.cells_x) * 8 - static_cast<int>(ty) * kTile;
            for (int y = cy; y < cy + 8 && tile.empty; ++y)
                for (int x = cx; x < cx + 8 && tile.empty; ++x)
                    tile.empty = alpha[static_cast<size_t>(y + kPad) * kSide + x + kPad] == 0;
            if (!tile.empty)
                break;
        }
        for (size_t head = 0; head < frontier.size(); ++head) {
            const size_t i = frontier[head];
            const int x = static_cast<int>(i % kSide), y = static_cast<int>(i / kSide);
            const int steps[4][2] = { { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 } };
            for (const auto& step : steps) {
                const int nx = x + step[0], ny = y + step[1];
                if (nx < 0 || ny < 0 || nx >= kSide || ny >= kSide)
                    continue;
                const size_t j = static_cast<size_t>(ny) * kSide + nx;
                if (from[j] >= 0)
                    continue;
                from[j] = from[i];
                frontier.push_back(j);
            }
        }
        tile.alpha.assign(static_cast<size_t>(kTile) * kTile, 0);
        for (int y = 0; y < kTile; ++y)
            for (int x = 0; x < kTile; ++x)
                tile.alpha[static_cast<size_t>(y) * kTile + x] =
                    alpha[static_cast<size_t>(y + kPad) * kSide + x + kPad];
        if (tile.empty)
            return tile;
        // Coverage goes through too where a cell it gives has a clear texel.
        for (uint32_t cell : tile.cells) {
            const int cx = static_cast<int>(cell % source.cells_x) * 8 - static_cast<int>(tx) * kTile;
            const int cy = static_cast<int>(cell / source.cells_x) * 8 - static_cast<int>(ty) * kTile;
            for (int y = cy; y < cy + 8 && !tile.coverage; ++y)
                for (int x = cx; x < cx + 8 && !tile.coverage; ++x)
                    if (static_cast<int>(tx) * kTile + x < w && static_cast<int>(ty) * kTile + y < h)
                        tile.coverage = tile.alpha[static_cast<size_t>(y) * kTile + x] == 0;
        }
        tile.rgb.resize(n * 3);
        for (size_t i = 0; i < n; ++i)
            for (int c = 0; c < 3; ++c)
                tile.rgb[c * n + i] = colour[static_cast<size_t>(from[i]) * 3 + c] / 255.0f;
        if (tile.coverage) {
            tile.cover.resize(n * 3);
            for (size_t i = 0; i < n; ++i) {
                const float covered = alpha[i] != 0 ? 1.0f : 0.0f;
                tile.cover[i] = tile.cover[n + i] = tile.cover[2 * n + i] = covered;
            }
        }
        return tile;
    }

    void AiTextures::Assemble(const Prepared& tile, const SourceTexels& source, const float* rgb,
                              const float* cover, int k, int scale,
                              std::vector<std::pair<uint32_t, std::vector<uint8_t>>>* blocks) {
        const int f = k / scale;   // the model's pixels to one of ours, each way
        const int out_side = kSide * k;
        const size_t plane = static_cast<size_t>(out_side) * out_side;
        const int side = 8 * scale;
        for (uint32_t cell : tile.cells) {
            const int lx = static_cast<int>(cell % source.cells_x) * 8 - static_cast<int>(tile.tx) * kTile;
            const int ly = static_cast<int>(cell / source.cells_x) * 8 - static_cast<int>(tile.ty) * kTile;
            std::vector<uint8_t> block(static_cast<size_t>(side) * side * 4, 0);
            bool any = false;
            for (int py = 0; py < side; ++py)
                for (int px = 0; px < side; ++px) {
                    const int tlx = lx + px / scale, tly = ly + py / scale;
                    if (static_cast<int>(tile.tx) * kTile + tlx >= static_cast<int>(source.width) ||
                        static_cast<int>(tile.ty) * kTile + tly >= static_cast<int>(source.height))
                        continue;
                    const uint8_t texel = tile.alpha[static_cast<size_t>(tly) * kTile + tlx];
                    float sum[3] = { 0.0f, 0.0f, 0.0f }, covered = 0.0f;
                    const int mx = (kPad + lx) * k + px * f, my = (kPad + ly) * k + py * f;
                    for (int sy = 0; sy < f; ++sy)
                        for (int sx = 0; sx < f; ++sx) {
                            const size_t i = static_cast<size_t>(my + sy) * out_side + mx + sx;
                            for (int c = 0; c < 3; ++c)
                                sum[c] += rgb[c * plane + i];
                        }
                    // Coverage smoothed before it is cut at a half - the model sharpens it, and
                    // cut as it comes its edges are ragged: the mean of the pixels round each.
                    if (cover != nullptr) {
                        float total = 0.0f;
                        int count = 0;
                        for (int sy = -1; sy <= f; ++sy)
                            for (int sx = -1; sx <= f; ++sx) {
                                const int y = my + sy, x = mx + sx;
                                if (x < 0 || y < 0 || x >= out_side || y >= out_side)
                                    continue;
                                const size_t i = static_cast<size_t>(y) * out_side + x;
                                total += (cover[i] + cover[plane + i] + cover[2 * plane + i]) / 3.0f;
                                ++count;
                            }
                        covered = total / count * (f * f);
                    }
                    uint8_t a;
                    if (cover != nullptr)
                        a = covered / (f * f) >= 0.5f ? (texel == 128 ? 128 : 255) : 0;
                    else
                        a = texel;
                    if (a == 0)
                        continue;
                    uint8_t* p = &block[(static_cast<size_t>(py) * side + px) * 4];
                    for (int c = 0; c < 3; ++c) {
                        const float v = sum[c] / (f * f);
                        p[c] = static_cast<uint8_t>(std::min(std::max(v, 0.0f), 1.0f) * 255.0f + 0.5f);
                    }
                    p[3] = a;
                    any = true;
                }
            if (any)
                blocks->emplace_back(cell, std::move(block));
        }
    }

    // ---- Calls -----------------------------------------------------------------------------

    void AiTextures::RunBatch() {
        // Up to kGrid squared slots, a tile's colour in one and its coverage, if it has any, in
        // another.
        struct Slot {
            size_t tile;
            bool coverage;
        };
        std::vector<Prepared> tiles;
        std::vector<Slot> slots;
        std::vector<uint64_t> touched;
        const int rows = std::min(std::max(options_.grid, 1), static_cast<int>(kGrid));
        const int cols = rows == 1 ? 2 : rows;   // one tile: its colour and its coverage beside it
        const size_t most = static_cast<size_t>(cols) * rows;
        while (!tiles_.empty() && slots.size() < most) {
            const auto [key, packed] = tiles_.front();
            auto found = outputs_.find(key);
            if (found == outputs_.end() || !found->second.latest) {
                tiles_.pop_front();
                continue;
            }
            Output& output = found->second;
            std::vector<uint8_t> wanted(output.done.size(), 0);
            for (size_t i = 0; i < wanted.size(); ++i)
                wanted[i] = output.latest->cells[i] && !output.done[i];
            Prepared tile = Prepare(*output.latest, packed & 0xFFFF, packed >> 16, wanted);
            const size_t need = tile.empty || tile.cells.empty() ? 0 : (tile.coverage ? 2 : 1);
            if (slots.size() + need > most)
                break;   // next call
            tiles_.pop_front();
            output.queued.erase(packed);
            if (need == 0) {
                // Nothing drawn in what it gives: the game's own texels, which are clear.
                for (uint32_t cell : tile.cells)
                    output.done[cell] = 1;
                --output.pending;
                touched.push_back(key);
                std::lock_guard<std::mutex> lock(mutex_);
                ++counts_.empty_tiles;
                continue;
            }
            slots.push_back({ tiles.size(), false });
            if (tile.coverage)
                slots.push_back({ tiles.size(), true });
            tiles.push_back(std::move(tile));
        }

        if (!slots.empty()) {
            // The slots side by side, three by three - or one tile's two - the same size every
            // call, so the model's graph is made once.
            const int canvas_w = cols * kSide, canvas_h = rows * kSide;
            const size_t plane = static_cast<size_t>(canvas_w) * canvas_h;
            const size_t slot_plane = static_cast<size_t>(kSide) * kSide;
            std::vector<float> input(plane * 3, 0.0f);
            for (size_t s = 0; s < slots.size(); ++s) {
                const Prepared& tile = tiles[slots[s].tile];
                const std::vector<float>& from = slots[s].coverage ? tile.cover : tile.rgb;
                const int ox = static_cast<int>(s % cols) * kSide, oy = static_cast<int>(s / cols) * kSide;
                for (int c = 0; c < 3; ++c)
                    for (int y = 0; y < kSide; ++y)
                        memcpy(&input[c * plane + static_cast<size_t>(oy + y) * canvas_w + ox],
                               &from[c * slot_plane + static_cast<size_t>(y) * kSide], kSide * sizeof(float));
            }
            std::vector<float> output;
            int ow = 0, oh = 0;
            std::string error;
            const auto start = std::chrono::steady_clock::now();
            const bool ran = backend_->Run(input, canvas_w, canvas_h, &output, &ow, &oh, &error);
            const double ms = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - start).count();
            const int k = ran ? ow / canvas_w : 0;
            const int scale = std::min(options_.scale, k);
            const bool usable = ran && k > 0 && scale > 0 && k % scale == 0;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                ++counts_.calls;
                counts_.call_ms += ms;
                if (!usable) {
                    ++counts_.failures;
                    counts_.last_error = ran ? "the model's scale does not suit" : error;
                } else {
                    counts_.tiles += tiles.size();
                }
            }
            // Each tile's results cut back out of the call's, and put together.
            const int out_side = kSide * k;
            const size_t out_plane = static_cast<size_t>(ow) * oh;
            const size_t slot_out = static_cast<size_t>(out_side) * out_side;
            std::vector<std::vector<float>> results(slots.size());
            for (size_t s = 0; usable && s < slots.size(); ++s) {
                results[s].resize(slot_out * 3);
                const int ox = static_cast<int>(s % cols) * out_side, oy = static_cast<int>(s / cols) * out_side;
                for (int c = 0; c < 3; ++c)
                    for (int y = 0; y < out_side; ++y)
                        memcpy(&results[s][c * slot_out + static_cast<size_t>(y) * out_side],
                               &output[c * out_plane + static_cast<size_t>(oy + y) * ow + ox],
                               out_side * sizeof(float));
            }
            for (size_t t = 0; t < tiles.size(); ++t) {
                const Prepared& tile = tiles[t];
                Output& out = outputs_[tile.key];
                if (usable) {
                    const float* rgb = nullptr;
                    const float* cover = nullptr;
                    for (size_t s = 0; s < slots.size(); ++s)
                        if (slots[s].tile == t)
                            (slots[s].coverage ? cover : rgb) = results[s].data();
                    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> blocks;
                    Assemble(tile, *out.latest, rgb, cover, k, scale, &blocks);
                    for (auto& [cell, pixels] : blocks)
                        out.blocks[cell] = std::move(pixels);
                    out.changed = out.changed || !blocks.empty();
                }
                // Done either way: a call that failed is not tried again and again.
                for (uint32_t cell : tile.cells)
                    out.done[cell] = 1;
                --out.pending;
                touched.push_back(tile.key);
            }
            // Held to its share of the time.
            if (options_.share > 0.0 && options_.share < 1.0) {
                const double rest = std::min(ms * (1.0 - options_.share) / options_.share, 2000.0);
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait_for(lock, std::chrono::duration<double, std::milli>(rest),
                               [this] { return stopping_; });
            }
        }

        // A source with nothing more waiting is handed over, and its texels let go.
        std::sort(touched.begin(), touched.end());
        touched.erase(std::unique(touched.begin(), touched.end()), touched.end());
        for (uint64_t key : touched) {
            Output& output = outputs_[key];
            if (output.pending > 0)
                continue;
            if (output.changed)
                Publish(key, &output);
            output.changed = false;
            output.latest.reset();
        }
    }

    void AiTextures::Publish(uint64_t key, Output* output) {
        const int s = options_.scale;
        auto image = std::make_shared<ReplacementImage>();
        image->id = TexturePack::NewImageId();
        image->width = output->width;
        image->height = output->height;
        image->scale = static_cast<uint32_t>(s);
        std::vector<uint32_t> cells;
        for (const auto& [cell, pixels] : output->blocks)
            cells.push_back(cell);
        std::sort(cells.begin(), cells.end());
        const size_t bytes = static_cast<size_t>(8 * s) * (8 * s) * 4;
        image->pixels.reserve(cells.size() * bytes);
        for (uint32_t cell : cells) {
            image->blocks.push_back(cell);   // a block is a cell: both are 8x8 texels
            const std::vector<uint8_t>& pixels = output->blocks[cell];
            image->pixels.insert(image->pixels.end(), pixels.begin(), pixels.end());
        }

        // Written first, so the pack can read it back if it lets the image go.
        std::string path;
        if (!options_.output.empty()) {
            std::error_code failed;
            std::filesystem::create_directories(std::filesystem::path(options_.output), failed);
            path = options_.output + "/" + output->name + ".png";
            const int w = static_cast<int>(output->width) * s, h = static_cast<int>(output->height) * s;
            const int side = 8 * s;
            std::vector<uint8_t> canvas(static_cast<size_t>(w) * h * 4, 0);
            for (uint32_t cell : cells) {
                const int bx = static_cast<int>(cell % output->cells_x) * side;
                const int by = static_cast<int>(cell / output->cells_x) * side;
                const std::vector<uint8_t>& pixels = output->blocks[cell];
                for (int y = 0; y < side && by + y < h; ++y) {
                    const int span = std::min(side, w - bx);
                    memcpy(&canvas[(static_cast<size_t>(by + y) * w + bx) * 4],
                           &pixels[static_cast<size_t>(y) * side * 4], static_cast<size_t>(span) * 4);
                }
            }
            if (!emulation::image::WritePngFile(path, w, h, canvas.data(), 1)) {
                std::lock_guard<std::mutex> lock(mutex_);
                counts_.last_error = "could not write " + path;
            }
        }
        {
            std::lock_guard<std::mutex> lock(pack_mutex_);
            if (pack_ != nullptr)
                pack_->Provide(key, output->name, path, image);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        ++counts_.published;
    }

    // ---- The ONNX backend ------------------------------------------------------------------

    std::unique_ptr<TextureUpscaler> MakeOnnxBackend(const std::wstring& runtime_folder,
                                                     const std::wstring& model, uint64_t adapter,
                                                     std::string* error) {
        if (!OnnxRuntime::Get().Load(runtime_folder, error))
            return nullptr;
        Microsoft::WRL::ComPtr<IDXGIAdapter1> card;
        if (adapter != 0)
            card = OpenAdapter(adapter);
        Microsoft::WRL::ComPtr<ID3D12Device> device;
        if (FAILED(D3D12CreateDevice(card.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
            *error = "no Direct3D 12 device on the card for the upscaler";
            return nullptr;
        }
        std::unique_ptr<OnnxUpscaler> upscaler = OnnxUpscaler::Create(model, device.Get(), error);
        if (!upscaler)
            return nullptr;
        return upscaler;
    }

    std::unique_ptr<TextureUpscaler> MakeRtxVideoBackend(const std::wstring& folder, int scale,
                                                         int quality, uint64_t adapter,
                                                         std::string* error) {
        if (!RtxVideoUpscaler::Built()) {
            *error = "this build has no RTX Video";
            return nullptr;
        }
        Microsoft::WRL::ComPtr<IDXGIAdapter1> card;
        if (adapter != 0)
            card = OpenAdapter(adapter);
        Microsoft::WRL::ComPtr<ID3D12Device> device;
        if (FAILED(D3D12CreateDevice(card.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
            *error = "no Direct3D 12 device on the card for the upscaler";
            return nullptr;
        }
        std::wstring dll_folder = folder;
        while (!dll_folder.empty() && (dll_folder.back() == L'\\' || dll_folder.back() == L'/'))
            dll_folder.pop_back();
        std::unique_ptr<RtxVideoUpscaler> upscaler =
            RtxVideoUpscaler::Create(device.Get(), dll_folder, scale, quality, error);
        if (!upscaler)
            return nullptr;
        return upscaler;
    }

}   // namespace psxemu
