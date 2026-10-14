#pragma once

// The BIOS's A0h, B0h and C0h functions, by name - for the debugger's call log, in any build.

#include <cstdint>
#include <string>

namespace emulation {
namespace psx {

struct BiosCall {
  int address;             // A0h, B0h or C0h
  int operation;           // the function number, in t1
  const char* prototype;   // in double quotes, as the debug build's CSV log writes it
};

extern const BiosCall kBiosCalls[3][256];

// "int open(const char *name, int mode)", or "A0(99h)" for a number with no name.
std::string BiosCallName(uint32_t vector, uint32_t function);

}  // namespace psx
}  // namespace emulation
