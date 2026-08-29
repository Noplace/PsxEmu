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
#include "psx/psx.h"

#include <cctype>
#include <cstring>

namespace emulation {
namespace psx {

namespace {

// The primary volume descriptor always lives at filesystem sector 16.
const uint32_t kVolumeDescriptorSector = 16;

// A directory record is at least this long before its name starts.
const uint32_t kDirectoryRecordHeader = 33;

// Nothing on a PlayStation disc is anywhere near this big, and trusting a
// size field off a disc image without a bound is how a corrupt image turns
// into an allocation failure.
const uint32_t kMaximumFileSize = 16u * 1024 * 1024;

// ISO9660 stores 32-bit numbers twice, little-endian then big-endian. The
// little-endian copy is the one to read.
uint32_t ReadU32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

char UpperCase(char c) {
  return static_cast<char>(toupper(static_cast<unsigned char>(c)));
}

// Strips the ";1" version suffix and any trailing dot, which identifiers for
// extension-less names carry.
std::string CanonicalName(const std::string& name) {
  std::string result;
  for (size_t i = 0; i < name.size(); ++i) {
    if (name[i] == ';')
      break;
    result += UpperCase(name[i]);
  }
  while (!result.empty() && result[result.size() - 1] == '.')
    result.erase(result.size() - 1);
  return result;
}

}  // namespace

Iso9660::Iso9660()
    : disc_(nullptr), loaded_(false), root_lba_(0), root_size_(0) {
}

Iso9660::~Iso9660() {
}

void Iso9660::Close() {
  disc_ = nullptr;
  loaded_ = false;
  volume_id_.clear();
  root_lba_ = 0;
  root_size_ = 0;
  root_.clear();
}

bool Iso9660::ReadLogicalSector(uint32_t sector, uint8_t* out) const {
  if (disc_ == nullptr)
    return false;

  uint8_t raw[Disc::kRawSectorSize];
  // Filesystem sector 0 is the first sector of the data track, which the disc
  // places after the lead-in.
  if (!disc_->ReadSector(Disc::kLeadInSectors + sector, raw))
    return false;

  // A Mode 2 Form 1 sector carries its 2048 user bytes after the sync,
  // header and subheader.
  memcpy(out, raw + 24, kSectorSize);
  return true;
}

bool Iso9660::Open(Disc* disc) {
  Close();
  if (disc == nullptr || !disc->loaded())
    return false;
  disc_ = disc;

  uint8_t sector[kSectorSize];
  if (!ReadLogicalSector(kVolumeDescriptorSector, sector)) {
    disc_ = nullptr;
    return false;
  }

  // Type 1 is the primary volume descriptor, and every descriptor carries the
  // "CD001" signature. Checking both is what tells an ISO9660 disc from an
  // audio one rather than reading garbage as a filesystem.
  if (sector[0] != 0x01 || memcmp(sector + 1, "CD001", 5) != 0) {
    disc_ = nullptr;
    return false;
  }

  volume_id_.assign(reinterpret_cast<const char*>(sector + 40), 32);
  while (!volume_id_.empty() && volume_id_[volume_id_.size() - 1] == ' ')
    volume_id_.erase(volume_id_.size() - 1);

  // The root directory record sits inline in the descriptor at offset 156.
  const uint8_t* root_record = sector + 156;
  root_lba_ = ReadU32(root_record + 2);
  root_size_ = ReadU32(root_record + 10);

  if (!ReadDirectory(root_lba_, root_size_, &root_)) {
    Close();
    return false;
  }

  loaded_ = true;
  return true;
}

bool Iso9660::ReadDirectory(uint32_t lba, uint32_t size,
                            std::vector<File>* out) const {
  out->clear();
  if (size == 0 || size > kMaximumFileSize)
    return false;

  const uint32_t sectors = (size + kSectorSize - 1) / kSectorSize;
  uint8_t sector[kSectorSize];

  for (uint32_t s = 0; s < sectors; ++s) {
    if (!ReadLogicalSector(lba + s, sector))
      return false;

    uint32_t offset = 0;
    while (offset < kSectorSize) {
      const uint32_t length = sector[offset];
      // A record length of zero means the rest of this sector is padding;
      // records never straddle a sector boundary.
      if (length == 0)
        break;
      if (length < kDirectoryRecordHeader || offset + length > kSectorSize)
        break;

      const uint8_t name_length = sector[offset + 32];
      if (kDirectoryRecordHeader + name_length > length)
        break;

      File file;
      file.lba = ReadU32(sector + offset + 2);
      file.size = ReadU32(sector + offset + 10);
      file.directory = (sector[offset + 25] & 0x02) != 0;

      // The two special entries are stored as a single 0x00 or 0x01 byte
      // rather than as text.
      if (name_length == 1 && sector[offset + 33] == 0x00) {
        file.name = ".";
      } else if (name_length == 1 && sector[offset + 33] == 0x01) {
        file.name = "..";
      } else {
        file.name.assign(reinterpret_cast<const char*>(sector + offset + 33),
                         name_length);
      }

      out->push_back(file);
      offset += length;
    }
  }
  return true;
}

bool Iso9660::NameMatches(const std::string& entry, const std::string& wanted) {
  return CanonicalName(entry) == CanonicalName(wanted);
}

bool Iso9660::Find(const char* path, File* out) const {
  if (!loaded_ || path == nullptr || out == nullptr)
    return false;

  std::string remaining = path;

  // Software writes the boot path with a device prefix; strip it.
  const size_t colon = remaining.find(':');
  if (colon != std::string::npos)
    remaining.erase(0, colon + 1);
  // And with any number of leading slashes, of either kind.
  while (!remaining.empty() &&
         (remaining[0] == '\\' || remaining[0] == '/'))
    remaining.erase(0, 1);
  if (remaining.empty())
    return false;

  std::vector<File> directory = root_;

  for (;;) {
    size_t separator = remaining.find_first_of("\\/");
    const std::string component =
        (separator == std::string::npos) ? remaining
                                         : remaining.substr(0, separator);
    if (component.empty())
      return false;

    const File* found = nullptr;
    for (size_t i = 0; i < directory.size(); ++i) {
      if (NameMatches(directory[i].name, component)) {
        found = &directory[i];
        break;
      }
    }
    if (found == nullptr)
      return false;

    if (separator == std::string::npos) {
      *out = *found;
      return true;
    }

    // More path to walk, so this component has to be a directory.
    if (!found->directory)
      return false;

    std::vector<File> child;
    if (!ReadDirectory(found->lba, found->size, &child))
      return false;
    directory.swap(child);

    remaining.erase(0, separator + 1);
    while (!remaining.empty() &&
           (remaining[0] == '\\' || remaining[0] == '/'))
      remaining.erase(0, 1);
    if (remaining.empty())
      return false;
  }
}

bool Iso9660::Read(const File& file, std::vector<uint8_t>* out) const {
  if (!loaded_ || out == nullptr)
    return false;
  if (file.size == 0 || file.size > kMaximumFileSize)
    return false;

  const uint32_t sectors = (file.size + kSectorSize - 1) / kSectorSize;
  out->resize(static_cast<size_t>(sectors) * kSectorSize);

  for (uint32_t s = 0; s < sectors; ++s) {
    if (!ReadLogicalSector(file.lba + s, &(*out)[s * kSectorSize])) {
      out->clear();
      return false;
    }
  }
  out->resize(file.size);
  return true;
}

}
}
