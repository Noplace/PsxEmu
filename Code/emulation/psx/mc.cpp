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
#include "global.h"

namespace emulation {
namespace psx {

MC::MC() {
}

MC::~MC() {
}

int MC::Initialize() {
  mcfile = nullptr;
  return S_OK;
}

int MC::Deinitialize() {
  SafeDelete(&mcfile);
  return S_OK;
}

int MC::LoadFile(char* filename) {
  FILE* fp = fopen(filename,"rb");
  fseek(fp,0,SEEK_END);
  int size = ftell(fp);
  fseek(fp,0,SEEK_SET);
  if (size != 0x20000) 
    return S_FALSE;
  //uint8_t* buffer = new uint8_t[0x20000];
  mcfile = new MCFile();
  fread(mcfile,sizeof(uint8_t),sizeof(MCFile),fp);

  uint8_t* buf = (uint8_t*)mcfile;
  for (int i=0;i<20;++i) {
    if (mcfile->dir.broken_sectors[i].number != 0xffffffff) {

    }
  }


  fclose(fp);


  ReadMCFile(0);
  return S_OK;
}

int MC::ReadMCFile(int index) {
  
  //if (mcfile->dir.dir[index].block_alloc_state 
  
  return S_OK;
}

}
}