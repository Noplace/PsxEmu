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

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>

#include <libchdr/cdrom.h>
#include <libchdr/chd.h>

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


// Fixed places in a media descriptor. The header names where the session
// blocks are rather than fixing them, and files differ, so the offset is read
// rather than assumed.
const size_t kMdsSessionCountOffset = 0x14;
const size_t kMdsSessionOffset = 0x50;
const size_t kMdsSessionBlockSize = 24;
const size_t kMdsTrackBlockSize = 0x50;

bool Fits(const std::vector<uint8_t>& data, size_t offset, size_t length) {
  return offset <= data.size() && length <= data.size() - offset;
}

// The descriptor is little-endian throughout, and is read a field at a time
// rather than cast onto a struct: nothing guarantees the file is aligned or
// packed the way this compiler would lay one out.
uint16_t ReadU16(const std::vector<uint8_t>& data, size_t offset) {
  return static_cast<uint16_t>(data[offset] |
                               (static_cast<uint16_t>(data[offset + 1]) << 8));
}

uint32_t ReadU32(const std::vector<uint8_t>& data, size_t offset) {
  return static_cast<uint32_t>(data[offset]) |
         (static_cast<uint32_t>(data[offset + 1]) << 8) |
         (static_cast<uint32_t>(data[offset + 2]) << 16) |
         (static_cast<uint32_t>(data[offset + 3]) << 24);
}

uint64_t ReadU64(const std::vector<uint8_t>& data, size_t offset) {
  return static_cast<uint64_t>(ReadU32(data, offset)) |
         (static_cast<uint64_t>(ReadU32(data, offset + 4)) << 32);
}

// A descriptor is a few hundred bytes for an ordinary disc, and a few hundred
// kilobytes when the dump kept its density map. The cap is only there so that
// a file that is not one of these at all cannot ask for an arbitrary
// allocation before the signature has been looked at.
bool ReadWholeFile(const char* path, std::vector<uint8_t>* out) {
  const long long kMaxSize = 64 * 1024 * 1024;
  FILE* fp = fopen(path, "rb");
  if (fp == nullptr)
    return false;
  fseek(fp, 0, SEEK_END);
  const long long size = _ftelli64(fp);
  fseek(fp, 0, SEEK_SET);
  if (size <= 0 || size > kMaxSize) {
    fclose(fp);
    return false;
  }
  out->resize(static_cast<size_t>(size));
  const size_t read = fread(&(*out)[0], 1, out->size(), fp);
  fclose(fp);
  return read == out->size();
}

std::string StemOf(const std::string& path) {
  const size_t dot = path.find_last_of('.');
  const size_t slash = path.find_last_of("/\\");
  if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
    return path;
  return path.substr(0, dot);
}

// Pulls a string out of the descriptor at `offset`, either bytes or UTF-16.
// Stops at the terminator or at the end of the file, so a truncated
// descriptor gives a short name rather than a read past the end.
std::string ReadMdsString(const std::vector<uint8_t>& mds, size_t offset,
                          bool wide) {
  if (offset >= mds.size())
    return std::string();

  if (!wide) {
    std::string text;
    for (size_t i = offset; i < mds.size() && mds[i] != 0; ++i)
      text.push_back(static_cast<char>(mds[i]));
    return text;
  }

  std::wstring text;
  for (size_t i = offset; i + 1 < mds.size(); i += 2) {
    const wchar_t ch = static_cast<wchar_t>(ReadU16(mds, i));
    if (ch == 0)
      break;
    text.push_back(ch);
  }
  if (text.empty())
    return std::string();
  const int size = WideCharToMultiByte(CP_ACP, 0, text.c_str(), -1, nullptr, 0,
                                       nullptr, nullptr);
  if (size <= 1)
    return std::string();
  std::string narrow(static_cast<size_t>(size - 1), '\0');
  WideCharToMultiByte(CP_ACP, 0, text.c_str(), -1, &narrow[0], size, nullptr,
                      nullptr);
  return narrow;
}

// Which file holds the sectors. Every track block ends with a footer naming
// it, and what is nearly always written there is "*.mdf" - the star standing
// for the descriptor's own name, which is what lets a pair be renamed
// together without breaking. Anything else is taken as a name, relative to
// the descriptor unless it carries a path of its own.
std::string ResolveMdfName(const std::vector<uint8_t>& mds,
                           uint32_t footer_offset,
                           const std::string& directory,
                           const std::string& stem) {
  const std::string fallback = stem + ".mdf";
  if (footer_offset == 0 || !Fits(mds, footer_offset, 8))
    return fallback;

  const uint32_t name_offset = ReadU32(mds, footer_offset);
  const bool wide = ReadU32(mds, footer_offset + 4) != 0;
  const std::string name = ReadMdsString(mds, name_offset, wide);
  if (name.empty())
    return fallback;

  if (name[0] == '*')
    return stem + name.substr(1);

  const bool absolute = name.find(':') != std::string::npos ||
                        name[0] == '/' || name[0] == '\\';
  return absolute ? name : directory + name;
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
    if (sources_[i].chd != nullptr)
      chd_close(static_cast<chd_file*>(sources_[i].chd));
    if (sources_[i].file != nullptr)
      fclose(sources_[i].file);
    if (sources_[i].device != nullptr)
      CloseHandle(static_cast<HANDLE>(sources_[i].device));
  }
  sources_.clear();
  tracks_.clear();
  track_sources_.clear();
  chd_runs_.clear();
  total_sectors_ = 0;
  path_.clear();
  if (sub_file_ != nullptr)
    fclose(sub_file_);
  sub_file_ = nullptr;
  sub_sectors_ = 0;
  sub_block_.clear();
  sub_block_first_ = -1;
}

bool Disc::ReadSubchannelQ(uint32_t lba, uint8_t* q) const {
  if (sub_file_ == nullptr || lba < kLeadInSectors)
    return false;
  const uint32_t sector = lba - kLeadInSectors;
  if (sector >= sub_sectors_)
    return false;
  const uint32_t kBlockSectors = 64;
  const uint32_t kBytes = 96;
  if (sub_block_first_ < 0 || sector < sub_block_first_ ||
      sector >= sub_block_first_ + sub_block_.size() / kBytes) {
    const uint32_t count = std::min(kBlockSectors, sub_sectors_ - sector);
    sub_block_.resize(static_cast<size_t>(count) * kBytes);
    if (_fseeki64(sub_file_, static_cast<long long>(sector) * kBytes, SEEK_SET) != 0 ||
        fread(sub_block_.data(), 1, sub_block_.size(), sub_file_) != sub_block_.size()) {
      sub_block_first_ = -1;
      return false;
    }
    sub_block_first_ = sector;
  }
  // Q is the second of the eight 12-byte channels.
  memcpy(q, &sub_block_[static_cast<size_t>(sector - sub_block_first_) * kBytes + 12], 12);
  return true;
}

// A bare image carries no track layout, and there is nowhere in the file it
// could: the table of contents lives in the disc's lead-in, which a dump of
// the data area does not include. So a `.bin` of a disc with CD music mounts
// as one data track, the game asks how many tracks there are, is told one, and
// never plays a note - not because playback is broken but because it never
// starts.
//
// The layout is almost always sitting right next to the image in a descriptor
// of the same name - a `.cue` beside a `.bin`, a `.mds` beside a `.mdf` -
// which is how the image was written out in the first place. Picking it up is
// the difference between music and silence, and picking the image rather than
// the descriptor is an easy thing for someone to do.
//
// Returns the descriptor's path if one is there beside this image, empty
// otherwise. Anything that does not open is left alone - a wrong layout would
// be worse than none.
std::string Disc::FindSibling(const std::string& image_path,
                              const char* extension) {
  const size_t dot = image_path.find_last_of('.');
  const size_t slash = image_path.find_last_of("/\\");
  if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
    return std::string();

  const std::string stem = image_path.substr(0, dot) + ".";
  // A network share can be case sensitive where the local disk is not, so the
  // three spellings anyone actually writes are all tried.
  const std::string lower = ToLower(extension);
  std::string upper = lower;
  for (size_t i = 0; i < upper.size(); ++i)
    upper[i] = static_cast<char>(toupper(static_cast<unsigned char>(upper[i])));
  std::string title = lower;
  if (!title.empty())
    title[0] = upper[0];

  const std::string candidates[] = { stem + lower, stem + upper, stem + title };
  for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
    FILE* fp = fopen(candidates[i].c_str(), "rb");
    if (fp == nullptr)
      continue;
    fclose(fp);
    return candidates[i];
  }
  return std::string();
}

bool Disc::Open(const char* path) {
  Close();
  open_error_.clear();
  if (path == nullptr || path[0] == '\0')
    return false;

  const std::string text = path;
  const std::string extension = Extension(text);

  bool ok = false;
  if (extension == ".cue")
    ok = OpenCue(path);
  else if (extension == ".chd")
    ok = OpenChd(path);
  else if (extension == ".mds") {
    ok = OpenMds(path);
    if (!ok) {
      // A descriptor that cannot be read still has its image sitting beside
      // it, and that image is worth trying: version 2 of the format keeps
      // the signature and encrypts everything after it, so there is nothing
      // to parse, but the `.mdf` is an ordinary one and mounts on its own.
      // What is lost is the track list, which costs a disc with CD music its
      // music and costs a single-track disc - which is most of them -
      // nothing at all.
      Close();
      const std::string image = FindSibling(text, "mdf");
      if (!image.empty())
        ok = OpenImage(image.c_str());
    }
  } else if (extension == ".ccd") {
    bool scrambled = false;
    ok = OpenCcd(path, &scrambled);
    if (!ok && !scrambled) {
      // As for an .mds: the image beside it is an ordinary one and mounts on
      // its own, minus the track list. Not for a scrambled image, though -
      // that one is refused here and stays refused, because its sectors are
      // not sectors yet and a bare mount would read noise as a disc.
      Close();
      const std::string image = FindSibling(text, "img");
      if (!image.empty())
        ok = OpenImage(image.c_str());
    }
  } else if (text.size() <= 3 && text.size() >= 2 && text[1] == ':')
    ok = OpenDevice(path);            // "D:" or "D:\"
  else if (text.compare(0, 4, "\\\\.\\") == 0)
    ok = OpenDevice(path);            // "\\.\D:"
  else {
    // Prefer a descriptor sitting beside the image: it is the only place the
    // track layout exists, and without it a disc with CD music mounts as one
    // data track and no game will ever ask for a note of it. For an .mdf the
    // stakes are higher still - the descriptor also carries the sector
    // stride, without which the file does not read as a disc at all.
    const std::string cue = FindSibling(text, "cue");
    if (!cue.empty())
      ok = OpenCue(cue.c_str());
    if (!ok) {
      Close();
      const std::string mds = FindSibling(text, "mds");
      if (!mds.empty())
        ok = OpenMds(mds.c_str());
    }
    bool scrambled = false;
    if (!ok) {
      Close();
      const std::string ccd = FindSibling(text, "ccd");
      if (!ccd.empty())
        ok = OpenCcd(ccd.c_str(), &scrambled);
    }
    // A scrambled image is not sectors at all, so opening it bare would mount
    // noise rather than lose a track list.
    if (!ok && !scrambled) {
      Close();
      ok = OpenImage(path);
    }
  }

  if (!ok) {
    Close();
    return false;
  }
  path_ = text;
  return true;
}

// Works out how sectors are laid out from the file length, unless a
// descriptor has already said. A raw dump divides by 2352; a Mode 2 dump
// without sync or header by 2336; a cooked ISO by 2048.
bool Disc::AddFileSource(const std::string& path, Source* out,
                         uint32_t sector_size) const {
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

  if (sector_size != 0) {
    // A descriptor said so, which beats anything the length can suggest.
    out->sector_size = sector_size;
  } else if ((size % kRawSectorSize) == 0) {
    out->sector_size = kRawSectorSize;
  } else if ((size % 2336) == 0) {
    out->sector_size = 2336;
  } else if ((size % 2048) == 0) {
    out->sector_size = 2048;
  } else if ((size % 2448) == 0) {
    // Raw sectors with their 96 bytes of subchannel still attached, which is
    // how Alcohol and CloneCD dump a disc. Checked after the three ordinary
    // strides because a 2448 image is only rarely a multiple of any of them,
    // whereas a plain raw image is a multiple of 2352 always.
    out->sector_size = 2448;
  } else if ((size % 2368) == 0) {
    out->sector_size = 2368;          // raw plus the 16-byte Q subchannel
  } else {
    // Not a recognisable sector multiple. Raw is the best guess for a PSX
    // image, and a partial trailing sector is simply not readable.
    out->sector_size = kRawSectorSize;
  }

  // Where the 2048 user bytes begin. Anything at or above a raw sector keeps
  // the sync and header in front of them; what follows the sector is
  // subchannel and is ignored.
  out->data_offset = (out->sector_size >= kRawSectorSize) ? 24
                   : (out->sector_size >= 2336) ? 8 : 0;

  out->sector_count = static_cast<uint32_t>(size / out->sector_size);
  return true;
}

// Where the data track ends in a raw image, found by looking at the sectors
// themselves.
//
// With no cue sheet there is no table of contents, but a data sector is
// recognisable on sight: it opens with a twelve byte sync pattern that audio,
// being arbitrary PCM, does not carry. A PSX disc with music is laid out data
// first and audio after, so the boundary is a single step from one to the
// other and a binary search finds it in about twenty reads of a file that may
// be half a gigabyte.
//
// This recovers where the audio starts. It cannot recover where one audio
// track ends and the next begins - nothing in the data area records that, it
// lived in the lead-in - so a disc with several music tracks still needs its
// cue sheet. Reporting one data track and one audio track is still better
// than calling the whole disc data: a game reading past the data track was
// otherwise handed music and told it was a filesystem.
//
// Returns the number of sectors in the data track, which is the whole file
// when there is no audio at all - the ordinary case.
uint32_t Disc::FindDataTrackLength(const Source& source) const {
  if (source.file == nullptr || source.sector_size < kRawSectorSize ||
      source.sector_count == 0)
    return source.sector_count;   // cooked images carry no sync to look for

  // If the last sector is data there is no audio, which is most discs, and
  // this costs one read.
  if (IsDataSector(source, source.sector_count - 1))
    return source.sector_count;
  // If the first is not data, this is not a layout worth guessing at.
  if (!IsDataSector(source, 0))
    return source.sector_count;

  uint32_t data = 0;                       // known data
  uint32_t audio = source.sector_count - 1;  // known not data
  while (audio - data > 1) {
    const uint32_t middle = data + (audio - data) / 2;
    if (IsDataSector(source, middle))
      data = middle;
    else
      audio = middle;
  }
  return data + 1;
}

bool Disc::IsDataSector(const Source& source, uint32_t file_sector) const {
  static const uint8_t kSync[12] = { 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                     0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00 };
  const long long offset =
      static_cast<long long>(file_sector) * source.sector_size;
  if (_fseeki64(source.file, offset, SEEK_SET) != 0)
    return false;
  uint8_t head[12];
  if (fread(head, 1, sizeof(head), source.file) != sizeof(head))
    return false;
  return memcmp(head, kSync, sizeof(kSync)) == 0;
}

bool Disc::OpenImage(const char* path) {
  Source source;
  if (!AddFileSource(path, &source))
    return false;
  sources_.push_back(source);

  // With no cue sheet the layout has to come from the sectors. Usually that
  // is one data track covering everything; a disc with music splits in two.
  const uint32_t data_length = FindDataTrackLength(source);

  Track track;
  track.number = 1;
  track.type = kTrackData;
  track.start_lba = kLeadInSectors;
  track.length = data_length;
  tracks_.push_back(track);

  TrackSource track_source;
  track_source.source = 0;
  track_source.file_lba = 0;
  track_sources_.push_back(track_source);

  if (data_length < source.sector_count) {
    // Everything after the data is audio. Where one music track ends and the
    // next begins is not recorded anywhere in the data area, so this is one
    // track however many the disc really had - a game that asks for track 5
    // by number still needs the cue sheet.
    Track audio;
    audio.number = 2;
    audio.type = kTrackAudio;
    audio.start_lba = kLeadInSectors + data_length;
    audio.length = source.sector_count - data_length;
    tracks_.push_back(audio);

    TrackSource audio_source;
    audio_source.source = 0;
    audio_source.file_lba = data_length;
    track_sources_.push_back(audio_source);
  }

  total_sectors_ = kLeadInSectors + source.sector_count;
  return true;
}

// Alcohol 120%'s pair: the `.mds` holds the layout and the `.mdf` holds the
// sectors, which is the same division of labour as a cue sheet and its bin.
// Two things make it worth reading rather than falling back on the image.
//
// The first is the usual one - the track list lives here and nowhere else, so
// without it a disc with music is one data track.
//
// The second is particular to this format and is worse. These dumps normally
// keep the 96 bytes of subchannel that follow every sector, giving a stride of
// 2448 rather than 2352. That is not a multiple of 2352, 2336 or 2048, so the
// guess `AddFileSource` makes from the file length alone lands on none of
// them, every sector after the first is read from the wrong offset, and the
// disc is not merely quiet but unreadable. The descriptor is the only thing
// that says 2448, which is why a `.mdf` on its own is not worth trying.
//
// The descriptor is small - a few hundred bytes for a normal disc - and is
// read whole.
bool Disc::OpenMds(const char* path) {
  std::vector<uint8_t> mds;
  if (!ReadWholeFile(path, &mds) || mds.size() < kMdsSessionOffset + 4)
    return false;
  if (memcmp(&mds[0], "MEDIA DESCRIPTOR", 16) != 0)
    return false;

  // Only version 1 is a descriptor in the sense of being readable. Version 2,
  // which DAEMON Tools has written since about 2011, keeps the signature and
  // then encrypts the whole of the rest - what follows the version in one is
  // a copyright string and several hundred bytes of noise. Checking the
  // version matters rather than letting the parse fail on its own: the
  // offsets read out of that noise are arbitrary numbers, and one of them
  // landing somewhere plausible would mount a disc made of nothing.
  if (mds[0x10] != 1)
    return false;

  const uint32_t sessions_offset = ReadU32(mds, kMdsSessionOffset);
  const uint16_t session_count = ReadU16(mds, kMdsSessionCountOffset);
  if (session_count == 0)
    return false;

  const std::string directory = DirectoryOf(path);
  const std::string stem = StemOf(path);

  for (uint16_t session = 0; session < session_count; ++session) {
    const size_t block = sessions_offset + session * kMdsSessionBlockSize;
    if (!Fits(mds, block, kMdsSessionBlockSize))
      return false;

    // The lead-in descriptors - the points that name the first track, the
    // last track and the lead-out - are counted in with the real ones and
    // filtered out below by their point number.
    const uint8_t block_count = mds[block + 0x0A];
    const uint32_t tracks_offset = ReadU32(mds, block + 0x14);

    for (uint8_t index = 0; index < block_count; ++index) {
      const size_t track = tracks_offset + index * kMdsTrackBlockSize;
      if (!Fits(mds, track, kMdsTrackBlockSize))
        return false;

      const uint8_t point = mds[track + 0x04];
      if (point < 1 || point > 99)
        continue;                     // 0xA0, 0xA1, 0xA2: the lead-in

      // Bit 2 of the control nibble is what tells a data track from an audio
      // one, the same bit the drive reports in its Q subchannel.
      const uint8_t adr_ctl = mds[track + 0x02];
      const uint16_t stated_size = ReadU16(mds, track + 0x10);
      const uint32_t start_sector = ReadU32(mds, track + 0x24);
      const uint64_t start_offset = ReadU64(mds, track + 0x28);
      const uint32_t footer_offset = ReadU32(mds, track + 0x34);

      // A stride outside this range is not something this can read, and
      // trusting it would put every sector in the wrong place; fall back to
      // the file length, which at least has a chance of being right.
      const uint32_t sector_size =
          (stated_size >= 2048 && stated_size <= 2448) ? stated_size : 0;

      const std::string file = ResolveMdfName(mds, footer_offset, directory,
                                              stem);
      if (file.empty())
        return false;

      // The same file backs every track of an ordinary dump, and it is a
      // whole disc: open it once.
      int source_index = -1;
      for (size_t i = 0; i < sources_.size(); ++i) {
        if (sources_[i].name == file) {
          source_index = static_cast<int>(i);
          break;
        }
      }
      const bool first_track_in_file = (source_index < 0);
      if (first_track_in_file) {
        Source source;
        if (!AddFileSource(file, &source, sector_size))
          return false;
        sources_.push_back(source);
        source_index = static_cast<int>(sources_.size()) - 1;
      }

      // Where this track begins, twice over: the descriptor gives a disc
      // address and, separately, a byte position in the file. They are not
      // the same distance apart, and assuming they were is the mistake this
      // format invites: the two seconds of pregap before an audio track have
      // a disc address but are usually not written to the file at all, so
      // every track after one is 150 sectors further along the disc than it
      // is along the file. Only the byte offset knows where the bytes are.
      const uint32_t stride = sources_[source_index].sector_size;
      uint32_t file_lba = static_cast<uint32_t>(start_offset / stride);
      if (file_lba == 0 && !first_track_in_file) {
        // No offset recorded for a track that cannot be at the start of its
        // file. The sector number is the only other thing that could place
        // it, and is right whenever the pregaps were written out.
        file_lba = start_sector;
      }

      Track entry;
      entry.number = point;
      entry.type = (adr_ctl & 0x04) ? kTrackData : kTrackAudio;
      entry.start_lba = kLeadInSectors + start_sector;
      entry.length = 0;               // filled in once the next track is known
      tracks_.push_back(entry);

      TrackSource track_source;
      track_source.source = source_index;
      track_source.file_lba = file_lba;
      track_sources_.push_back(track_source);
    }
  }

  if (tracks_.empty() || sources_.empty())
    return false;

  FinishTrackLayout();
  return true;
}

// CloneCD's .ccd: the disc's table of contents, written out as an INI file,
// beside a .img of raw 2352-byte sectors (and a .sub of subchannel this does
// not read).
//
// The TOC is the same set of points a real lead-in carries, one [Entry] each:
// A0 names the first track, A1 the last and A2 the lead-out, and points 1 to
// 99 are the tracks themselves. What each entry gives that matters here is
// PLBA - where the track starts - and Control, whose bit 2 separates a data
// track from an audio one, exactly as in an .mds.
//
// PLBA is image-relative, not absolute: it counts from the first sector of
// track 1 rather than from the lead-in, which is what the three discs to hand
// confirm - each one's lead-out PLBA is its image's sector count exactly
// (Area 51, 263,990 of them). So it is a file offset as it stands, and the
// absolute address is that plus the lead-in.
bool Disc::OpenCcd(const char* path, bool* scrambled_out) {
  if (scrambled_out != nullptr)
    *scrambled_out = false;

  FILE* fp = fopen(path, "r");
  if (fp == nullptr)
    return false;

  struct Entry {
    int point = -1;
    int control = 0;
    int64_t plba = 0;
  };
  std::vector<Entry> entries;
  bool in_entry = false;
  bool scrambled = false;
  // [TRACK n]'s INDEX 0 and INDEX 1, image-relative like PLBA: where one is
  // given, the difference is the track's pregap. Not every dump writes INDEX 0.
  int in_track = 0;
  std::map<int, int64_t> index0, index1;

  char line[1024];
  while (fgets(line, sizeof(line), fp) != nullptr) {
    const std::string trimmed = Trim(line);
    if (trimmed.empty())
      continue;

    if (trimmed[0] == '[') {
      // [Entry n] carries a TOC point, [TRACK n] the track's indexes (its mode
      // Control already says), and [Session n] nothing needed here.
      const std::string header = ToLower(trimmed);
      in_entry = header.compare(0, 6, "[entry") == 0;
      in_track = header.compare(0, 6, "[track") == 0 ? atoi(header.c_str() + 6) : 0;
      if (in_entry)
        entries.push_back(Entry());
      continue;
    }

    const size_t equals = trimmed.find('=');
    if (equals == std::string::npos)
      continue;
    const std::string key = ToLower(Trim(trimmed.substr(0, equals)));
    const std::string value = Trim(trimmed.substr(equals + 1));

    if (in_track > 0) {
      if (key == "index 0")
        index0[in_track] = strtoll(value.c_str(), nullptr, 0);
      else if (key == "index 1")
        index1[in_track] = strtoll(value.c_str(), nullptr, 0);
      continue;
    }

    if (!in_entry) {
      if (key == "datatracksscrambled")
        scrambled = strtol(value.c_str(), nullptr, 0) != 0;
      continue;
    }

    // Written as 0x01 for the points and plain decimal for the addresses, so
    // base 0 rather than 10 - strtol reads the prefix either way.
    Entry& entry = entries.back();
    if (key == "point")
      entry.point = static_cast<int>(strtol(value.c_str(), nullptr, 0));
    else if (key == "control")
      entry.control = static_cast<int>(strtol(value.c_str(), nullptr, 0));
    else if (key == "plba")
      entry.plba = strtoll(value.c_str(), nullptr, 0);
  }
  fclose(fp);

  // A scrambled image is the raw channel before descrambling, not sectors.
  // Nothing here unscrambles one, and mounting it would hand the controller
  // noise that looks like a disc, so it is refused rather than half-read -
  // and the caller is told, so it does not go on to mount the image bare.
  if (scrambled) {
    if (scrambled_out != nullptr)
      *scrambled_out = true;
    return false;
  }

  const std::string image = FindSibling(path, "img");
  if (image.empty())
    return false;

  Source source;
  // CloneCD always writes whole sectors, so the stride is known rather than
  // worked out from the length - the .img of a Mode 2 disc divides by 2352
  // and by 2336 alike, and guessing wrong shifts every sector.
  if (!AddFileSource(image, &source, kRawSectorSize))
    return false;
  sources_.push_back(source);

  for (size_t i = 0; i < entries.size(); ++i) {
    const Entry& entry = entries[i];
    // A0/A1/A2 describe the lead-in and lead-out; the tracks are 1 to 99.
    if (entry.point < 1 || entry.point > 99)
      continue;
    if (entry.plba < 0)
      continue;
    const uint32_t file_lba = static_cast<uint32_t>(entry.plba);
    if (file_lba >= source.sector_count)
      continue;

    Track track;
    track.number = entry.point;
    track.type = (entry.control & 0x04) ? kTrackData : kTrackAudio;
    track.start_lba = kLeadInSectors + file_lba;
    track.length = 0;                 // filled in once the next track is known
    tracks_.push_back(track);

    TrackSource track_source;
    track_source.source = 0;
    track_source.file_lba = file_lba;
    track_sources_.push_back(track_source);
  }

  if (tracks_.empty())
    return false;

  // The entries are written in TOC order, which is track order on every
  // descriptor seen - but the layout arithmetic depends on it, so make sure
  // rather than assume, and keep each track with its own source row.
  for (size_t i = 1; i < tracks_.size(); ++i) {
    for (size_t j = i; j > 0 && tracks_[j - 1].number > tracks_[j].number; --j) {
      std::swap(tracks_[j - 1], tracks_[j]);
      std::swap(track_sources_[j - 1], track_sources_[j]);
    }
  }

  for (Track& track : tracks_) {
    const auto zero = index0.find(track.number);
    const auto one = index1.find(track.number);
    if (zero != index0.end() && one != index1.end() && one->second > zero->second)
      track.pregap = static_cast<uint32_t>(one->second - zero->second);
  }

  FinishTrackLayout();

  // The subchannel, when the dump kept it. It is the one place a pregap is
  // recorded when the .ccd itself does not list INDEX 0 - which some do not -
  // and it is what the drive reads its position from, so where it exists the
  // controller answers from it (Cdrom::GetPosition). Optional: without it the
  // image mounts as before.
  const std::string sub = FindSibling(path, "sub");
  if (!sub.empty()) {
    sub_file_ = fopen(sub.c_str(), "rb");
    if (sub_file_ != nullptr) {
      _fseeki64(sub_file_, 0, SEEK_END);
      sub_sectors_ = static_cast<uint32_t>(_ftelli64(sub_file_) / 96);
      if (sub_sectors_ == 0) {
        fclose(sub_file_);
        sub_file_ = nullptr;
      }
    }
  }
  return true;
}

// A CHD: MAME's compressed disc image. libchdr does the container and the
// decompression; what is left here is the disc's layout, which a CD CHD keeps
// in one metadata entry per track - "TRACK:2 TYPE:AUDIO SUBTYPE:NONE
// FRAMES:4213 PREGAP:150 PGTYPE:VAUDIO PGSUB:RW POSTGAP:0", or the older form
// with only the first four fields. Everything about turning that into sector
// numbers follows chdman's conventions, as DuckStation reads them too:
//
//  - Each track's frames follow the last one's in the file, padded out to a
//    multiple of four.
//  - A PGTYPE starting with V means the pregap is stored, as the first PREGAP
//    of the track's FRAMES. Otherwise the pregap takes disc time but has no
//    frames, and reads as silence.
//  - A data track with no pregap stated has the standard two seconds - for
//    track 1, the lead-in every image here already assumes.
//
// The track list comes out the way a cue sheet of the same disc gives it: a
// track starts at its index 1 and runs until the next one does, so a pregap
// belongs to the end of the track before.
bool Disc::OpenChd(const char* path) {
  chd_file* chd = nullptr;
  const chd_error opened = chd_open(path, CHD_OPEN_READ, nullptr, &chd);
  if (opened != CHDERR_NONE) {
    chd_header header;
    bool zstd = false;
    if (chd_read_header(path, &header) == CHDERR_NONE) {
      for (int i = 0; i < 4; ++i)
        zstd = zstd || header.compression[i] == CHD_CODEC_ZSTD ||
               header.compression[i] == CHD_CODEC_CD_ZSTD;
    }
    if (zstd)
      open_error_ = "This CHD is compressed with zstd, which this build cannot read. "
                    "chdman's default codecs (LZMA, zlib and FLAC) all work.";
    else if (opened == CHDERR_REQUIRES_PARENT)
      open_error_ = "This CHD is a difference file that needs its parent CHD, "
                    "which is not supported.";
    else if (opened != CHDERR_FILE_NOT_FOUND)
      open_error_ = std::string("Could not read the CHD: ") + chd_error_string(opened) + ".";
    return false;
  }

  const chd_header* header = chd_get_header(chd);
  const uint32_t frames_per_hunk =
      header->unitbytes == CD_FRAME_SIZE ? header->hunkbytes / CD_FRAME_SIZE : 0;
  if (frames_per_hunk == 0) {
    open_error_ = "This CHD is not a CD image.";
    chd_close(chd);
    return false;
  }

  Source source;
  source.file = nullptr;
  source.device = nullptr;
  source.sector_size = CD_FRAME_SIZE;
  source.data_offset = 0;
  source.sector_count = static_cast<uint32_t>(header->logicalbytes / CD_FRAME_SIZE);
  source.name = path;
  source.chd = chd;
  source.chd_hunk_bytes = header->hunkbytes;
  sources_.push_back(source);   // Close() closes it from here on

  uint32_t disc_lba = 0;        // where the next track's pregap begins
  uint32_t frame = 0;           // where its frames begin in the CHD
  for (uint32_t index = 0;; ++index) {
    char text[256] = {};
    uint32_t length = 0;
    int number = 0, frames = 0, pregap = 0, postgap = 0;
    char type[64] = {}, subtype[64] = {}, pregap_type[64] = {}, pregap_sub[64] = {};
    if (chd_get_metadata(chd, CDROM_TRACK_METADATA2_TAG, index, text, sizeof(text) - 1,
                         &length, nullptr, nullptr) == CHDERR_NONE) {
      if (sscanf(text,
                 "TRACK:%d TYPE:%63s SUBTYPE:%63s FRAMES:%d PREGAP:%d PGTYPE:%63s "
                 "PGSUB:%63s POSTGAP:%d",
                 &number, type, subtype, &frames, &pregap, pregap_type, pregap_sub,
                 &postgap) != 8) {
        open_error_ = std::string("This CHD's track list is damaged: ") + text;
        return false;
      }
    } else if (chd_get_metadata(chd, CDROM_TRACK_METADATA_TAG, index, text,
                                sizeof(text) - 1, &length, nullptr,
                                nullptr) == CHDERR_NONE) {
      if (sscanf(text, "TRACK:%d TYPE:%63s SUBTYPE:%63s FRAMES:%d", &number, type,
                 subtype, &frames) != 4) {
        open_error_ = std::string("This CHD's track list is damaged: ") + text;
        return false;
      }
    } else {
      break;
    }

    const std::string mode = type;
    uint32_t data_size = 0;
    if (mode == "AUDIO" || mode == "MODE1_RAW" || mode == "MODE2_RAW" ||
        mode == "MODE2_FORM_MIX")
      data_size = kRawSectorSize;
    else if (mode == "MODE2")
      data_size = 2336;
    else if (mode == "MODE1" || mode == "MODE2_FORM1")
      data_size = 2048;
    if (data_size == 0 || number != static_cast<int>(index) + 1 || frames <= 0) {
      open_error_ = std::string("This CHD has a track this cannot read: ") + text;
      return false;
    }
    const bool audio = (mode == "AUDIO");

    const bool pregap_stored = pregap > 0 && pregap_type[0] == 'V';
    uint32_t data_frames = static_cast<uint32_t>(frames);
    if (pregap_stored) {
      if (pregap >= frames) {
        open_error_ = std::string("This CHD's track list is damaged: ") + text;
        return false;
      }
      data_frames -= static_cast<uint32_t>(pregap);
    }
    if (pregap <= 0 && !audio)
      pregap = static_cast<int>(kLeadInSectors);

    // Track 1 starts where every image here starts it, after the lead-in,
    // with whatever pregap it stores just in front.
    const uint32_t start = tracks_.empty() ? kLeadInSectors
                                           : disc_lba + static_cast<uint32_t>(pregap);
    if (pregap_stored) {
      if (static_cast<uint32_t>(pregap) > start) {
        open_error_ = std::string("This CHD's track list is damaged: ") + text;
        return false;
      }
      chd_runs_.push_back({ start - static_cast<uint32_t>(pregap),
                            static_cast<uint32_t>(pregap), frame, data_size, audio });
      frame += static_cast<uint32_t>(pregap);
    }
    chd_runs_.push_back({ start, data_frames, frame, data_size, audio });

    Track track;
    track.number = number;
    track.type = audio ? kTrackAudio : kTrackData;
    track.start_lba = start;
    track.length = data_frames;
    // Stored or not, the pregap is disc time that belongs to this track.
    track.pregap = pregap > 0 ? std::min(static_cast<uint32_t>(pregap), start) : 0;
    tracks_.push_back(track);
    track_sources_.push_back({ 0, frame });

    frame += data_frames;
    frame = (frame + 3) & ~3u;
    disc_lba = start + data_frames;
  }

  if (tracks_.empty()) {
    open_error_ = "This CHD is not a CD image: it has no track list.";
    return false;
  }
  if (frame > sources_[0].sector_count + 3) {
    open_error_ = "This CHD is shorter than its own track list.";
    return false;
  }
  // A track runs until the next one starts, so its stretch of pregap - stored
  // or not - is part of it, as it is from a cue sheet.
  for (size_t i = 0; i + 1 < tracks_.size(); ++i)
    tracks_[i].length = tracks_[i + 1].start_lba - tracks_[i].start_lba;
  total_sectors_ = tracks_.back().start_lba + tracks_.back().length;
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
  // The pending track's INDEX 00, if it had one, in the same file as its
  // INDEX 01 is about to be: the distance between them is its pregap.
  int64_t pending_index0 = -1;
  int pending_index0_source = -1;

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
      pending_index0 = -1;
      continue;
    }

    if (keyword == "index" && have_pending_track && current_source >= 0) {
      const size_t index_space = arguments.find(' ');
      if (index_space == std::string::npos)
        continue;
      const int index_number = atoi(arguments.substr(0, index_space).c_str());
      // INDEX 00 is the pregap; the track proper starts at INDEX 01.
      if (index_number != 0 && index_number != 1)
        continue;

      const std::string stamp = Trim(arguments.substr(index_space + 1));
      unsigned minute = 0, second = 0, frame = 0;
      if (sscanf(stamp.c_str(), "%u:%u:%u", &minute, &second, &frame) != 3)
        continue;
      const uint32_t file_lba = (minute * 60u + second) * 75u + frame;
      if (index_number == 0) {
        pending_index0 = file_lba;
        pending_index0_source = current_source;
        continue;
      }

      Track track;
      track.number = pending_track_number;
      track.type = pending_track_type;
      track.start_lba = kLeadInSectors + file_lba;
      track.length = 0;                 // filled in once the next track is known
      if (pending_index0 >= 0 && pending_index0_source == current_source &&
          pending_index0 < file_lba)
        track.pregap = static_cast<uint32_t>(file_lba - pending_index0);
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

  FinishTrackLayout();
  return true;
}

// A track runs until the next one starts, or to the end of its file. The last
// track's end is where the lead-out begins, which is what the controller
// reports as the end of the disc.
void Disc::FinishTrackLayout() {
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
}

// Whether an address falls in a hole between two tracks. Tracks are laid out
// in order, so this is one pass and nearly always finds nothing: only an
// image that left its pregaps out has holes at all.
bool Disc::IsUnstoredGap(uint32_t lba) const {
  for (size_t i = 0; i + 1 < tracks_.size(); ++i) {
    if (lba >= tracks_[i].start_lba + tracks_[i].length &&
        lba < tracks_[i + 1].start_lba)
      return true;
  }
  return false;
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

// One sector out of an image file, through the read-ahead block described in
// disc.h. A hit is a memcpy; a miss reads a block of sectors and serves this one
// out of its front. Sequential reads - which is nearly all of them - hit
// thirty-one times in thirty-two.
//
// A short read at the end of the file is not a failure as long as the sector
// asked for came back whole: the block deliberately reaches past it.
bool Disc::ReadFileSector(const Source& source, long long offset, uint32_t wanted,
                          uint8_t* out) const {
  if (source.ahead_offset >= 0 && offset >= source.ahead_offset &&
      offset + static_cast<long long>(wanted) <=
          source.ahead_offset + static_cast<long long>(source.ahead_size)) {
    memcpy(out, &source.ahead[static_cast<size_t>(offset - source.ahead_offset)],
           wanted);
    return true;
  }

  const size_t block =
      static_cast<size_t>(kReadAheadSectors) *
      (source.sector_size > 0 ? source.sector_size : kRawSectorSize);
  if (source.ahead.size() < block)
    source.ahead.resize(block);

  source.ahead_offset = -1;
  source.ahead_size = 0;
  if (_fseeki64(source.file, offset, SEEK_SET) != 0)
    return false;
  const size_t got = fread(&source.ahead[0], 1, block, source.file);
  if (got < wanted)
    return false;
  source.ahead_offset = offset;
  source.ahead_size = static_cast<uint32_t>(got);
  memcpy(out, &source.ahead[0], wanted);
  return true;
}

// One sector of a CHD: the run it falls in says which frame, the frame says
// which hunk, and a hunk is decompressed whole and kept until a sector outside
// it is wanted.
bool Disc::ReadChdSector(uint32_t lba, uint8_t* out) const {
  const Source& source = sources_[0];
  for (const ChdRun& run : chd_runs_) {
    if (lba < run.lba || lba >= run.lba + run.count)
      continue;
    const uint32_t frame = run.frame + (lba - run.lba);
    const uint32_t frames_per_hunk = source.chd_hunk_bytes / CD_FRAME_SIZE;
    const uint32_t hunk = frame / frames_per_hunk;
    if (hunk != source.chd_hunk_number) {
      source.chd_hunk.resize(source.chd_hunk_bytes);
      source.chd_hunk_number = 0xFFFFFFFFu;
      if (chd_read(static_cast<chd_file*>(source.chd), hunk, source.chd_hunk.data()) !=
          CHDERR_NONE)
        return false;
      source.chd_hunk_number = hunk;
    }
    const uint8_t* in = &source.chd_hunk[(frame % frames_per_hunk) * CD_FRAME_SIZE];
    memset(out, 0, kRawSectorSize);
    if (run.audio) {
      // Stored big-endian; the controller wants CD audio as it comes off a
      // disc, low byte first.
      for (uint32_t i = 0; i < kRawSectorSize; i += 2) {
        out[i] = in[i + 1];
        out[i + 1] = in[i];
      }
    } else {
      memcpy(out, in, run.data_size);
    }
    SynthesiseSectorHeader(out, lba, run.data_size);
    return true;
  }

  // Not stored: a pregap the CHD leaves out, which on a disc is silence - the
  // same answer a cue sheet's unstored gap gets. Outside the tracks
  // altogether is still a failed read.
  for (const Track& track : tracks_) {
    if (lba >= track.start_lba && lba < track.start_lba + track.length) {
      memset(out, 0, kRawSectorSize);
      return true;
    }
  }
  return false;
}

bool Disc::ReadSector(uint32_t lba, uint8_t* out) const {
  if (sources_.empty() || out == nullptr)
    return false;
  if (sources_[0].chd != nullptr)
    return ReadChdSector(lba, out);

  // Find the track this sector falls in.
  int track_index = -1;
  for (size_t i = 0; i < tracks_.size(); ++i) {
    if (lba >= tracks_[i].start_lba &&
        lba < tracks_[i].start_lba + tracks_[i].length) {
      track_index = static_cast<int>(i);
      break;
    }
  }
  if (track_index < 0) {
    // Between two tracks and inside neither: a pregap the image does not
    // store. Alcohol leaves the two seconds before an audio track out of the
    // file, so those sectors have a disc address and no bytes behind them.
    // They are silence on a real disc, and answering with silence is what
    // keeps a player that starts there running - a failed read stops it with
    // an error instead. Anything outside the tracks altogether still fails.
    if (IsUnstoredGap(lba)) {
      memset(out, 0, kRawSectorSize);
      return true;
    }
    return false;
  }

  const TrackSource& track_source = track_sources_[track_index];
  const Source& source = sources_[track_source.source];
  const uint32_t offset_in_track = lba - tracks_[track_index].start_lba;
  const uint32_t file_sector = track_source.file_lba + offset_in_track;
  if (file_sector >= source.sector_count)
    return false;

  memset(out, 0, kRawSectorSize);
  const long long byte_offset =
      static_cast<long long>(file_sector) * source.sector_size;
  // The stride is how far apart sectors sit; what is wanted out of each one is
  // only ever the raw sector at its front. An image that kept its subchannel
  // strides 2448 or 2368 bytes, and the extra belongs to neither the caller's
  // buffer nor anything above here.
  const uint32_t wanted = (source.sector_size < kRawSectorSize)
                              ? source.sector_size
                              : static_cast<uint32_t>(kRawSectorSize);

  if (source.file != nullptr) {
    if (!ReadFileSector(source, byte_offset, wanted, out))
      return false;
  } else if (source.device != nullptr) {
    HANDLE handle = static_cast<HANDLE>(source.device);
    LARGE_INTEGER position;
    position.QuadPart = byte_offset;
    if (!SetFilePointerEx(handle, position, NULL, FILE_BEGIN))
      return false;
    DWORD read = 0;
    if (!ReadFile(handle, out, wanted, &read, NULL) || read != wanted)
      return false;
  } else {
    return false;
  }

  SynthesiseSectorHeader(out, lba, source.sector_size);
  return true;
}

}
}
