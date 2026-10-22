// The PlayStation as host::Machine runs it (Emu.Core: emucore/core.h; Docs/Shared-Libraries-Plan.md,
// phase 4): every call the machine's thread makes into a System, and what the PSX front end asks
// of the machine beyond what any console has - the rasteriser's settings, texture dumping and
// replacement, DLSS's planes. Those it asks of this directly, in a request (Machine::Post), on
// the machine's thread.
//
// It adds nothing to the System: the frame is run as boot_runner runs it, instruction by
// instruction, so a machine run through this lands where boot_runner does (host_test).
#pragma once

#include "emucore/core.h"
#include "host/machine.h"
#include "render/raster.h"

#include <cstdint>
#include <memory>
#include <string>

namespace emulation {
namespace psx {

class System;
struct EmuConfig;
class TextureTracker;
class TexturePack;
class TextureUpscaleQueue;

class PsxCore : public ICore {
 public:
  // `system` outlives this, and nothing else calls into it while a machine runs it.
  explicit PsxCore(System* system);
  ~PsxCore() override;
  PsxCore(const PsxCore&) = delete;
  PsxCore& operator=(const PsxCore&) = delete;

  System& system() { return *system_; }

  // ---- ICore ---------------------------------------------------------------------------------
  bool RunFrame(uint64_t* steps, uint64_t max_steps) override;
  bool debugger_armed() const override;
  bool halted() const override;
  bool RunAheadFits() const override;
  double refresh_hz() const override;
  uint32_t picture_number() const override;
  void FillFrame(host::VideoFrame* frame, bool raw_view) override;
  int sample_rate() const override;
  int ReadSamples(int16_t* stereo, int max_frames) override;
  void FillReport(host::MachineReport* report) const override;
  std::unique_ptr<MemoryState> NewMemoryState() override;
  void SaveBeforeRunAhead(MemoryState* state) override;
  bool LoadAfterRunAhead(const MemoryState& state) override;
  void SaveForRewind(MemoryState* state) override;
  bool LoadForRewind(const MemoryState& state) override;
  void OnFrameEnd() override;
  void FlushSaves() override;

  // ---- The PSX front end's, in a request -----------------------------------------------------

  // Replaces the console's settings with the front end's. A change of rasteriser, its scale,
  // its colour, or its textures' budget or filter makes the rasteriser again. What the machine
  // itself paces and keeps by (MachineConfigFrom) goes to Machine::ApplyConfig beside this.
  void ApplyConfig(const EmuConfig& config);

  // View > Depth and Motion: the plane beside VRAM that DLSS will use, shown in place of the
  // picture (Docs/DLSS-Plan.md) - by the hardware rasteriser, above 1x.
  void set_plane_view(PlaneView view);
  // DLSS running in the renderer (Docs/DLSS-Plan.md, phase 4): the plane kept beside VRAM, with
  // motion, for every picture, and the triangles jittered through `jitter_phases` offsets.
  // Nothing the machine sees changes. Off: neither.
  void set_dlss(bool on, int jitter_phases);

  // Textures (Docs/Texture-Upscaling-Plan.md, phases 2 and 3): every texture named as it is drawn,
  // written into `dump` - in the background - and drawn from the pack in `replace` by the hardware
  // rasteriser; and upscaled by `ai` (render/texture_upscale.h), its images kept in `ai_folder`
  // and drawn under the pack's own. Any empty or null for not; all of them, and nothing watches
  // the drawing at all. Kept across boots and changes of rasteriser; a folder that is the same as
  // before is left as it is.
  void SetTextures(const std::string& dump, const std::string& replace, uint64_t memory_budget,
                   const std::string& ai_folder, std::shared_ptr<TextureUpscaleQueue> ai);
  // The pack read again from its folder: what changed on disk drawn from the next job on.
  void ReloadTextures();
  // A save state's texture uploads (render/texture_tracker.h): which uploads are live, written
  // to `path` as the state is saved - and the file taken away without textures on - and read
  // back before it is loaded, so the textures it holds are named, and replaced, from its first
  // frame. Without the file - a state from before this, or an empty path, which drops one read
  // for a load that then failed - the load brings back the uploads this run saw.
  void SaveTextureUploads(const std::string& path) const;
  void LoadTextureUploads(const std::string& path);

 private:
  System* system_;

  PlaneView plane_view_ = PlaneView::kPicture;
  bool dlss_ = false;
  // SetTextures': the tracker naming each job and dumping, and the pack it names them for, with
  // the folders they were given. The pack is dropped before the tracker it reads.
  std::unique_ptr<TextureTracker> texture_tracker_;
  std::unique_ptr<TexturePack> texture_pack_;
  std::string texture_dump_, texture_replace_;
  std::string replacement_error_;
  // ...and the upscaler, shared with the front end, which reads how far it has got.
  std::shared_ptr<TextureUpscaleQueue> texture_ai_;
  std::string texture_ai_folder_;
  bool pack_opened_ = false;
};

// What the machine paces and keeps by, from the console's settings: the speed, the limiter and
// Frame Pacing, run-ahead and rewind.
host::MachineConfig MachineConfigFrom(const EmuConfig& config);

// The PSX front end's machine, and its tools', always runs a PsxCore: what they ask of the
// console, in a request, they ask through these.
inline PsxCore& CoreOf(host::Machine& machine) { return static_cast<PsxCore&>(machine.core()); }
inline System& SystemOf(host::Machine& machine) { return CoreOf(machine).system(); }

// Both halves of the front end's settings, on the machine's thread: the console's
// (PsxCore::ApplyConfig), then the machine's own (Machine::ApplyConfig).
void ApplyConfig(host::Machine& machine, const EmuConfig& config);

}  // namespace psx
}  // namespace emulation
