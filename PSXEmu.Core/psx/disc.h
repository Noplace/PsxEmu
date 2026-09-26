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
    .mds .mdf     Alcohol 120%'s descriptor and the sectors it describes
    .ccd .img     CloneCD's table of contents and the sectors it describes
    .chd          MAME's compressed format, read through libchdr (lib/libchdr):
                  the track list from its metadata, a hunk decompressed at a
                  time
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
    uint32_t start_lba;    // absolute, including the lead-in - where index 1 is
    uint32_t length;       // in sectors, from index 1 to the next track's
    // Index 0: how many sectors just before start_lba belong to this track,
    // its pregap, which the subchannel counts down towards index 1 - the two
    // seconds before the first music track on a disc with data first, for one.
    // Zero where the descriptor names none. The same sectors are the tail of
    // the previous track's `length`: where they are is the image's business,
    // which track they belong to is this.
    uint32_t pregap = 0;
  };

  Disc();
  ~Disc();

  // Opens an image file, a cue sheet or a drive letter. Replaces whatever was
  // mounted before. Returns false and leaves nothing mounted on failure.
  bool Open(const char* path);
  void Close();

  bool loaded() const { return !sources_.empty(); }
  // Why the last Open failed, when there is more to say than that it did -
  // empty otherwise. Only a CHD sets it so far: "compressed with zstd" is
  // worth a person knowing, where "not a disc image" is not.
  const std::string& open_error() const { return open_error_; }
  const std::string& path() const { return path_; }

  // Only the path - everything else here (sources_, tracks_, the rest) is
  // derived by Open() and is re-derived by it on load, not saved. Reopening
  // itself, and reporting a moved image as a failure, is the caller's job
  // (Cdrom::Serialise) - this only moves the one field that is real state.
  void Serialise(StateIO& io) { io.Str(path_); }

  // Reads one sector as 2352 raw bytes. `lba` is absolute, so the first sector
  // of track 1 is kLeadInSectors.
  bool ReadSector(uint32_t lba, uint8_t* out) const;

  // How many sectors a read-ahead block covers. Thirty-two of them is about
  // 75 KB, which is one network round trip instead of thirty-two.
  static const uint32_t kReadAheadSectors = 32;

  int track_count() const { return static_cast<int>(tracks_.size()); }
  const Track& track(int index) const { return tracks_[index]; }

  // The Q subchannel a real drive would read at `lba`: twelve bytes - control
  // and ADR, track, index, the time within the track, a zero, the time on the
  // disc, and a CRC - exactly as the image's subchannel file has them. Only a
  // CloneCD image with its .sub beside it has one; false for anything else,
  // and for a sector the file does not cover. `lba` is absolute, as for
  // ReadSector.
  bool ReadSubchannelQ(uint32_t lba, uint8_t* q) const;
  bool has_subchannel() const { return sub_file_ != nullptr; }

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

    // Read-ahead. A disc is read almost entirely forwards, a sector at a time,
    // and every one of those used to be a seek and a 2,352-byte read - which on
    // an image sitting on a network share is a round trip each. Reading a block
    // of sectors and serving the rest from it turns thirty-two of those into
    // one. Purely a cache: the bytes handed back are the same bytes.
    //
    // Mutable because reading a sector does not change the disc, and
    // Disc::ReadSector is const for that reason.
    mutable std::vector<uint8_t> ahead;
    mutable long long ahead_offset = -1;   // where the block starts in the file
    mutable uint32_t ahead_size = 0;       // how much of it came back

    // A CHD, when this is one (chd_file*): sectors come out of hunks of
    // several frames each, decompressed whole, and the last one is kept -
    // the CHD's own read-ahead, a hunk being eight sectors in a typical one.
    void* chd = nullptr;
    uint32_t chd_hunk_bytes = 0;
    mutable std::vector<uint8_t> chd_hunk;
    mutable uint32_t chd_hunk_number = 0xFFFFFFFFu;
  };

  // One sector out of an image file, through the read-ahead block above.
  bool ReadFileSector(const Source& source, long long offset, uint32_t wanted,
                      uint8_t* out) const;

  struct TrackSource {
    int source;             // index into sources_
    uint32_t file_lba;      // first sector of this track within that file
  };

  std::vector<Source> sources_;
  std::vector<Track> tracks_;
  std::vector<TrackSource> track_sources_;
  uint32_t total_sectors_;
  std::string path_;
  std::string open_error_;

  // A CloneCD .sub: 96 bytes a sector, the eight subchannels one after
  // another (P, then Q, then R to W), 12 bytes each. Read a block at a time
  // for the same reason the image is - it is usually on a network share.
  FILE* sub_file_ = nullptr;
  uint32_t sub_sectors_ = 0;
  mutable std::vector<uint8_t> sub_block_;
  mutable long long sub_block_first_ = -1;   // the first sector the block holds

  // Where a CHD keeps each stretch of the disc: `count` sectors from `lba`
  // are frames from `frame` on. A track is one run, and a pregap the CHD
  // stores is another; a pregap it leaves out has none and reads as silence.
  // A CHD's audio is stored big-endian, and a cooked track keeps only its
  // 2048 or 2336 bytes at the front of each frame.
  struct ChdRun {
    uint32_t lba;
    uint32_t count;
    uint32_t frame;
    uint32_t data_size;     // 2352, 2336 or 2048
    bool audio;
  };
  std::vector<ChdRun> chd_runs_;
  bool OpenChd(const char* path);
  bool ReadChdSector(uint32_t lba, uint8_t* out) const;

  bool OpenCue(const char* path);
  bool OpenMds(const char* path);
  // `scrambled` comes back true for an image this cannot mount at all - the
  // raw channel rather than sectors - as against an ordinary parse failure,
  // where the image beside the descriptor is still worth opening on its own.
  bool OpenCcd(const char* path, bool* scrambled = nullptr);
  bool OpenImage(const char* path);
  // Where the data track ends in a raw image, found from the sectors
  // themselves when no cue sheet says.
  uint32_t FindDataTrackLength(const Source& source) const;
  bool IsDataSector(const Source& source, uint32_t file_sector) const;
  // A descriptor of the same name beside an image, if there is one - the only
  // place a track layout exists for a bare dump. `extension` is given without
  // its dot.
  static std::string FindSibling(const std::string& image_path,
                                 const char* extension);
  bool OpenDevice(const char* path);
  // `sector_size` of zero means work the layout out from the file length; a
  // descriptor that states one passes it here instead, which is the only way
  // to read a stride the length alone cannot reveal.
  bool AddFileSource(const std::string& path, Source* out,
                     uint32_t sector_size = 0) const;

  // Whether an address falls between two tracks rather than inside one.
  bool IsUnstoredGap(uint32_t lba) const;

  // Turns a list of track start points into lengths. Neither a cue sheet nor
  // a media descriptor states how long a track is; both say where each one
  // begins and leave the rest to arithmetic.
  void FinishTrackLayout();

  // Fills in sync, header and mode for a sector that was not stored raw.
  static void SynthesiseSectorHeader(uint8_t* sector, uint32_t lba,
                                     uint32_t sector_size);
};

}
}
