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
#ifndef EMULATION_PSX_MC_H
#define EMULATION_PSX_MC_H

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

class MC : public Component {
 public:
  MC();
  ~MC();
  int Initialize();
  int Deinitialize();
  int LoadFile(char* filename);
 private:
  MCFile* mcfile;
  int ReadMCFile(int index);

};

}
}

#endif