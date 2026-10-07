# AMD FSR: Upscaling and Frame Generation

**Status: built 2026-10-07, upscaling and Frame Generation running on the Radeon 780M; the review
of the pictures in motion to do.** The Video Settings window's AMD FSR group runs AMD's FSR Upscaling -
FSR 4 where the card and driver have it, FSR 3.1.5 everywhere else - and Native AA, through AMD's
FidelityFX SDK 2.3.0, in the Direct3D 12 renderer, on any Direct3D 12 card. It takes exactly what
DLSS takes, from the same places: the plane beside VRAM, the motion from the GTE and 2D matching,
the jitter, all built for DLSS (Docs/DLSS-Plan.md, phases 1-3). FSR Frame Generation runs through
AMD's own swap chain. The machine runs exactly as without any of it.

**Decided 2026-10-07:**

- **Scope:** Upscaling and Native AA, then Frame Generation - as DLSS.
- **The menu:** a group of its own in the Video Settings window, beside DLSS's. Choosing an FSR
  mode turns DLSS off, and the other way round; never both.
- **The DLLs:** AMD's signed DLLs, MIT, fetched by a script and copied beside the executable by
  the build, never committed (69 MB). The API headers, MIT, are committed.

---

## The conclusion first

- **Nothing new from the PlayStation.** FSR wants colour, depth, motion vectors and jitter, the
  inputs DLSS wanted; phases 1-3 of the DLSS plan made them. FSR is a second consumer of the same
  plane, through the same compute pass, into the same output texture, with the same history rules.
- **One integration, in the Direct3D 12 renderer.** AMD's API (`ffx_api`) is Direct3D 12 and
  Vulkan only, and Frame Generation is a swap chain of AMD's; the Direct3D 12 renderer is where
  DLSS already is. The other renderers get neither.
- **Any card.** FSR 3.1 runs on any Direct3D 12 card - the Radeon 780M and the RTX 4060 alike - so
  unlike DLSS nearly all of it is checked on the Radeon, with no NVIDIA card needed.
- **FSR 4 is AMD's to give.** AMD's loader offers "4.1.1 \*" on the 780M, and it gives the same
  pictures as 3.1.5 to the bit (below): FSR 4 needs a discrete RX 7000 or an RX 9000. The menu
  says so beside the version.
- **The signs measured, not assumed**: `ffx_probe --jitter-test`, as `sl_probe` did for DLSS. They
  are DLSS's: the picture's jitter negated, the motion as the plane has it.

---

## What there is (2026-10-07)

AMD FSR SDK 2.3.0 (24 June 2026), `GPUOpen-LibrariesAndSDKs/FidelityFX-SDK`, MIT:

| DLL | What | Size |
|---|---|---|
| `amd_fidelityfx_loader_dx12.dll` | the five functions; loads the effects | 26 KB |
| `amd_fidelityfx_upscaler_dx12.dll` | FSR Upscaling 4.1.1 (ML), 3.1.5, 2.3.4 | 28.8 MB |
| `amd_fidelityfx_framegeneration_dx12.dll` | FSR Frame Generation 4.0.1 (ML), 3.1.6; the swap chain 3.1.7 | 40.1 MB |

- **FSR 4** (ML) needs a Radeon RX 7000 (discrete, since 4.1.1) or RX 9000; ML Frame Generation
  RX 9000. **FSR 3.1** runs on any Direct3D 12 card with typed UAV loads.
- **The API**: five functions, descriptors chained by `pNext`, contexts per effect; versions
  listed per device and chosen with `ffxOverrideVersion`.
- **AMD's loader finds the effect DLLs only beside the running executable**, not beside itself:
  run from `Temp\tools` with the DLLs in `Temp\fidelityfx`, it offered no versions at all. The
  build copies them beside `PSXEmu.Win32.exe`, `build_tools.bat` beside the tools.

---

## The design

### Where FSR runs

- **In `D3D12GraphicsEngine`** (`graphics/d3d12_fsr.cpp`), beside DLSS (`d3d12_dlss.cpp`).
  `graphics/fsr/fidelityfx.h/.cpp` loads the loader once per process after Windows' Authenticode
  check, with the signer's certificate read and required to be Advanced Micro Devices - on the
  loader, the upscaler and Frame Generation's DLL - and takes the five functions by name. Nothing
  links against AMD's code.
- **Started when asked for**: `SetFsr` with a mode, at `Initialize` or later; no engine is made
  again for upscaling. The versions the card runs are asked once (`ffxQueryDescGetVersions`).
- **One upscaler context per pair of sizes and version**, made at the first picture and again
  when either changes, after the card has finished with the old.
- **DLSS first**: with a DLSS mode asked for, FSR does not start, and says so. The front end
  never asks for both; a settings file with both keeps DLSS.

### What FSR is given

From the plane, by DLSS's compute pass (`MakeUpscalerInputs`), which now takes the hint's two
values from its caller:

| FSR's | From | Notes |
|---|---|---|
| colour | the picture where this frame has it | the console's colours, gamma and all: `NON_LINEAR_COLORSPACE`, `NON_LINEAR_COLOR_SRGB` |
| depth | the plane's 1/z | `DEPTH_INVERTED | DEPTH_INFINITE`, near 1: FSR's view depth is the GTE's z |
| motion | the plane's, in the picture's pixels | `motionVectorScale` 1: FSR's own scale; where it was minus where it is |
| reactive mask | DLSS's hint texture | 0.9 where motion is unknown (AMD advise no more), 0.3 where translucent |
| exposure | a 1x1 texture holding 1.0 | the picture is 0-1 already; without one AMD warns at every picture |
| jitter | the picture's, negated | measured, below |

- **`viewSpaceToMetersFactor` 0.01** - a GTE unit taken as a centimetre. FSR 3.1's disocclusion
  test is in proportion to depth (`Ksep * halfViewport * depth`), so units do not matter to it;
  what is in metres is the motion it ignores as too small for a depth, and a clamp at 65,504 that
  the GTE's 64-65,000 would otherwise reach.
- **Camera**: near 1, far 65,536 (unused with infinite depth), a vertical field of view of 1 radian;
  there is no one camera in a PlayStation game, as for DLSS.
- **Frame time**: the time since the last picture FSR made, 1-100 ms.
- **Sharpening**: AMD's RCAS after the upscale, Off/Low/Medium/High/Maximum (0, 0.25, 0.5, 0.8,
  1); Medium by default.

### Sizes

`graphics/fsr/fsr_choice.h`, header-only, `tools/fsr_choice_test.cpp`:

- **FSR takes any input for any output.** Its quality modes are names for ratios (Native AA 1,
  Quality 1.5, Balanced 1.7, Performance 2, Ultra Performance 3); the dispatch is told the two
  sizes and nothing else.
- **The scale** is the mode's ratio as near as a whole scale of the console's 240 lines gets it,
  2-8x, no 7x; Native AA the largest that fits. On the laptop's 1600-line screen: Native AA 6x,
  Quality and Balanced 4x, Performance 3x, Ultra Performance 2x.
- **The output** is the picture's rectangle of the screen, one to one, unless the input is larger
  either way - a 640-wide or 480-line picture at a high scale - when FSR works at the input's own
  size and the result is scaled onto the screen as any picture is.
- **The jitter's length**: AMD's rule, `ceil(8 x ratio^2)`, at the ratio actually used: 18 for 1.5,
  32 for 2, 72 for 3, as AMD's table. The core's Halton sequence starts at index 1, so it never
  gives (0, 0), which AMD forbids.

### Frame Generation

- **AMD's swap chain**, made by `ffxCreateContext` with
  `ffxCreateContextDescFrameGenerationSwapChainForHwndDX12` in place of DXGI's, on the renderer's
  queue. It is an `IDXGISwapChain4` like any; its own threads pace the frames it shows. So turning
  Frame Generation on or off makes the renderer again (`FsrNeedsRemaking`), as DLSS's does.
- **Its context** is made at the first picture it can generate after, for the window's size and the
  picture's, and again when either changes (resizing destroys it first, generation off on the swap
  chain before, as AMD asks).
- **Each present**: `ffxConfigureDescFrameGeneration` - on for a new picture FSR has just made at
  the screen's own size, at 100% speed with the limiter pacing; off for anything else - with a
  frame id counting presents one by one, the generation rectangle the picture's (the bars left
  alone), and when on `ffxDispatchDescFrameGenerationPrepareV2` with FSR's own depth, motion,
  jitter and reset for that picture. AMD's callback dispatches the generation from the swap
  chain's present.
- **Our overlay is the UI layer**: drawn alone into a clear premultiplied layer, registered with the
  swap chain (`USE_PREMUL_ALPHA | ENABLE_INTERNAL_UI_DOUBLE_BUFFERING`), which puts it over every
  frame, real and generated; the back buffer holds the picture alone. Off, the overlay is drawn into
  the back buffer as ever and the layer taken away.
- **New pictures only** (`TakesOnlyNewPictures`), as under DLSS Frame Generation: a 30 fps game
  presents 30 times a second.
- **Not Reflex**: AMD's swap chain paces itself; the machine's limiter keeps the console's rate.
- **AMD's own limit**: FSR 3.1's Frame Generation is made for 60 pictures a second and up; most
  PlayStation games draw 30, where generated frames will show more artefacts. The window says so.

### The front end

- **Settings > Video, AMD FSR**: Mode (Off, Native AA, Quality, Balanced, Performance, Ultra
  Performance), Upscaler (Automatic, FSR 4, FSR 3.1), Sharpening, and Frame generation (2x); a line
  saying what runs and at what internal resolution, or why not, and one for Frame Generation. The
  group needs Direct3D 12 and the hardware rasteriser, on any card; greyed otherwise, with the
  reason. "Get AMD's FSR files..." while they are not all beside the emulator.
- **The emulator downloads AMD's files itself** (`graphics/fsr/fsr_download.h/.cpp`, added
  2026-10-07): that link's dialog leads with "Download them now". DLSS's can only send people to
  NVIDIA's 280 MB archive; AMD's DLLs are MIT and published one by one, so the same three files
  `fetch_fidelityfx.ps1` fetches come straight from AMD's v2.3.0 release over WinHTTP, each into a
  `.part` file with its git blob id worked out as it streams, checked against the release's size
  and id, then against AMD's Authenticode signature, and only then moved beside the emulator; the
  MIT notice goes beside them as `fidelityfx.license.txt`. A file already there and the release's
  is left alone - it may be loaded. A progress dialog shows each file and the megabytes, and can
  cancel; then the renderer is made again and FSR starts. Checked on the Radeon from a copy with
  none of the files: 66 MB in about fifteen seconds, FSR running at once; cancelled half-way,
  nothing left behind and nothing said. A folder that cannot be written (Program Files) says so,
  and the copy-by-hand way stays in the same dialog.
- **The window is three columns now**: Rasteriser, NVIDIA DLSS, AMD FSR. One above the other
  would have made it taller than a 1600-line screen at 200%.
- **`EmuConfig`**: `fsr_mode`, `fsr_version`, `fsr_sharpness`, `fsr_frame_generation`; the mode
  and Frame Generation are a game's own keys, as DLSS's.
- **While FSR runs** the machine is told exactly what it is told for DLSS (`Machine::set_dlss`): the
  plane kept, the jitter on, the scale and PGXP's precise vertices. App's `UpdateUpscaler` (was
  `UpdateDlss`) works out which of the two runs.
- **Show Timings**: `fsr 3.02 ms`, the card's time for its inputs and FSR, as DLSS's.
- **Help > About**: AMD's attribution and the MIT notice, `fidelityfx.license.txt` beside the
  DLLs, linked.
- **`PSXEMU_FSR_LOG=<folder>`** writes `fsr.log`: what started, the versions offered, each
  context made, why anything did not run, and AMD's own debug checks (`ENABLE_DEBUG_CHECKING`, on
  only with the log), each message once.

---

## Phase 0, as built: the SDK and `ffx_probe`

- **`graphics/fsr/fetch_fidelityfx.ps1`** downloads the three DLLs of v2.3.0 from AMD's repository
  into `Temp\fidelityfx\v2.3.0`, checking each one's size and git blob id against the release's
  tree; running it again does nothing. Windows' check: all three validly signed by Advanced Micro
  Devices.
- **`ffx_probe`** (`tools/ffx_probe.cpp`): the versions AMD offers on each card - the NVIDIA one only
  with `--all` or `--card`, one device a run - and, with `--jitter-test`, the signs; `--cost` the
  time; `--version` a version by name.

On the Radeon 780M, 2026-10-07:

- **Offered**: Upscaling `4.1.1 *, 3.1.5, 2.3.4`; Frame Generation `3.1.6`; the swap chain `3.1.7`.
- **The signs** - a sharp-edged pattern through 48 frames of FSR Quality, 192x144 to 288x216,
  scored against the pattern drawn at the output's size (mean difference, lower is better):

  | | 3.1.5 (and "4.1.1 \*") | 2.3.4 |
  |---|---|---|
  | jitter as the picture's | 0.103 | 0.124 |
  | **jitter negated** | **0.054** | **0.034** |
  | jitter +x, -y | 0.092 | 0.096 |
  | jitter -x, +y | 0.084 | 0.092 |
  | no jitter at all | 0.078 | 0.085 |
  | moving, **motion as the plane has it** | **0.094** | **0.099** |
  | moving, motion negated | 0.221 | 0.234 |
  | moving, no motion | 0.219 | 0.233 |

  Every wrong sign is worse than no jitter or no motion. "4.1.1 \*" gives 3.1.5's numbers to the
  fourth decimal on all eight: on this card it is 3.1.5.
- **The cost on the card** (median of 30, ms; "4.1.1 \*" within 10%):

  | | In | Out | ms |
  |---|---|---|---|
  | Quality | 640x480 | 945x709 | 1.52 |
  | Quality | 960x720 | 1440x1080 | 3.01 |
  | Quality | 1280x960 | 2133x1600 | 5.21 |
  | Performance | 960x720 | 2133x1600 | 3.85 |
  | Native AA | 1280x960 | 1280x960 | 4.18 |
  | Native AA | 1920x1440 | 1920x1440 | 21.3 |
  | Ultra Performance | 640x480 | 1920x1440 | 2.33 |

  On this laptop's own window (1280x960 client): Quality from 3x, about 3 ms a picture.

## Phases 1-2, as built

**Upscaling** (`d3d12_fsr.cpp`): as above. DLSS's `DrawDlss` became `DrawUpscaled`, choosing
the output and the evaluation by which upscaler runs; DLSS's input pass and its end became
`MakeUpscalerInputs` and `FinishUpscalerInputs`, shared.

**Frame Generation**: as above.

**Verified** (2026-10-07, a scratch copy of the app, its own settings file):

- **Harnesses**: `fsr_choice_test` 36 (new), `dlss_choice_test` 42, `media_test` 479 (+5, the FSR
  settings), all green. The app builds clean, Release x64.
- **Upscaling on the Radeon 780M**, Ridge Racer (PAL), FSR Quality in the 1280x960 window:
  - 960x720 to 1280x960 in the race, **49.7 fps, full speed, FSR 5.1-5.5 ms a picture** (Show
    Timings); the bridge's cables thin and unbroken, the text on the cars legible.
  - The window: "Running: AMD FSR 4.1.1, at 3x internal resolution. FSR 4 only where the card
    and its driver have it, FSR 3.1 otherwise."; DLSS's group greyed, "needs the NVIDIA graphics
    card".
  - AMD's own checks (`PSXEMU_FSR_LOG`, which turns them on) quiet once the exposure was given;
    before, "exposure resource is null and auto exposure flag is unset" at every picture.
- **480-line pictures cost FSR more**: the BIOS intro and 480-line menus are drawn at the scale
  chosen for 240 lines, so at Quality FSR takes 1920x1434 - Native AA at that size, 20-30 ms on
  the 780M, 11-13 fps for the BIOS intro. FSR downscaling them onto the screen instead costs the
  same (`ffx_probe`: 20.2 ms against 22.6), its time going by the input. At Performance or Ultra
  Performance (2x) they are 1280x956 and run at full speed. Left as it is: the game itself runs at
  full speed, and a faster card does not care. DLSS does the same with them.
- **Frame Generation on the Radeon**, Ridge Racer:
  - **Ultra Performance (2x)**: 49.7 fps, full speed, **frame gen 2.0x** - the swap chain's own
    count of presents, AMD's generated frames among them, exactly twice ours: 99.4 frames a second
    on the screen. FSR 3.8 ms a picture.
  - **Quality (3x)**: 2.0x too, but the 780M has not the time for both - 13-39 fps. FSR's
    upscale and its Frame Generation (3.1.6: optical flow and interpolation, on the same queue)
    on an integrated card at 1280x960.
  - The overlay - the full statistics panel - crisp over the generated frames, composed by AMD's
    swap chain from its own layer.
  - From the window, in one run: Frame Generation off (the renderer made again, without AMD's
    swap chain), FSR off (the picture as it is), FSR back to Quality (started again with no
    renderer made), each at full speed, no hang, and it closed cleanly.
- **DLSS unchanged** after its input pass and its draw were shared: one run on the RTX 4060,
  Ridge Racer with DLSS Quality, "Running: NVIDIA DLSS 310.9.1, at 3x", 49.7 fps, the picture
  right, the sizes in `dlss.log` as before.

**Not yet:**

- **The review in motion**, as for DLSS: ghosting and shimmer on the 26 discs against FSR off, and
  the reactive values (0.9, 0.3) and sharpening default chosen from it.
- **FSR 4 itself**, on a card that has it: none here does.
- **FSR on the RTX 4060**: AMD's FSR 3.1 is the same code on any card, and the 4060 is spared
  device-making runs (the 0x9F of 2026-09-29); one run would settle it.
- **A faint smear beside the car** in one capture under Frame Generation at Ultra Performance -
  a generated frame caught mid-way, or ghosting; stills cannot say.
- **Frame Generation's evenness on the screen**, and the HUD - the game's own cannot be told from
  the picture, as under DLSS.

---

## Sources

- [FidelityFX SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK) v2.3.0 (MIT):
  `Kits/FidelityFX/docs` - `getting-started/ffx-api.md`, `techniques/super-resolution-ml.md`,
  `super-resolution-upscaler.md`, `frame-interpolation-api.md`, `frame-interpolation-swap-chain.md`,
  `license.md` - and the headers, copied into `PSXEmu.Win32/graphics/fsr/fidelityfx/`;
  `upscalers/fsr3/include/gpu/fsr3upscaler/` for how FSR 3.1 uses depth in metres.
- [AMD: FSR 4 on GPUOpen](https://gpuopen.com/learn/amd-fsr4-gpuopen-release/)
