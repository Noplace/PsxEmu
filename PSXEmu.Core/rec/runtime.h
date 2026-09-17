/*****************************************************************************************************************
* Copyright (c) 2012 Khalid Ali Al-Kooheji                                                                       *
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
#pragma once

// What a compiled block is handed, and what it can call back into.
//
// One pointer arrives in the block and everything else hangs off it at a fixed
// offset, so the emitted code is a series of loads from one base register
// rather than four arguments to juggle.
//
// The callbacks are why this file exists. A compiled load has to go through
// the same path the interpreter's does - the region decode, the timing, and
// later the check that a store is not landing on compiled code - and that path
// lives in Cpu. Rather than have the recompiler include and depend on the CPU,
// the host fills these in with thunks; the tests fill them in with a fake
// memory. Nothing in rec/ knows psx/ exists, which is what keeps the
// recompiler switchable rather than woven in.

#include <cstdint>

namespace emulation {
namespace rec {

// `context` is whatever the host wants back - a Cpu*, in the end.
typedef uint32_t (*Load32Fn)(void* context, uint32_t address);
typedef uint32_t (*Load16Fn)(void* context, uint32_t address);
typedef uint32_t (*Load8Fn)(void* context, uint32_t address);
typedef void (*Store32Fn)(void* context, uint32_t address, uint32_t value);
typedef void (*Store16Fn)(void* context, uint32_t address, uint32_t value);
typedef void (*Store8Fn)(void* context, uint32_t address, uint32_t value);

// The layout the emitted code addresses by offset. Field order is load-bearing
// in the sense that the compiler hard-codes the offsets - keep the two in step,
// and BlockCompiler asserts the ones it uses against offsetof.
struct BlockState {
  uint32_t* regs = nullptr;     // the guest's 32 general-purpose registers
  void* context = nullptr;      // handed back to every callback

  Load32Fn load32 = nullptr;
  Load16Fn load16 = nullptr;
  Load8Fn load8 = nullptr;
  Store32Fn store32 = nullptr;
  Store16Fn store16 = nullptr;
  Store8Fn store8 = nullptr;

  // Where execution goes when the block ends. A block that falls out of its
  // last instruction sets this to the address after it; a branch or jump sets
  // it to wherever it goes - computed from the registers *as they were at the
  // branch*, before the delay slot ran, which is what the hardware does.
  uint32_t next_pc = 0;

  // Set by a compiled load or store that the host's callback wants to treat as
  // an exception - an unaligned address, say. Nothing sets it yet; it is here
  // so that adding that later does not change the struct's shape, and with it
  // every hard-coded offset in the compiler.
  uint32_t fault = 0;
};

}  // namespace rec
}  // namespace emulation
