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
#pragma once

#include <cstdint>

namespace emulation {
namespace psx {

// Every BREAKPOINT in the core marks a path that is not implemented. In a
// debug build it traps; in a release build it used to expand to nothing, which
// meant an unimplemented path was completely silent. It now counts instead, so
// a headless harness can report "this run hit 4102 unimplemented paths" rather
// than quietly producing a wrong frame.
struct TrapCounter {
  static uint64_t count;
  static uint64_t rfe_count;

  // Which unimplemented paths were hit, not just how many times. A count on
  // its own says something is missing; the site says what.
  struct Site { const char* file; int line; uint64_t hits; uint32_t detail; };
  static const int kSiteCapacity = 24;
  static Site sites[kSiteCapacity];
  static uint32_t site_count;

  // `detail` is whatever identifies the case that was not handled - the
  // instruction word, usually. The first one seen is kept, because the first
  // is the one that has not been explained yet.
  static void Hit(const char* file, int line, uint32_t detail = 0) {
    ++count;
    for (uint32_t i = 0; i < site_count; ++i) {
      if (sites[i].line == line && sites[i].file == file) {
        ++sites[i].hits;
        return;
      }
    }
    if (site_count < kSiteCapacity) {
      sites[site_count].file = file;
      sites[site_count].line = line;
      sites[site_count].hits = 1;
      sites[site_count].detail = detail;
      ++site_count;
    }
  }
};

// A ring of the most recent Cop0 status-register events. With only a handful
// of exceptions in a whole run, printing the lot is more use than any counter:
// it shows the order things happened in, which is what a stuck status stack
// hides.
struct ExceptionLog {
  enum Kind { kException, kReturn, kStatusWrite };
  struct Entry {
    Kind kind;
    uint32_t pc;
    uint32_t epc;
    uint32_t cause;
    uint32_t status_before;
    uint32_t status_after;
  };
  static const int kCapacity = 64;
  static Entry entries[kCapacity];
  static uint32_t written;   // total, which may exceed kCapacity

  static void Record(Kind kind, uint32_t pc, uint32_t epc, uint32_t cause,
                     uint32_t before, uint32_t after) {
    Entry& entry = entries[written % kCapacity];
    entry.kind = kind;
    entry.pc = pc;
    entry.epc = epc;
    entry.cause = cause;
    entry.status_before = before;
    entry.status_after = after;
    ++written;
  }
};

}
}

#ifdef _DEBUG
#define BREAKPOINT { ::emulation::psx::TrapCounter::Hit(__FILE__, __LINE__); DebugBreak(); }
#define BREAKPOINT_DETAIL(x) { ::emulation::psx::TrapCounter::Hit(__FILE__, __LINE__, (x)); DebugBreak(); }
#define PC_BREAKPOINT(x) if (context_->pc==x) { DebugBreak(); }
#include <Windows.h>
#include <assert.h>
#include <stdio.h>
#include <sys/types.h>
#include <time.h>
#include "psx/debug_assist.h"
#else
#define BREAKPOINT { ::emulation::psx::TrapCounter::Hit(__FILE__, __LINE__); }
#define BREAKPOINT_DETAIL(x) { ::emulation::psx::TrapCounter::Hit(__FILE__, __LINE__, (x)); }
#define PC_BREAKPOINT(x)
#endif
