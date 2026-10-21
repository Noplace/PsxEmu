#pragma once

// Where a frame's time goes, for NVIDIA Reflex - which DLSS Frame Generation needs
// (Docs/DLSS-Plan.md, phase 5). The front end gives the machine one of these while Frame
// Generation runs, and nothing otherwise; nothing here knows about Reflex itself.
//
// A Reflex frame is a new picture, not a vblank: a game drawing at 30 frames a second has two
// vblanks to a picture, and only new pictures are presented. So the frames are numbered by
// SharedPicture::picture - the simulation of picture n starts at the first vblank after picture
// n - 1 was drawn, and ends at the vblank that resolves picture n.

#include <cstdint>

namespace emulation {
namespace host {

class LatencyMarkers {
 public:
  enum class Marker { kSimulationStart, kSimulationEnd, kInputSample };

  virtual ~LatencyMarkers() = default;

  // At the start of picture `picture`'s simulation, before its input is read: Reflex may hold
  // the machine here, a little, so the picture is not drawn sooner than the card can show it.
  virtual void Sleep(uint32_t picture) = 0;
  // A point in picture `picture`'s frame. The machine's thread; cheap.
  virtual void Mark(Marker marker, uint32_t picture) = 0;
};

}  // namespace host
}  // namespace emulation
