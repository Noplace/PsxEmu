// chd_writer.h - writes a CD image as a CHD, the way chdman createcd does.
//
// For the harnesses and tools/make_chd.cpp, not the emulator: psx/disc.cpp only
// ever reads CHDs, through libchdr. This exists so the reading can be tested
// without MAME's chdman, which is not on this machine - and so that a test
// can build exactly the CHD it needs, a stored pregap or a zstd header
// included.
//
// It follows chdman's format and conventions:
//  - CHD version 5, hunks of 8 frames of 2,448 bytes (a sector plus 96 bytes
//    of subcode), each track padded to a multiple of 4 frames;
//  - the CD codecs chdman uses by default - cdlz (LZMA), cdzl (zlib) and cdfl
//    (FLAC) - trying each on every hunk and keeping the smallest, storing a
//    hunk raw when none of them helps, and a repeat of an earlier hunk as a
//    reference to it;
//  - a data sector whose ECC checks out stored without its sync and ECC,
//    which the reader regenerates;
//  - CD audio stored big-endian;
//  - one CHT2 metadata entry per track.
//
// The FLAC it writes is the simplest the format allows - every subframe
// VERBATIM, samples as they are - so it compresses nothing and cdfl only ever
// wins on silence. What matters is that it is FLAC, decoded by the same code
// that decodes chdman's.

#pragma once

#include <libchdr/cdrom.h>
#include <libchdr/chd.h>
#include <zlib.h>
#include "LzmaEnc.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

// libchdr's, the CRC it checks every hunk against.
extern "C" uint16_t crc16(const void* data, uint32_t length);

namespace chd_writer {

enum class Codec {
  kBest,   // chdman's way: whichever of the three is smallest, per hunk
  kNone,   // every hunk stored raw
  kLzma,   // every hunk cdlz, unless it would not fit
  kZlib,   // cdzl
  kFlac    // cdfl
};

struct Track {
  bool audio = false;
  // Sectors in the image, a stored pregap included.
  uint32_t frames = 0;
  // How many of those frames are pregap (PGTYPE V...): the reader counts the
  // track as starting that far in.
  uint32_t stored_pregap = 0;
  // A pregap the image does not store: disc time before the track with no
  // frames behind it.
  uint32_t unstored_pregap = 0;
  // One raw 2,352-byte sector of this track, index 0 to frames - 1, as it is
  // in a .bin - CD audio little-endian.
  std::function<bool(uint32_t index, uint8_t* sector)> read;
};

struct Stats {
  uint32_t hunks = 0;
  uint32_t lzma = 0, zlib = 0, flac = 0, raw = 0, repeated = 0;
  uint64_t bytes = 0;
};

// Extra header codecs a test can ask for, to build a CHD this reader must
// refuse - a zstd one, say. Zero leaves chdman's list alone.
struct Options {
  uint32_t fourth_codec = 0;
};

namespace detail {

const uint32_t kFramesPerHunk = 8;
const uint32_t kFrameBytes = CD_FRAME_SIZE;               // 2448
const uint32_t kSectorBytes = CD_MAX_SECTOR_DATA;         // 2352
const uint32_t kSubcodeBytes = CD_MAX_SUBCODE_DATA;       // 96
const uint32_t kHunkBytes = kFramesPerHunk * kFrameBytes;
const uint8_t kCdSync[12] = { 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                              0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00 };

// The map's compression types (libchdr's enum).
const int kTypeLzma = 0, kTypeZlib = 1, kTypeFlac = 2, kTypeNone = 4, kTypeSelf = 5;

inline void PutBig(std::vector<uint8_t>& out, uint64_t value, int bytes) {
  for (int i = bytes - 1; i >= 0; --i)
    out.push_back(static_cast<uint8_t>(value >> (i * 8)));
}

inline void SetBig(uint8_t* at, uint64_t value, int bytes) {
  for (int i = 0; i < bytes; ++i)
    at[i] = static_cast<uint8_t>(value >> ((bytes - 1 - i) * 8));
}

// Most significant bit first, which is how libchdr's bitstream reads.
class BitWriter {
 public:
  void Write(uint32_t value, int bits) {
    for (int i = bits - 1; i >= 0; --i) {
      if (used_ == 0)
        bytes_.push_back(0);
      if ((value >> i) & 1)
        bytes_.back() |= static_cast<uint8_t>(0x80 >> used_);
      used_ = (used_ + 1) & 7;
    }
  }
  const std::vector<uint8_t>& bytes() const { return bytes_; }

 private:
  std::vector<uint8_t> bytes_;
  int used_ = 0;
};

inline std::vector<uint8_t> Deflate(const uint8_t* data, size_t size) {
  z_stream stream = {};
  deflateInit2(&stream, Z_BEST_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
  std::vector<uint8_t> out(deflateBound(&stream, static_cast<uLong>(size)) + 16);
  stream.next_in = const_cast<Bytef*>(data);
  stream.avail_in = static_cast<uInt>(size);
  stream.next_out = out.data();
  stream.avail_out = static_cast<uInt>(out.size());
  deflate(&stream, Z_FINISH);
  out.resize(stream.total_out);
  deflateEnd(&stream);
  return out;
}

inline void* LzmaAlloc(ISzAllocPtr, size_t size) { return malloc(size); }
inline void LzmaFree(ISzAllocPtr, void* address) { free(address); }

// Raw LZMA, no header and no end mark, with the properties libchdr derives
// its decoder from: level 9, sized for a hunk's sectors.
inline std::vector<uint8_t> Lzma(const uint8_t* data, size_t size) {
  static const ISzAlloc alloc = { LzmaAlloc, LzmaFree };
  CLzmaEncProps props;
  LzmaEncProps_Init(&props);
  props.level = 9;
  props.reduceSize = kFramesPerHunk * kSectorBytes;
  LzmaEncProps_Normalize(&props);
  std::vector<uint8_t> out(size + size / 2 + 1024);
  SizeT out_size = out.size();
  Byte encoded_props[LZMA_PROPS_SIZE];
  SizeT props_size = LZMA_PROPS_SIZE;
  if (LzmaEncode(out.data(), &out_size, data, size, &props, encoded_props, &props_size, 0,
                 nullptr, &alloc, &alloc) != SZ_OK)
    return std::vector<uint8_t>(size * 2 + 1);   // too big to be chosen
  out.resize(out_size);
  return out;
}

inline uint8_t Crc8(const uint8_t* data, size_t size) {
  uint8_t crc = 0;
  for (size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (int b = 0; b < 8; ++b)
      crc = static_cast<uint8_t>((crc & 0x80) ? (crc << 1) ^ 0x07 : crc << 1);
  }
  return crc;
}

inline uint16_t FlacCrc16(const uint8_t* data, size_t size) {
  uint16_t crc = 0;
  for (size_t i = 0; i < size; ++i) {
    crc ^= static_cast<uint16_t>(data[i] << 8);
    for (int b = 0; b < 8; ++b)
      crc = static_cast<uint16_t>((crc & 0x8000) ? (crc << 1) ^ 0x8005 : crc << 1);
  }
  return crc;
}

// libchdr's cdfl block size for a hunk: a quarter of its sector bytes, halved
// down to 2,048 samples or fewer.
inline uint32_t FlacBlockSize(uint32_t bytes) {
  uint32_t block = bytes / 4;
  while (block > 2048)
    block /= 2;
  return block;
}

// FLAC frames for a hunk's sector bytes, read as big-endian stereo samples -
// what cdfl's decoder turns them back into. Fixed-size blocks, 44.1 kHz, 16
// bits, two independent channels, each subframe VERBATIM.
inline std::vector<uint8_t> Flac(const uint8_t* data, size_t size) {
  const uint32_t block = FlacBlockSize(static_cast<uint32_t>(size));
  const uint32_t pairs = static_cast<uint32_t>(size / 4);
  std::vector<uint8_t> out;
  for (uint32_t first = 0, number = 0; first < pairs; first += block, ++number) {
    const uint32_t count = (pairs - first < block) ? pairs - first : block;
    std::vector<uint8_t> frame;
    frame.push_back(0xFF);
    frame.push_back(0xF8);                  // sync, fixed block size
    frame.push_back(0x79);                  // block size in 16 bits below; 44.1 kHz
    frame.push_back(0x18);                  // 2 channels independent; 16-bit
    frame.push_back(static_cast<uint8_t>(number));   // frame number, UTF-8 (< 128)
    frame.push_back(static_cast<uint8_t>((count - 1) >> 8));
    frame.push_back(static_cast<uint8_t>(count - 1));
    frame.push_back(Crc8(frame.data(), frame.size()));
    for (int channel = 0; channel < 2; ++channel) {
      frame.push_back(0x02);                // VERBATIM, no wasted bits
      for (uint32_t i = 0; i < count; ++i) {
        const size_t at = (static_cast<size_t>(first + i) * 2 + channel) * 2;
        frame.push_back(data[at]);          // the sample big-endian, as FLAC stores it
        frame.push_back(data[at + 1]);
      }
    }
    const uint16_t crc = FlacCrc16(frame.data(), frame.size());
    frame.push_back(static_cast<uint8_t>(crc >> 8));
    frame.push_back(static_cast<uint8_t>(crc));
    out.insert(out.end(), frame.begin(), frame.end());
  }
  return out;
}

// cdlz and cdzl: which frames had their sync and ECC taken out, the length of
// the compressed sectors, the sectors, and the subcode deflated after them.
inline std::vector<uint8_t> CdCodec(const uint8_t* hunk, bool lzma) {
  std::vector<uint8_t> sectors(kFramesPerHunk * kSectorBytes);
  std::vector<uint8_t> subcode(kFramesPerHunk * kSubcodeBytes);
  std::vector<uint8_t> ecc_bits((kFramesPerHunk + 7) / 8, 0);
  for (uint32_t f = 0; f < kFramesPerHunk; ++f) {
    uint8_t* sector = &sectors[f * kSectorBytes];
    memcpy(sector, &hunk[f * kFrameBytes], kSectorBytes);
    memcpy(&subcode[f * kSubcodeBytes], &hunk[f * kFrameBytes + kSectorBytes], kSubcodeBytes);
    if (memcmp(sector, kCdSync, sizeof(kCdSync)) == 0 && ecc_verify(sector)) {
      ecc_bits[f / 8] |= static_cast<uint8_t>(1 << (f % 8));
      memset(sector, 0, sizeof(kCdSync));
      ecc_clear(sector);
    }
  }
  const std::vector<uint8_t> base =
      lzma ? Lzma(sectors.data(), sectors.size()) : Deflate(sectors.data(), sectors.size());
  const std::vector<uint8_t> sub = Deflate(subcode.data(), subcode.size());
  std::vector<uint8_t> out = ecc_bits;
  PutBig(out, base.size(), kHunkBytes < 65536 ? 2 : 3);
  out.insert(out.end(), base.begin(), base.end());
  out.insert(out.end(), sub.begin(), sub.end());
  return out;
}

// cdfl: the sectors as FLAC, the subcode deflated after them.
inline std::vector<uint8_t> CdFlac(const uint8_t* hunk) {
  std::vector<uint8_t> sectors(kFramesPerHunk * kSectorBytes);
  std::vector<uint8_t> subcode(kFramesPerHunk * kSubcodeBytes);
  for (uint32_t f = 0; f < kFramesPerHunk; ++f) {
    memcpy(&sectors[f * kSectorBytes], &hunk[f * kFrameBytes], kSectorBytes);
    memcpy(&subcode[f * kSubcodeBytes], &hunk[f * kFrameBytes + kSectorBytes], kSubcodeBytes);
  }
  std::vector<uint8_t> out = Flac(sectors.data(), sectors.size());
  const std::vector<uint8_t> sub = Deflate(subcode.data(), subcode.size());
  out.insert(out.end(), sub.begin(), sub.end());
  return out;
}

inline uint64_t Fnv(const uint8_t* data, size_t size) {
  uint64_t hash = 14695981039346656037ull;
  for (size_t i = 0; i < size; ++i)
    hash = (hash ^ data[i]) * 1099511628211ull;
  return hash;
}

inline int BitsFor(uint64_t value) {
  int bits = 0;
  while (value >> bits)
    ++bits;
  return bits;
}

}  // namespace detail

// Writes `tracks` to `path`. Returns an empty string, or what went wrong.
inline std::string Write(const std::string& path, const std::vector<Track>& tracks, Codec codec,
                         Stats* stats = nullptr, const Options& options = Options()) {
  using namespace detail;
  if (tracks.empty())
    return "no tracks";

  // Where each track's frames sit in the CHD: one after another, each padded
  // out to a multiple of four.
  std::vector<uint32_t> first_frame;
  uint32_t total_frames = 0;
  for (const Track& track : tracks) {
    first_frame.push_back(total_frames);
    total_frames += (track.frames + 3) & ~3u;
  }
  const uint32_t hunk_count = (total_frames + kFramesPerHunk - 1) / kFramesPerHunk;

  // One hunk as the reader will hand it back: sectors (audio big-endian) and
  // zero subcode, and zero frames for padding.
  auto build_hunk = [&](uint32_t hunk, uint8_t* out) -> bool {
    memset(out, 0, kHunkBytes);
    for (uint32_t f = 0; f < kFramesPerHunk; ++f) {
      const uint32_t frame = hunk * kFramesPerHunk + f;
      for (size_t t = 0; t < tracks.size(); ++t) {
        if (frame < first_frame[t] || frame >= first_frame[t] + tracks[t].frames)
          continue;
        uint8_t* sector = &out[f * kFrameBytes];
        if (!tracks[t].read(frame - first_frame[t], sector))
          return false;
        if (tracks[t].audio) {
          for (uint32_t i = 0; i < kSectorBytes; i += 2) {
            const uint8_t low = sector[i];
            sector[i] = sector[i + 1];
            sector[i + 1] = low;
          }
        }
        break;
      }
    }
    return true;
  };

  FILE* file = fopen(path.c_str(), "wb");
  if (file == nullptr)
    return "cannot create " + path;
  std::vector<uint8_t> out;
  auto flush = [&]() {
    if (!out.empty())
      fwrite(out.data(), 1, out.size(), file);
    out.clear();
  };

  // The header, filled in once the offsets are known.
  std::vector<uint8_t> header(124, 0);
  fwrite(header.data(), 1, header.size(), file);
  uint64_t position = header.size();

  // Metadata: one CHT2 entry per track, each pointing at the next.
  const uint64_t meta_offset = position;
  for (size_t t = 0; t < tracks.size(); ++t) {
    const Track& track = tracks[t];
    const char* type = track.audio ? "AUDIO" : "MODE2_RAW";
    char text[256];
    snprintf(text, sizeof(text),
             "TRACK:%d TYPE:%s SUBTYPE:NONE FRAMES:%u PREGAP:%u PGTYPE:%s%s PGSUB:RW POSTGAP:0",
             static_cast<int>(t + 1), type, track.frames,
             track.stored_pregap ? track.stored_pregap : track.unstored_pregap,
             track.stored_pregap ? "V" : "", track.stored_pregap || track.unstored_pregap ? type : "MODE1");
    const uint32_t length = static_cast<uint32_t>(strlen(text) + 1);
    const uint64_t next = (t + 1 < tracks.size()) ? position + 16 + length : 0;
    PutBig(out, CDROM_TRACK_METADATA2_TAG, 4);
    out.push_back(0x01);                    // CHD_MDFLAGS_CHECKSUM, as chdman sets it
    PutBig(out, length, 3);
    PutBig(out, next, 8);
    out.insert(out.end(), text, text + length);
    position += 16 + length;
  }
  flush();

  // The hunks.
  struct Entry {
    int type;
    uint32_t length;
    uint64_t offset;   // in the file, or the hunk repeated for kTypeSelf
    uint16_t crc;
  };
  std::vector<Entry> map(hunk_count);
  std::unordered_multimap<uint64_t, uint32_t> seen;
  std::vector<uint8_t> hunk(kHunkBytes), earlier(kHunkBytes);
  Stats local;
  const uint64_t first_offset = position;
  for (uint32_t h = 0; h < hunk_count; ++h) {
    if (!build_hunk(h, hunk.data())) {
      fclose(file);
      return "a sector could not be read";
    }
    const uint16_t crc = crc16(hunk.data(), kHunkBytes);

    // A hunk that repeats an earlier one is a reference to it.
    const uint64_t hash = Fnv(hunk.data(), kHunkBytes);
    bool repeated = false;
    auto range = seen.equal_range(hash);
    for (auto it = range.first; it != range.second && !repeated; ++it) {
      if (build_hunk(it->second, earlier.data()) && earlier == hunk) {
        map[h] = { kTypeSelf, 0, it->second, 0 };
        repeated = true;
      }
    }
    if (repeated) {
      ++local.repeated;
      continue;
    }
    seen.emplace(hash, h);

    std::vector<uint8_t> best;
    int type = kTypeNone;
    auto consider = [&](int candidate, std::vector<uint8_t> data) {
      if (data.size() < kHunkBytes && (best.empty() || data.size() < best.size())) {
        best = std::move(data);
        type = candidate;
      }
    };
    if (codec == Codec::kBest || codec == Codec::kLzma)
      consider(kTypeLzma, CdCodec(hunk.data(), true));
    if (codec == Codec::kBest || codec == Codec::kZlib)
      consider(kTypeZlib, CdCodec(hunk.data(), false));
    if (codec == Codec::kBest || codec == Codec::kFlac)
      consider(kTypeFlac, CdFlac(hunk.data()));
    if (type == kTypeNone)
      best = hunk;
    map[h] = { type, static_cast<uint32_t>(best.size()), position, crc };
    fwrite(best.data(), 1, best.size(), file);
    position += best.size();
    switch (type) {
      case kTypeLzma: ++local.lzma; break;
      case kTypeZlib: ++local.zlib; break;
      case kTypeFlac: ++local.flac; break;
      default: ++local.raw; break;
    }
  }

  // The map, as libchdr decodes it: a Huffman code for each hunk's type -
  // its table sent first, four bits a symbol - then each hunk's length and
  // CRC, or the hunk it repeats. Its CRC is over the 12-byte-a-hunk table
  // libchdr rebuilds from it.
  std::vector<uint8_t> raw_map(static_cast<size_t>(hunk_count) * 12, 0);
  uint32_t max_length = 0;
  for (uint32_t h = 0; h < hunk_count; ++h) {
    uint8_t* entry = &raw_map[static_cast<size_t>(h) * 12];
    entry[0] = static_cast<uint8_t>(map[h].type);
    SetBig(&entry[1], map[h].length, 3);
    SetBig(&entry[4], map[h].offset, 6);
    SetBig(&entry[10], map[h].crc, 2);
    if (map[h].type != kTypeSelf && map[h].type != kTypeNone && map[h].length > max_length)
      max_length = map[h].length;
  }
  const int length_bits = BitsFor(max_length);
  const int self_bits = BitsFor(hunk_count);

  // A complete canonical code over the types that occur: with n of them,
  // lengths k-1 and k where 2^(k-1) < n <= 2^k.
  int used[16] = {};
  for (const Entry& entry : map)
    used[entry.type] = 1;
  std::vector<int> symbols;
  for (int s = 0; s < 16; ++s)
    if (used[s])
      symbols.push_back(s);
  int lengths[16] = {};
  if (symbols.size() == 1) {
    lengths[symbols[0]] = 1;
  } else {
    int k = 0;
    while ((1u << k) < symbols.size())
      ++k;
    const int shorter = (1 << k) - static_cast<int>(symbols.size());
    for (size_t i = 0; i < symbols.size(); ++i)
      lengths[symbols[i]] = static_cast<int>(i) < shorter ? k - 1 : k;
  }
  // libchdr's canonical assignment: codes counted up from the longest length.
  uint32_t codes[16] = {};
  {
    uint32_t histogram[33] = {};
    for (int s = 0; s < 16; ++s)
      histogram[lengths[s]]++;
    uint32_t start = 0;
    for (int length = 32; length > 0; --length) {
      const uint32_t next = (start + histogram[length]) >> 1;
      histogram[length] = start;
      start = next;
    }
    for (int s = 0; s < 16; ++s)
      if (lengths[s] > 0)
        codes[s] = histogram[lengths[s]]++;
  }
  BitWriter bits;
  for (int s = 0; s < 16; ++s) {
    if (lengths[s] == 1) {
      bits.Write(1, 4);                      // a 1 is escaped as 1, 1
      bits.Write(1, 4);
    } else {
      bits.Write(static_cast<uint32_t>(lengths[s]), 4);
    }
  }
  for (const Entry& entry : map)
    bits.Write(codes[entry.type], lengths[entry.type]);
  for (const Entry& entry : map) {
    if (entry.type == kTypeSelf) {
      bits.Write(static_cast<uint32_t>(entry.offset), self_bits);
    } else {
      if (entry.type != kTypeNone)
        bits.Write(entry.length, length_bits);
      bits.Write(entry.crc, 16);
    }
  }
  const uint64_t map_offset = position;
  PutBig(out, bits.bytes().size(), 4);
  PutBig(out, first_offset, 6);
  PutBig(out, crc16(raw_map.data(), static_cast<uint32_t>(raw_map.size())), 2);
  out.push_back(static_cast<uint8_t>(length_bits));
  out.push_back(static_cast<uint8_t>(self_bits));
  out.push_back(0);                          // no parent
  out.push_back(0);
  out.insert(out.end(), bits.bytes().begin(), bits.bytes().end());
  flush();

  // And now the header.
  memcpy(&header[0], "MComprHD", 8);
  SetBig(&header[8], 124, 4);
  SetBig(&header[12], 5, 4);
  SetBig(&header[16], CHD_CODEC_CD_LZMA, 4);
  SetBig(&header[20], CHD_CODEC_CD_ZLIB, 4);
  SetBig(&header[24], CHD_CODEC_CD_FLAC, 4);
  SetBig(&header[28], options.fourth_codec, 4);
  SetBig(&header[32], static_cast<uint64_t>(total_frames) * kFrameBytes, 8);
  SetBig(&header[40], map_offset, 8);
  SetBig(&header[48], meta_offset, 8);
  SetBig(&header[56], kHunkBytes, 4);
  SetBig(&header[60], kFrameBytes, 4);
  fseek(file, 0, SEEK_SET);
  fwrite(header.data(), 1, header.size(), file);
  fclose(file);

  local.hunks = hunk_count;
  local.bytes = map_offset + 16 + bits.bytes().size();
  if (stats != nullptr)
    *stats = local;
  return std::string();
}

}  // namespace chd_writer
