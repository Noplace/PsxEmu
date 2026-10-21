# Shared libraries: Emu.Core and Emu.Frontend.Win32

> **Status: phase 1 done, 2026-10-21**: the Emu.Core project exists and holds the vendored libraries. The baseline of phase 0 is recorded at `5babf24`. Phase 2 is next. Every phase is a move, not a
> change: it ends with the same twelve-disc table, the same harness counts and the same pictures
> as before it, and a phase that changes a single checksum is wrong, however tidy it looks.

The goal: the parts of this emulator that are not a PlayStation, in two libraries a second
emulator can link (GBAEmu, GBEmu, Atari2600Emu, or a new one):

- **Emu.Core**, a static library with no UI framework and no graphics API. It holds the
  machine's thread and its channels, frame pacing, save-state and rewind plumbing, the settings
  file, disc images, PNGs, the x64 code emitter, and the vendored compression libraries.
- **Emu.Frontend.Win32**, a static library that holds what a Windows front end needs whatever it
  emulates. That is the four presenters, the picture filters, DLSS and FSR, the texture
  upscaler's backends, the audio devices, the host's pads and mouse, the overlay, and DPI.

The two are separate projects, and the dependencies only run one way:

```
PSXEmu.Win32 (exe)  ──>  Emu.Frontend.Win32 (lib)  ──>  Emu.Core (lib)
      │                                                      ^
      └──────────>  PSXEmu.Core (lib)  ──────────────────────┘
```

Emu.Core knows nothing about either PSX project or the front-end library. Emu.Frontend.Win32
knows Emu.Core and nothing about the PSX. PSXEmu.Core knows Emu.Core and nothing about any front
end. That is the rule of [Emulator-Project-Standards.md](Emulator-Project-Standards.md) section 1,
"the core never knows which front end is running", applied once more, one level down.

## The conclusion first

**Most of what moves is already generic. Only its namespace and folder say "psx".** In
PSXEmu.Core, `host/` (bar `machine`), `platform/`, `audio/`, `emu/rewind_buffer`, `StateIO`,
`SettingsFile`, `render/png_file`, `render/shared_picture` and the recompiler's back end
(`rec/emitter`, `x64_asm`, `x86_extras`, `block_cache`) include nothing from `psx/`. `media/disc`,
`ecm` and `iso9660` include `psx/psx.h` only to get `windows.h`. Moving these is about 8,000
lines of our own code, plus zlib, LZMA and libchdr, and changes no logic at all.

**The one real piece of work is `host::Machine`.** Run-ahead, rewind, debugger halts, pacing,
audio trim and the frame samples are all generic, but it holds a `psx::System*` and reaches
into it about 25 times (`gpu().frame_count`, `spu().ReadSamples`, `SaveMemoryState`,
`debugger().armed`, `mc(port, slot).OnFrame`, and so on). It also owns the PSX texture tracker
and pack, and DLSS's planes. Phase 4 puts an `ICore` interface between them. Everything else in
this plan is file moves.

**On the front-end side the presenters are nearly free already.** Their only PSX dependencies
are `psx::SharedPicture` and `SharedPictureSource`, which are generic types in the wrong
namespace. They also pick up `app/framework.h`, which includes the whole of `psx/psx.h`, and
`tools/letterbox.h`. Cut those three and the D3D11, D3D12, OpenGL and Vulkan engines, the
filters, DLSS and FSR move as they are. That is about 20,000 lines, not counting shaders and
third-party headers.

## Decisions (settled 2026-10-21, each as recommended)

1. **Where the libraries live.** In this repository first, as `PsxEmu/Emu.Core/`
   and `PsxEmu/Emu.Frontend.Win32/`, so every move is guarded by this emulator's gates in the
   same commit. When a second emulator takes them on, split them into a repository of their own
   (`C:\dev\Noplace\Emu`, history kept with `git subtree split`) and have both solutions use it
   as a submodule or a sibling path.
2. **The name `Emu.Core` is taken.** GBEmu has a small project of that name
   (`GBEmu/Solution/Emu.Core`: `Emulator`, `ring_buffer`, `timer`, `io`, `hash_table`). It is
   older than and smaller than what moves here. This one becomes Emu.Core, and GBEmu's
   is retired when GBEmu adopts it. What it has that is worth keeping (`hash_table`, `io`) is
   looked at then.
3. **Namespaces.** Keep `emulation::` as the root it already is, and give the
   generic code namespaces that are not a console's: `emulation::host`, `emulation::rec` (both
   unchanged), `emulation::media`, `emulation::state`, `emulation::image`, and
   `emulation::timing` for what is `utilities::` today. `emulation::psx` is then only the
   PlayStation. The front-end library takes `emulation::win32` in place of today's `psxemu::`.
   That keeps most of the existing code unchanged rather than renaming everything.
4. **Include roots.** Both libraries get one top-level folder under their project folder,
   `Emu.Core/emucore/...` and `Emu.Frontend.Win32/emuwin32/...`, so an include says which library
   it comes from: `#include "emucore/host/doorbell.h"`. Without that, the two roots would each
   have a `media/`, a `render/` and a `rec/`, and which header wins would depend on the order of
   the include path. Section 3 of the standards, "the core folder is an include root", holds for
   each project.

## What moves, and where

### Emu.Core

| Today (PSXEmu.Core) | Becomes | Phase | Note |
|---|---|---|---|
| `lib/zlib`, `lib/lzma`, `lib/libchdr`, `lib/zstd_stub` | `emucore/lib/...` | 1 | vendored, unchanged |
| `lib/reccore` | deleted | 1 | built by nothing since `rec/emitter.h` replaced it |
| `host/doorbell.h`, `request_queue.h`, `sample_ring.h`, `latency_markers.h` | `emucore/host/` | 2 | as they are |
| `host/audio_output.*`, `host/video_output.*`, `host/input_exchange.h` | `emucore/host/` | 2 | as they are |
| `host/frame_mailbox.h` | `emucore/host/` | 2 | `VideoFrame::vram` becomes a raw 16-bit plane with a format tag. BGR555 is what GBA and GB Color use as well |
| `platform/frame_limiter.h`, `display_sync.h`, `speed_resampler.h` | `emucore/timing/` | 2 | `utilities::` → `emulation::timing` |
| `platform/types.h`, `util.h` | `emucore/` | 2 | |
| `platform/input_bindings.h`, `sony_pad_reports.h` | `emucore/input/` | 2 | host-side controls. `PadInputName::playstation` becomes a "Sony pad" label, which is what it already means |
| `platform/mouse_scaling.h` | `emucore/input/` | 2 | the hardware mode's counts per inch are the PSX mouse's, so they become a parameter |
| `audio/iaudioengine.h` | `emucore/audio/` | 2 | |
| `emu/state.h` (`StateIO`, `Fnv1a64`), `state.cpp` | `emucore/state/state_io.*` | 2 | `kStateMagic` and `kStateVersion` stay PSX, in a `psx/state_format.h` |
| `emu/rewind_buffer.*` | `emucore/state/` | 2 | |
| `SettingsFile` from `emu/settings.h` | `emucore/config/settings_file.h` | 2 | loading and storing `EmuConfig` stays PSX |
| `render/png_file.*`, `render/shared_picture.h` | `emucore/image/` | 2 | |
| `rec/emitter.h`, `x64_asm.h`, `x86_extras.h`, `block_cache.h` | `emucore/rec/` | 2 | they already say "knows nothing of psx/" |
| `media/disc.*`, `ecm.*`, `iso9660.*` | `emucore/media/` | 3 | the `psx/psx.h` include is replaced by what they use |
| `host/machine.*` | `emucore/host/` | 4 | after `ICore`, below |
| new: `emucore/core.h` | | 4 | `ICore`, the machine's view of any console |

### Emu.Frontend.Win32

| Today (PSXEmu.Win32) | Becomes | Phase |
|---|---|---|
| new: `emuwin32/framework.h` | windows.h and the standard set, **not** `psx/psx.h` | 6 |
| `tools/letterbox.h` (in PSXEmu.Core) | `emuwin32/present/letterbox.h` | 6 |
| `graphics/present/*`: `igraphicsengine.h`, the D3D11, D3D12, OpenGL and Vulkan engines, `video_presenter`, `display_clock` | `emuwin32/present/` | 6 |
| `graphics/adapters.h`, `graphics/shared_device.h` | `emuwin32/present/` | 6 |
| `graphics/filters/` with its shaders and their build steps | `emuwin32/filters/` | 6 |
| `graphics/upscale/dlss/`, `graphics/upscale/fsr/`, with the fetch scripts | `emuwin32/upscale/` | 6 |
| `graphics/upscale/texture/`: `onnx_runtime`, `torch_model`, `texture_upscaler.h`, `rtx_video`, `ai_download` | `emuwin32/upscale/texture/` | 6 |
| `graphics/third_party/` | `emuwin32/third_party/` | 6 |
| `audio/` (WASAPI, DirectSound) | `emuwin32/audio/` | 6 |
| `input/gamepad.h`, `sony_pads.*`, `mouse.h`, the key names in `keyboard.h` | `emuwin32/input/` | 6 |
| `input/input_thread.*` | `emuwin32/input/` | 6, split: polling moves, the bindings it maps through stay |
| `ui/dpi.*`, `ui/overlay/*`, `ui/console_window.*` | `emuwin32/ui/` | 6 |
| `app/win32_dialogs.*`, `app/screenshot.*`, `app/engine_factory.*`, `Widen` from `win32_paths` | `emuwin32/app/` | 6 |
| `graphics/upscale/texture/ai_textures.*` | `emuwin32/upscale/texture/` | 5, once the texture interfaces are in Emu.Core |

### What stays, and why

- **PSXEmu.Core:** `psx/*`, `bios/*` (and the HLE BIOS), `media/mc_directory` (the PSX card's
  filesystem), `emu/cheats.h` (GameShark codes on PSX RAM), `emu/emuconfig.h`,
  `emu/recompiler_bridge.h`, `rec/recompiler.h`, `block_decoder.h`, `block_compiler.h` and
  `gte_compiler.h` (the R3000A and its delay slots), `enhance/pgxp.h` and `vertex_motion.h`
  (GTE shadows), `render/raster.h`, `software_raster`, `texture_tracker` (PSX VRAM uploads),
  `debug/disasm.h` and `debug_assist`. A future PS2 or N64 core could share a MIPS library made
  from the decoder, the disassembler and the block decoder. That would be a library of its own,
  not Emu.Core.
- **PSXEmu.Win32:** `app/` (App, menus, `const.h`, `bios_database.h`, `disc_set.h`), every
  settings window, `input/controller_bindings` and its window (PSX pads and ports), the
  cheats, debugger and memory card windows, and `graphics/hw_raster/*`, which draws PSX
  `DrawJob`s.

## Phase 0: the baseline, and the decisions

Before anything moves, record at one commit:

- Release x64 of both projects builds, into a scratch OutDir if the emulator is running from
  `Build\x64\Release`.
- Every harness passes with the counts in [Test-Suite.md](Test-Suite.md).
- The twelve-disc table (`boot_runner ... --frames 3000 --frame-log 1000`) gives the same
  checksums.
- `boot_runner --run-ahead 2` and `--rewind-test` reach the same ends.
- `host_test` passes 33, `hw_raster_test` 91.

Those are the gates for every phase below. A phase builds only the harnesses that link what it
touched, from a scratch .bat with `/MP`, not the whole of `build_tools.bat`. The full run happens
once, at the end of the phase.

The four decisions above are settled.

### Recorded 2026-10-21 at `5babf24`

The binaries are kept in `Temp\p0\` (gitignored), so any later phase can be compared against the
same executables, not only against these numbers. They were built by a copy of `build_tools.bat`
with `/MP` added and its output sent to `Temp\p0`: all 36 in 5 minutes, against 45+ serially.
Later phases are built the same way.

**Builds.** PSXEmu.Win32 Release x64 (with PSXEmu.Core) builds, with one compiler warning:
C4018 at `psx/gpu.cpp(548)`, already there.

**Harnesses**, each run alone from the repository root:

| Harness | Checks | Failures | | Harness | Checks | Failures |
|---|---|---|---|---|---|---|
| `cpu_test` | 341 | **2** | | `rec_test` | 2,059 | 0 |
| `gte_test` | 197 | 0 | | `movie_test` | 242 | 0 |
| `gpu_test` | 106 | 0 | | `cheats_test` | 72 | 0 |
| `gpu_test --hw-raster` | 106 | 0 | | `bindings_test` | 117 | 0 |
| `gpu_test --d3d12` | 106 | 0 | | `texture_tracker_test` | 73 | 0 |
| `mdec_test` | 85 | 0 | | `ai_textures_test` | 19 | 0 |
| `timer_test` | 80 | 0 | | `host_test` | 38 | **4** |
| `media_test` | 564 | 0 | | `hw_raster_test` | 91 | 0 |
| `sio_test` | 224 | 0 | | `hw_raster_test --d3d12` | 94 | 0 |
| `spu_test` | 199 | 0 | | `letterbox_test` | 12 | 0 |
| `mc_test` | 112 | 0 | | `frame_limiter_test` | 30 | 0 |
| `debug_test` | 174 | 0 | | `speed_resampler_test` | 13 | 0 |
| `hle_test` | 81 | 0 | | `mouse_scaling_test` | 37 | 0 |
| `filter_chain_test` | 27 | 0 | | `dlss_choice_test` | 42 | 0 |
| `fsr_choice_test` | 36 | 0 | | `timing_test` | 38 | 0 (a record, not a count) |

**Two failures that come before this plan.** Neither is in code this plan moves, and phases 1-7
carry both forward unchanged:

- **`cpu_test` `overflowslot`, 2 checks, and not deterministic.** "recompiled: add/addi in a
  delay slot, without overflow ... the branch went to the instruction after the next": the pc
  should be `8000100C`, and three runs of one binary gave `8000170C`, `80001C0C` and
  `8000140C`. A compiled step runs on past its block, and not by the same amount each time. A
  `cpu_test` built on 2026-10-10 passes, so this came in after that. Bisecting it is a task
  of its own, apart from this plan.
- **`host_test`, 4 checks: a stale constant.** The threaded BIOS boot is checked against
  `kBaselineChecksum = 435bad9a6c5e4004` (`host_test.cpp:575`). The threaded run gives
  `4a65781bfb98db5a`, which is what `boot_runner` gives at HEAD, with the same 92,822,656
  instructions. Every build since at least 2026-10-17 gives that, which is most likely bug 152's
  textured tint. Test-Suite.md's BIOS table has the same stale value. The threads are fine.
  It is the constant that has to be brought up to date, after the picture is checked by eye.

**BIOS boot** (`boot_runner bios/SCPH1001.BIN --frames 400 --quiet`): 92,822,656 instructions,
640x478, checksum `4a65781bfb98db5a`, 305,920 non-black of 305,920.

**Discs: seven local CHDs in place of the twelve.** The share was unreachable, even by ping, so
the disc gate uses local images instead:
`C:\Users\nopla\Downloads\Minerva_Myrient\Internet Archive\chadmaster\chd_psx\CHD-PSX-USA`. Many
files there are empty or incomplete downloads. Every non-empty one was read hunk by hunk
(`Temp\p0\scripts\chd_check`, libchdr checking each hunk's CRC), and these are the seven
whole ones used, one disc per game. Each run is 3,000 frames from cold through SCPH1001, a
checksum every 1,000 (`Temp\p0\scripts\run_discs.sh`, four at a time, since a local disc does not
starve):

| Disc | Interpreter f1000 / f2000 / f3000 | sectors | MDEC | Recompiled, pressed: f1000 / f2000 / f3000 |
|---|---|---|---|---|
| Armored Core | `3108040b5a8e12d8` `f50e3264fbb12421` `14ae9f6d1f94958c` | 4,572 | 272,100 | `3108040b5a8e12d8` `03efeeecccb3981f` `b7b627fc3f1137a2` |
| Armored Core - Master of Arena (Disc 1) | `bbbb18dffe854383` `dd07567d960ca426` `9b800862daf26172` | 4,218 | 244,500 | `bbbb18dffe854383` `643e10c321912615` `9903265848cb52f2` |
| Armored Core - Project Phantasma | `6a438209f291761b` `c90eb6684f76d1be` `b93838abca226180` | 5,076 | 309,600 | `6a438209f291761b` `1231fb07903fb98b` `eeecca29b7fffff0` |
| Final Fantasy VII (Disc 1) | `37991653287d63d1` `bbbb18dffe854383` `04e1e2cfb9649a5e` | 668 | 0 | `37991653287d63d1` `c7e931ab39e30639` `c7e931ab39e30639` |
| Formula 1 98 | `115d8a2509a3c644` `e117dba8070a629a` `1197f0e1190e7505` | 5,494 | 165,600 | `115d8a2509a3c644` `a1b9a2922d8aaf13` `66c76db0e8b1fc9a` |
| Grand Theft Auto | `8fae185652dff527` `221023bced346b3a` `02e1ac746e767030` | 5,296 | 160,800 | `8fae185652dff527` `6bc4fea96f696446` `9303eadd16f2e33b` |
| Grand Theft Auto 2 | `bbbb18dffe854383` `bb905b947bb75fc7` `747388235982886e` | 5,239 | 146,100 | `bbbb18dffe854383` `b356b16223137319` `b356b16223137319` |

The recompiled runs press Start at frame 1500 and Cross at 2200 (`--press start@1500+6 --press
cross@2200+6`), so they are not the interpreted runs' pictures from frame 2000 on.

- **Final Fantasy VII is the share's disc.** Its interpreted run is exactly Test-Suite.md's
  twelve-disc row for it: the same three checksums and the same 668 sectors. So the local set
  lines up with the recorded table rather than being a fresh start.
- **Run-ahead.** `--run-ahead 2` on the same presses gives every recompiled run's three
  checksums and instruction count exactly, on all seven discs. Its sector, MDEC and GP0 counts
  are about three times as high, because they also count the frames run ahead.
- **Rewind.** `--rewind-test 300` on Armored Core, recompiled: "300 states back: the frame kept
  aside, byte for byte; played on 300 frames: the run's own end, byte for byte".
- **The same twice.** Two batches of the same binary give identical summaries
  (`Temp\p0\discs_summary.txt` and `discs_repeat_summary.txt`: every checksum, instruction,
  sector, macroblock and GP0 count). Rewind's held-state count and every timing are left out of
  the summaries, since they follow the host's speed, not the machine's.

**The gate for every later phase:** build its harnesses and boot_runner the same way, run
`run_harnesses.sh` and `run_discs.sh` against them, and `diff` the two summaries against
`Temp\p0`'s. Any difference fails the phase, except `cpu_test`'s flaky pair. When the share is
back, the twelve-disc table is run once against `Temp\p0\boot_runner.exe` and added here.

## Phase 1: the Emu.Core project, and the vendored libraries

- `Emu.Core/Emu.Core.vcxproj`: a static library on the toolset, standard and conformance
  settings of section 2 of the standards, with `$(ProjectDir)` as its include root. Added to
  `PSXEmu.sln`. PSXEmu.Core and PSXEmu.Win32 get a `ProjectReference` to it, and
  `$(SolutionDir)Emu.Core` on their include paths.
- Move `lib/zlib`, `lzma`, `libchdr` and `zstd_stub` with `git mv`, so their history follows.
  Update their include directories in both vcxprojs and the `THIRD=` line in `build_tools.bat`.
- Delete `lib/reccore`.

**Gate:** both projects build, and the harnesses that read CHDs (`media_test`, `boot_runner` on
a `.chd` disc) pass.

**Done 2026-10-21.** Emu.Core holds the four libraries, built with the same per-file settings
PSXEmu.Core had. PSXEmu.Core references it for build order (not merged into its .lib), and
PSXEmu.Win32 links it. Release x64 builds, and every harness and the seven-disc batch give
phase 0's results exactly. Visual Studio, open on the solution, held `lib\libchdr`'s directory,
so its files were `git mv`'d one at a time. Release Win32 does not link on this machine either
before or after the move: the RTX Video SDK's block in PSXEmu.Win32.vcxproj applied on any
platform, and NVIDIA ships its library for x64 and arm64 only. That is fixed in a commit of its
own.

## Phase 2: what is already generic

The table's phase 2 rows: `git mv` each file, change its namespace, and change every include of
it in the same commit. No forwarding headers, because they would leave two names for one file
for as long as nobody removes them. There are few enough includes to change in one go.

The three files that split:

- `emu/state.h`: `StateIO` and `Fnv1a64` go, and the file format's magic and version stay.
- `emu/settings.h`: `SettingsFile` goes, and `EmuConfig`'s load and store stay.
- `host/frame_mailbox.h`: `vram` and `is_vram` become a raw plane (`raw`, `raw_width`,
  `raw_height`, `RawFormat::kBgr555`). The video thread's conversion, which reads it, keys on the
  format instead of assuming PSX VRAM.

The harnesses that test only these files (`frame_limiter_test`, `speed_resampler_test`,
`mouse_scaling_test`, the `platform/` half of `bindings_test`) move into `Emu.Core/tools/` with a
build script of their own. `host_test` stays, because it checks a threaded `System` against
`boot_runner`.

**Gate:** phase 0's numbers, unchanged. In `git diff -M` every moved file should show as a rename
of at least 90%, so `git log --follow` still finds its history.

## Phase 3: disc images

`media/disc`, `ecm` and `iso9660` move to `emucore/media/`, in `emulation::media`. Each loses
`#include "psx/psx.h"` for the handful of headers it uses. Nothing in them is PlayStation-specific
beyond a comment (`Cdrom::GetPosition`). `mc_directory` stays in PSXEmu.Core.

This only serves disc-based consoles: Saturn, Sega CD, PC Engine CD, PS2. It is worth doing
anyway, because it is where the CUE, MDS, CCD, CHD and ECM work lives, and nobody should do
that twice.

**Gate:** `media_test` 564, the twelve-disc table (which covers `.cue`, `.chd` and `.ecm`
images), and a `.mds` and a `.ccd` disc booted once.

## Phase 4: `ICore`, and `Machine` moved

The keystone. `Machine` is given a console through an interface, and everything PSX it does
today either moves behind that interface or moves out of `Machine` altogether.

### The interface

What `Machine` asks of `psx::System` today, as a console-neutral interface:

```cpp
namespace emulation {

// A state kept in memory, for run-ahead and rewind. The payload is what RewindBuffer stores;
// a core keeps whatever else it needs (the PSX keeps its memory cards) in a class of its own
// derived from this one.
struct MemoryState {
  virtual ~MemoryState() = default;
  std::vector<uint8_t> payload;
};

class ICore {
 public:
  virtual ~ICore() = default;

  // One frame, stepped exactly as Machine::RunOneFrame steps it now. A frame the debugger
  // halted in carries on from where it stopped. `steps` counts what ran, and a halted step is
  // not one.
  enum class FrameEnd { kDone, kHalted, kGaveUp };
  virtual FrameEnd RunFrame(uint64_t max_steps, uint64_t* steps) = 0;

  virtual uint64_t frame_count() const = 0;
  virtual double refresh_hz() const = 0;          // the emulated display's rate, now
  virtual uint32_t picture_number() const = 0;    // for the Reflex markers

  // The frame to show: pixels, a picture left on the card, or the core's raw debug plane.
  virtual void FillFrame(host::VideoFrame* frame, bool raw_view) = 0;

  // Sound, interleaved stereo, at the core's own rate (44,100 for the PSX, 32,768 for a GBA).
  virtual int sample_rate() const = 0;
  virtual int ReadSamples(int16_t* stereo, int max_frames) = 0;

  virtual std::unique_ptr<MemoryState> NewMemoryState() = 0;
  virtual void SaveMemoryState(MemoryState* state, bool with_saves) = 0;
  virtual std::string LoadMemoryState(const MemoryState& state, bool continuing) = 0;

  // Whether running frames ahead fits in a frame. For the PSX: the recompiler is on and the
  // debugger is not armed. Machine keeps its own conditions (the setting, a movie playing).
  virtual bool RunAheadAllowed() const = 0;

  virtual void OnFrameEnd() = 0;   // write-behind saves: memory cards, cartridge RAM
  virtual void FlushSaves() = 0;   // on pausing
};

}  // namespace emulation
```

`psx::PsxCore` implements it as an adapter around `System`: a new file in PSXEmu.Core, and no
change to `System`. `RunFrame` is the `RunOneFrame` loop moved as it is, the armed and unarmed
paths both, calling `StepInstruction` and `StepInstructionUnarmed` in the same order, so the
machine computes exactly what it does now.

### What leaves `Machine`

- **`EmuConfig`.** `Machine` reads `emulation_speed`, `frame_limiter`, `frame_pacing`,
  `run_ahead`, `rewind_interval` and `rewind_budget_mb`. These become an `emucore`
  `MachineConfig`, which the front end fills from `EmuConfig`. `gpu_rasteriser`,
  `resolution_scale`, `true_color`, `texture_filter` and `texture_budget_mb` are the PSX
  GPU's, and the PSX front end applies them to the `System` itself, in a request.
- **The texture tracker, the pack and the upscale queue** (`SetTextures`, `ReloadTextures`,
  `Save/LoadTextureUploads`). They go into a PSX-side owner, a `psx::TextureFeature` held by the
  PSX front end, and are run through `Post` requests. A request already runs on the machine's
  thread, so it may touch the `System`. This is already the rule.
- **DLSS's planes and jitter** (`set_dlss`, `set_plane_view`). Same treatment. They are calls on
  the PSX GPU.
- **`MachineReport`'s PSX fields** (`hardware_raster`, `raster_error`, the texture counts).
  The generic report stays. The PSX front end adds its own fields in the `report` hook, which
  runs on the machine's thread.
- **`Hooks::apply_input`** loses its `psx::System&`. It becomes `void(const HostInput&)`, and the
  front end's lambda captures its own core.
- **`kAudioTargetFrames`** (`44100 * 40 / 1000`) is worked out from `sample_rate()`.

Then `machine.*` moves to `emucore/host/`.

**Gate:** phase 0's numbers. On top of them, because this is the phase that can change
behaviour:

- `host_test`: the threaded run lands on the same instruction and checksum as `boot_runner`.
- `boot_runner --run-ahead 2` over the twelve discs: the same checkpoint pictures, instruction
  counts and final states.
- `--rewind-test`: the run's own end, byte for byte.
- In the app on the Radeon: a game played with run-ahead and rewind, a debugger breakpoint hit
  and stepped, a save state saved and loaded with texture replacement on, and FSR on.
- DLSS once on the RTX 4060, in one run (one device per run there).

## Phase 5: on demand, when a second emulator needs it

These are generic in shape but tied to the PSX in detail. Splitting them before anything else
wants them would be guessing at an interface with only one user, which is what the standards
warn against ("shared code moves the moment a second front end wants it"):

- **Movies.** The `.bk2` archive and its log-key parser are BizHawk's format, whatever the
  console. Mapping Octoshock's and Nymashock's columns onto `Sio`'s pads stays PSX.
- **Texture packs and the upscaler's queue.** `TexturePack`'s folder, `pack.ini` and loader
  thread, and `SourceTexels` and `TextureUpscaleQueue`, are generic. How a texture is named is
  the PSX tracker's. When these move, `ai_textures` moves into the front-end library after them.
- **The debugger's breakpoints and watchpoints.** The halt-without-running rule is generic, but
  the 29-bit physical address mask and `kBiosCall` are the PSX's.

## Phase 6: the Emu.Frontend.Win32 project

After phase 4, because the overlay and the presenter include `host/machine.h` for
`FrameSample` and `MachineReport`.

### Cutting the front end loose from the PSX

1. **`emuwin32/framework.h`.** `app/framework.h` includes `psx/psx.h` for `windows.h`, so today
   every front-end file gets the whole PSX core. The library's own framework header has
   `windows.h` (with `WIN32_LEAN_AND_MEAN` and `NOMINMAX`) and the standard set, and nothing from
   either core.
2. **`psx::SharedPicture` / `SharedPictureSource`** are `emulation::image::` after phase 2, so the
   engines need nothing more for them.
3. **`tools/letterbox.h`** moves into `emuwin32/present/`. Four engines, the overlay and the
   input thread use it.
4. **`app/const.h`'s `RendererHasFilters`** (used by `video_presenter.cpp`) moves to
   `igraphicsengine.h`. `app/win32_dialogs` and `app/engine_factory` move with the presenter.
5. **The console's numbers become parameters:**
   - `kDlssNativeLines = 240`, the PSX's lines, becomes the core's native height.
   - `kFilterChainMaxWidth`'s 1024 is reasoned from the PSX's 640-wide pictures, so it is
     looked at again.
   - `gamepad.h`'s axis convention (0x00 left and up) stays, as `HostInput`'s documented
     convention rather than "the PSX pad's".
6. **`psxemu::` → `emulation::win32`.**

### What it means for the build

- The HLSL, GLSL and SPIR-V filters and the overlay's shaders are compiled into headers by
  custom build steps in `PSXEmu.Win32.vcxproj`, and by `build_spirv.bat` (see Bugs-Found 109).
  Those move with them into the library's project.
- The DLLs that sit beside the exe (Streamline's, AMD's FidelityFX, ONNX Runtime and DirectML)
  are copied by post-build steps. They stay the exe's job, because AMD's loader looks beside the
  running exe. They move into an `Emu.Frontend.Win32.targets` that each exe's project imports, so
  the copy rules are not written once per emulator.
- `build_tools.bat`'s `HWRASTER` and AI upscaler lines name front-end sources directly, and get
  the new paths.

**Gate:** the app on the Radeon through all four renderers, with a filter chain, the overlay,
FSR and FSR Frame Generation, and the AI texture upscaler. A DualSense, an XInput pad and the
mouse as a PSX mouse. `filter_chain_test`, `dlss_choice_test`, `fsr_choice_test`,
`letterbox_test`, `ai_textures_test` and `hw_raster_test` pass. DLSS and Frame Generation on the
RTX 4060, in one run.

## Phase 7: the documents

- [Project-Layout.md](Project-Layout.md): the four projects and their trees. Its tree is already
  behind in two places: it puts the audio engines in PSXEmu.Core's `audio/`, where only
  `iaudioengine.h` is, and lists a `utilities/` folder that is no longer there. Both are fixed
  in the same pass.
- [Architecture.md](Architecture.md), with its component diagram, and Emulator-Project-Standards
  section 1: two shared libraries under every emulator's two projects.
- [Test-Suite.md](Test-Suite.md): which harnesses live in `Emu.Core/tools/`.
- [README.md](README.md): this plan's status.

## What a second emulator would take on day one

GBAEmu already has its own copies of several of these: `audio/iaudioengine.h` with WASAPI and
DirectSound engines, `emulation/settings.h` (which `SettingsFile` was modelled on),
`emulation/serializer.h`, and the `IGraphicsEngine` that this one was "adapted from". So
adopting the libraries there is mostly deleting: the audio engines, the settings file, and the
presenters. GBAEmu's ImGui backend lifecycle would become an optional part of `IGraphicsEngine`,
since PSXEmu.Win32 has native menus and no ImGui.

The machine thread, run-ahead, rewind and frame pacing are what GBAEmu does not have, and they
come with `ICore`: about a dozen functions for a GBA core to implement.
