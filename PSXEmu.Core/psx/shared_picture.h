// A picture left on the graphics card: what the hardware rasteriser hands the presenter instead
// of pixels, when the presenter can take it there (Docs/Hardware-Renderer-Plan.md, phase 6).
//
// The rasteriser draws the display area into a texture another device can open - Direct3D 11
// and 12 by its NT handle - and the presenter draws from that, so the picture never crosses to
// system memory and back. The core knows no graphics API: to it these are handles and numbers,
// carried from `Gpu` through `Machine` to the video thread in a `VideoFrame`.
//
// Each texture is drawn again once nobody reads it: the rasteriser keeps a few, and one comes
// back when the presenter's own card has finished with it (Release) or when the frame carrying
// it was never shown at all (Dropped).
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace emulation {
namespace psx {

// How many textures a source rotates through: the one on the screen, those still in a queue or the
// mailbox, and the presenter's release trailing a few frames behind. Five ran out often enough to
// show as slow frames - each a read-back - at 8x on an integrated card; eight almost never. A
// presenter keeps what it has opened by texture id, and its cache has to hold more than this or
// it closes and reopens a texture every frame - so it holds twice as many, which is also room for
// the last size's textures for a while after a resolution change.
inline constexpr int kSharedTextureCount = 8;
inline constexpr size_t kMaxOpenedPictures = 2 * kSharedTextureCount;

// The plane beside VRAM (Docs/DLSS-Plan.md): what DLSS needs to know about each pixel that the
// picture does not say. RGBA16F, one texel per sub-pixel, in VRAM's layout - a game draws its
// next frame into one buffer while showing another, and what goes with a picture has to be what
// was drawn into that buffer.
//
//   R, G  where it was in the last picture minus where it is now, in sub-pixels; both
//         kUnknownMotion where that is not known
//   B     kPlaneDepthScale over the depth PGXP projected it from, GTE units: nearer is larger,
//         and 0 is unknown, which reads as infinitely far
//   A     1 where the last thing drawn over it was opaque, 0 where it was translucent - which
//         leaves R, G and B as they were underneath
inline constexpr float kUnknownMotion = 32768.0f;
inline constexpr float kPlaneDepthScale = 256.0f;

// The rasteriser's side of its pictures: what a presenter waits on and gives back. It lives as
// long as any frame refers to it - past the rasteriser that made it, whose textures a frame
// still in flight can still be drawn from. Every call is safe from any thread.
class SharedPictureSource {
 public:
  virtual ~SharedPictureSource() = default;

  // Waits up to `timeout_ms` for picture `serial` to be drawn: the rasteriser hands a picture
  // over as soon as it has asked for it, not when its card has finished. False if it is not
  // drawn by then - the rasteriser's card lost, say - and the picture must not be shown.
  virtual bool WaitReady(uint64_t serial, uint32_t timeout_ms) = 0;

  // A frame carrying picture `serial` was replaced before the presenter took it, so nothing will
  // ever read it: its texture is free at once.
  virtual void Dropped(uint64_t serial) = 0;

  // The presenter's card has finished reading every picture before `serial`: it has moved on to
  // that one, and whatever it drew from the older ones is done. Never moves back.
  void Release(uint64_t serial) {
    uint64_t seen = released_.load(std::memory_order_relaxed);
    while (seen < serial &&
           !released_.compare_exchange_weak(seen, serial, std::memory_order_release,
                                            std::memory_order_relaxed)) {
    }
  }
  uint64_t released() const { return released_.load(std::memory_order_acquire); }

  // The graphics adapter the textures are on, by its LUID: a presenter can only open them on
  // the same one.
  virtual uint64_t adapter() const = 0;

 private:
  std::atomic<uint64_t> released_{0};
};

// One picture. Plain values, copied freely; `source` keeps the handles valid.
struct SharedPicture {
  std::shared_ptr<SharedPictureSource> source;   // null: there is no picture on the card
  // The texture, by an NT handle any Direct3D 11 or 12 device on the adapter can open: 32-bit
  // B8G8R8A8, exactly width x height, alpha opaque - the presenters' own 0xFFRRGGBB words.
  void* texture = nullptr;
  // Different for every texture the source ever makes, so a presenter can keep what it opened.
  uint64_t texture_id = 0;
  uint64_t serial = 0;   // which picture this is, counting up
  int width = 0;
  int height = 0;
  // The plane beside the picture, when the rasteriser keeps one: the same display area of it,
  // width x height, RGBA16F as above, by an NT handle like `texture`'s; null when it keeps none.
  // Drawn with the picture, so the same fence says both are ready.
  void* planes = nullptr;
  uint64_t planes_id = 0;

  explicit operator bool() const { return source != nullptr; }
};

}  // namespace psx
}  // namespace emulation
