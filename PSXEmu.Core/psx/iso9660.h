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

#include <string>
#include <vector>

namespace emulation {
namespace psx {

/*
  The ISO9660 filesystem on a mounted disc.

  Only what is needed to start a game: read the volume descriptor, walk
  directories, and pull a file out by path. No Rock Ridge, no Joliet - a
  PlayStation disc uses neither, and the boot executable is always named by an
  ordinary 8.3 identifier in SYSTEM.CNF.

  Sector numbers here are *filesystem* sectors, counted from the start of the
  data track. The disc underneath counts from the lead-in, so everything goes
  through ReadLogicalSector, which is the only place that offset lives.
*/
class Iso9660 {
 public:
  static const uint32_t kSectorSize = 2048;

  struct File {
    uint32_t lba;          // filesystem sector the file starts at
    uint32_t size;         // in bytes
    bool directory;
    std::string name;      // as stored, version suffix and all
  };

  Iso9660();
  ~Iso9660();

  // Reads the primary volume descriptor and the root directory. Returns false
  // if the disc has no ISO9660 filesystem on it - an audio CD, for instance.
  bool Open(Disc* disc);
  void Close();
  bool loaded() const { return loaded_; }

  const std::string& volume_id() const { return volume_id_; }
  const std::vector<File>& root() const { return root_; }

  // Finds a file by path. Accepts the forms software actually writes:
  // "SYSTEM.CNF", "\SYSTEM.CNF", "cdrom:\SLUS_007.55;1", with either slash,
  // in any case, and with or without the ";1" version suffix.
  bool Find(const char* path, File* out) const;

  // Reads a whole file. Rejects anything implausibly large rather than
  // trusting a size field off the disc.
  bool Read(const File& file, std::vector<uint8_t>* out) const;

  // Reads one filesystem sector into a 2048-byte buffer.
  bool ReadLogicalSector(uint32_t sector, uint8_t* out) const;

 private:
  Disc* disc_;
  bool loaded_;
  std::string volume_id_;
  uint32_t root_lba_;
  uint32_t root_size_;
  std::vector<File> root_;

  bool ReadDirectory(uint32_t lba, uint32_t size,
                     std::vector<File>* out) const;
  // Matches one directory entry against a wanted name, ignoring case and the
  // version suffix.
  static bool NameMatches(const std::string& entry, const std::string& wanted);
};

}
}
