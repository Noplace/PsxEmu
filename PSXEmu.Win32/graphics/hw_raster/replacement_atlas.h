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

// Where a hardware rasteriser keeps replacement textures on the card (Docs/Texture-Upscaling-Plan.md,
// phase 2), worked out once here for Direct3D 11's and 12's alike; each only writes what this says.
//
// - **Pages**: the slices of one RGBA8 texture array, 4096x4096 each - 64 MB - as many as a budget
//   allows. A page holds blocks of one scale, in a grid of cells (8 x scale) pixels square: a
//   replacement image's 8x8-texel blocks (render/raster.h's ReplacementImage), one to a cell.
// - **The block table**: a buffer of words, a run of them for each image on the card - one per
//   block of the image, row by row - saying where in the pages each block is, or 0 for a block the
//   image left clear (drawn with the game's own texels):
//     bit 31 set, the page in bits 22-30, the cell in bits 0-21 (row-major, 4096 / (8 x scale) to a
//     row).
// - **A job drawn with one** carries four words for the shader, p[20-23]:
//     p[20]  its run's first word in the table (bits 0-23), the scale less one (24-26), sampled
//            bilinear (27), and that it is replaced at all (28)
//     p[21]  the image's width (bits 0-12) and height (13-22) in texels
//     p[22]  where the job's texture page's corner is in the image, x and y 16 bits apiece, signed
//     p[23]  unused
// - **Evicted** longest unused first, whole images, when one more will not fit. The rasteriser
//   draws everything batched before placing an image, so nothing still to be drawn reads a page
//   being written.

#include "render/raster.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace psxemu {
namespace raster {

    class ReplacementAtlas {
     public:
        static constexpr uint32_t kPage = 4096;      // a page's side, in pixels
        static constexpr uint32_t kMostPages = 512;  // Direct3D 10's most slices in an array
        static constexpr uint64_t kPageBytes = static_cast<uint64_t>(kPage) * kPage * 4;
        static constexpr uint32_t kMostTable = 1u << 24;   // what p[20] can point into
        static constexpr uint32_t kReplaced = 1u << 28;    // p[20]'s flag

        // Blocks to write into a page, side by side along a row of its cells: `blocks` of them,
        // each `side` pixels square, from `pixels` (each block's own, RGBA8 rows).
        struct Run {
            uint32_t page = 0, x = 0, y = 0, side = 0;
            std::vector<const uint8_t*> pixels;
        };

        // At most `budget` bytes of pages, and at least one.
        explicit ReplacementAtlas(uint64_t budget)
            : most_pages_(static_cast<uint32_t>(
                  std::min<uint64_t>(std::max<uint64_t>(budget / kPageBytes, 1), kMostPages))) {}

        // The pages ever used: the slices the texture array needs.
        uint32_t pages() const { return static_cast<uint32_t>(pages_.size()); }
        uint32_t most_pages() const { return most_pages_; }
        // The block table, all of it, and the words of it changed since TakeChanged last said.
        const std::vector<uint32_t>& table() const { return table_; }
        bool TakeChanged(uint32_t* first, uint32_t* end) {
            if (changed_first_ >= changed_end_)
                return false;
            *first = changed_first_;
            *end = changed_end_;
            changed_first_ = UINT32_MAX;
            changed_end_ = 0;
            return true;
        }
        size_t resident() const { return residents_.size(); }
        uint64_t evictions() const { return evictions_; }

        // The words a job drawn with `replacement` carries (p[20-23]), if its image is on the card
        // - which counts as using it now. False if it is not.
        bool Words(const emulation::psx::TextureReplacement& replacement, uint32_t* words) {
            const auto found = residents_.find(replacement.image->id);
            if (found == residents_.end())
                return false;
            found->second.used = ++serial_;
            const emulation::psx::ReplacementImage& image = *replacement.image;
            words[0] = found->second.offset | ((image.scale - 1) << 24) |
                       (image.bilinear ? 1u << 27 : 0) | kReplaced;
            words[1] = (image.width & 8191) | ((image.height & 1023) << 13);
            words[2] = (static_cast<uint32_t>(replacement.page_x) & 0xFFFF) |
                       (static_cast<uint32_t>(replacement.page_y) << 16);
            words[3] = 0;
            return true;
        }

        // Puts `image` on the card: the blocks to write in `runs`, and its words in the table
        // (TakeChanged). Evicts others to make room. False if it cannot fit within the budget
        // even alone, or is too large for the shader's words.
        bool Place(const std::shared_ptr<const emulation::psx::ReplacementImage>& image,
                   std::vector<Run>* runs) {
            runs->clear();
            const emulation::psx::ReplacementImage& im = *image;
            const uint32_t side = 8 * im.scale, per_row = kPage / side;
            const uint32_t per_page = per_row * per_row;
            const size_t need = im.blocks.size();
            const uint32_t entries = im.blocks_x() * im.blocks_y();
            if (im.scale < 1 || im.scale > 8 || im.width > 4096 || im.height > 512 ||
                need > static_cast<size_t>(per_page) * most_pages_ || entries > kMostTable)
                return false;
            std::vector<uint32_t> cells;   // page << 22 | cell, a block's each
            cells.reserve(need);
            for (;;) {
                Grab(im.scale, need, &cells);
                if (cells.size() == need)
                    break;
                if (!EvictOldest(im.id)) {
                    for (uint32_t cell : cells)
                        Free(cell);
                    return false;
                }
            }
            uint32_t offset;
            while (!AllocateTable(entries, &offset)) {
                if (!EvictOldest(im.id)) {
                    for (uint32_t cell : cells)
                        Free(cell);
                    return false;
                }
            }
            std::fill(table_.begin() + offset, table_.begin() + offset + entries, 0u);
            for (size_t i = 0; i < need; ++i)
                table_[offset + im.blocks[i]] = 0x80000000u | cells[i];
            Changed(offset, offset + entries);
            Resident& resident = residents_[im.id];
            resident.image = image;
            resident.offset = offset;
            resident.entries = entries;
            resident.cells = cells;
            resident.used = ++serial_;

            // Runs: blocks whose cells follow each other along a row of one page.
            const size_t bytes = static_cast<size_t>(side) * side * 4;
            for (size_t i = 0; i < need; ++i) {
                const uint32_t page = cells[i] >> 22, cell = cells[i] & 0x3FFFFF;
                Run* run = runs->empty() ? nullptr : &runs->back();
                const bool follows = run != nullptr && i > 0 && run->page == page &&
                                     cell == (cells[i - 1] & 0x3FFFFF) + 1 && cell % per_row != 0;
                if (!follows) {
                    runs->emplace_back();
                    run = &runs->back();
                    run->page = page;
                    run->x = (cell % per_row) * side;
                    run->y = (cell / per_row) * side;
                    run->side = side;
                }
                run->pixels.push_back(im.pixels.data() + i * bytes);
            }
            return true;
        }

        // Everything forgotten - the card's copy is gone.
        void Clear() {
            residents_.clear();
            pages_.clear();
            table_.clear();
            free_ranges_.clear();
            changed_first_ = UINT32_MAX;
            changed_end_ = 0;
        }

     private:
        struct Page {
            uint32_t scale = 0;        // 0 while it holds nothing
            uint32_t free = 0;
            uint32_t hint = 0;         // no free cell before this one
            std::vector<uint8_t> taken;
        };
        struct Resident {
            std::shared_ptr<const emulation::psx::ReplacementImage> image;
            uint32_t offset = 0, entries = 0;
            std::vector<uint32_t> cells;
            uint64_t used = 0;
        };

        // Free cells of pages at `scale` - empty pages taken for it, new pages made within the
        // budget - until `cells` has `need`.
        void Grab(uint32_t scale, size_t need, std::vector<uint32_t>* cells) {
            const uint32_t per_row = kPage / (8 * scale);
            const uint32_t per_page = per_row * per_row;
            auto take = [&](uint32_t index) {
                Page& page = pages_[index];
                for (uint32_t cell = page.hint; cell < per_page && cells->size() < need; ++cell) {
                    if (page.taken[cell])
                        continue;
                    page.taken[cell] = 1;
                    --page.free;
                    page.hint = cell + 1;
                    cells->push_back((index << 22) | cell);
                }
            };
            for (uint32_t i = 0; i < pages_.size() && cells->size() < need; ++i)
                if (pages_[i].scale == scale && pages_[i].free > 0)
                    take(i);
            for (uint32_t i = 0; i < pages_.size() && cells->size() < need; ++i)
                if (pages_[i].scale == 0) {
                    Start(&pages_[i], scale);
                    take(i);
                }
            while (cells->size() < need && pages_.size() < most_pages_) {
                pages_.emplace_back();
                Start(&pages_.back(), scale);
                take(static_cast<uint32_t>(pages_.size() - 1));
            }
        }

        void Start(Page* page, uint32_t scale) {
            const uint32_t per_row = kPage / (8 * scale);
            page->scale = scale;
            page->free = per_row * per_row;
            page->hint = 0;
            page->taken.assign(page->free, 0);
        }

        void Free(uint32_t cell_word) {
            Page& page = pages_[cell_word >> 22];
            const uint32_t cell = cell_word & 0x3FFFFF;
            page.taken[cell] = 0;
            ++page.free;
            page.hint = std::min(page.hint, cell);
            if (page.free == page.taken.size()) {   // empty: any scale may have it
                page.scale = 0;
                page.taken.clear();
            }
        }

        // The resident longest unused, other than `keep`, let go. False if there is none.
        bool EvictOldest(uint64_t keep) {
            auto oldest = residents_.end();
            for (auto it = residents_.begin(); it != residents_.end(); ++it)
                if (it->first != keep && (oldest == residents_.end() || it->second.used < oldest->second.used))
                    oldest = it;
            if (oldest == residents_.end())
                return false;
            for (uint32_t cell : oldest->second.cells)
                Free(cell);
            free_ranges_.emplace_back(oldest->second.offset, oldest->second.entries);
            residents_.erase(oldest);
            ++evictions_;
            return true;
        }

        // A run of `entries` words of the table: a free one, or the table grown.
        bool AllocateTable(uint32_t entries, uint32_t* offset) {
            for (size_t i = 0; i < free_ranges_.size(); ++i) {
                auto& range = free_ranges_[i];
                if (range.second < entries)
                    continue;
                *offset = range.first;
                range.first += entries;
                range.second -= entries;
                if (range.second == 0)
                    free_ranges_.erase(free_ranges_.begin() + static_cast<ptrdiff_t>(i));
                return true;
            }
            if (table_.size() + entries > kMostTable)
                return false;
            *offset = static_cast<uint32_t>(table_.size());
            table_.resize(table_.size() + entries, 0);
            return true;
        }

        void Changed(uint32_t first, uint32_t end) {
            changed_first_ = std::min(changed_first_, first);
            changed_end_ = std::max(changed_end_, end);
        }

        const uint32_t most_pages_;
        std::vector<Page> pages_;
        std::unordered_map<uint64_t, Resident> residents_;   // by ReplacementImage::id
        std::vector<uint32_t> table_;
        std::vector<std::pair<uint32_t, uint32_t>> free_ranges_;   // offset, entries
        uint32_t changed_first_ = UINT32_MAX, changed_end_ = 0;
        uint64_t serial_ = 0;
        uint64_t evictions_ = 0;
    };

}   // namespace raster
}   // namespace psxemu
