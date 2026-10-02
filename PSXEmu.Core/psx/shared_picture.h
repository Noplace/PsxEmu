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

  // Whether the textures are Direct3D 12 resources rather than Direct3D 11 textures: the
  // hardware rasteriser drew them with Direct3D 12. Direct3D 11 and 12 open either the same
  // way; Vulkan and OpenGL import the two by different handle types.
  virtual bool d3d12() const { return false; }

  // The renderer's own Direct3D 12 device (an ID3D12Device), when the rasteriser draws on it
  // too, or null. Then a picture's `texture` and `planes` are the resources themselves rather
  // than handles, made for nothing but that device: only a renderer on it can show them, and
  // it waits for one on the card - its queue waiting for `device_fence()` (an ID3D12Fence) to
  // reach the picture's serial - rather than in WaitReady.
  virtual void* device() const { return nullptr; }
  virtual void* device_fence() const { return nullptr; }

 private:
  std::atomic<uint64_t> released_{0};
};

// One picture. Plain values, copied freely; `source` keeps the handles valid.
struct SharedPicture {
  std::shared_ptr<SharedPictureSource> source;   // null: there is no picture on the card
  // The texture, by an NT handle any Direct3D 11 or 12 device on the adapter can open: 32-bit
  // B8G8R8A8, exactly width x height, alpha opaque - the presenters' own 0xFFRRGGBB words. On
  // the renderer's own device (SharedPictureSource::device) the ID3D12Resource itself, left
  // readable by any shader (D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE); the source keeps it.
  void* texture = nullptr;
  // Different for every texture the source ever makes, so a presenter can keep what it opened.
  uint64_t texture_id = 0;
  // A Direct3D 12 texture's allocation on the card, in bytes, which OpenGL has to be told to
  // import one (SharedPictureSource::d3d12); 0 for a Direct3D 11 texture.
  uint64_t texture_bytes = 0;
  uint64_t serial = 0;   // which picture this is, counting up
  int width = 0;
  int height = 0;
  // The plane beside the picture, when the rasteriser keeps one: the same display area of it,
  // width x height, RGBA16F as above, by an NT handle like `texture`'s - or the resource, as
  // `texture` is - and null when it keeps none. Drawn with the picture, so the same fence says
  // both are ready.
  void* planes = nullptr;
  uint64_t planes_id = 0;
  // Whether this is a new picture, not the last one shown again - a game at 30 frames a second
  // shows each twice - and whether it has nothing to do with the one before: a cut, a film, the
  // display changed. What DLSS needs to know of the picture as a whole (Gpu::NewPicture).
  bool new_picture = false;
  bool reset = false;
  // Which new picture this is, counting from 1 while the plane is kept, 0 otherwise - and the
  // same in every repeat of it. A presenter that takes only new pictures goes by this rather
  // than `new_picture`: a new picture replaced in the mailbox by its own repeat is still new to
  // it, and a number skipped is a picture it never saw. It is also the picture's frame for
  // NVIDIA Reflex (host/latency_markers.h).
  uint32_t picture = 0;
  // 480 lines interlaced, drawn a field at a time and not filled: half of each picture is the
  // last field's lines, which no motion describes (Gpu::MixesFields). DLSS leaves such
  // pictures as they are.
  bool interlaced = false;
  // Filled (RasterBackend::FillsSkippedFields): the console's own sample of every pixel on the
  // rows a field-at-a-time draw left alone still holds the last field, as native VRAM does -
  // the top-left sub-pixel of each `scale` x `scale` block on the console rows, counted from
  // the picture's top, whose parity is `filled_rows` - 1. The picture shows each made again
  // from this frame's sub-pixels around it; the plane beside it does not, and whatever reads
  // the plane takes the sub-pixel just below instead. 0 when there are none.
  int filled_rows = 0;
  int scale = 1;
  // Where the picture's triangles were sampled within each of its pixels, when jittered for DLSS
  // (Docs/DLSS-Plan.md, phase 3): in its own pixels, each within [-0.5, 0.5). A pixel shows what
  // is at itself plus this - the sample point moved, not the picture. 0 when not jittered.
  float jitter_x = 0.0f;
  float jitter_y = 0.0f;

  explicit operator bool() const { return source != nullptr; }
};

}  // namespace psx
}  // namespace emulation
