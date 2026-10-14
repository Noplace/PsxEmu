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

// What is on a memory card: the directory in block 0, the saves it describes, and the operations
// an editor needs - list, delete, undelete, export, import, format.
//
// Pure functions over the 128 KB image, with no file, no UI and no machine, so mc_test can drive
// every one of them headlessly and a front end on another platform gets them for free. The
// layout is psx-spx's "Memory Card Data Format":
//
//   block 0   frame 0       header, "MC"
//             frames 1-15   one directory frame per data block 1-15
//             frames 16-35  broken sector list; 36-55 their replacements; 63 write test
//   block 1-15              one per data block: a title frame, icon frames, then the save
//
// Every frame ends in a checksum - the XOR of its first 127 bytes - and the BIOS rejects a
// directory frame whose checksum is wrong, so everything here that writes one recomputes it.
//
// Directory states, from the frame's first byte:
//   51h  first (or only) block of a save    A1h  the same, deleted
//   52h  middle block                       A2h  the same, deleted
//   53h  last block                         A3h  the same, deleted
//   A0h  free
// A deleted save keeps its links, which is what makes undelete possible; its blocks count as
// free, and are reused like any other.

#include <cstdint>
#include <string>
#include <vector>

namespace emulation {
namespace psx {
namespace mcdir {

const size_t kFrameSize = 128;
const size_t kBlockSize = 8192;           // 64 frames
const int kBlocks = 16;                   // block 0 and the 15 a save can use
const size_t kCardSize = kBlockSize * kBlocks;
const int kIconSize = 16;

struct Save {
  int first_block = 0;            // 1-15
  int blocks = 0;                 // length of the chain
  uint32_t size = 0;              // the file size the directory records
  bool deleted = false;
  std::string filename;           // e.g. BASLUS-01013LOM - region, product code, the game's own id
  std::wstring title;             // decoded from Shift-JIS, full-width ASCII narrowed
  int icon_frames = 0;            // 0-3
  // Each icon frame as 16x16 0xAARRGGBB, alpha 0 where the palette colour is 0 (transparent).
  std::vector<uint32_t> icons;    // icon_frames * 256 pixels
};

// XOR of a frame's first 127 bytes - what belongs in its last.
uint8_t FrameChecksum(const uint8_t* frame);

// Writes a freshly formatted, empty card: what the BIOS's own format produces.
void Format(uint8_t* card);

// Whether block 0 starts with the "MC" a formatted card carries.
bool IsFormatted(const uint8_t* card);

// Saves on the card, in directory order. With `include_deleted`, deleted saves whose chains are
// still whole are listed too, marked `deleted`.
std::vector<Save> List(const uint8_t* card, bool include_deleted);

// Blocks a new save could use: free ones and deleted ones.
int FreeBlocks(const uint8_t* card);

// Marks a save's blocks deleted, links intact. False if `first_block` does not start one.
bool Delete(uint8_t* card, int first_block, std::string* error);

// Restores a deleted save - only if every block of its chain is still marked deleted, since a
// block that has been reused belongs to something else now.
bool Undelete(uint8_t* card, int first_block, std::string* error);

// A save as a .mcs: its directory frame followed by its blocks.
bool Export(const uint8_t* card, int first_block, std::vector<uint8_t>* mcs, std::string* error);

// Writes a .mcs into free blocks. Refuses a name that is already in use by a live save; a deleted
// save of the same name is cleared first, as the BIOS's own copy does.
bool Import(uint8_t* card, const std::vector<uint8_t>& mcs, std::string* error);

// ---- Other tools' files ----------------------------------------------------------------------
//
// Card managers and other emulators wrap cards and saves in formats of their own. These unwrap
// them into what the functions above take - a 128 KB image, or a .mcs - recognising each by
// what is in the file rather than by its name. The formats and their headers are the ones
// DuckStation reads.

// A whole card: the plain 128 KB image, which a dozen extensions hold as it is (.mcr, .mcd,
// .mc, .srm, .psm, .ps, .ddf, .bin); a DexDrive .gme, 3,904 bytes of header in front of it
// (and padded out if the file stops short, as some do); a Connectix VGS .mem or .vgs, 64 bytes
// starting "VgsM"; or a .psx card, 256 bytes starting "PSV". `format` names which, for a
// message. False, with the reason, for anything else, or a card that is not formatted.
bool CardFromFile(const std::vector<uint8_t>& file, std::vector<uint8_t>* card,
                  std::string* format, std::string* error);

// A single save as a .mcs: a .mcs as it is, or a raw save - its 1-15 blocks with no directory
// frame in front, which is how several tools export one. A raw save has no name of its own,
// so `file_title` - the file's name without its extension, which is what such a save is named
// after (BASLUS-01013LOM...) - stands in, cut to the 20 characters a directory frame holds.
bool SaveFromFile(const std::vector<uint8_t>& file, const std::string& file_title,
                  std::vector<uint8_t>* mcs, std::string* error);

// Every live save on `source`, imported onto `card` one at a time, as far as they fit.
// `report` says how many went, and which did not and why. False if none did, and then `card`
// is left exactly as it was.
bool ImportCard(uint8_t* card, const uint8_t* source, std::string* report);

}  // namespace mcdir
}  // namespace psx
}  // namespace emulation
