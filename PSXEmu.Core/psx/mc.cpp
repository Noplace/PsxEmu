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

namespace emulation {
namespace psx {

MC::MC() {
}

MC::~MC() {
}

int MC::Initialize() {
  mcfile = nullptr;
  flag_ = 0x08;
  return S_OK;
}

int MC::Deinitialize() {
  SafeDelete(&mcfile);
  return S_OK;
}

int MC::LoadFile(const char* filename) {
  FILE* fp = fopen(filename,"rb");
  if (!fp) return S_FALSE;
  fseek(fp,0,SEEK_END);
  int size = ftell(fp);
  fseek(fp,0,SEEK_SET);
  if (size != 0x20000) {
    fclose(fp);
    return S_FALSE;
  }
  mcfile = new MCFile();
  fread(mcfile,sizeof(uint8_t),sizeof(MCFile),fp);
  fclose(fp);

  filename_ = filename;
  flag_ = 0x08; // bit3=1 indicates directory not read yet
  return S_OK;
}

int MC::CreateFile(const char* filename) {
  FILE* fp = fopen(filename, "wb");
  if (!fp) return S_FALSE;
  
  if (!mcfile) {
    mcfile = new MCFile();
  }
  
  // Flash memory defaults to 0xFF, but let's just initialize it blank.
  // The BIOS / games will format it if it detects it as unformatted.
  memset(mcfile, 0, sizeof(MCFile));
  
  fwrite(mcfile, sizeof(uint8_t), sizeof(MCFile), fp);
  fclose(fp);

  filename_ = filename;
  flag_ = 0x08;
  return S_OK;
}

bool MC::ReadSector(uint16_t sector, uint8_t* out_buffer) {
  if (!mcfile || sector >= 1024) return false;
  uint8_t* buf = (uint8_t*)mcfile;
  memcpy(out_buffer, &buf[sector * 128], 128);
  return true;
}

bool MC::WriteSector(uint16_t sector, const uint8_t* in_buffer) {
  if (!mcfile || sector >= 1024) return false;
  uint8_t* buf = (uint8_t*)mcfile;
  memcpy(&buf[sector * 128], in_buffer, 128);

  // Write through to file
  if (!filename_.empty()) {
    FILE* fp = fopen(filename_.c_str(), "r+b");
    if (fp) {
      fseek(fp, sector * 128, SEEK_SET);
      fwrite(in_buffer, 1, 128, fp);
      fclose(fp);
    }
  }

  // Bit3 is reset when writing to the card
  clear_flag(0x08);
  return true;
}

int MC::ReadMCFile(int index) {
  return S_OK;
}

}
}