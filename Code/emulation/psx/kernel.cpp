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
//#include <stdio.h>

//#define KERNEL_DEBUG
#define PSX_OUTPUT

namespace emulation {
namespace psx {

Kernel::Kernel() {

}

Kernel::~Kernel() {

}

void Kernel::Initialize() {
  #if defined(_DEBUG) && defined(KERNEL_DEBUG)
    //debug.Open("kernel system calls.txt");
  #endif
  #if defined(_DEBUG) && defined(PSX_OUTPUT)
    psxout.Open("psxout.txt");
  #endif
}

void Kernel::Call() {
  int call_type = system().cpu().context()->pc&0xff;
  int call_index = system().cpu().context()->gp.t1 & 0xFF;
  CpuContext* context = system().cpu().context();


  #if defined(_DEBUG) && defined(KERNEL_DEBUG)
    if (system().cpu().bios_logged[((call_type & 0x7F) >> 4) - 2][call_index] == false) {
      system().cpu().inside_bios_call = true;
      system().cpu().bios_logged[((call_type & 0x7F) >> 4) - 2][call_index] = true;
    }
    BiosCall call = system_->csvlog.bios_call_[((call_type & 0x7F) >> 4) - 2][call_index];
    if (system_->csvlog.fp) {
      fprintf(system_->csvlog.fp,"0x%08X,0x%08X,%s,0x%02X,0x%02X\n",system().cpu().index,context->prev_pc,call.prototype,call.address,call.operation);
    }
    //if (system().cpu().bioscode.fp) {
      //fprintf(system().cpu().bioscode.fp,"call @ %08X $%02X $%02X %s \n",system().cpu().context()->prev_pc,call.address,call.operation,call.prototype);
   // }
  #endif

  if (call_type == 0xa0) {
    switch (call_index) {

      case 0x08: {
        //context->gp.v0() = fgetc(psxout.fp);
        //context->pc = context->gp.ra();
        break;
      }

      case 0x12: {
        //int a = context->gp.a0();
        //context->pc = context->gp.ra();
        break;
      }
      case 0x44: 
        //context->gp.t1 = 0x18;
        //context->gp.t2 = 0xB0;
        //context->pc = context->gp.t2;
        break;
                 
      case 0x3F: {
        #ifdef KERNEL_DEBUG
          //if (debug.fp)
            //fprintf(debug.fp,);
        #endif
        break;
      }
    }
  }

  if (call_type == 0xb0) {
    switch (call_index) {

      case 0x00: {
        
        //context->pc = context->gp.ra();
        break;
      }
      case 0x3D: {
        #if defined(_DEBUG) && defined(PSX_OUTPUT)
          fputc(context->gp.a0,psxout.fp);
        #endif
          //extern bool output_inst;
          //extern uint32_t until_address;
          //output_inst = true;
          //until_address = context->gp.ra;
        //putc(context->gp.a0(),context->gp.a1());
        //context->pc = context->gp.ra();
        break;
      }
    }
  }

  if (call_type == 0xc0) {
    switch (call_index) {
      case 0x18: 
        //putc(context->gp.a0(),context->gp.a1());
        #if defined(_DEBUG) && defined(PSX_OUTPUT)
          fputc(context->gp.a0,psxout.fp);
        #endif
        //context->pc = context->gp.ra();
        break;

      case 0x1C:
        
        break;
    }
  }
  //system().cpu().context()->pc = system().cpu().context()->gp.ra();
  //throw;
}

void Kernel::putc(char c,int fd) {
  #if defined(_DEBUG) && defined(KERNEL_DEBUG)
    if (psxout.fp)
      fputc(c,psxout.fp);
  #endif
}

}
}