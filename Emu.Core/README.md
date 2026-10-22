# Emu.Core

A static library of what any emulator here shares, apart from any one console or front end:
the machine's thread and the channels around it, frame pacing, save states in memory and on
disk, rewind, the settings file, disc images, PNGs, host input, the x64 code emitter, and the
compression libraries under all of it.

It was taken out of PSXEmu in October 2026 (PSXEmu's `Docs/Shared-Libraries-Plan.md` records how
and why, phase by phase). PSXEmu is its first
user and the reference for every pattern below. Its Windows front-end half is
`Emu.Frontend.Win32`, which has a guide of its own.

## The rules

- **Nothing of any console.** Emu.Core includes nothing from PSXEmu.Core, PSXEmu.Win32 or
  Emu.Frontend.Win32, and never may. Whatever is a console's goes behind `ICore` (below) or stays
  in that console's own library.
- **Windows-specific, framework-free.** It uses `windows.h` (the frame limiter's timer, a physical
  disc drive, executable memory) but no UI framework and no graphics API.
- **Deterministic.** Nothing here may change what an emulated machine computes. A console's
  results with and without the shared code must be the same to the bit, which is how every move
  into it was checked.
- **One include root.** The project folder is the include root and everything is under
  `emucore/`, so every include names the library: `#include "emucore/host/machine.h"`.

## Using it from a project

In the project that uses it (`.vcxproj`):

```xml
<AdditionalIncludeDirectories>$(SolutionDir)Emu.Core;...</AdditionalIncludeDirectories>

<ProjectReference Include="..\Emu.Core\Emu.Core.vcxproj">
  <Project>{14fceedf-673e-4b07-a60c-2b90c1342f10}</Project>
  <!-- In a static library that uses Emu.Core: build order only. The executable links it. -->
  <LinkLibraryDependencies>false</LinkLibraryDependencies>
</ProjectReference>
```

An executable references Emu.Core itself (without `LinkLibraryDependencies`), so it links it. A
file that uses zlib (`#include "zlib.h"`) or libchdr also needs
`$(SolutionDir)Emu.Core\emucore\lib\zlib` or `...\lib\libchdr\include` on its own include path, as
Emu.Core's own `png_file.cpp` and `rewind_buffer.cpp` have.

The headless harnesses build without the solution: `Emu.Core\tools\build_tools.bat [out folder]`
builds Emu.Core's own (frame limiter, speed resampler, mouse scaling) into `Temp\tools` or the
folder given. PSXEmu's `PSXEmu.Core\tools\build_tools.bat` calls it first.

## What is in it

| Folder | Namespace | What | Main types |
|---|---|---|---|
| `emucore/core.h` | `emulation` | What the machine's thread asks of a console | `ICore`, `MemoryState` |
| `emucore/host/` | `emulation::host` | The machine's thread and the channels between a front end's threads | `Machine`, `MachineConfig`, `MachineReport`, `FrameSample`, `FrameMailbox`, `VideoFrame`, `SampleRing`, `InputExchange`, `HostInput`, `RequestQueue`, `Doorbell`, `AudioOutput`, `VideoOutput`, `Presenter`, `LatencyMarkers` |
| `emucore/timing/` | `emulation::timing` | Holding a loop to a rate, matching the display, stretching sound to a speed | `FrameLimiter`, `MatchDisplay`, `DisplayTiming`, `DisplayMatch`, `PhaseCorrection`, `SpeedResampler` |
| `emucore/state/` | `emulation::state` | Save states, read and written through one path, and rewind's store | `StateIO`, `Fnv1a64`, `RewindBuffer` |
| `emucore/config/` | `emulation::config` | A hand-editable `key = value` settings file | `SettingsFile` |
| `emucore/media/` | `emulation::media` | Disc images and the ISO9660 filesystem | `Disc`, `EcmImage`, `Iso9660` |
| `emucore/image/` | `emulation::image` | PNG files, and a picture left on the graphics card | `WritePngFile`, `ReadPngFile`, `EncodePng`, `DecodePng`, `SharedPicture`, `SharedPictureSource` |
| `emucore/input/` | `emulation::input` | A host pad's controls as bits, binding maps, Sony pads' HID reports, mouse scaling | `PadInput`, `PadInputBit`, `KeyHeld`, `MapKeys`, `ParseSonyPadReport`, `BuildSonyRumbleReport`, `MouseAccumulator`, `WindowsCurve` |
| `emucore/audio/` | (global) | What a sound device has to do | `IAudioEngine` |
| `emucore/rec/` | `emulation::rec` | A recompiler's back end: executable memory, x86-64 encodings, a cache of compiled blocks | `Emitter`, `CodeBlock`, `x64::Asm`, `BlockCache`, `Block` |
| `emucore/lib/` | - | Vendored C libraries, unchanged: zlib 1.3.1, the LZMA SDK, libchdr, a zstd stub (`README-chd.md`) | |

Every header opens with what it is for and why it is that way. Read the header before using a
type; this guide is the map, not the manual.

## Running a console: `ICore` and `Machine`

`host::Machine` runs a console a frame at a time on a thread of its own: requests between frames,
input in, the picture and the sound out, pacing to the console's rate or the display's,
run-ahead, rewind, and the debugger's halts. It knows the console only through `ICore`
(`emucore/core.h`). A new emulator implements `ICore` once and gets all of that.

### Implementing `ICore`

PSXEmu's implementation is `psx::PsxCore` (`PSXEmu.Core/emu/psx_core.h`, about 300 lines). Read it
next to this list:

| Method | What it has to do |
|---|---|
| `RunFrame(uint64_t* steps, uint64_t max_steps)` | Step the console until its frame ends (the picture is finished), counting steps in `*steps`, stopping at `max_steps`. Return false if the debugger halted it mid-frame; the next call carries on to the same frame's end. A console without a debugger always returns true. |
| `debugger_armed()`, `halted()` | Whether a debugger may stop the machine (run-ahead and rewind are then off), and whether it has. `false`, `false` without one. |
| `RunAheadFits()` | Whether running frames ahead fits in a frame's time. PSXEmu: only with the recompiler. |
| `refresh_hz()`, `picture_number()` | The emulated display's rate now, and the picture's number, for pacing and NVIDIA Reflex. |
| `FillFrame(VideoFrame*, bool raw_view)` | The finished picture: 32-bit `0xAARRGGBB` in `pixels`, or on the card in `shared`, or with `raw_view` the console's video memory in `raw` with its `raw_format`. Set `width` and `height`; the machine numbers the frame. |
| `sample_rate()`, `ReadSamples(int16_t*, int)` | Interleaved stereo at the core's rate, drained until 0. The machine resamples it for the speed and keeps the ring at 40 ms. |
| `FillReport(MachineReport*)` | Optional: what the console adds to the once-a-second report. |
| `NewMemoryState()` | A `MemoryState` of the core's own, derived to hold anything beside `payload()`. |
| `SaveBeforeRunAhead` / `LoadAfterRunAhead` | The real frame kept before frames run ahead, including whatever the console saves to (a save made ahead is on input not given yet), and loaded back after them. |
| `SaveForRewind` / `LoadForRewind` | A state to step back to, without what the console saves to. |
| `OnFrameEnd()`, `FlushSaves()` | Saves written behind (memory cards, cartridge RAM): due ones at each real frame's end, and all of them on pausing. |

Everything else a front end wants of its console (a rasteriser, textures, cheats) it asks of its
own implementation directly, in a request. PSXEmu gives itself `psx::CoreOf(machine)` and
`psx::SystemOf(machine)` for that, which cast `machine.core()` to the type it knows it made.

### Running `Machine`

```cpp
#include "emucore/host/machine.h"
#include "emucore/host/audio_output.h"
#include "emucore/host/video_output.h"

MyCore core(&my_console);                      // : public emulation::ICore
emulation::host::Machine::Hooks hooks;
hooks.apply_input = [&](const emulation::host::HostInput& in) { /* onto the console's ports */ };
hooks.report = [&](const emulation::host::MachineReport& r) { /* to the UI thread */ };

emulation::host::MachineConfig config;         // speed, limiter, pacing, run-ahead, rewind
emulation::host::Machine machine(&core, config, &video.frames(), &audio.samples(), hooks);
machine.Start(emulation::host::kPausedByUser);
machine.Post([](emulation::host::Machine& m) { m.SetPaused(emulation::host::kPausedByUser, false); });
// ...
machine.Stop();                                 // before `core`, before the console
```

- **The only way in is `Post`.** A request runs on the machine's thread between frames, or at once
  while it is paused, so it may touch the console. Nothing else may call into the console while
  the machine runs.
- **Settings:** `ApplyConfig(MachineConfig)` for the machine's own. Give the console its own beside
  it in the same request (PSXEmu: `psx::ApplyConfig(machine, config)` does both).
- **Hooks** run on the machine's thread: `after_frame` once a frame shown, `after_each_frame` after
  frames run ahead too (cheats), `may_run_ahead` / `may_rewind` to hold them off (a movie
  playing), `halted` when the debugger stops it, `display_timing` for Frame Pacing's "Match the
  display", `frame_done` for performance graphs.
- **Input:** an input thread publishes into `machine.input()` (`InputExchange::Publish`); the
  machine takes the latest reading each frame and passes it to `apply_input`.
- **Output:** `VideoOutput` (the video thread) shows frames from its `FrameMailbox` through a
  `Presenter` the front end makes; `AudioOutput` (the audio thread) feeds an `IAudioEngine` from its
  `SampleRing`. Emu.Frontend.Win32 has both for Windows.
- **Lifetime:** stop the machine, then the outputs, then destroy the core, then the console.

## Save states: `StateIO`

One class, one code path both ways. Every component has `void Serialise(state::StateIO& io)`,
which calls the same `Plain` / `Bytes` / `Deque` / `Str` / `Ram` whether it is saving or loading, so
a field can never be written by one path and read back by another that has drifted from it.

```cpp
state::StateIO io(/*saving=*/true);
io.ReuseBuffer(&buffer_);        // no allocation on the next save
console.SerialisePayload(io);    // every component, in a fixed order
io.TakeBytes(&buffer_);
```

Loading is `BeginLoad(data, size)` (or `BeginLoadView`, without copying), the same
`SerialisePayload`, then `truncated()` and `error()`. What a file holds around the payload (a magic,
a version, a hash of the BIOS) is the console's own: PSXEmu's is `psx/state_format.h`. `Ram()`,
with `WatchRam`, tells a recompiler which 4 KB pages of RAM a load changed.

`RewindBuffer` keeps payloads in memory as reverse deltas of the pages that changed, compressed on
a worker thread within a memory budget. `Machine` drives it; it needs nothing from a console but
`SaveForRewind` / `LoadForRewind`.

## Settings: `SettingsFile`

A flat, hand-editable `key = value` file. Unknown keys are kept, every getter takes the current
value as its default, and the file is written in a stable order. The line it opens with is given
by whoever makes it, so an emulator derives one that names itself, as PSXEmu does in
`emu/settings.h`:

```cpp
class SettingsFile : public emulation::config::SettingsFile {
 public:
  SettingsFile() : emulation::config::SettingsFile("# MyEmu settings. Edit freely; unknown keys are preserved.\n\n") {}
};
```

The console's own settings struct, and loading and storing it, stay the console's.

## Discs: `Disc`, `EcmImage`, `Iso9660`

`media::Disc::Open(path)` mounts `.cue`, `.mds`/`.mdf`, `.ccd`/`.img`, `.chd`, `.ecm`, `.bin`, `.iso`
or a physical drive (`D:`), and hands out raw 2,352-byte sectors (`ReadSector(lba, out)`), a track
list (`track_count()`, `track(i)`), subchannel Q where the image has it, and why it would not open
(`open_error()`). LBAs are absolute, counting the 150-sector lead-in (`Disc::kLeadInSectors`).
`Iso9660::Open(&disc)` then reads the filesystem: `Find`, `List` and `Read` by path. CD-based
consoles (Saturn, Sega CD, PC Engine CD, PS2) get all of it.

## Images: PNG and `SharedPicture`

`image::WritePngFile` / `ReadPngFile` (and `EncodePng` / `DecodePng` in memory) write and read PNGs
with the bundled zlib: RGBA8, any PNG colour type read. `SharedPicture` is a picture a hardware
rasteriser left on the graphics card, carried in a `VideoFrame` to a presenter that opens it there,
with a plane of depth and motion beside it for DLSS and FSR. A console without a hardware
rasteriser never fills it.

## Timing

`Machine` uses all of these, and each stands alone too:

- `timing::FrameLimiter`: `Wait(hz)` holds a loop to a rate on a high-resolution waitable timer,
  evenly spaced, catching up after a hitch. `Shift` moves its deadline (display phase).
- `timing::MatchDisplay(console_hz, display_hz)`: whether to run in step with the display (a whole
  number of refreshes a frame, within 2%) and at what rate. `PhaseCorrection` keeps the frames at
  one point in the refresh.
- `timing::SpeedResampler`: stretches a stereo stream by a speed factor, continuous across calls,
  so a fixed-rate device stays fed at 50-300%.

## Host input

- `host::HostInput` is one reading of the host: the keys held (one bit per virtual key), four pads
  (`PadReading`: `inputs` as `input::PadInput` bits, sticks with 0x80 centred, 0x00 left and up),
  the mouse's buttons and motion, and where the cursor is over the picture.
- `input/input_bindings.h`: a pad's controls by position (`kPadA` ... `kPadRStickRight`), named as on
  an XInput pad with the Sony names beside them, and binding maps as arithmetic and as settings
  text.
- `input/sony_pad_reports.h`: the DualShock 4's and DualSense's HID reports parsed, and their motor
  reports built (USB and Bluetooth, with its CRC). No I/O.
- `input/mouse_scaling.h`: a host mouse's movement as a console mouse's counts, three ways.

## Recompiler back end

- `rec::Emitter` / `CodeBlock`: executable memory and a cursor that refuses to write past the end.
- `rec/x86_extras.h`: hand-written x86-64 encodings, the fixed shapes a block compiler emits.
- `rec::x64::Asm`: a general x86-64 encoder (registers or `[base + index*scale + disp]`, any operand
  size), for arithmetic-heavy code.
- `rec::BlockCache`: compiled blocks by guest address, and which 4 KB pages each was compiled from,
  so a store into one throws it away.

The decoder and compiler for a guest CPU are that console's (PSXEmu: `rec/`).

## Still the PlayStation's, until a second user wants otherwise

These came here with PlayStation details still in them. Taking the details out would change
behaviour rather than just move code, so each waits for a second emulator to need it, and then
becomes a parameter:

- `BlockCache::Normalise` masks addresses to 29 bits (the R3000A's KSEG mirrors), and its fast
  lookup assumes 4-byte instructions.
- `mouse_scaling.h`'s hardware mode is scaled for the PlayStation mouse.
- `input_bindings.h`'s third name column is the PlayStation's button names.
- `MachineReport` has fields only PSXEmu fills (`hardware_raster`, the texture counts); another core
  leaves them as they are.
- The sound device opens at the rate the front end gives it (Emu.Frontend.Win32's
  `CreateAudioEngine`); a core with another rate passes its own.
- `IAudioEngine` is in the global namespace.

## Candidates to move here, when a second emulator needs them

Three parts of PSXEmu are generic in shape but tied to the PlayStation in detail. They stay in
PSXEmu.Core until a second emulator wants one: splitting them sooner would mean guessing at an
interface with only one user to check it against. When one is wanted, the generic half comes here
and the PlayStation's half stays behind as a thin layer over it.

| Candidate (in PSXEmu.Core) | What would come to Emu.Core | What would stay the console's |
|---|---|---|
| Input movies (`emu/movie`) | Reading BizHawk's `.bk2` (a zip of text files: the header, the input log's key and one line a frame), which is the same for every console, and stepping through it by frame | Which column is which control: mapping the log onto the console's own pads and ports (for the PlayStation, two BizHawk cores that name the same controls differently, and the multitap) |
| Texture replacement (`render/texture_pack`, `render/texture_upscale.h`) | The pack: a folder of PNGs read on a loader thread, `pack.ini` (filters, aliases, textures never replaced), images 1x-8x; and the AI upscaler's queue, with the texels it is handed | How a texture is named: from the uploads into the console's video memory and its palettes (`render/texture_tracker`), which is the PlayStation's GPU |
| Debugger breakpoints (`debug/debugger`) | The rule that a halt runs nothing - stopped before a step, so a run that halts a thousand times computes what one that never halts does - and the breakpoints, watchpoints and stepping around it | How addresses match (the R3000A's 29-bit mask, one instruction seen through three windows) and the PlayStation BIOS's calls as a reason to break |

The AI upscaler's own thread (`ai_textures`, in PSXEmu.Win32) would follow the texture pack into
Emu.Frontend.Win32.
