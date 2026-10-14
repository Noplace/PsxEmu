// ECM images - see media/ecm.h. The sector layout, the EDC and the P and Q parity are ECMA-130's
// (CD-ROM, 2nd edition, Annex A and section 14).

#include "media/ecm.h"

#include <algorithm>
#include <cstring>

namespace emulation {
namespace psx {

namespace {

// GF(2^8) over x^8 + x^4 + x^3 + x^2 + 1, for the Reed-Solomon parity: multiplying by alpha, and
// its inverse, as tables. And the EDC's CRC table, polynomial 8001801Bh bit-reversed.
struct Tables {
  uint8_t forward[256];
  uint8_t backward[256];
  uint32_t edc[256];
  Tables() {
    for (uint32_t i = 0; i < 256; ++i) {
      const uint32_t f = (i << 1) ^ ((i & 0x80) != 0 ? 0x11D : 0);
      forward[i] = static_cast<uint8_t>(f);
      backward[i ^ f] = static_cast<uint8_t>(i);
      uint32_t e = i;
      for (int bit = 0; bit < 8; ++bit)
        e = (e >> 1) ^ ((e & 1) != 0 ? 0xD8018001u : 0u);
      edc[i] = e;
    }
  }
};

const Tables& T() {
  static const Tables tables;
  return tables;
}

// One of the two parity codes: `major_count` columns (or diagonals) of `minor_count` bytes each,
// the parity for each written twice over at `dest` and `dest + major_count`.
void EccBlock(const uint8_t* source, uint32_t major_count, uint32_t minor_count,
              uint32_t major_mult, uint32_t minor_inc, uint8_t* dest) {
  const Tables& t = T();
  const uint32_t size = major_count * minor_count;
  for (uint32_t major = 0; major < major_count; ++major) {
    uint32_t index = (major >> 1) * major_mult + (major & 1);
    uint8_t a = 0, b = 0;
    for (uint32_t minor = 0; minor < minor_count; ++minor) {
      const uint8_t byte = source[index];
      index += minor_inc;
      if (index >= size)
        index -= size;
      a ^= byte;
      b ^= byte;
      a = t.forward[a];
    }
    a = t.backward[t.forward[a] ^ b];
    dest[major] = a;
    dest[major + major_count] = a ^ b;
  }
}

// P parity then Q, over the sector from its header on.
void Ecc(uint8_t* sector) {
  EccBlock(sector + 0x0C, 86, 24, 2, 86, sector + 0x81C);
  EccBlock(sector + 0x0C, 52, 43, 86, 88, sector + 0x8C8);
}

void PutEdc(uint8_t* at, uint32_t edc) {
  at[0] = static_cast<uint8_t>(edc);
  at[1] = static_cast<uint8_t>(edc >> 8);
  at[2] = static_cast<uint8_t>(edc >> 16);
  at[3] = static_cast<uint8_t>(edc >> 24);
}

// The file read forwards through a buffer, skipping what is not a record's header - one pass at
// the speed of the medium, however many records there are.
class Scanner {
 public:
  explicit Scanner(FILE* file) : file_(file), buffer_(1 << 20) {}
  bool Get(uint8_t* byte) {
    if (position_ == length_ && !Fill())
      return false;
    *byte = buffer_[position_++];
    ++offset_;
    return true;
  }
  bool Skip(uint64_t bytes) {
    if (bytes <= length_ - position_) {
      position_ += static_cast<size_t>(bytes);
      offset_ += bytes;
      return true;
    }
    offset_ += bytes;
    position_ = length_ = 0;
    return _fseeki64(file_, static_cast<long long>(offset_), SEEK_SET) == 0;
  }
  uint64_t offset() const { return offset_; }

 private:
  bool Fill() {
    length_ = fread(buffer_.data(), 1, buffer_.size(), file_);
    position_ = 0;
    return length_ > 0;
  }
  FILE* file_;
  std::vector<uint8_t> buffer_;
  size_t position_ = 0, length_ = 0;
  uint64_t offset_ = 0;
};

}  // namespace

EcmImage::~EcmImage() {
  if (file_ != nullptr)
    fclose(file_);
}

uint32_t EcmImage::StoredBytes(int type) {
  return type == 1 ? 0x803 : (type == 2 ? 0x804 : 0x918);   // address + data, subheader + data
}

uint32_t EcmImage::ImageBytes(int type) {
  return type == 1 ? 2352 : 2336;   // Mode 2 sectors' sync and header are literals of their own
}

uint32_t EcmImage::Edc(uint32_t edc, const uint8_t* data, size_t bytes) {
  const Tables& t = T();
  for (size_t i = 0; i < bytes; ++i)
    edc = (edc >> 8) ^ t.edc[(edc ^ data[i]) & 0xFF];
  return edc;
}

void EcmImage::Reconstruct(uint8_t* s, int type) {
  if (type == 1) {
    s[0] = 0x00;
    memset(s + 1, 0xFF, 10);
    s[11] = 0x00;
    s[0x0F] = 0x01;
    PutEdc(s + 0x810, Edc(0, s, 0x810));
    memset(s + 0x814, 0, 8);
    Ecc(s);
  } else if (type == 2) {
    memcpy(s + 0x10, s + 0x14, 4);   // the subheader is stored twice
    PutEdc(s + 0x818, Edc(0, s + 0x10, 0x808));
    // Form 1's parity leaves the header out: worked out as if it were zero.
    uint8_t header[4];
    memcpy(header, s + 0x0C, 4);
    memset(s + 0x0C, 0, 4);
    Ecc(s);
    memcpy(s + 0x0C, header, 4);
  } else if (type == 3) {
    memcpy(s + 0x10, s + 0x14, 4);
    PutEdc(s + 0x92C, Edc(0, s + 0x10, 0x91C));
  }
}

bool EcmImage::Open(const std::string& path, std::string* error) {
  file_ = fopen(path.c_str(), "rb");
  if (file_ == nullptr) {
    *error = "cannot open " + path;
    return false;
  }
  Scanner scan(file_);
  uint8_t magic[4];
  for (uint8_t& byte : magic)
    if (!scan.Get(&byte)) {
      *error = "not an ECM image (too short)";
      return false;
    }
  if (memcmp(magic, "ECM\0", 4) != 0) {
    *error = "not an ECM image";
    return false;
  }

  // Each record: a type in the low two bits of the first byte, and a count below it carried on
  // in 7-bit groups while the top bit is set. A count of FFFFFFFFh ends them.
  for (;;) {
    uint8_t c;
    if (!scan.Get(&c)) {
      *error = "the ECM image is cut short";
      return false;
    }
    const int type = c & 3;
    uint32_t count = (c >> 2) & 0x1F;
    int bits = 5;
    while ((c & 0x80) != 0) {
      if (!scan.Get(&c) || bits > 26) {
        *error = "a corrupt record in the ECM image";
        return false;
      }
      count |= static_cast<uint32_t>(c & 0x7F) << bits;
      bits += 7;
    }
    if (count == 0xFFFFFFFFu)
      break;
    ++count;
    if (count >= 0x80000000u) {
      *error = "a corrupt record in the ECM image";
      return false;
    }
    Chunk chunk;
    chunk.image = size_;
    chunk.file = scan.offset();
    chunk.count = count;
    chunk.type = static_cast<uint8_t>(type);
    chunks_.push_back(chunk);
    const uint64_t stored = type == 0 ? count : static_cast<uint64_t>(count) * StoredBytes(type);
    size_ += type == 0 ? count : static_cast<uint64_t>(count) * ImageBytes(type);
    if (!scan.Skip(stored)) {
      *error = "the ECM image is cut short";
      return false;
    }
  }
  uint8_t edc[4];
  for (uint8_t& byte : edc)
    if (!scan.Get(&byte)) {
      *error = "the ECM image is cut short";
      return false;
    }
  stored_edc_ = edc[0] | (edc[1] << 8) | (edc[2] << 16) | (static_cast<uint32_t>(edc[3]) << 24);
  if (size_ == 0) {
    *error = "an empty ECM image";
    return false;
  }
  return true;
}

size_t EcmImage::Read(uint64_t offset, uint8_t* out, size_t bytes) {
  if (file_ == nullptr || offset >= size_)
    return 0;
  if (bytes > size_ - offset)
    bytes = static_cast<size_t>(size_ - offset);
  if (bytes == 0)
    return 0;
  // The record holding `offset`: the last to begin at or before it.
  size_t first_chunk = static_cast<size_t>(
      std::upper_bound(chunks_.begin(), chunks_.end(), offset,
                       [](uint64_t value, const Chunk& c) { return value < c.image; }) -
      chunks_.begin());
  if (first_chunk == 0)
    return 0;
  --first_chunk;

  // What of the file the request needs, from each record it touches: the literal bytes
  // themselves, or the whole of every sector it reaches into.
  struct Piece {
    size_t chunk;
    uint64_t from, to;        // of the image, within the record
    uint64_t file_from, file_to;
  };
  auto piece_of = [&](size_t index, uint64_t from, uint64_t to) {
    const Chunk& c = chunks_[index];
    Piece p = { index, from, to, 0, 0 };
    if (c.type == 0) {
      p.file_from = c.file + (from - c.image);
      p.file_to = c.file + (to - c.image);
    } else {
      const uint32_t image_bytes = ImageBytes(c.type);
      p.file_from = c.file + (from - c.image) / image_bytes * StoredBytes(c.type);
      p.file_to = c.file + (to - c.image + image_bytes - 1) / image_bytes * StoredBytes(c.type);
    }
    return p;
  };
  const uint64_t end = offset + bytes;
  uint64_t file_from = 0, file_to = 0;
  size_t last_chunk = first_chunk;
  for (size_t i = first_chunk; i < chunks_.size() && chunks_[i].image < end; ++i) {
    const uint64_t chunk_end = i + 1 < chunks_.size() ? chunks_[i + 1].image : size_;
    const Piece p = piece_of(i, std::max(offset, chunks_[i].image), std::min(end, chunk_end));
    if (i == first_chunk)
      file_from = p.file_from;
    file_to = p.file_to;
    last_chunk = i;
  }

  // One read for all of it - the records sit one after another in the file, with only their few
  // header bytes between - and then the sectors rebuilt from memory.
  stored_.resize(static_cast<size_t>(file_to - file_from));
  if (_fseeki64(file_, static_cast<long long>(file_from), SEEK_SET) != 0 ||
      fread(stored_.data(), 1, stored_.size(), file_) != stored_.size())
    return 0;

  size_t produced = 0;
  uint8_t sector[2352];
  for (size_t i = first_chunk; i <= last_chunk; ++i) {
    const Chunk& c = chunks_[i];
    const uint64_t chunk_end = i + 1 < chunks_.size() ? chunks_[i + 1].image : size_;
    const Piece p = piece_of(i, std::max(offset, c.image), std::min(end, chunk_end));
    const uint8_t* in = &stored_[static_cast<size_t>(p.file_from - file_from)];
    if (c.type == 0) {
      memcpy(out + produced, in, static_cast<size_t>(p.to - p.from));
      produced += static_cast<size_t>(p.to - p.from);
      continue;
    }
    const uint32_t image_bytes = ImageBytes(c.type);
    const uint32_t stored_bytes = StoredBytes(c.type);
    for (uint64_t at = p.from; at < p.to;) {
      const uint64_t s = (at - c.image) / image_bytes;
      const uint8_t* source = in + static_cast<size_t>(s - (p.from - c.image) / image_bytes) * stored_bytes;
      memset(sector, 0, sizeof(sector));
      if (c.type == 1) {
        memcpy(sector + 0x0C, source, 3);
        memcpy(sector + 0x10, source + 3, 0x800);
      } else {
        memcpy(sector + 0x14, source, stored_bytes);
      }
      Reconstruct(sector, c.type);
      const uint8_t* image = c.type == 1 ? sector : sector + 0x10;
      const size_t within = static_cast<size_t>((at - c.image) % image_bytes);
      const size_t n = static_cast<size_t>(std::min<uint64_t>(p.to - at, image_bytes - within));
      memcpy(out + produced, image + within, n);
      produced += n;
      at += n;
    }
  }
  return produced;
}

bool EcmImage::Verify() {
  std::vector<uint8_t> block(1 << 20);
  uint32_t edc = 0;
  for (uint64_t at = 0; at < size_;) {
    const size_t want = static_cast<size_t>(std::min<uint64_t>(block.size(), size_ - at));
    const size_t got = Read(at, block.data(), want);
    if (got != want)
      return false;
    edc = Edc(edc, block.data(), got);
    at += got;
  }
  return edc == stored_edc_;
}

}  // namespace psx
}  // namespace emulation
