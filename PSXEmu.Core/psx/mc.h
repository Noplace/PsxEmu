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

#include <string>

namespace emulation {
namespace psx {

#pragma pack(push, 1)
struct MCHeaderFrame {
  char id[2];
  uint8_t _unused[125];
  uint8_t checksum;
};

struct MCDirectoryFrame {
  uint32_t block_alloc_state;
  uint32_t filesize;
  uint16_t next_block;
  char filename[21];
  uint8_t _unused0;
  uint8_t _unused1[95];
  uint8_t checksum;
};

struct MCBrokerSectorFrame {
  uint32_t number;
  uint8_t _unused[123];
  uint8_t checksum;
};

typedef uint8_t MCSector[128];
typedef MCSector MCFrame;

struct MCBlock0 {
  MCHeaderFrame header;
  MCDirectoryFrame dir[15];
  MCBrokerSectorFrame broken_sectors[20];
  MCFrame broken_sector_replacements[20];
  MCFrame _unused[7];
  MCFrame write_test;
};

struct MCTitleFrame {
  char id[2];
  uint8_t icon_display_flag;
  uint8_t block_number;
  uint8_t title_shift_jis[64];
  uint8_t _reserved1[12];
  uint8_t _reserved2[16];
  uint16_t icon_pallete[16];
};

typedef MCSector MCIconFrame; //16x16 pixels,4bit


struct MCBlock {
  MCTitleFrame title;
  MCFrame frames[63];
};

struct MCFile {
  MCBlock0 dir;
  MCBlock blocks[15];
};
#pragma pack(pop)

// One memory card slot. The card is held in memory; a game's writes land there and the file is
// rewritten whole once they stop (Flush), not sector by sector. Everything here belongs to the
// machine's thread - Docs/Threading-Plan.md, rule 4.
class MC : public Component {
 public:
  MC();
  ~MC();
  int Initialize();
  int Deinitialize();

  // Inserts the card in `filename`, ejecting - and so saving - whatever was in the slot.
  int LoadFile(const char* filename);
  // Writes a new, formatted card to `filename` and inserts it.
  int CreateFile(const char* filename);
  // Takes the card out, saved first. The slot then reports no card.
  void Eject();

  bool connected() const { return mcfile != nullptr; }
  const std::string& filename() const { return filename_; }
  uint8_t flag() const { return flag_; }
  void clear_flag(uint8_t mask) { flag_ &= ~mask; }
  void set_flag(uint8_t mask) { flag_ |= mask; }

  bool ReadSector(uint16_t sector, uint8_t* out_buffer);
  bool WriteSector(uint16_t sector, const uint8_t* in_buffer);

  // Once a frame, from the host: saves the card once a second has passed with no writes, so a
  // save is on disk within about a second of the game finishing it.
  void OnFrame();
  // Writes the card to its file now if anything changed. False if that failed.
  bool Flush();
  bool dirty() const { return dirty_; }
  uint64_t flush_failures() const { return flush_failures_; }

  // The raw 128 KB image, for psx/mc_directory.h. Null with no card in. After changing it,
  // call Modified: the card is saved, and flagged as swapped so a running game re-reads the
  // directory instead of trusting its copy.
  uint8_t* data();
  const uint8_t* data() const;
  void Modified();

  static const int kFlushAfterIdleFrames = 60;

 private:
  static bool WriteWholeFile(const std::string& path, const MCFile* card);

  MCFile* mcfile;
  std::string filename_;
  uint8_t flag_;
  bool dirty_ = false;
  int idle_frames_ = 0;
  uint64_t flush_failures_ = 0;
};

}
}
