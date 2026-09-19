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

MC::MC() : mcfile(nullptr), flag_(0x08) {
}

MC::~MC() {
  // The last chance to keep what a game saved, if nothing flushed it first.
  Eject();
}

int MC::Initialize() {
  mcfile = nullptr;
  flag_ = 0x08;
  dirty_ = false;
  idle_frames_ = 0;
  return S_OK;
}

int MC::Deinitialize() {
  Eject();
  return S_OK;
}

int MC::LoadFile(const char* filename) {
  FILE* fp = fopen(filename, "rb");
  if (!fp) return S_FALSE;
  fseek(fp, 0, SEEK_END);
  const long size = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  if (size != static_cast<long>(sizeof(MCFile))) {
    fclose(fp);
    return S_FALSE;
  }
  MCFile* card = new MCFile();
  const size_t read = fread(card, 1, sizeof(MCFile), fp);
  fclose(fp);
  if (read != sizeof(MCFile)) {
    delete card;
    return S_FALSE;
  }

  // Whatever was in the slot comes out first, saved - inserting over a card used to leak it and
  // drop anything not yet written.
  Eject();
  mcfile = card;
  filename_ = filename;
  flag_ = 0x08;   // bit 3: a new card, its directory not read yet
  return S_OK;
}

int MC::CreateFile(const char* filename) {
  MCFile* card = new MCFile();
  // Formatted, as the BIOS's own format leaves one - a card of zeroes makes the BIOS offer to
  // format it before a game can save.
  mcdir::Format(reinterpret_cast<uint8_t*>(card));
  if (!WriteWholeFile(filename, card)) {
    delete card;
    return S_FALSE;
  }
  Eject();
  mcfile = card;
  filename_ = filename;
  flag_ = 0x08;
  return S_OK;
}

void MC::Eject() {
  if (mcfile == nullptr)
    return;
  Flush();
  delete mcfile;
  mcfile = nullptr;
  filename_.clear();
  dirty_ = false;
  idle_frames_ = 0;
  flag_ = 0x08;
}

bool MC::ReadSector(uint16_t sector, uint8_t* out_buffer) {
  if (!mcfile || sector >= 1024) return false;
  const uint8_t* buf = reinterpret_cast<const uint8_t*>(mcfile);
  memcpy(out_buffer, &buf[sector * 128], 128);
  return true;
}

// Into memory only. A save is 64 of these in a row, and writing each through to the file - open,
// seek, write, close - was slow, and a crash part-way through left a card with half a save on it.
// The whole card goes to disk at once, from Flush, once the writes stop.
bool MC::WriteSector(uint16_t sector, const uint8_t* in_buffer) {
  if (!mcfile || sector >= 1024) return false;
  uint8_t* buf = reinterpret_cast<uint8_t*>(mcfile);
  memcpy(&buf[sector * 128], in_buffer, 128);
  dirty_ = true;
  idle_frames_ = 0;

  // Bit3 is reset when writing to the card
  clear_flag(0x08);
  return true;
}

void MC::OnFrame() {
  if (!dirty_)
    return;
  if (++idle_frames_ >= kFlushAfterIdleFrames)
    Flush();
}

bool MC::Flush() {
  if (mcfile == nullptr || !dirty_)
    return true;
  if (filename_.empty() || !WriteWholeFile(filename_, mcfile)) {
    ++flush_failures_;
    return false;
  }
  dirty_ = false;
  idle_frames_ = 0;
  return true;
}

void MC::Modified() {
  dirty_ = true;
  idle_frames_ = 0;
  // What a card swap tells software: this is not the card you read the directory of.
  flag_ |= 0x08;
}

uint8_t* MC::data() { return reinterpret_cast<uint8_t*>(mcfile); }
const uint8_t* MC::data() const { return reinterpret_cast<const uint8_t*>(mcfile); }

// Written beside the card and renamed over it, so the file on disk is always a whole card - the
// old one or the new one, never part of each.
bool MC::WriteWholeFile(const std::string& path, const MCFile* card) {
  const std::string temp = path + ".tmp";
  FILE* fp = fopen(temp.c_str(), "wb");
  if (!fp)
    return false;
  const size_t written = fwrite(card, 1, sizeof(MCFile), fp);
  const bool closed = (fclose(fp) == 0);
  if (written != sizeof(MCFile) || !closed) {
    DeleteFileA(temp.c_str());
    return false;
  }
  if (!MoveFileExA(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    DeleteFileA(temp.c_str());
    return false;
  }
  return true;
}

}
}
