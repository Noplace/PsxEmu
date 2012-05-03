/******************************************************************************
* Copyright Khalid Al-Kooheji 2010
* Filename    : system.cpp
* Description : 
* 
*
* 
* 
* 
*******************************************************************************/
#include "system.h"
#include <stdio.h>
#include <stdlib.h>
#pragma warning( disable : 4996 )

namespace emulation {
namespace psx {

/******************************************************************************
* Name        : System
* Description : System Constructor
* Parameters  : (none)
*
* Notes :
* 
* 
*******************************************************************************/
System::System() {
  memset(&cpu_context_,0,sizeof(cpu_context_));
}

/******************************************************************************
* Name        : ~System
* Description : System Destructor
* Parameters  : (none)
*
* Notes :
* 
* 
*******************************************************************************/
System::~System() {
    
    
}

/******************************************************************************
* Name        : Initialize
* Description : initialize system
* Parameters  : (none)
*
* Notes :
* 
* 
*******************************************************************************/
int System::Initialize() {
  #if defined(_DEBUG)
    csvlog.system_ = this;
    csvlog.Open("log.csv");
  #endif
 
  io_.set_system(this);
  cpu_.set_system(this);
  spu_.set_system(this);
  kernel_.set_system(this);

  auto set_comp_systems = [&](Component& comp) {
    //comp.set_system(this);
    //comp.cpu_ = &this->cpu();
    //comp.io_ = &this->io();
  };


  io_.Initialize();
  LoadBiosFromFile("D:\\Personal\\Projects\\PsxEmu\\Bios\\SCPH1001.BIN");
  cpu_.set_context(&cpu_context_);
  cpu_.Initialize();
  cpu_.Reset();
  spu_.Initialize();
  kernel_.Initialize();

  while (cpu_.context()->pc!=0x80030000) {
	   Step();
  }

  return 0;
}

int System::Deinitialize() {
  io_.Deinitialize();
  return 0;
}

/******************************************************************************
* Name        : Cpu
* Description : Cpu Constructor
* Parameters  : (none)
*
* Notes :
* 
* 
*******************************************************************************/
void System::Step() {

  cpu_.ExecuteInstruction();
  if (cpu_.context()->pc == 0xa0 || 
      cpu_.context()->pc == 0xb0 || 
      cpu_.context()->pc == 0xc0) {
        //bios call
        kernel_.Call();
        int a= 1;
  }

  io_.rootcounter_[0].Tick();
  io_.rootcounter_[1].Tick();
  io_.rootcounter_[2].Tick();
  io_.rootcounter_[3].Tick();


  if (io_.interrupt_reg() & io_.interrupt_mask())	{
		if ((cpu_.context()->ctrl.SR & 0x400)&&(cpu_.context()->ctrl.SR & 1))	{
        cpu_.RaiseException(cpu_.context()->pc,kOtherException,kExceptionCodeInt);
        cpu_.context()->ctrl.Cause |= 0x400;
	   	  //Exception(0x400,branch_slot);
		}
	}

}

/******************************************************************************
* Name        : LoadBiosFromMemory
* Description : loads bios from a buffer in ram
* Parameters  : buffer
*
* Notes : must be 0x80000 bytes
* 
* 
*******************************************************************************/
void System::LoadBiosFromMemory(void* buffer) {
  memcpy(io_.bios_buffer.u8,(uint8_t*)buffer,0x80000);
}

/******************************************************************************
* Name        : LoadBiosFromFile
* Description : loads bios file
* Parameters  : filename
*
* Notes : must be 0x80000 bytes
* 
* 
*******************************************************************************/
void System::LoadBiosFromFile(char* filename) {
  FILE* fp = fopen(filename,"rb");
  fseek(fp,0,SEEK_END);
  int size = ftell(fp);
  fseek(fp,0,SEEK_SET);
  if (size != 0x80000) 
    return;
  uint8_t* buffer = new uint8_t[0x80000];
  fread(buffer,sizeof(uint8_t),0x80000,fp);
  fclose(fp);
  LoadBiosFromMemory(buffer);
  delete [] buffer;
  buffer = NULL;
}

}
}