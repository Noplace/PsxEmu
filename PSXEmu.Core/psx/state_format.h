/*****************************************************************************************************************
* Copyright (c) 2026 Khalid Ali Al-Kooheji                                                                       *
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

// What a PSXEmu save-state file holds around the payload StateIO reads and writes
// (Emu.Core: emucore/state/state_io.h; Docs/Save-States-Plan.md).

#include "emucore/state/state_io.h"

#include <cstdint>

namespace emulation {
namespace psx {

// Every component's Serialise takes the shared serialiser, by its own short name here.
using state::Fnv1a64;
using state::StateIO;

// A save-state file: magic, then a version (bumped whenever any component's
// Serialise changes - old files are refused outright, never migrated), then
// an FNV-1a hash of the BIOS image the state was made against, then the
// mounted disc's path (empty for none - a courtesy copy for a future save
// browser; System::LoadState restores the real one from the payload, not
// from here), then the payload itself: every component's Serialise, called
// in the same fixed order on both sides. See System::SaveState/LoadState.
constexpr char kStateMagic[8] = {'P', 'S', 'X', 'S', 'T', 'A', 'T', 'E'};
constexpr uint32_t kStateVersion = 8;   // 4: Sio gained a per-slot Mouse array.
                                         // 5: Sio's Pad became polymorphic (Multitap).
                                         // 6: Multitap keeps method 1's replies.
                                         // 7: Dma feeds channel 0 a block at a time.
                                         // 8: SIO1, the serial port, has registers to save.

}  // namespace psx
}  // namespace emulation
