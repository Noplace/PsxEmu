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
  if (path == nullptr || path[0] == '\0')
    return false;

  const std::string text = path;
  const std::string extension = Extension(text);

  bool ok = false;
  if (extension == ".cue")
    ok = OpenCue(path);
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
    if (!ok) {
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
    if (_fseeki64(source.file, byte_offset, SEEK_SET) != 0)
      return false;
    if (fread(out, 1, wanted, source.file) != wanted)
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
