#include "../system.h"

#define KERNEL_DEBUG

namespace emulation {
namespace psx {

Kernel::Kernel() {

}

Kernel::~Kernel() {

}

void Kernel::Initialize() {
  #if defined(_DEBUG) && defined(KERNEL_DEBUG)
    //debug.Open("kernel system calls.txt");
    psxout.Open("psxout.txt");
  #endif
}

void Kernel::Call() {
  int call_type = system().cpu().context()->pc&0xff;
  int call_index = system().cpu().context()->gp.t1;
  CpuContext* context = system().cpu().context();

  if (system().cpu().bios_logged[((call_type & 0x7F) >> 4) - 2][call_index] == false) {
    system().cpu().inside_bios_call = true;
    system().cpu().bios_logged[((call_type & 0x7F) >> 4) - 2][call_index] = true;
  }

  #if defined(_DEBUG) && defined(KERNEL_DEBUG)
    BiosCall call = debug.bios_call_[((call_type & 0x7F) >> 4) - 2][call_index];
    if (system_->log.fp) {
      fprintf(system_->log.fp,"call @ %08X $%02X $%02X %s \n",system().cpu().context()->prev_pc,call.address,call.operation,call.prototype);
    }
    if (system().cpu().bioscode.fp) {
      fprintf(system().cpu().bioscode.fp,"call @ %08X $%02X $%02X %s \n",system().cpu().context()->prev_pc,call.address,call.operation,call.prototype);
    }
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
        //fputc(context->gp.a0(),psxout.fp);
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
    if (debug.fp)
      fputc(c,psxout.fp);
  #endif
}

}
}