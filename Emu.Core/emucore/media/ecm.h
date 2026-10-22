#pragma once

// An ECM image: a disc image with the parts of each sector that can be worked out from the rest -
// sync, header, EDC and ECC - left out, the format of Neill Corlett's ECM tool. Reading one puts
// them back.
//
// The file is a run of records, each some literal bytes or some sectors of one kind (Mode 1,
// Mode 2 Form 1, Mode 2 Form 2), ending in the EDC of the whole image. A raw PlayStation image
// becomes, sector after sector, a 16-byte literal (a Mode 2 sector's sync and header) and a Mode 2
// sector without them, so there is no stride to seek by: Open walks the records once and keeps
// where each begins in the image and in the file, and Read decodes only what is asked for.
//
// Written from the format as ECMA-130 defines the sectors (the EDC polynomial, the P and Q
// Reed-Solomon parity); it shares no code with the original tool.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace emulation {
namespace media {

class EcmImage {
 public:
  EcmImage() = default;
  ~EcmImage();
  EcmImage(const EcmImage&) = delete;
  EcmImage& operator=(const EcmImage&) = delete;

  // Indexes the file. False, with the reason, if it is not an ECM image or is cut short.
  bool Open(const std::string& path, std::string* error);

  // The size of the image it holds.
  uint64_t size() const { return size_; }

  // Bytes of the image from `offset`: how many were produced, fewer only at its end or on a read
  // error.
  size_t Read(uint64_t offset, uint8_t* out, size_t bytes);

  // The EDC the file ends with, and the EDC of everything Read would give: equal for an intact
  // image. Decodes all of it - for a test, not for opening.
  uint32_t stored_edc() const { return stored_edc_; }
  bool Verify();

  // Puts back the sync, header, EDC and ECC of a sector whose user data is in place - exposed
  // for the tests, which build sectors with it. `type` 1 is Mode 1 (all 2352 bytes, the address
  // already at 0Ch); 2 and 3 are Mode 2 Form 1 and Form 2, their subheader at 14h.
  static void Reconstruct(uint8_t* sector, int type);
  static uint32_t Edc(uint32_t edc, const uint8_t* data, size_t bytes);

 private:
  struct Chunk {
    uint64_t image;   // where its bytes begin in the image
    uint64_t file;    // and in the file
    uint32_t count;   // bytes for a literal, sectors otherwise
    uint8_t type;     // 0 literal, 1 Mode 1, 2 Mode 2 Form 1, 3 Mode 2 Form 2
  };
  static uint32_t StoredBytes(int type);   // what one sector of `type` takes in the file
  static uint32_t ImageBytes(int type);    // and in the image

  FILE* file_ = nullptr;
  std::vector<Chunk> chunks_;
  uint64_t size_ = 0;
  uint32_t stored_edc_ = 0;
  std::vector<uint8_t> stored_;   // a request's sectors as the file has them
};

}  // namespace media
}  // namespace emulation
