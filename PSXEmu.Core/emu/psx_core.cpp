#include "emu/psx_core.h"

#include "psx/psx.h"
#include "render/texture_pack.h"
#include "render/texture_tracker.h"

#include <cstdio>
#include <utility>

namespace emulation {
namespace psx {

namespace {

// Run-ahead's kept state, or rewind's: System's own, which keeps the memory cards beside the
// payload for run-ahead.
class PsxMemoryState : public MemoryState {
 public:
  std::vector<uint8_t>* payload() override { return &state.payload; }
  System::MemoryState state;
};

const System::MemoryState& Of(const MemoryState& state) {
  return static_cast<const PsxMemoryState&>(state).state;
}
System::MemoryState* Of(MemoryState* state) {
  return &static_cast<PsxMemoryState*>(state)->state;
}

}  // namespace

static_assert(host::MachineConfig::kMaxRunAhead == EmuConfig::kMaxRunAhead,
              "the machine holds run-ahead to what the settings offer");

PsxCore::PsxCore(System* system) : system_(system) {
  // What is shown is picture() (FillFrame); the native framebuffer beside a sharper one is
  // for measuring, and costs a wait for the hardware rasteriser every frame.
  system_->gpu().set_native_picture(false);
}

PsxCore::~PsxCore() {
  // The GPU outlives this: nothing of the machine's may be left watching it.
  SetTextures(std::string(), std::string(), 0, std::string(), nullptr);
}

// Runs the machine until the GPU says a frame is finished - the same loop boot_runner runs,
// which is what makes the two comparable.
bool PsxCore::RunFrame(uint64_t* steps, uint64_t max_steps) {
  const uint64_t target = system_->gpu().frame_count() + 1;
  // Nothing arms the debugger in the middle of a frame - the window's requests are drained
  // between frames - so an unarmed one is asked once here, not once an instruction.
  if (!system_->debugger().armed()) {
    uint64_t n = *steps;
    while (system_->gpu().frame_count() < target && n < max_steps) {
      system_->StepInstructionUnarmed();
      ++n;
    }
    *steps = n;
    return true;
  }
  while (system_->gpu().frame_count() < target && *steps < max_steps) {
    // A halt ran nothing, so it is not an instruction; the frame carries on from here when the
    // debugger lets it, towards the same frame boundary.
    if (!system_->StepInstruction())
      return false;
    ++*steps;
  }
  return true;
}

bool PsxCore::debugger_armed() const { return system_->debugger().armed(); }

bool PsxCore::halted() const { return system_->debugger().halted(); }

// Interpreted, even one frame ahead does not fit in a frame (Docs/Rewind-and-Run-Ahead-Plan.md:
// Ridge Racer's frame alone was 24 ms) - and with PGXP on the interpreter is what runs.
bool PsxCore::RunAheadFits() const { return system_->recompiler_enabled(); }

double PsxCore::refresh_hz() const { return system_->gpu().refresh_hz(); }

uint32_t PsxCore::picture_number() const { return system_->gpu().picture_number(); }

// The frame is resolved at the start of vblank, which is exactly when RunFrame returns - so this
// copies a finished picture, never a half-drawn one. About a tenth of a millisecond for 640x480.
void PsxCore::FillFrame(host::VideoFrame* frame, bool raw_view) {
  using host::VideoFrame;
  if (raw_view) {
    // View > VRAM: all of it, as the GPU holds it.
    const int width = GpuCore::kVramWidth;
    const int height = GpuCore::kVramHeight;
    const uint16_t* vram = system_->gpu().vram();
    frame->raw.assign(vram, vram + static_cast<size_t>(width) * height);
    frame->raw_format = VideoFrame::RawFormat::kBgr555;
    frame->shared = image::SharedPicture();
    frame->width = width;
    frame->height = height;
  } else if (const image::SharedPicture& shared = system_->gpu().shared_picture()) {
    // Left on the graphics card by the hardware rasteriser: nothing to copy.
    frame->pixels.clear();
    frame->shared = shared;
    frame->raw_format = VideoFrame::RawFormat::kNone;
    frame->width = shared.width;
    frame->height = shared.height;
  } else {
    int width = 0;
    int height = 0;
    // At the hardware rasteriser's resolution, when that is higher than native.
    const uint32_t* pixels = system_->gpu().picture(width, height);
    frame->pixels.assign(pixels, pixels + static_cast<size_t>(width) * height);
    frame->shared = image::SharedPicture();
    frame->raw_format = VideoFrame::RawFormat::kNone;
    frame->width = width;
    frame->height = height;
  }
}

int PsxCore::sample_rate() const { return Spu::kSampleRate; }

int PsxCore::ReadSamples(int16_t* stereo, int max_frames) {
  return system_->spu().ReadSamples(stereo, max_frames);
}

void PsxCore::FillReport(host::MachineReport* report) const {
  report->hardware_raster = system_->gpu().hardware_raster();
  report->raster_error = system_->gpu().raster_error();
  if (texture_tracker_) {
    // Read while the rasteriser may be drawing: only what may be.
    report->textures = true;
    report->textures_named = texture_tracker_->named_so_far();
    report->textures_dumped = texture_tracker_->written_so_far();
    if (texture_pack_) {
      const TexturePack::Counts pack = texture_pack_->counts();
      report->replacements = pack.files;
      report->jobs_replaced = pack.jobs_replaced;
    }
    report->replacement_error = replacement_error_;
  }
}

std::unique_ptr<MemoryState> PsxCore::NewMemoryState() {
  return std::make_unique<PsxMemoryState>();
}

// The machine is kept as it stands - cards and all, since a save the frames ahead write was made
// on input the player has not given yet.
void PsxCore::SaveBeforeRunAhead(MemoryState* state) {
  system_->SaveMemoryState(Of(state), /*with_cards=*/true);
  // The save waited for the rasteriser. The texture tracker keeps its records for the load back,
  // so replacements carry on rather than starting over every frame.
  if (texture_tracker_)
    texture_tracker_->Hold();
}

// Only a disc that has gone could fail this, and then the machine is wherever the load stopped.
bool PsxCore::LoadAfterRunAhead(const MemoryState& state) {
  const bool loaded = system_->LoadMemoryState(Of(state), /*continuing=*/true).empty();
  if (texture_tracker_)
    texture_tracker_->Release();
  return loaded;
}

// Without the cards: rewind leaves them as they are.
void PsxCore::SaveForRewind(MemoryState* state) {
  system_->SaveMemoryState(Of(state), /*with_cards=*/false);
}

bool PsxCore::LoadForRewind(const MemoryState& state) {
  return system_->LoadMemoryState(Of(state)).empty();
}

// Memory cards go to disk a second after the game stops writing them (psx/mc.h).
void PsxCore::OnFrameEnd() {
  for (int port = 0; port < 2; ++port)
    for (int slot = 0; slot < System::kCardSlotsPerPort; ++slot)
      system_->mc(port, slot).OnFrame();
}

// A save made just before pausing is on disk now, not a second of running later.
void PsxCore::FlushSaves() {
  for (int port = 0; port < 2; ++port)
    for (int slot = 0; slot < System::kCardSlotsPerPort; ++slot)
      system_->mc(port, slot).Flush();
}

void PsxCore::ApplyConfig(const EmuConfig& config) {
  EmuConfig& current = system_->config();
  const bool rasteriser_changed = current.gpu_rasteriser != config.gpu_rasteriser ||
                                  current.resolution_scale != config.resolution_scale ||
                                  current.true_color != config.true_color ||
                                  current.texture_budget_mb != config.texture_budget_mb ||
                                  current.texture_filter != config.texture_filter;
  current = config;
  if (rasteriser_changed)
    system_->gpu().ChooseRasteriser();
}

void PsxCore::set_plane_view(PlaneView view) {
  plane_view_ = view;
  system_->gpu().SetPlanes(dlss_, view);
}

void PsxCore::set_dlss(bool on, int jitter_phases) {
  dlss_ = on;
  system_->gpu().SetPlanes(on, plane_view_);
  system_->gpu().SetJitter(on ? jitter_phases : 0);
}

void PsxCore::SetTextures(const std::string& dump, const std::string& replace,
                          uint64_t memory_budget, const std::string& ai_folder,
                          std::shared_ptr<TextureUpscaleQueue> ai) {
  Gpu& gpu = system_->gpu();
  if (ai_folder.empty())
    ai.reset();
  // An upscaler going, or another coming: the old one told of nothing more, and given no pack.
  if (ai != texture_ai_) {
    if (texture_tracker_) {
      gpu.set_raster_observer(texture_tracker_.get());   // waits for the rasteriser
      texture_tracker_->set_upscale_queue(nullptr);
    }
    if (texture_ai_)
      texture_ai_->set_pack(nullptr);
    texture_ai_ = ai;
  }
  if (dump.empty() && replace.empty() && !texture_ai_) {
    gpu.set_texture_replacer(nullptr);
    gpu.set_raster_observer(nullptr);   // waits for the rasteriser
    texture_pack_.reset();
    if (texture_tracker_)
      texture_tracker_->Flush();   // what it was gathering written first
    texture_tracker_.reset();
    texture_dump_.clear();
    texture_replace_.clear();
    texture_ai_folder_.clear();
    replacement_error_.clear();
    return;
  }
  if (!texture_tracker_) {
    // Records start empty: what the game uploaded before is not known, and is named once it
    // uploads it again - the next level, the next scene.
    texture_tracker_ = std::make_unique<TextureTracker>(gpu.vram());
    gpu.set_raster_observer(texture_tracker_.get());
  }
  if (dump != texture_dump_) {
    texture_tracker_->Flush();   // waits for the rasteriser; what was gathered written where it was going
    texture_tracker_->set_dump_folder(dump, 8, true);
    texture_dump_ = dump;
  }
  // The pack: the game's own replacements, the upscaler's, or both.
  if (replace.empty() && !texture_ai_) {
    gpu.set_texture_replacer(nullptr);
    texture_pack_.reset();
    texture_replace_.clear();
    texture_ai_folder_.clear();
    replacement_error_.clear();
    return;
  }
  if (!texture_pack_) {
    texture_pack_ = std::make_unique<TexturePack>(texture_tracker_.get());
    texture_replace_.clear();
    texture_ai_folder_.clear();
    pack_opened_ = false;
  }
  texture_pack_->set_memory_budget(memory_budget);
  const std::string upscaled = texture_ai_ ? ai_folder : std::string();
  if (!pack_opened_ || replace != texture_replace_ || upscaled != texture_ai_folder_) {
    texture_replace_ = replace;
    texture_ai_folder_ = upscaled;
    pack_opened_ = true;
    ReloadTextures();
  }
  if (texture_ai_) {
    gpu.set_raster_observer(texture_tracker_.get());   // waits for the rasteriser
    texture_ai_->set_pack(texture_pack_.get());
    texture_tracker_->set_upscale_queue(texture_ai_.get());
  }
}

void PsxCore::ReloadTextures() {
  if (!texture_pack_)
    return;
  Gpu& gpu = system_->gpu();
  gpu.set_texture_replacer(nullptr);   // waits for the rasteriser
  if (texture_pack_->Open(texture_replace_, &replacement_error_, texture_ai_folder_)) {
    replacement_error_.clear();
    gpu.set_texture_replacer(texture_pack_.get());
  } else {
    texture_pack_->Close();
  }
}

void PsxCore::SaveTextureUploads(const std::string& path) const {
  // The save has just waited for the rasteriser; nothing is being drawn.
  if (!texture_tracker_ || !TextureTracker::WriteUploads(path, texture_tracker_->LiveUploads()))
    std::remove(path.c_str());   // an older one would say nothing true of this state
}

void PsxCore::LoadTextureUploads(const std::string& path) {
  // Without the file - or with an empty path, after a load that failed - the next load brings
  // back what this run saw.
  std::vector<TextureTracker::Upload> uploads;
  if (!texture_tracker_)
    return;
  texture_tracker_->ForgetNextLoad();
  if (!path.empty() && TextureTracker::ReadUploads(path, &uploads))
    texture_tracker_->NextLoad(std::move(uploads));
}

host::MachineConfig MachineConfigFrom(const EmuConfig& config) {
  host::MachineConfig machine;
  machine.emulation_speed = config.emulation_speed;
  machine.frame_limiter = config.frame_limiter;
  machine.frame_pacing = config.frame_pacing;
  machine.run_ahead = config.run_ahead;
  machine.rewind_interval = config.rewind_interval;
  machine.rewind_budget_mb = config.rewind_budget_mb;
  return machine;
}

void ApplyConfig(host::Machine& machine, const EmuConfig& config) {
  CoreOf(machine).ApplyConfig(config);
  machine.ApplyConfig(MachineConfigFrom(config));
}

}  // namespace psx
}  // namespace emulation
