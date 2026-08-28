// Replaces <WinCore/types.h>, the external library the original tree depended
// on and which is not part of this repository. Only the fixed-width integer
// names the emulation code actually uses are needed.
#pragma once

#include <cstdint>
#include <cstddef>

typedef int8_t   int8;
typedef int16_t  int16;
typedef int32_t  int32;
typedef int64_t  int64;
typedef uint8_t  uint8;
typedef uint16_t uint16;
typedef uint32_t uint32;
typedef uint64_t uint64;
