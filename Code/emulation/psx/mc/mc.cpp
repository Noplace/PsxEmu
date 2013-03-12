#include "../system.h"

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