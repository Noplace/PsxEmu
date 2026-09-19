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
#include "psx/psx.h"
#include "psx/mc_directory.h"

namespace emulation {
namespace psx {
namespace mcdir {

namespace {

// Directory frame fields (block 0, frames 1-15).
const size_t kStateOffset = 0x00;      // 32-bit; only the low byte carries anything
const size_t kSizeOffset = 0x04;
const size_t kNextOffset = 0x08;       // 16-bit: next block - 1, FFFFh at the end
const size_t kNameOffset = 0x0A;       // 20 characters and a terminator
const size_t kNameLength = 20;
const uint16_t kEndOfChain = 0xFFFF;

const uint8_t kFree = 0xA0;
const uint8_t kFirst = 0x51, kMiddle = 0x52, kLast = 0x53;
const uint8_t kDeletedFirst = 0xA1, kDeletedMiddle = 0xA2, kDeletedLast = 0xA3;

// Title frame fields (frame 0 of a save's first block).
const size_t kIconFlagOffset = 0x02;   // 11h, 12h, 13h: one, two, three icon frames
const size_t kTitleOffset = 0x04;
const size_t kTitleLength = 64;
const size_t kPaletteOffset = 0x60;    // 16 colours, 15-bit like VRAM

uint8_t* Frame(uint8_t* card, int block, int frame) {
  return card + block * kBlockSize + frame * kFrameSize;
}
const uint8_t* Frame(const uint8_t* card, int block, int frame) {
  return card + block * kBlockSize + frame * kFrameSize;
}

uint8_t* DirFrame(uint8_t* card, int block) { return Frame(card, 0, block); }
const uint8_t* DirFrame(const uint8_t* card, int block) { return Frame(card, 0, block); }

uint8_t State(const uint8_t* dir) { return dir[kStateOffset]; }

uint32_t Read32(const uint8_t* p) {
  return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
uint16_t Read16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
void Write32(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
  p[2] = static_cast<uint8_t>(v >> 16);
  p[3] = static_cast<uint8_t>(v >> 24);
}
void Write16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
}

void Seal(uint8_t* frame) { frame[kFrameSize - 1] = FrameChecksum(frame); }

std::string NameOf(const uint8_t* dir) {
  size_t length = 0;
  while (length < kNameLength && dir[kNameOffset + length] != '\0')
    ++length;
  return std::string(reinterpret_cast<const char*>(dir + kNameOffset), length);
}

bool IsFreeForUse(uint8_t state) { return (state & 0xF0) == 0xA0; }

// Follows a chain from `first`, returning its blocks in order, or an empty list if the links
// leave the card, loop, or run past fifteen blocks - a damaged directory, not a save.
std::vector<int> Chain(const uint8_t* card, int first) {
  std::vector<int> blocks;
  bool seen[kBlocks] = {};
  int block = first;
  while (true) {
    if (block < 1 || block >= kBlocks || seen[block])
      return {};
    seen[block] = true;
    blocks.push_back(block);
    const uint16_t next = Read16(DirFrame(card, block) + kNextOffset);
    if (next == kEndOfChain)
      return blocks;
    block = next + 1;
  }
}

// Shift-JIS to UTF-16, then the full-width forms most titles are written in narrowed to plain
// ASCII - "ＦＩＮＡＬ" reads better as "FINAL" in a list.
std::wstring DecodeTitle(const uint8_t* title) {
  size_t length = 0;
  while (length < kTitleLength && title[length] != 0)
    ++length;
  if (length == 0)
    return std::wstring();
  const int count = MultiByteToWideChar(932, 0, reinterpret_cast<const char*>(title),
                                        static_cast<int>(length), nullptr, 0);
  if (count <= 0)
    return std::wstring();
  std::wstring wide(static_cast<size_t>(count), L'\0');
  MultiByteToWideChar(932, 0, reinterpret_cast<const char*>(title), static_cast<int>(length),
                      &wide[0], count);
  for (wchar_t& c : wide) {
    if (c >= 0xFF01 && c <= 0xFF5E)
      c = static_cast<wchar_t>(c - 0xFEE0);
    else if (c == 0x3000)
      c = L' ';
  }
  while (!wide.empty() && (wide.back() == L' ' || wide.back() == L'\0'))
    wide.pop_back();
  return wide;
}

uint32_t PaletteToArgb(uint16_t colour) {
  if (colour == 0)
    return 0;   // transparent
  const uint32_t r = colour & 0x1F, g = (colour >> 5) & 0x1F, b = (colour >> 10) & 0x1F;
  return 0xFF000000u | (((r << 3) | (r >> 2)) << 16) | (((g << 3) | (g >> 2)) << 8) |
         ((b << 3) | (b >> 2));
}

}  // namespace

uint8_t FrameChecksum(const uint8_t* frame) {
  uint8_t sum = 0;
  for (size_t i = 0; i < kFrameSize - 1; ++i)
    sum ^= frame[i];
  return sum;
}

void Format(uint8_t* card) {
  memset(card, 0xFF, kCardSize);

  uint8_t* header = Frame(card, 0, 0);
  memset(header, 0, kFrameSize);
  header[0] = 'M';
  header[1] = 'C';
  Seal(header);

  for (int block = 1; block < kBlocks; ++block) {
    uint8_t* dir = DirFrame(card, block);
    memset(dir, 0, kFrameSize);
    dir[kStateOffset] = kFree;
    Write16(dir + kNextOffset, kEndOfChain);
    Seal(dir);
  }

  // The broken sector list: no broken sectors.
  for (int frame = 16; frame < 36; ++frame) {
    uint8_t* entry = Frame(card, 0, frame);
    memset(entry, 0, kFrameSize);
    Write32(entry, 0xFFFFFFFF);
    Write16(entry + kNextOffset, kEndOfChain);
    Seal(entry);
  }
  for (int frame = 36; frame < 63; ++frame)
    memset(Frame(card, 0, frame), 0, kFrameSize);

  // The write-test frame is a copy of the header.
  memcpy(Frame(card, 0, 63), header, kFrameSize);
}

bool IsFormatted(const uint8_t* card) { return card[0] == 'M' && card[1] == 'C'; }

std::vector<Save> List(const uint8_t* card, bool include_deleted) {
  std::vector<Save> saves;
  for (int block = 1; block < kBlocks; ++block) {
    const uint8_t* dir = DirFrame(card, block);
    const uint8_t state = State(dir);
    const bool deleted = (state == kDeletedFirst);
    if (state != kFirst && !(include_deleted && deleted))
      continue;

    const std::vector<int> chain = Chain(card, block);
    if (chain.empty())
      continue;

    Save save;
    save.first_block = block;
    save.blocks = static_cast<int>(chain.size());
    save.size = Read32(dir + kSizeOffset);
    save.deleted = deleted;
    save.filename = NameOf(dir);

    const uint8_t* title = Frame(card, block, 0);
    save.title = DecodeTitle(title + kTitleOffset);
    const uint8_t flag = title[kIconFlagOffset];
    save.icon_frames = (flag >= 0x11 && flag <= 0x13) ? flag - 0x10 : 0;
    save.icons.resize(static_cast<size_t>(save.icon_frames) * kIconSize * kIconSize);
    for (int f = 0; f < save.icon_frames; ++f) {
      const uint8_t* pixels = Frame(card, block, 1 + f);
      uint32_t* out = &save.icons[static_cast<size_t>(f) * kIconSize * kIconSize];
      for (int i = 0; i < kIconSize * kIconSize / 2; ++i) {
        // Four bits a pixel, the left one in the low nibble.
        out[i * 2] = PaletteToArgb(Read16(title + kPaletteOffset + (pixels[i] & 0x0F) * 2));
        out[i * 2 + 1] = PaletteToArgb(Read16(title + kPaletteOffset + (pixels[i] >> 4) * 2));
      }
    }
    saves.push_back(std::move(save));
  }
  return saves;
}

int FreeBlocks(const uint8_t* card) {
  int count = 0;
  for (int block = 1; block < kBlocks; ++block)
    if (IsFreeForUse(State(DirFrame(card, block))))
      ++count;
  return count;
}

bool Delete(uint8_t* card, int first_block, std::string* error) {
  if (first_block < 1 || first_block >= kBlocks || State(DirFrame(card, first_block)) != kFirst) {
    if (error) *error = "That block does not start a save.";
    return false;
  }
  const std::vector<int> chain = Chain(card, first_block);
  if (chain.empty()) {
    if (error) *error = "The save's block chain is damaged.";
    return false;
  }
  for (size_t i = 0; i < chain.size(); ++i) {
    uint8_t* dir = DirFrame(card, chain[i]);
    dir[kStateOffset] = (i == 0) ? kDeletedFirst
                                 : (i + 1 == chain.size()) ? kDeletedLast : kDeletedMiddle;
    Seal(dir);
  }
  return true;
}

bool Undelete(uint8_t* card, int first_block, std::string* error) {
  if (first_block < 1 || first_block >= kBlocks ||
      State(DirFrame(card, first_block)) != kDeletedFirst) {
    if (error) *error = "That block does not start a deleted save.";
    return false;
  }
  const std::vector<int> chain = Chain(card, first_block);
  if (chain.empty()) {
    if (error) *error = "The save's block chain is damaged.";
    return false;
  }
  for (size_t i = 0; i < chain.size(); ++i) {
    const uint8_t want = (i == 0) ? kDeletedFirst
                                  : (i + 1 == chain.size()) ? kDeletedLast : kDeletedMiddle;
    if (State(DirFrame(card, chain[i])) != want) {
      if (error) *error = "Part of this save has been reused by another; it cannot be restored.";
      return false;
    }
  }
  const std::string name = NameOf(DirFrame(card, first_block));
  for (const Save& other : List(card, false)) {
    if (other.filename == name) {
      if (error) *error = "A save with the same name is already on the card.";
      return false;
    }
  }
  for (size_t i = 0; i < chain.size(); ++i) {
    uint8_t* dir = DirFrame(card, chain[i]);
    dir[kStateOffset] = (i == 0) ? kFirst : (i + 1 == chain.size()) ? kLast : kMiddle;
    Seal(dir);
  }
  return true;
}

bool Export(const uint8_t* card, int first_block, std::vector<uint8_t>* mcs,
            std::string* error) {
  if (first_block < 1 || first_block >= kBlocks) {
    if (error) *error = "No such block.";
    return false;
  }
  const uint8_t state = State(DirFrame(card, first_block));
  if (state != kFirst && state != kDeletedFirst) {
    if (error) *error = "That block does not start a save.";
    return false;
  }
  const std::vector<int> chain = Chain(card, first_block);
  if (chain.empty()) {
    if (error) *error = "The save's block chain is damaged.";
    return false;
  }
  mcs->assign(kFrameSize + chain.size() * kBlockSize, 0);
  // The directory frame as a lone, live save: no link to a block on this card.
  uint8_t* dir = mcs->data();
  memcpy(dir, DirFrame(card, first_block), kFrameSize);
  dir[kStateOffset] = kFirst;
  Write16(dir + kNextOffset, kEndOfChain);
  Seal(dir);
  for (size_t i = 0; i < chain.size(); ++i)
    memcpy(mcs->data() + kFrameSize + i * kBlockSize, Frame(card, chain[i], 0), kBlockSize);
  return true;
}

bool Import(uint8_t* card, const std::vector<uint8_t>& mcs, std::string* error) {
  if (mcs.size() <= kFrameSize || (mcs.size() - kFrameSize) % kBlockSize != 0 ||
      (mcs.size() - kFrameSize) / kBlockSize > static_cast<size_t>(kBlocks - 1)) {
    if (error) *error = "Not a .mcs save: it must be one directory frame and 1-15 blocks.";
    return false;
  }
  const int count = static_cast<int>((mcs.size() - kFrameSize) / kBlockSize);
  const uint8_t* source_dir = mcs.data();
  const std::string name = NameOf(source_dir);
  if (name.empty()) {
    if (error) *error = "The save has no name.";
    return false;
  }

  for (const Save& existing : List(card, true)) {
    if (existing.filename != name)
      continue;
    if (!existing.deleted) {
      if (error) *error = "A save with the same name is already on the card.";
      return false;
    }
    // A deleted copy of the same save would be restorable over the new one; clear it.
    for (int block : Chain(card, existing.first_block)) {
      uint8_t* dir = DirFrame(card, block);
      memset(dir, 0, kFrameSize);
      dir[kStateOffset] = kFree;
      Write16(dir + kNextOffset, kEndOfChain);
      Seal(dir);
    }
  }

  std::vector<int> blocks;
  for (int block = 1; block < kBlocks && static_cast<int>(blocks.size()) < count; ++block)
    if (State(DirFrame(card, block)) == kFree)
      blocks.push_back(block);
  // Free blocks first; deleted ones only if there are not enough, so an import does not
  // needlessly destroy something that could still be undeleted.
  for (int block = 1; block < kBlocks && static_cast<int>(blocks.size()) < count; ++block)
    if (IsFreeForUse(State(DirFrame(card, block))) && State(DirFrame(card, block)) != kFree)
      blocks.push_back(block);
  if (static_cast<int>(blocks.size()) < count) {
    if (error) {
      *error = "Not enough free blocks: the save needs " + std::to_string(count) +
               " and the card has " + std::to_string(FreeBlocks(card)) + ".";
    }
    return false;
  }

  for (int i = 0; i < count; ++i) {
    uint8_t* dir = DirFrame(card, blocks[i]);
    memset(dir, 0, kFrameSize);
    if (i == 0) {
      dir[kStateOffset] = kFirst;
      uint32_t size = Read32(source_dir + kSizeOffset);
      if (size == 0 || size > static_cast<uint32_t>(count * kBlockSize))
        size = static_cast<uint32_t>(count * kBlockSize);
      Write32(dir + kSizeOffset, size);
      memcpy(dir + kNameOffset, name.data(), name.size());
    } else {
      dir[kStateOffset] = (i + 1 == count) ? kLast : kMiddle;
    }
    Write16(dir + kNextOffset,
            (i + 1 == count) ? kEndOfChain : static_cast<uint16_t>(blocks[i + 1] - 1));
    Seal(dir);
    memcpy(Frame(card, blocks[i], 0), mcs.data() + kFrameSize + i * kBlockSize, kBlockSize);
  }
  return true;
}

}  // namespace mcdir
}  // namespace psx
}  // namespace emulation
