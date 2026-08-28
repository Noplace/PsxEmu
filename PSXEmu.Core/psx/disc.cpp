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

#include <winioctl.h>   // DISK_GEOMETRY, for a physical drive; WIN32_LEAN_AND_MEAN excludes it

#include <cctype>
#include <cstring>

namespace emulation {
namespace psx {

namespace {

const uint8_t kSyncPattern[12] = {
  0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00
};

std::string ToLower(const std::string& text) {
  std::string result = text;
  for (size_t i = 0; i < result.size(); ++i)
    result[i] = static_cast<char>(tolower(static_cast<unsigned char>(result[i])));
  return result;
}

std::string Extension(const std::string& path) {
  const size_t dot = path.find_last_of('.');
  const size_t slash = path.find_last_of("/\\");
  if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
    return std::string();
  return ToLower(path.substr(dot));
}

std::string DirectoryOf(const std::string& path) {
  const size_t slash = path.find_last_of("/\\");
  if (slash == std::string::npos)
    return std::string();
  return path.substr(0, slash + 1);
}

std::string Trim(const std::string& text) {
  size_t begin = 0;
  size_t end = text.size();
  while (begin < end && isspace(static_cast<unsigned char>(text[begin])))
    ++begin;
  while (end > begin && isspace(static_cast<unsigned char>(text[end - 1])))
    --end;
  return text.substr(begin, end - begin);
}

// A cue sheet quotes any path containing a space, so the filename has to be
// pulled out of quotes when there are any and taken up to the type keyword
// when there are not.
std::string ParseCueFileName(const std::string& arguments) {
  const size_t first_quote = arguments.find('"');
  if (first_quote != std::string::npos) {
    const size_t second_quote = arguments.find('"', first_quote + 1);
    if (second_quote != std::string::npos)
      return arguments.substr(first_quote + 1, second_quote - first_quote - 1);
  }
  const size_t space = arguments.find_last_of(' ');
  if (space != std::string::npos)
    return Trim(arguments.substr(0, space));
  return Trim(arguments);
}

}  // namespace

Disc::Disc() : total_sectors_(0) {
}

Disc::~Disc() {
  Close();
}

uint8_t Disc::ToBcd(uint8_t value) {
  return static_cast<uint8_t>(((value / 10) << 4) | (value % 10));
}

uint8_t Disc::FromBcd(uint8_t value) {
  return static_cast<uint8_t>(((value >> 4) * 10) + (value & 0x0F));
}

void Disc::LbaToMsf(uint32_t lba, uint8_t* minute, uint8_t* second,
                    uint8_t* frame) {
  *minute = ToBcd(static_cast<uint8_t>(lba / (60 * 75)));
  *second = ToBcd(static_cast<uint8_t>((lba / 75) % 60));
  *frame  = ToBcd(static_cast<uint8_t>(lba % 75));
}

uint32_t Disc::MsfToLba(uint8_t minute, uint8_t second, uint8_t frame) {
  return (FromBcd(minute) * 60u + FromBcd(second)) * 75u + FromBcd(frame);
}

void Disc::Close() {
  for (size_t i = 0; i < sources_.size(); ++i) {
    if (sources_[i].file != nullptr)
      fclose(sources_[i].file);
    if (sources_[i].device != nullptr)
      CloseHandle(static_cast<HANDLE>(sources_[i].device));
  }
  sources_.clear();
  tracks_.clear();
  track_sources_.clear();
  total_sectors_ = 0;
  path_.clear();
}

bool Disc::Open(const char* path) {
  Close();
  if (path == nullptr || path[0] == '\0')
    return false;

  const std::string text = path;
  const std::string extension = Extension(text);

  bool ok = false;
  if (extension == ".cue")
    ok = OpenCue(path);
  else if (text.size() <= 3 && text.size() >= 2 && text[1] == ':')
    ok = OpenDevice(path);            // "D:" or "D:\"
  else if (text.compare(0, 4, "\\\\.\\") == 0)
    ok = OpenDevice(path);            // "\\.\D:"
  else
    ok = OpenImage(path);

  if (!ok) {
    Close();
    return false;
  }
  path_ = text;
  return true;
}

// Works out how sectors are laid out from the file length. A raw dump divides
// by 2352; a Mode 2 dump without sync or header by 2336; a cooked ISO by 2048.
bool Disc::AddFileSource(const std::string& path, Source* out) const {
  FILE* fp = fopen(path.c_str(), "rb");
  if (fp == nullptr)
    return false;

  fseek(fp, 0, SEEK_END);
  const long long size = _ftelli64(fp);
  fseek(fp, 0, SEEK_SET);
  if (size <= 0) {
    fclose(fp);
    return false;
  }

  out->file = fp;
  out->device = nullptr;
  out->name = path;

  if ((size % kRawSectorSize) == 0) {
    out->sector_size = kRawSectorSize;
    out->data_offset = 24;            // sync 12 + header 4 + subheader 8
  } else if ((size % 2336) == 0) {
    out->sector_size = 2336;
    out->data_offset = 8;             // subheader only
  } else if ((size % 2048) == 0) {
    out->sector_size = 2048;
    out->data_offset = 0;
  } else {
    // Not a recognisable sector multiple. Raw is the best guess for a PSX
    // image, and a partial trailing sector is simply not readable.
    out->sector_size = kRawSectorSize;
    out->data_offset = 24;
  }

  out->sector_count = static_cast<uint32_t>(size / out->sector_size);
  return true;
}

bool Disc::OpenImage(const char* path) {
  Source source;
  if (!AddFileSource(path, &source))
    return false;
  sources_.push_back(source);

  // A bare image is one data track covering the whole file.
  Track track;
  track.number = 1;
  track.type = kTrackData;
  track.start_lba = kLeadInSectors;
  track.length = source.sector_count;
  tracks_.push_back(track);

  TrackSource track_source;
  track_source.source = 0;
  track_source.file_lba = 0;
  track_sources_.push_back(track_source);

  total_sectors_ = kLeadInSectors + source.sector_count;
  return true;
}

bool Disc::OpenDevice(const char* path) {
  // Normalise "D:" or "D:\" into the device form CreateFile wants.
  std::string device = path;
  if (device.compare(0, 4, "\\\\.\\") != 0) {
    if (device.size() >= 2 && device[1] == ':')
      device = std::string("\\\\.\\") + device.substr(0, 2);
    else
      return false;
  }

  HANDLE handle = CreateFileA(device.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (handle == INVALID_HANDLE_VALUE)
    return false;

  DISK_GEOMETRY geometry;
  DWORD returned = 0;
  if (!DeviceIoControl(handle, IOCTL_DISK_GET_DRIVE_GEOMETRY, NULL, 0,
                       &geometry, sizeof(geometry), &returned, NULL)) {
    CloseHandle(handle);
    return false;
  }

  Source source;
  source.file = nullptr;
  source.device = handle;
  source.name = device;
  // Reading a mounted drive through the filesystem layer gives cooked 2048
  // byte sectors, so only data tracks are readable this way. Raw reads would
  // need IOCTL_CDROM_RAW_READ and a track map from the TOC; see Docs/Gaps.md.
  source.sector_size = geometry.BytesPerSector ? geometry.BytesPerSector : 2048;
  source.data_offset = 0;
  const long long total_bytes = geometry.Cylinders.QuadPart *
                                geometry.TracksPerCylinder *
                                geometry.SectorsPerTrack *
                                geometry.BytesPerSector;
  source.sector_count = static_cast<uint32_t>(total_bytes / source.sector_size);
  sources_.push_back(source);

  Track track;
  track.number = 1;
  track.type = kTrackData;
  track.start_lba = kLeadInSectors;
  track.length = source.sector_count;
  tracks_.push_back(track);

  TrackSource track_source;
  track_source.source = 0;
  track_source.file_lba = 0;
  track_sources_.push_back(track_source);

  total_sectors_ = kLeadInSectors + source.sector_count;
  return true;
}

bool Disc::OpenCue(const char* path) {
  FILE* fp = fopen(path, "r");
  if (fp == nullptr)
    return false;

  const std::string directory = DirectoryOf(path);
  int current_source = -1;
  int pending_track_number = 0;
  TrackType pending_track_type = kTrackData;
  bool have_pending_track = false;

  char line[1024];
  while (fgets(line, sizeof(line), fp) != nullptr) {
    const std::string trimmed = Trim(line);
    if (trimmed.empty())
      continue;

    const size_t space = trimmed.find(' ');
    const std::string keyword =
        ToLower(space == std::string::npos ? trimmed : trimmed.substr(0, space));
    const std::string arguments =
        space == std::string::npos ? std::string() : Trim(trimmed.substr(space + 1));

    if (keyword == "file") {
      const std::string name = ParseCueFileName(arguments);
      Source source;
      // Try the path as written first, then relative to the sheet, which is
      // how nearly every cue sheet in the wild is actually laid out.
      if (!AddFileSource(name, &source) &&
          !AddFileSource(directory + name, &source)) {
        fclose(fp);
        return false;
      }
      sources_.push_back(source);
      current_source = static_cast<int>(sources_.size()) - 1;
      continue;
    }

    if (keyword == "track") {
      const size_t type_space = arguments.find(' ');
      if (type_space == std::string::npos)
        continue;
      pending_track_number = atoi(arguments.substr(0, type_space).c_str());
      const std::string type = ToLower(Trim(arguments.substr(type_space + 1)));
      pending_track_type = (type.compare(0, 5, "audio") == 0) ? kTrackAudio
                                                             : kTrackData;
      have_pending_track = true;
      continue;
    }

    if (keyword == "index" && have_pending_track && current_source >= 0) {
      const size_t index_space = arguments.find(' ');
      if (index_space == std::string::npos)
        continue;
      const int index_number = atoi(arguments.substr(0, index_space).c_str());
      // INDEX 00 is the pregap; the track proper starts at INDEX 01.
      if (index_number != 1)
        continue;

      const std::string stamp = Trim(arguments.substr(index_space + 1));
      unsigned minute = 0, second = 0, frame = 0;
      if (sscanf(stamp.c_str(), "%u:%u:%u", &minute, &second, &frame) != 3)
        continue;
      const uint32_t file_lba = (minute * 60u + second) * 75u + frame;

      Track track;
      track.number = pending_track_number;
      track.type = pending_track_type;
      track.start_lba = kLeadInSectors + file_lba;
      track.length = 0;                 // filled in once the next track is known
      tracks_.push_back(track);

      TrackSource track_source;
      track_source.source = current_source;
      track_source.file_lba = file_lba;
      track_sources_.push_back(track_source);
      have_pending_track = false;
    }
  }
  fclose(fp);

  if (tracks_.empty())
    return false;

  // A track runs until the next one starts, or to the end of its file.
  for (size_t i = 0; i < tracks_.size(); ++i) {
    const Source& source = sources_[track_sources_[i].source];
    const bool same_file = (i + 1 < tracks_.size()) &&
                           (track_sources_[i + 1].source ==
                            track_sources_[i].source);
    if (same_file)
      tracks_[i].length = track_sources_[i + 1].file_lba -
                          track_sources_[i].file_lba;
    else
      tracks_[i].length = source.sector_count - track_sources_[i].file_lba;
  }

  const Track& last = tracks_.back();
  total_sectors_ = last.start_lba + last.length;
  return true;
}

void Disc::SynthesiseSectorHeader(uint8_t* sector, uint32_t lba,
                                  uint32_t sector_size) {
  if (sector_size >= kRawSectorSize)
    return;                                  // already raw, nothing to do

  if (sector_size == 2048) {
    // Shift the user data up and build sync, header and subheader in front of
    // it. Mode 2 Form 1 is what a PSX data track uses.
    memmove(sector + 24, sector, 2048);
    memcpy(sector, kSyncPattern, sizeof(kSyncPattern));
    uint8_t minute, second, frame;
    LbaToMsf(lba, &minute, &second, &frame);
    sector[12] = minute;
    sector[13] = second;
    sector[14] = frame;
    sector[15] = 0x02;                       // mode 2
    sector[16] = 0x00;                       // file
    sector[17] = 0x00;                       // channel
    sector[18] = 0x08;                       // submode: data
    sector[19] = 0x00;                       // coding
    memcpy(sector + 20, sector + 16, 4);     // the subheader is stored twice
    return;
  }

  if (sector_size == 2336) {
    memmove(sector + 16, sector, 2336);
    memcpy(sector, kSyncPattern, sizeof(kSyncPattern));
    uint8_t minute, second, frame;
    LbaToMsf(lba, &minute, &second, &frame);
    sector[12] = minute;
    sector[13] = second;
    sector[14] = frame;
    sector[15] = 0x02;
  }
}

bool Disc::ReadSector(uint32_t lba, uint8_t* out) const {
  if (sources_.empty() || out == nullptr)
    return false;

  // Find the track this sector falls in.
  int track_index = -1;
  for (size_t i = 0; i < tracks_.size(); ++i) {
    if (lba >= tracks_[i].start_lba &&
        lba < tracks_[i].start_lba + tracks_[i].length) {
      track_index = static_cast<int>(i);
      break;
    }
  }
  if (track_index < 0)
    return false;

  const TrackSource& track_source = track_sources_[track_index];
  const Source& source = sources_[track_source.source];
  const uint32_t offset_in_track = lba - tracks_[track_index].start_lba;
  const uint32_t file_sector = track_source.file_lba + offset_in_track;
  if (file_sector >= source.sector_count)
    return false;

  memset(out, 0, kRawSectorSize);
  const long long byte_offset =
      static_cast<long long>(file_sector) * source.sector_size;

  if (source.file != nullptr) {
    if (_fseeki64(source.file, byte_offset, SEEK_SET) != 0)
      return false;
    if (fread(out, 1, source.sector_size, source.file) != source.sector_size)
      return false;
  } else if (source.device != nullptr) {
    HANDLE handle = static_cast<HANDLE>(source.device);
    LARGE_INTEGER position;
    position.QuadPart = byte_offset;
    if (!SetFilePointerEx(handle, position, NULL, FILE_BEGIN))
      return false;
    DWORD read = 0;
    if (!ReadFile(handle, out, source.sector_size, &read, NULL) ||
        read != source.sector_size)
      return false;
  } else {
    return false;
  }

  SynthesiseSectorHeader(out, lba, source.sector_size);
  return true;
}

}
}
