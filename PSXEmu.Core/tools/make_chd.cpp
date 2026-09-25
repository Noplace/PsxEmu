// make_chd - converts any disc image PSXEmu can mount into a CHD.
//
//   make_chd <image> <out.chd> [best|none|lzma|zlib|flac]
//
// Reads through psx::Disc, so a .cue, .mds, .ccd, .bin or .iso all work, and
// writes with tools/chd_writer.h - chdman's format and default codecs, see
// there. For testing CHD reading without chdman: mount the original and the
// CHD and compare them (media_test does, sector by sector), or boot both
// with boot_runner and compare the checksums.
//
// Every track goes in as the disc presents it: from its start to the next
// track's start, a pregap included, which reads back as the same disc. A data
// track after the first would come back two seconds later - a CHD data track
// without a pregap is read as having the standard one - so that is refused.

#include "psx/psx.h"
#include "tools/chd_writer.h"

#include <chrono>
#include <cstdio>
#include <cstring>

using emulation::psx::Disc;

int main(int argc, char** argv) {
  if (argc < 3) {
    printf("usage: make_chd <image> <out.chd> [best|none|lzma|zlib|flac]\n");
    return 2;
  }
  chd_writer::Codec codec = chd_writer::Codec::kBest;
  if (argc > 3) {
    const char* name = argv[3];
    if (strcmp(name, "none") == 0) codec = chd_writer::Codec::kNone;
    else if (strcmp(name, "lzma") == 0) codec = chd_writer::Codec::kLzma;
    else if (strcmp(name, "zlib") == 0) codec = chd_writer::Codec::kZlib;
    else if (strcmp(name, "flac") == 0) codec = chd_writer::Codec::kFlac;
    else if (strcmp(name, "best") != 0) {
      printf("unknown codec %s\n", name);
      return 2;
    }
  }

  Disc disc;
  if (!disc.Open(argv[1])) {
    printf("cannot mount %s\n", argv[1]);
    return 1;
  }
  std::vector<chd_writer::Track> tracks;
  for (int i = 0; i < disc.track_count(); ++i) {
    const Disc::Track& track = disc.track(i);
    if (i > 0 && track.type == Disc::kTrackData) {
      printf("track %d is a data track after the first; not supported here\n", track.number);
      return 1;
    }
    const uint32_t end = (i + 1 < disc.track_count()) ? disc.track(i + 1).start_lba
                                                     : disc.total_sectors();
    chd_writer::Track out;
    out.audio = (track.type == Disc::kTrackAudio);
    out.frames = end - track.start_lba;
    const uint32_t start = track.start_lba;
    out.read = [&disc, start](uint32_t index, uint8_t* sector) {
      return disc.ReadSector(start + index, sector);
    };
    tracks.push_back(out);
    printf("track %2d  %s  %u sectors from %u\n", track.number, out.audio ? "audio" : "data ",
           out.frames, track.start_lba);
  }

  const auto begin = std::chrono::steady_clock::now();
  chd_writer::Stats stats;
  const std::string error = chd_writer::Write(argv[2], tracks, codec, &stats);
  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
  if (!error.empty()) {
    printf("failed: %s\n", error.c_str());
    return 1;
  }
  printf("wrote %s: %llu bytes, %u hunks - %u lzma, %u zlib, %u flac, %u raw, %u repeats - in "
         "%.1f s\n",
         argv[2], static_cast<unsigned long long>(stats.bytes), stats.hunks, stats.lzma,
         stats.zlib, stats.flac, stats.raw, stats.repeated, seconds);
  return 0;
}
