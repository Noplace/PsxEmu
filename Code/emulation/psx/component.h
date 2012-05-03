/******************************************************************************
* Copyright Khalid Al-Kooheji 2010
* Filename    : component.h
* Description : 
* 
*
* 
* 
* 
*******************************************************************************/
#ifndef EMULATION_PSX_COMPONENT_H
#define EMULATION_PSX_COMPONENT_H

namespace emulation {
namespace psx {

class Component {
 public:
  System& system() { return *system_; }
  void set_system(System* system) { 
    system_ = system;
  }
 protected:
  System* system_;
  Cpu* cpu_;
  IOInterface* io_;
};

}
}

#endif