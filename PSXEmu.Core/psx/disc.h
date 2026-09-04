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

#include <cstdio>
#include <string>
#include <vector>

namespace emulation {
namespace psx {

/*
  A mounted disc.

  Hands the CD-ROM controller raw 2352-byte sectors and a track list, and hides
  where they came from. Understands:

    .cue          a sheet naming one or more binary files and their tracks
    .bin .img     raw sectors, sector size detected from the file length
    .iso          usually 2048-byte cooked sectors, also detected
    D: \\.\D:     a physical drive (data tracks only)

  Sectors that are not stored raw are completed on the way out - the sync
  pattern, the MSF header and the mode byte are synthesised so that the
  controller only ever deals with one shape of sector.
*/
class Disc {
 public:
  static const int kRawSectorSize = 2352;
  // The first track starts 2 seconds (150 sectors) into the disc; that pregap
  // is not present in an image file, so every image offset is shifted by it.
  static const uint32_t kLeadInSectors = 150;

  enum TrackType { kTrackData, kTrackAudio };

  struct Track {
    int number;
    TrackType type;
    uint32_t start_lba;    // absolute, including the lead-in
    uint32_t length;       // in sectors
  };

  Disc();
  ~Disc();

  // Opens an image file, a cue sheet or a drive letter. Replaces whatever was
  // mounted before. Returns false and leaves nothing mounted on failure.
  bool Open(const char* path);
  void Close();

  bool loaded() const { return !sources_.empty(); }
  const std::string& path() const { return path_; }

  // Reads one sector as 2352 raw bytes. `lba` is absolute, so the first sector
  // of track 1 is kLeadInSectors.
  bool ReadSector(uint32_t lba, uint8_t* out) const;

  int track_count() const { return static_cast<int>(tracks_.size()); }
  const Track& track(int index) const { return tracks_[index]; }

  // Total length including the lead-in, which is what the controller reports
  // as the end of the disc.
  uint32_t total_sectors() const { return total_sectors_; }

  // Splits an absolute sector number into the minute/second/frame form the
  // CD-ROM registers use, in BCD.
  static void LbaToMsf(uint32_t lba, uint8_t* minute, uint8_t* second,
                       uint8_t* frame);
  static uint32_t MsfToLba(uint8_t minute, uint8_t second, uint8_t frame);
  static uint8_t ToBcd(uint8_t value);
  static uint8_t FromBcd(uint8_t value);

 private:
  // One backing file (or device) and how sectors are laid out inside it.
  struct Source {
    FILE* file;
    void* device;           // HANDLE when this is a physical drive
    uint32_t sector_size;   // 2352, 2336 or 2048 in the file
    uint32_t data_offset;   // where the 2048 user bytes start in a sector
    uint32_t sector_count;
    std::string name;
  };

  struct TrackSource {
    int source;             // index into sources_
    uint32_t file_lba;      // first sector of this track within that file
  };

  std::vector<Source> sources_;
  std::vector<Track> tracks_;
  std::vector<TrackSource> track_sources_;
  uint32_t total_sectors_;
  std::string path_;

  bool OpenCue(const char* path);
  bool OpenImage(const char* path);
  // Where the data track ends in a raw image, found from the sectors
  // themselves when no cue sheet says.
  uint32_t FindDataTrackLength(const Source& source) const;
  bool IsDataSector(const Source& source, uint32_t file_sector) const;
  // A cue sheet of the same name beside an image, if there is one - the only
  // place a track layout exists for a bare dump.
  static std::string FindSiblingCue(const std::string& image_path);
  bool OpenDevice(const char* path);
  bool AddFileSource(const std::string& path, Source* out) const;

  // Fills in sync, header and mode for a sector that was not stored raw.
  static void SynthesiseSectorHeader(uint8_t* sector, uint32_t lba,
                                     uint32_t sector_size);
};

}
}
