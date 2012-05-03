/******************************************************************************
* Copyright Khalid Al-Kooheji 2010
* Filename    : kernel.h
* Description : 
* 
*
* 
* 
* 
*******************************************************************************/
#ifndef EMULATION_PSX_KERNEL_H
#define EMULATION_PSX_KERNEL_H

namespace emulation {
namespace psx {

class Kernel : public Component {
 public:
  Kernel();
  ~Kernel();
  void Initialize();
  void Call();
  
 private:
   void putc(char c,int fd);

#ifndef NDEBUG
   //DebugAssist debug;
   DebugAssist psxout;
#endif
};

}
}

#endif