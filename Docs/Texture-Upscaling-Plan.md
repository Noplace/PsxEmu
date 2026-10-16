# Texture upscaling: a texture cache, replacement packs, and an AI upscaler

**Status: phase 0 done 2026-10-16.** ONNX Runtime and DirectML load and run here, and `.pth`
models convert to ONNX, matching their authors' own exports. Real-ESRGAN's animevideo-v3 (BSD-3)
is the default model: 22-58 ms for a 256x256 texture at 4x. RTX Video Super Resolution runs on
the RTX 4060 at 5.5 ms for the same, and looks as good, but only on RTX cards; phase 4 settles
it on real textures ([Phase 0, as built](#phase-0-as-built-2026-10-16)). Nothing in the
emulator itself has changed yet. The hardware rasteriser draws polygons at up to 8x,
but the game's textures are still the console's 4-, 8- and 15-bit images, point-sampled. Each
texel becomes a block of 8x8 pixels at 8x. This plan gets better textures in four ways, all opt-in
and all built on one texture cache:

- **Dumping**: textures saved as PNGs.
- **Replacement packs**: PNGs loaded in their place.
- **An AI upscaler**: ONNX Runtime on DirectML, on any Direct3D 12 card, making those PNGs itself.
- **Texture filtering in the shader**: bilinear, xBR and others, with no cache needed.

NVIDIA's RTX Video Super Resolution is an optional second backend for the AI upscaler, kept only
if it measures better.

**Decided 2026-10-16:**

- **Order:** the cache with dumping and replacement first, then the AI upscaler through DirectML.
- **RTX Video:** an optional NVIDIA backend behind the same interface. It is measured against the
  DirectML one and offered only if it is better.
- **Texture filtering:** a cheap phase of its own. It depends on nothing else here, so it can be
  built at any point.
- **An enhancement,** like PGXP and DLSS. It is off by default and off whenever accuracy is asked
  for (the two goals: accuracy and enhancement).

DLSS was the first idea, and it cannot do this; the first section says why.

---

## The conclusion first

- **DLSS cannot upscale textures.** DLSS Super Resolution, DLAA and DLSS 5 all work on the
  finished screen, not on texture images. They are temporal: each frame is rebuilt from the frames
  before it, using depth and motion. Their scale ratios are fixed at 3x or less, and one feature
  instance serves one pair of sizes. A texture has no history, no motion and no depth. Calling DLSS
  once per texture, with the history reset every time, would throw away the method that makes DLSS
  good. DLSS already runs here on the whole picture (Docs/DLSS-Plan.md), and that picture includes
  the textures as drawn.
- **NVIDIA's single-image upscaler is RTX Video Super Resolution** (the RTX Video SDK 1.1). It
  works on Direct3D 11, Direct3D 12, Vulkan and CUDA, on RTX 20 cards and newer. Its model is
  trained on compressed video, not on palette art with hard edges and see-through holes. It cannot
  run on the Radeon. So it is the second backend, and it has to win a comparison to be offered.
- **The first backend is an ONNX model on DirectML**: ONNX Runtime (MIT) with its DirectML
  execution provider. That runs on any Direct3D 12 card, the Radeon 780M and the RTX 4060 alike,
  and loads any `.onnx` upscaler. Pixel-art models and Real-ESRGAN's compact models are published
  already trained, so there is nothing to train. Microsoft now keeps DirectML in sustained
  engineering, and new work goes into Windows ML, which offers the same ONNX Runtime API. Moving to
  Windows ML later changes how the session is made, not the rest of this plan.
- **Most of the work is the cache, and it belongs to the rasteriser, not to the model.** The
  PlayStation has no texture objects. A draw names a 256x256 texture page, a colour mode and a
  CLUT, and the shader reads VRAM texel by texel (`raster_common.h`, `PsDraw`). Before a texture can
  be swapped for anything, the emulator has to know which texture a draw is using:
  - which image it is, by a name that is stable from one run to the next
  - whether that image is still in VRAM unchanged
  - and where its replacement is
  Dumping, replacement packs, the AI upscaler and RTX Video all sit on that cache. The AI upscaler
  is simply another source of replacement images, written to disk, so a second play of a game has
  them all from the start.
- **Texture filtering is separate and cheap.** Bilinear, JINC2, xBR, Scale2x/3x and MMPX run on the
  texels already decoded in `PsDraw`, with no cache at all. This is the "texture filtering"
  item in Gaps.md.
- **The machine never sees any of it**, the rule PGXP and DLSS follow. Native VRAM, DMA, the
  machine's timing and save states are untouched. A replaced texel can change what a game later
  reads back from a picture it drew, as PGXP and true colour can already. The twelve-disc table's
  instruction and sector counts are the check.
- **Both hardware rasterisers get it.** Direct3D 11 and Direct3D 12 share `raster_common.h`'s
  shaders. The AI upscaler runs on its own thread and device, so it does not depend on which
  rasteriser is drawing. The software rasteriser gets none of this.
- **Size:** roughly 10-14 sessions in all. The cache with dumping and replacement is about half.
  The one uncertain part is how many of a game's textured pixels can be matched to a named
  texture; phase 1 measures that before anything is built on it.

---

## What there is (2026-10-16)

| | What | Where it runs | Here |
|---|---|---|---|
| **ONNX Runtime + DirectML EP** | loads an `.onnx` model; DirectML 1.15.2, ONNX opset 20 | any Direct3D 12 card | the main backend |
| **Windows ML** | the same ONNX Runtime API; picks the execution provider per card | Windows 11 24H2 and later, best | later, if DirectML stops being enough |
| **RTX Video SDK 1.1** | Super Resolution, artefact reduction, SDR-to-HDR; models for video | RTX 20 and newer; D3D11, D3D12, Vulkan, CUDA | the second backend, measured |
| **Raw DirectML** | the network built by hand from DirectML's operators, with no ONNX Runtime | any Direct3D 12 card | the fallback if ONNX Runtime's size or licence rules it out: one architecture only |
| **DLSS 4.5 / DLSS 5** | temporal, screen-space | RTX | not applicable to textures (above) |

**Candidate models**, chosen in phase 0 by licence first and by how they look second:

- **Real-ESRGAN's compact model** (`SRVGGNetCompact`, 2x and 4x, BSD-3). It is small: 16-32
  3x3 convolutions of 64 channels and a pixel shuffle. Its weights are a few MB, and it is quick on a
  laptop card. It was trained on photographs, so it softens hard pixel edges.
- **Pixel-art and game-texture models from OpenModelDB.** These are trained on exactly this kind of
  image, but their licences vary, and many are non-commercial. Only one whose licence allows
  shipping it, or at least downloading it on the user's behalf, is used.
- **The user's own `.onnx` file**, from a folder, for anyone who wants to try another model.

---

## What the PlayStation gives, and what is missing

| A texture cache wants | The PlayStation | Where it comes from here |
|---|---|---|
| A texture object | a page (64x256 VRAM pixels at 4-bit, 128 at 8-bit, 256 at 15-bit), a mode and a CLUT, named in each draw | **new**: an *upload* (below), seen through a mode and CLUT |
| A stable name | nothing; games put the same image at different places in VRAM | **new**: a hash of the uploaded pixels and a hash of the CLUT |
| To know it changed | nothing; any CPU upload or draw can overwrite it | `RasterBackend::Written` for uploads; the rasteriser's own dirty rectangles for draws |
| Its extent | nothing; a draw only has its own UVs | the upload's rectangle; the draw's UV bounds must lie inside it |
| Transparency | texel 0000h is not drawn; bit 15 marks semi-transparency (and sets the mask) | carried in the replacement's alpha (below) |
| Its colours | 15 bits, or a 4/8-bit index into 15-bit CLUT entries | decoded to RGBA8 for dumping and upscaling |

---

## The design

### Uploads: what a texture is

A game loads a texture the way it was authored: one CPU-to-VRAM transfer of a TIM image's pixels,
and one of its CLUT. That transfer is the unit here.

- **At `Written(x, y, w, h)`**, on the machine thread while the rasteriser is idle, the rectangle
  is recorded with a hash of its pixels (XXH3-64 or similar: about 10 us for 128 KB). It replaces
  any record it overlaps. This happens only while a texture feature is on.
- **A draw into a recorded rectangle** - any triangle, rectangle, fill or copy whose destination
  overlaps it, as the rasteriser's dirty rectangles already track - retires the record. A texture
  the GPU drew (render-to-texture, a blurred copy of the screen) is never named, and is sampled as
  it is today.
- **Films are uploads too.** Each MDEC frame is recorded and replaced at the next frame's upload.
  Nothing is decoded or hashed further unless a draw samples it.
- **Save states** do not record the cache. After a state is loaded (`Reloaded`), nothing is named
  until the game uploads again, and pictures are native until then. Phase 1 measures how often that
  matters; if it does, the record list goes into the rasteriser's own side of the state, never the
  machine's.

### Sources: an upload seen through a CLUT

- **The key**: the upload's hash, the colour mode, the CLUT's hash (16 or 256 entries, read from
  native VRAM at draw time), and where the texture page puts the upload's left edge in texels.
  The same image drawn with two palettes is two sources.
- **The name on disk**: `<upload hash>-<CLUT hash>-<mode>`, 16-bit hex words. Only the image and
  its palette go into it, never the image's place in VRAM, so a pack works wherever the game puts
  the image.
- **Per draw**, on the rasteriser thread, when a textured job is batched:
  - Work out the job's UV bounds, after the texture window.
  - Look up the upload under its texture page (a 16x2 grid of page lists).
  - The source applies only if one upload holds the whole bounds and the CLUT is not dirty.
  - The job then carries the source's slot (a sixth vertex word, or an index into a table of
    slots: chosen in phase 2 by what `PsDraw`'s inputs have room for).
  - A job that spans two uploads, samples a dirty page, or has no replacement draws exactly as
    today.
- **Counted**: textured pixels drawn from a replacement, from native because the draw spanned two
  uploads, because it sampled a drawn page, and because it had no replacement. `boot_runner
  --textures` reports these counts.
- **The shader**: with a slot, `PsDraw` samples the slot's image at the texel's fine position -
  `t` before `floor`, the interpolated UV it already has - times the image's scale. This uses the
  same texture window, wrapping and sprite flips. Without a slot, nothing changes.

### Replacement images

- **RGBA8 PNG, any whole multiple of the source's size** (1x-8x), read through WIC, which the
  screenshots' GDI+ already depends on.
- **Alpha has three meanings**, because the PlayStation has three kinds of texel:
  - **0**: not drawn (texel 0000h)
  - **255**: drawn, bit 15 clear
  - **128**: drawn with bit 15 set (semi-transparent, and setting the mask)
  A dumped image uses exactly these three values. A replacement's alpha is rounded to the nearest
  of them.
- **Drawn at 8 bits a channel**, through `PsDraw`'s true-colour path, and not dithered, whatever
  the true-colour setting. Cutting an upscaled image back to 15 bits would band it.
- **Kept on the card in atlas pages**: 4096x4096 RGBA8 each, shelf-packed, evicted by least
  recently used, within a budget setting (default 512 MB). Loading is on a thread of its own. A
  source keeps drawing natively until its image is resident, so a replacement can arrive a few
  frames late, but the game never waits for it.

### Dumping

- **Each source the first time it is drawn**, decoded to RGBA8 with the alpha rule above. The PNG
  is written by a background thread to `textures\<serial>\dump\<name>.png` beside psxemu.ini (the
  serial is the per-game settings' key, as `cheats\<serial>.cht` uses).
- **The whole upload is dumped, not just the part the draw sampled**, so a tile sheet is dumped as
  one image and upscaled as one image.
- **Skipped** if the file is already there, and for any source smaller than 8x8 (configurable).
  This keeps out single-colour fills and font glyphs uploaded one at a time.

### Replacement packs

- **`textures\<serial>\replace\`**, read once at boot: a map from name to file. Images load when
  first drawn.
- **`textures\<serial>\pack.ini`**, optional:
  - aliases (one image for several names, for palette swaps)
  - names never to replace (fonts, the HUD)
  - whether to filter the replacement when it is sampled (nearest or bilinear)
- **Never machine state.** A pack changed while a game runs is picked up from Video > Reload
  Textures.

### The AI upscaler

- **A producer of replacement images**, behind one interface: `TextureUpscaler::Upscale(rgba, w, h,
  scale) -> rgba`. There are two backends: `OnnxUpscaler` (ONNX Runtime with DirectML) and
  `RtxVideoUpscaler`.
- **Its own thread and its own Direct3D 12 device** on the rasteriser's card. It is independent of
  the rasteriser's API and of Streamline, which only wraps the renderer's device. It takes RGBA in
  and gives RGBA out: a 256x256 texture is 256 KB in and 4 MB out at 4x. Sharing the D3D12
  rasteriser's device is an optimisation, measured before it is attempted.
- **The queue**: a source with no file in `replace\` or the cache is queued at its first draw,
  nearest the centre of the screen first. A per-frame budget (milliseconds on the card) keeps the
  game at full speed. Results are written to `textures\<serial>\ai\<model>-<scale>x\<name>.png`.
  On the next run they are simply there, so the AI folder is a replacement pack the emulator
  generated itself. Video > Upscale All Dumped Textures runs the queue over a whole dump folder,
  with a progress dialog.
- **Before inference**:
  - Transparent texels take the colour of the nearest opaque one (a dilation), so the model does
    not pull black halos into the edges of cut-out sprites.
  - The image is padded by repeating its edges, or by wrapping when the game uses a texture window
    over it, by at least the model's receptive field.
  - Small sources are packed into one padded atlas per call, so a hundred 16x16 glyphs are one
    inference rather than a hundred.
- **After inference**:
  - The alpha is upscaled separately: coverage is smoothed and thresholded at 0.5, so cut-out
    edges are smooth too; the bit-15 marking is taken by nearest.
  - Each source is cut back out of the atlas.
- **The model** is fetched by the emulator into `models\` beside it, as FSR's DLLs are, with its
  licence beside it. ONNX Runtime's DLL and DirectML's redistributable come the same way. They are
  never committed; `fetch_onnxruntime.ps1` does it for the tools.
- **Palette animation** (water, lava, flashing text) makes a new CLUT hash every few frames. Each
  key is one inference, cached, and the set is small and repeats. If a game cycles more than it is
  worth, `pack.ini` can mark the image "never upscale"; phase 3 counts how often that happens.

### RTX Video Super Resolution

- **The same interface, on an RTX card**, through the RTX Video SDK's Direct3D 12 path on the
  upscaler's own device.
- **What phase 0 found:**
  - It takes any size to any size, 16x16 included.
  - Quality runs from 0 (bicubic) to 4.
  - It may ship inside the emulator, but none of its SDK may be committed.

  It has no alpha, so the same dilation, padding and separate alpha as the ONNX backend apply.
- **Offered only if it wins** (phase 4): on the dumped textures of the 2D and 3D discs below,
  side by side with the best ONNX model, at the same scale, and judged by eye, since nothing
  numerical says which upscale of a sprite is better. If it does not win, it stays in the code
  behind a setting in psxemu.ini and is not on the menu.

### Texture filtering in the shader

Independent of everything above: a filter over the texels `PsDraw` already decodes.

- **Modes**: Nearest (today), Bilinear, JINC2, xBR, Scale2x, Scale3x, MMPX, each with a
  "binary alpha" variant that thresholds coverage rather than blending it.
- **Neighbours are decoded the same way as the texel itself** - through the CLUT for 4- and 8-bit
  modes - with the texture window's wrapping. Up to 12 taps for xBR.
- **Clamped to the primitive's own UV bounds**, so a sprite never pulls in its neighbour on the
  sheet. The bounds go to the shader with the job, as the sources' slot does in phase 2; the two
  phases share that word.
- **Transparent texels** are filtered as coverage. A pixel whose filtered coverage is under half is
  not drawn, and the colour is the average of the opaque taps only, so no dark fringe appears.
- **Semi-transparency** follows the nearest texel's bit 15, so blending does not flicker along an
  edge.
- **Separate from the presenter's filters**, which work on the finished picture. The two can be
  combined.
- **With a replacement present**, the replacement's own sampler setting applies and this does not.

### Settings and the menus

- **Settings > Video, a Textures group**, needing the hardware rasteriser:
  - **Texture filter**: Nearest, Bilinear, JINC2, xBR, Scale2x, Scale3x, MMPX (and binary-alpha
    variants)
  - **Dump textures**
  - **Use replacement textures**
  - **AI upscaling**: Off, 2x, 4x; model; backend (DirectML on any card, RTX Video on RTX cards
    only, once it has earned its place)
  - **Video memory budget**
  - A line saying what is running: sources replaced and upscaled, the queue's length, or why
    nothing is.
- **Per game**: `texture_filter`, `texture_replacement`, `texture_ai_scale`. A pack belongs to a
  game anyway.
- **Video > Reload Textures**, **Video > Open Texture Folder**, **Video > Upscale All Dumped
  Textures**.
- **Emulation > Show Timings**: the upscaler's milliseconds a frame and its queue.
- **Help > About**: ONNX Runtime's and the model's licences, and NVIDIA's for RTX Video.

---

## Phases

| # | What | Done when | Card | Rough size |
|---|---|---|---|---|
| 0 (**done**) | The SDKs read and licences settled: ONNX Runtime, the DirectML redistributable, the RTX Video SDK, and two or three candidate models. `ai_probe`: each backend's sizes, factors and cost on one 256x256 texture, one device per run | a model chosen and every file's licence allowing what we do with it - done for ONNX Runtime, DirectML, RTX Video and seven models | Radeon; 4060 once | 1 |
| 1 | Uploads and sources, with no drawing changed: records at `Written`, retired by draws, keys, `boot_runner --textures` counts, dumping | the share of textured pixels nameable measured on the 26 discs; the twelve-disc table's instruction and sector counts identical with dumping on; dumps reviewed on five discs | Radeon, WARP | 2-3 |
| 2 | Replacement: the slot in `PsDraw`, atlas pages, the loader thread, `pack.ini`, the menus | **the identity check**: a dump loaded back as its own pack at 1x, and nearest-upscaled at 2x-8x, draws every picture of the table bit for bit as without it on WARP (with the replacement cut to 15 bits and the console's colour path, a debug switch); a hand-edited texture seen in game | Radeon, WARP | 2-3 |
| 3 | The AI upscaler on ONNX Runtime and DirectML: thread, device, queue, dilation, padding, atlases, alpha, cache folder, Upscale All | full speed with the queue running on Ridge Racer and two 2D discs, on both cards; reviewed on the 26 discs' dumps; no halo on cut-out sprites | Radeon, then 4060 for a few long runs | 2-3 |
| 4 | RTX Video as the second backend, and the comparison | the side-by-side reviewed; RTX Video either on the menu or left in psxemu.ini only, and this plan says why | 4060 | 1-2 |
| F | Texture filtering in the shader: the modes, UV bounds, coverage | Nearest identical to today on the whole table; each mode reviewed on five discs; full speed at 4x on the Radeon | Radeon, WARP | 1-2 |

Phase F can come at any point. If it comes before phase 2, it adds the UV-bounds word that phase
2's slot then shares.

### Phase 0, as built (2026-10-16)

**What was built:**

- **`graphics/upscale/texture/fetch_onnxruntime.ps1`** fetches two NuGet packages from nuget.org:
  `Microsoft.ML.OnnxRuntime.DirectML` 1.24.4 (12.5 MB) and `Microsoft.AI.DirectML` 1.15.4
  (202 MB, every platform's builds). Each package is checked against the SHA-256 it had that day.
  It keeps three DLLs - `onnxruntime.dll` 17.3 MB, `onnxruntime_providers_shared.dll`,
  `DirectML.dll` 18.5 MB - and the licences, in `Temp\onnxruntime\1.24.4`. Run again, it does
  nothing.
- **`graphics/third_party/onnxruntime/`**: ONNX Runtime's C API headers, MIT. DirectML.h is the
  Windows SDK's.
- **`graphics/upscale/texture/onnx_runtime.h/.cpp`** loads the DLLs:
  - It checks Microsoft's Authenticode signature on all three DLLs, by the certificate's
    organisation: ONNX Runtime is signed "Microsoft Corporation", DirectML "Microsoft Windows
    Publisher".
  - It loads DirectML first, by full path. Windows has its own DirectML 1.15.5 in System32, which
    ONNX Runtime would otherwise find.
  - `OnnxUpscaler` runs a model on a Direct3D 12 device it is given, through
    `SessionOptionsAppendExecutionProvider_DML1` with a compute queue of its own. It takes float
    or half models and pictures of any size, or the one size a model insists on.
- **`graphics/upscale/texture/torch_model.h/.cpp`** reads a PyTorch `.pth` file:
  - It reads both layouts - the zip PyTorch has saved since 1.6, and the older single stream of
    pickles (PixelPerfect is one).
  - It reads the pickle as data. Nothing in the file is ever called; anything other than
    tensors, dicts and tuples stays an inert placeholder.
  - It writes ESRGAN (both namings, 1x-8x, pixel-unshuffled inputs too) and Real-ESRGAN's compact
    model as ONNX: opset 17, height and width left free, by a small protobuf writer.
- **`graphics/upscale/texture/texture_upscaler.h`**: what a backend is (RGB in, a whole number of
  times larger out), so both backends go through the same tests.
- **`graphics/upscale/texture/rtx_video.h/.cpp`**: RTX Video Super Resolution as the second
  backend, through NGX on a Direct3D 12 device it is given, with a queue of its own. It checks
  NVIDIA's signature on `nvngx_vsr.dll` before NGX loads it. It is built only where the SDK is
  unpacked in `Temp\rtxvideo\` (below); anywhere else it compiles to a backend that says it is
  not there.
- **`tools/ai_probe.cpp`** (built by `build_tools.bat`; Test-Suite.md says what each option does).
- **`build_tools.bat`**: its `if exist ... ^` copies had never run. A caret at the end of a line
  escapes the next line's first character too, so the escaped space was taken for the command
  (`' ' is not recognized`). They are blocks now - FSR's DLL copy too.

**Checked:**

- **The conversion matches the authors' own exports.** Two models are published as both `.pth`
  and ONNX by their authors:
  - StarSample (a compact model): the largest difference over a 64x48 picture is 0.000000.
  - Nomos WebPhoto (an ESRGAN, its weights stored as half floats): 0.000003, a thousandth of an
    8-bit step.

  The old ESRGAN naming is the same graph under other names. The two models that use it came
  out at ESRGAN's 16.70M parameters and drew the pictures below.

**The candidates**, chosen from OpenModelDB's 600-odd for a licence that allows shipping them or
fetching them for the user:

| Model | Licence | Kind | Published as |
|---|---|---|---|
| Real-ESRGAN animevideo-v3 | BSD-3 | compact, 4x, 0.62M | `.pth`, xinntao's GitHub release |
| Real-ESRGAN general-x4v3 | BSD-3 | compact, 4x, 1.21M | `.pth`, the same |
| PixelPerfect V4 | WTFPL | ESRGAN, 4x, 16.7M, pixel art | `.pth`, Google Drive |
| HDCube | CC0 | ESRGAN, 4x, 16.7M, GameCube textures | `.pth`, GitHub |
| StarSample V1 | CC0 | compact, 2x | `.pth` and ONNX |
| ModernSpanimation V1 | MIT | SPAN, 2x | ONNX, fp32 and fp16 |
| AoMR | CC-BY-4.0 | MoSR, 2x, game textures | ONNX, 256x256 only |

Every pixel-art model on OpenModelDB with an ONNX file is non-commercial (CC-BY-NC-SA). Every
permissive pixel-art model is `.pth` only. That is why the converter exists. It also means the
emulator can fetch a model from its author's own release and convert it in place, so we never host
a model ourselves.

**What each costs** - milliseconds a call from floats in to floats out, median of 10 after the
first:

| Model | 64x64, Radeon | 128x128, Radeon | 256x256, Radeon | 256x256, RTX 4060 |
|---|---|---|---|---|
| Real-ESRGAN animevideo-v3, 4x | 10.7 | 21.7 | 58.0 | 21.5 |
| Real-ESRGAN general-x4v3, 4x | 19.1 | 37.1 | 104.6 | 30.7 |
| StarSample, 2x | 10.2 | 21.8 | 53.0 | |
| ModernSpanimation fp16, 2x | 14.2 | 25.2 | 56.2 | |
| PixelPerfect / HDCube, 4x | 245 | 960 | resets the card | 492 |
| AoMR, 2x | | | 1,960 | |
| RTX Video Super Resolution, quality 4, 4x | | | not an RTX card | 5.5 |

- **A call under 64x64 costs 2-14 ms whatever its size**, so small textures go through packed
  together, as the design says.
- **The first call at each new size compiles the graph**: 13 ms to 1 s on the Radeon, once
  909 ms on the 4060. A run that always calls with the same few sizes (64, 128 and 256, padded up
  to them) pays this once a size, not once per texture.
- **An ESRGAN on 256x256 kept the Radeon busy past Windows' two seconds, and Windows reset it**
  (887A0006, twice; the screen hangs off the Radeon). A heavy model goes through in tiles no
  larger than the card can finish well inside that: 64x64 on the Radeon, about 400 ms each.
- **The 4060 is only about 3.5 times the Radeon on PixelPerfect**, which suggests DirectML's float
  path is not reaching its tensor cores. A half-float export is the thing to try in phase 3.
- **RTX Video takes any size** - 16x16 to 512x512 were all accepted - and costs next to nothing:
  0.6 ms for 16x16, 1.7 ms for 128x128, 5.5 ms for 256x256, 19.7 ms for 512x512, about four
  times as fast as the anime model on the same card. The first call at a size costs 4-60 ms; the
  feature takes 1.1 s to make, once.

**How they look**: five frames at native size (Breath of Fire III's sprites over painted
backgrounds; Legend of Mana's painted backgrounds; Ridge Racer's road and buildings) were upscaled
by each model - the ESRGANs in 64x64 tiles, the rest whole - and compared at 4x. Comparison
sheets are in `Temp\texup\cmp_*.png`.

- **Real-ESRGAN animevideo-v3 is the best default.** It is clean on all three games: dithering
  gone, edges and text crisp, a slightly painted look. It is also the cheapest of the 4x models.
- **Real-ESRGAN general-x4v3** is a little sharper and keeps more texture, at 1.4-1.8 times the
  cost. It is the "quality" choice.
- **PixelPerfect** gives a smooth, vector-like look that suits sprites, but costs 20-45 times as
  much. It is an option for a discrete card, not a default.
- **HDCube and StarSample** turn the PlayStation's dither into noise and pattern. They are out.
- **ModernSpanimation** is soft, and 2x only.
- **RTX Video at quality 4** is as good as the best of them:
  - On Ridge Racer it is the crispest 4x result: clean window frames, and no painted look.
  - On Breath of Fire III it keeps more of the wood's grain than the anime model, with the dither
    gone; its sprites' outlines are a little less clean than the anime model's.
  - On Legend of Mana it smooths less than the ESRGANs, and looks the most natural.

  Its model is made for video, and that has not hurt it here, even on hard pixel edges.

The frames carry the GPU's dither and Gouraud shading, which real textures do not, so phase 1's
dumps are the real test. The choice is reviewed again there.

**Decided by phase 0:**

- **The default model:** Real-ESRGAN animevideo-v3. **The quality model:** general-x4v3. Both are
  BSD-3, fetched from xinntao's GitHub release (SHA-256 `b8a83768...` and `8dc7edb9...`) and
  converted on the user's machine, with the licence beside them.
- **PixelPerfect** is offered on cards with dedicated memory, in tiles. It is fetched from its
  author's Google Drive link, which may need a fallback.
- **What ships with the emulator, fetched on first use:** ONNX Runtime and DirectML, 36 MB. MIT,
  and Microsoft's DirectML licence, which lets an application or game redistribute it. The data
  clause in DirectML's licence is to be noted in the About box.
- **RTX Video stays the second backend, and is not ruled out.** It is the fastest by far and
  looks as good as the best, but it runs only on RTX cards, and its SDK cannot be fetched for
  the user. Phase 4 compares it with the ONNX models on real texture dumps rather than frames.
- **How RTX Video can ship** (the RTX SDK licence, the one DLSS ships under):
  - `nvngx_vsr.dll` (19 MB, the SDK's `rel` build, signed by NVIDIA, version 1.6) and the
    linked NGX library may be shipped inside the emulator as object code.
  - None of the SDK may be committed: its headers and libraries are NVIDIA's own, not open
    like Streamline's MIT headers. The guide is marked confidential, so this plan only
    summarises it.
  - A release build is made on a machine that has the SDK, and ships the DLL beside the
    executable.
  - NVIDIA's marks go in the About box.
  - NGX asks for notice to NVIDIA before a *commercial* release, which this is not.

**The RTX Video SDK** (1.1.0) comes from NVIDIA's developer site, behind its sign-in, and is
unpacked by hand into `Temp\rtxvideo\`. It holds:

- headers and the NGX static library (`nvsdk_ngx_d.lib` for /MD)
- `nvngx_vsr.dll` and `nvngx_truehdr.dll`, each in a `dev` build and a `rel` build. The `dev` ones
  draw a watermark into their output, so only `rel` is ever measured or shipped.

VSR takes RGBA8 in and gives RGBA8 into a UAV texture out, any size to any size, at quality 0
(bicubic) to 4. It needs driver 550.58 or later and an RTX 20 card or newer. Its feature DLL is
updated by the driver's NGX runtime, so what ships is a floor, not a fixed version.

**Not done:**

- **RTX Video's quality levels 1-3**, and any texture-sized comparison: the one 4060 run measured
  quality 4 only.
- **Alpha through a real cut-out sprite.** The path is built - dilation, then coverage through the
  model and a threshold - but no frame had a transparent texel. It waits for phase 1's dumps.

---

## How it will be verified

- **The machine untouched**: the twelve-disc table's instruction and sector counts identical with
  each phase's feature on, as for PGXP and DLSS.
- **The identity check** (phase 2) is the strongest test of the whole path. A pack identical to the
  game's own textures, at any scale with nearest upscaling, must draw exactly today's pictures on
  WARP. Any mistake in the keys, UV coordinates, texture window, flips, alpha, semi-transparency or
  atlas addressing shows up as a pixel difference (`ppm_diff`).
- **Coverage, measured, not assumed**: `boot_runner --hw-raster --textures` on the 26 discs, giving
  the share of textured pixels nameable, and why the rest are not. If the share is low on 3D discs,
  phase 2 says so before phase 3 builds on it.
- **Full speed**: Show Timings on Ridge Racer, Wild Arms and a 2D disc, with the AI queue running,
  on the Radeon and on the 4060 (one device per run there).
- **By eye**, for the upscaled and filtered textures: stills at the table's checkpoints, and a
  sitting of each in motion, as the DLSS and FSR reviews still are.

---

## Risks, and the decisions taken

- **Coverage may be low in 3D games.** Many 3D games stream textures into shared pages piece by
  piece, and a polygon's UVs may cross two uploads. Phase 1 measures this first. If it is low, a
  second identity - the texture page and CLUT, hashed together - is the fallback (it is less stable
  across runs, but it covers everything). That fallback is a decision for after phase 1, not
  before.
- **AI output on tiled 2D.** A tile map upscaled as one sheet can still show seams where two tiles
  that sit side by side on screen are far apart on the sheet. `pack.ini` can exclude a sheet, and
  the binary-alpha filter is the alternative for it.
- **Fonts and HUDs** often look worse upscaled by a photographic model. They can be excluded by
  name, and a pixel-art model handles them better; phase 3's review decides the default list.
- **DirectML is in sustained engineering.** It still ships and runs, but new work is going into
  Windows ML. Using ONNX Runtime's API keeps that move small.
- **Size of what ships**: ONNX Runtime and DirectML are tens of MB, as FSR's DLLs are. They are
  downloaded when asked for, never committed, and measured in phase 0.
- **Licences**: ONNX Runtime is MIT; DirectML's redistributable and the RTX Video SDK's terms are
  read in phase 0; each model's licence decides whether it can be offered at all.
- **DuckStation** (CC-BY-NC-ND) has a texture cache with dumping and replacement
  (`gpu_hw_texture_cache.cpp`) and the same list of texture filters. It is read for the design only
  - hashing uploads, retiring sources on writes, the shape of a replacement folder - and none of
  its code is copied, as with the hardware rasteriser.

---

## Sources

- [RTX Video SDK](https://developer.nvidia.com/rtx-video-sdk) and its
  [getting started](https://developer.nvidia.com/rtx-video-sdk/getting-started) page (version 1.1:
  APIs, cards, effects).
- [DirectML](https://learn.microsoft.com/windows/ai/directml) - in sustained engineering, new work
  in Windows ML - and ONNX Runtime's
  [DirectML execution provider](https://onnxruntime.ai/docs/execution-providers/DirectML-ExecutionProvider.html)
  (DirectML 1.15.2, opset 20).
- [Dolphin's request for ESRGAN texture upscaling](https://bugs.dolphin-emu.org/issues/11614),
  closed there on the grounds that the model would need training. Already-trained models answer
  that; the cost and the cache are the real questions, and this plan measures both.
- DuckStation's `src/core/gpu_hw_texture_cache.cpp` and `GPUTextureFilter` (`types.h`), in
  `Temp/duckstation`, read for the design only.
