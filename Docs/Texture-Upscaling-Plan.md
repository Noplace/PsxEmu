# Texture upscaling: a texture cache, replacement packs, and an AI upscaler

**Status: phases 0 to 4 done (2026-10-16 to -17).** The emulator upscales a game's
textures itself as they are read, 2x or 4x with Real-ESRGAN on DirectML, and draws them like a
hand-made pack: Settings > Video > Textures > AI upscaling, per game, the files fetched by the
emulator. It runs on a card the game is not drawing on when there is one, at full speed on Ridge
Racer, Castlevania and Valkyrie Profile; on one card alone it takes a tenth of that card's time
([Phase 3, as built](#phase-3-as-built-2026-10-17)). NVIDIA's RTX Video Super Resolution runs as a
second backend, but on the 26 discs' textures it is softer than either Real-ESRGAN model and
only 1.5 times as fast. It is in the AI upscaling list on NVIDIA cards, the user's choice
([Phase 4, as built](#phase-4-as-built-2026-10-17)). Every textured draw is named by the
texture it reads, the same name every run, on either rasteriser: 95.2% of textured pixels on the 26 discs, 22 of them at 100%, the rest
textures the GPU drew. A game's textures are dumped as PNGs, and a pack of PNGs named the same -
any of 1x to 8x their size - is drawn in their place by the hardware rasteriser: Settings > Video
> Textures, per game. A dump drawn back as its own pack draws every picture of the twelve discs
exactly as without it, at 1x and 4x ([Phase 2, as built](#phase-2-as-built-2026-10-17),
[Phase 1](#phase-1-as-built-2026-10-16)). ONNX Runtime and DirectML load and run here, and `.pth`
models convert to ONNX, matching their authors' own exports. Real-ESRGAN's animevideo-v3 (BSD-3)
is the default model: 22-58 ms for a 256x256 texture at 4x. RTX Video Super Resolution runs on
the RTX 4060 at 5.5 ms for the same, and on whole frames looked as good, but only on RTX cards
([Phase 0, as built](#phase-0-as-built-2026-10-16)). Without a pack, the
hardware rasteriser draws polygons at up to 8x, but the game's textures are still the console's
4-, 8- and 15-bit images, point-sampled. Each
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

### Records: what a texture is

A game loads a texture the way it was authored: CPU-to-VRAM transfers of a TIM image's pixels and
of its CLUT. Those transfers are the unit here: *records*, rectangles of VRAM that hold what a
game uploaded, each with a hash of its pixels. Records never overlap. Phase 1 started from "one
transfer, one record"; the 26 discs showed that is too simple, and the rules became
(`render/texture_tracker.h`, phase 1 below):

- **Joined strips.** A transfer that carries straight on from the last one recorded - the same
  columns from the row it ended or began at, or the same rows from the column - joins it. Ridge
  Racer loads images 32 rows at a time, and one a row at a time from the bottom up; Gran Turismo 2
  loads all of VRAM in 1024x16 strips.
- **Patches.** A transfer inside a live record patches it, and the record's hash is taken again: a
  font sheet with a few glyphs changed. Films, uploaded strip by strip over the last frame, are
  patches too.
- **Cut, not retired.** Anything written over part of a record - an upload, or a draw: any job
  whose destination overlaps it - cuts that rectangle out, and what is left stays as up to four
  records. Gran Turismo 2 and Vagrant Story upload half or all of VRAM, then draw the framebuffer
  into it; retiring the whole record would lose every texture beside the framebuffer. Written over
  whole, a record is gone.
- **Copies carry records.** A VRAM-to-VRAM copy of uploaded texels makes a record of what it
  copied: Silent Hill copies its palettes into a corner of the framebuffer.
- **Not machine state.** After a state is loaded (`Reloaded`) nothing is named until the game
  uploads again. None of the 26 discs needed more; if a game does, the records go into the
  rasteriser's own side of a state, never the machine's.

### Sources: what a draw reads

- **A textured job names a source** when the texels it can sample lie:
  - inside one live record
  - or are tiled exactly by several, a *composite*: Castlevania uploads its backdrops as 64x128
    tiles a row of them at a time, and Wipeout one 15-bit picture as two uploads side by side
  - with whatever no record covers all zeros, the clear texel: Einhander's planes read whole
    pages around quarter-page images, the rest VRAM nothing wrote or a long-gone scrap of the
    BIOS's logo. Zeros as every rasteriser would know them (phase 2): native VRAM's where the GPU
    has not drawn since, and where it has, a fill of zeros, a black rectangle or a copy of known
    zeros - a hardware rasteriser draws on the card, and native VRAM may still say anything there.

  For 4- and 8-bit textures its CLUT must lie the same way - in one record, or tiled -
  read only as far as VRAM's right edge (Metal Gear Solid's 8-bit palettes start at x 928).
- **A triangle's far edges are a texel short.** Its furthest texture coordinates are usually not
  read - the fill rule leaves its right and bottom edges out - so one that names nothing is tried
  again a texel short, which also lets that edge alone run off VRAM (Xenogears).
- **The name**: `<image hash>-<CLUT hash>-<mode>`, 16-digit hex. The image hash is the record's,
  or a composite's (its records' hashes and where each lies from their corner). Never the image's
  place in VRAM, so a pack works wherever the game puts the image. The same image with two
  palettes is two sources.
- **Per draw** (phase 2), on the rasteriser thread: the tracker works out what the job names as
  `Gpu` tells it of the job, just before the rasteriser draws it, and keeps that as `current()`;
  the hardware rasteriser asks the pack for the job's replacement as it batches it, and the pack
  looks the name up. A job with one carries four more words to the shader, p[20-23] - a sixth
  `uint4` beside the five the vertex had - saying where the image is on the card and where the
  job's texture page lies in it. Anything else draws exactly as before.
- **Counted**: `boot_runner --textures` reports the textured pixels named and, for the rest,
  why: texels or palette drawn by the GPU, only partly uploaded, never seen uploaded, wrapping,
  outside the drawing area.
- **The shader**: with a replacement, `PsDraw` samples the image at the texel's fine position -
  the texel the texture window and a sprite's flips give, as ever, and where in that texel the
  sub-pixel falls (`t` before `floor` for a triangle) - times the image's scale. Where the image
  has nothing - outside it, or in an 8x8 block it left clear - that pixel is the game's own
  texel. Without a replacement, nothing changes.

### Replacement images

- **A PNG, any whole multiple of the source's size** (1x-8x), read by the core's own reader
  (`render/png_file.h`): every colour type and depth, palettes and tRNS, interlaced or not.
- **Alpha has three meanings**, because the PlayStation has three kinds of texel:
  - **0**: not drawn (texel 0000h)
  - **255**: drawn, bit 15 clear
  - **128**: drawn with bit 15 set (semi-transparent, and setting the mask)
  A dumped image uses exactly these three values. A replacement's alpha is rounded to the nearest
  of them.
- **Drawn at 8 bits a channel**, through `PsDraw`'s true-colour path, and not dithered, whatever
  the true-colour setting. Cutting an upscaled image back to 15 bits would band it.
- **Kept on the card in atlas pages** (`hw_raster/replacement_atlas.h`): the slices of one
  texture array, 4096x4096 RGBA8 each, as many as a budget allows (`texture_budget_mb`, default
  512). An image is kept as its 8x8-texel blocks, and only those with anything in them, a block
  to a cell of a page that holds one scale; a table of words says where each block is. So a
  sparse canvas - half of VRAM with a few cells filled, Spyro's - costs only its cells, at any
  scale, and a block a pack leaves wholly clear draws the game's own texels: a texel a dump never
  saw read stays the game's. Images are evicted whole, longest unused first.
- **Loaded on a thread of the pack's own** (`render/texture_pack.h`), read when a job first
  wants one. A source draws natively until its image is ready, so a replacement can arrive a few
  frames late, but the game never waits for it. The tools read each there and then instead, so
  their pictures do not depend on the loader's speed.

### Dumping

- **Each source as its records' extent in texels**, decoded to RGBA8 with the alpha rule above,
  with **only the 8x8-texel cells that draws read through that palette filled in** and the rest
  left clear. Dumping whole uploads was the first idea and fails at once: Ridge Racer reads one
  1536x256-texel sheet through 91 palettes, a corner each, which was 91 copies of the whole sheet,
  each mostly colour noise.
- **Written when the source is complete**: once one of its records stops being what it was
  (written over, patched, joined), at a state load, or at the end. A copy of each record is taken
  while it is still whole, so what is written is what was read.
- **Where**: in the app, on a thread of the tracker's own, into `textures\<serial>\dump\<name>.png`
  in the data folder, `Documents\My Games\PSXEmu\`, with the save states and memory cards (the
  serial is the per-game settings' key, as `cheats\<serial>.cht` uses); in
  the tools, `boot_runner --texture-dump <folder>`, as each source closes.
- **Added to** if the file is already there (phase 2): the cells read now are filled in over it,
  and it is written only if that changed it. A dump grows as a game is played further, and
  loaded back as a pack it holds every texel a run like the one that made it reads - which the
  identity check needs. Skipped for any source smaller than 8x8 (configurable).
- **Sparse canvases.** Where a game uploads half or all of VRAM as one image (Spyro 3), a source
  is a 2048x512 canvas with a few cells filled. Decided in phase 2: the canvas stays whole, so
  one name is one file, and the card keeps only its filled blocks (Replacement images, above).

### Replacement packs

- **`textures\<serial>\replace\`**, subfolders too, read when the game starts: a map from name to
  file. Images load when first drawn.
- **`replace\pack.ini`**, optional:

  ```ini
  [pack]
  filter = nearest            ; or bilinear: how every image is sampled
  [filter]
  <name> = bilinear           ; ...or one of them
  [alias]
  <name> = <another name>     ; draw <name> with the other's image: palette swaps
  [never]
  <name>                      ; never replace it: fonts, a HUD
  ```

  Names with or without `.png`. Bilinear takes the four pixels round the sample point that the
  image has, colours from the opaque ones, and draws where they cover half or more.
- **Never machine state.** A pack changed while a game runs is picked up from View > Reload
  Textures.

### The AI upscaler

As built in phase 3 (`graphics/upscale/texture/ai_textures.h`); where that differs from what was
planned, the plan's words follow in italics.

- **A producer of replacement images**, behind one interface, phase 0's `TextureUpscaler` (an RGB
  picture in, one a whole number of times larger out), so an ONNX model on DirectML and, in phase
  4, RTX Video plug in alike.
- **Fed by the texture tracker** (`render/texture_upscale.h`): each time draws read more of a
  source, the source's texels and the 8x8-texel cells read so far, handed over on the rasteriser's
  thread in a moment. Only the cells read are upscaled - Ridge Racer's sheet read through 91
  palettes is 91 corners, not 91 sheets - and each only once: what is done is kept, and written to
  `textures\<serial>\ai\<model>-<scale>x\<name>.png`. The next run reads those back, so the folder
  is a pack the emulator made itself, drawn under any hand-made one. *Planned: queued nearest the
  centre of the screen first; it is first come, first served, which has been enough.*
- **In tiles** of 64x64 texels, each with 16 texels round it - the source's own where it has
  them, its edge repeated past them - as the model looks about that far. Nine tiles go through
  as one picture, three by three, so a texture of a few cells shares a call rather than paying the
  2-14 ms a call costs whatever its size. The picture is always the same size, so the model's
  graph is made once.
- **Before**: texels not drawn - clear, or in cells not read through this palette - take the
  colour of the nearest drawn texel, so no dark fringe is drawn into a cut-out's edge.
- **After**: a tile with clear texels has its coverage go through the model too, in a slot of its
  own; smoothed over the pixels round each and cut at a half, so cut-outs have smooth edges.
  Semi-transparency (bit 15) follows the nearest texel. At 2x, the model's 4x halved.
- **Its own thread and its own Direct3D 12 device** - *planned: on the rasteriser's card; built:
  on a card the game is not drawing on, when there is one.* A call holds a card for tens of
  milliseconds, and on the card that draws, the game waits behind it (Phase 3, as built). Its
  results come back as pixels, so any card will do: by default the card with the most memory of
  its own other than the renderer's - a laptop's discrete card while the game draws on the
  integrated - with all of its time; otherwise the renderer's card, a tile a call, held to 10% of
  the time (`texture_ai_share`). `texture_ai_card` names one.
- **The game's dumps, in the background**: each time the upscaler starts, every dump in the
  game's `dump\` folder whose upscaled image is older than it, or missing, goes in behind what
  the game draws. **View > Upscale All Dumped Textures** does every dump, with a dialog saying how
  far it has got; `boot_runner --upscale-folder` the same for the tools.
- **The files** are fetched by the emulator (`ai_download.h`), from Settings > Video > Textures'
  "Get the AI files...": ONNX Runtime 1.24.4 and DirectML 1.15.4 out of their NuGet packages -
  only the entries wanted, by HTTP ranges, each checked against the zip's CRC-32 and Microsoft's
  signature - and the model from Real-ESRGAN's release, checked against its SHA-256 and converted
  to ONNX in `models\`, with its licence. About 20 MB. Never committed; `fetch_onnxruntime.ps1`
  still does it for the tools, and the build copies what it fetched beside the executable.
- **Palette animation** (water, lava, flashing text) makes a new CLUT hash every few frames, and
  each is upscaled once. The upscaler counts images read through more than eight palettes (Phase
  3, as built); `pack.ini`'s `[never]` keeps one from ever being replaced, upscaled or not.

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
  behind a setting in psxemu.ini and is not on the menu. *It did not win, and is on the menu all
  the same*, on NVIDIA cards, as the user asked ([Phase 4, as built](#phase-4-as-built-2026-10-17)).

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

- **Settings > Video, a Textures group** (phase 2 built the first two and the line), beside the
  on-screen display's:
  - **Dump textures as they are drawn**
  - **Draw replacements from the game's pack** - which needs the hardware rasteriser
  - **Texture filter**: Nearest, Bilinear, JINC2, xBR, Scale2x, Scale3x, MMPX (and binary-alpha
    variants)
  - **AI upscaling**: Off, 2x, 4x; model (RTX Video on NVIDIA cards - phase 4)
  - A line saying what is running: textures named and written, replacements and the draws they
    replaced, or why nothing is - later the upscaler's queue.
  - The video memory budget is `texture_budget_mb` in psxemu.ini, not in the window.
- **Per game**: `texture_dump`, `texture_replacement` (phase 2); `texture_filter`,
  `texture_ai_scale` later. A pack belongs to a game anyway.
- **View > Reload Textures**, **View > Open Texture Folder** (phase 2; there is no Video menu -
  the View menu has the Video Settings window), **View > Upscale All Dumped Textures** later.
- **Emulation > Show Timings**: the upscaler's milliseconds a frame and its queue.
- **Help > About**: ONNX Runtime's and the model's licences, and NVIDIA's for RTX Video.

---

## Phases

| # | What | Done when | Card | Rough size |
|---|---|---|---|---|
| 0 (**done**) | The SDKs read and licences settled: ONNX Runtime, the DirectML redistributable, the RTX Video SDK, and two or three candidate models. `ai_probe`: each backend's sizes, factors and cost on one 256x256 texture, one device per run | a model chosen and every file's licence allowing what we do with it - done for ONNX Runtime, DirectML, RTX Video and seven models | Radeon; 4060 once | 1 |
| 1 (**done**) | Uploads and sources, with no drawing changed: records at `Written`, retired by draws, keys, `boot_runner --textures` counts, dumping | the share of textured pixels nameable measured on the 26 discs; the twelve-disc table's instruction and sector counts identical with dumping on; dumps reviewed on five discs | Radeon, WARP | 2-3 |
| 2 (**done**) | Replacement: the slot in `PsDraw`, atlas pages, the loader thread, `pack.ini`, the menus | **the identity check**: a dump loaded back as its own pack at 1x, and nearest-upscaled at 2x-8x, draws every picture of the table bit for bit as without it on WARP (with the replacement cut to 15 bits and the console's colour path, a debug switch); a hand-edited texture seen in game | Radeon, WARP | 2-3 |
| 3 (**done**) | The AI upscaler on ONNX Runtime and DirectML: thread, device, queue, dilation, padding, atlases, alpha, cache folder, Upscale All | full speed with the queue running on Ridge Racer and two 2D discs, on both cards; reviewed on the 26 discs' dumps; no halo on cut-out sprites | Radeon, then 4060 for a few long runs | 2-3 |
| 4 (**done**) | RTX Video as the second backend, and the comparison | the side-by-side reviewed; RTX Video either on the menu or left in psxemu.ini only, and this plan says why | 4060 | 1-2 |
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

### Phase 1, as built (2026-10-16)

**What was built:**

- **`render/texture_tracker.h/.cpp`**, in the core: records, sources, the reasons a draw names
  nothing, and dumping, by the rules under [Records](#records-what-a-texture-is) and
  [Sources](#sources-what-a-draw-reads). It is a `RasterObserver` (`render/raster.h`, new): `Gpu`
  tells it of each job just before and after its rasteriser draws it, with the pixels it put down,
  and of uploads and state loads. Any rasteriser, and nothing the machine sees changes.
- **`render/png_file.h/.cpp`**: PNGs written with the core's own zlib, since the core knows no
  operating system.
- **`boot_runner --textures`** reports the share of textured pixels named and why the rest are
  not; `--texture-dump <folder>` writes the sources; `--texture-log <n>` prints the first n draws
  of each reason with what they read and the records there; `--texture-trace x,y,w,h` prints
  everything that happens to the records in a rectangle. The last two found every rule below.
- **`tools/texture_tracker_test.cpp`**: 45 checks, every rule on a VRAM of its own, and a dumped
  PNG read back texel by texel.

**How the rules were found** - the share named as each rule went in (Ridge Racer's first five over
3,000 frames, the rest on the 26 discs at 6,000 each):

| Rule | What it fixed | Where |
|---|---|---|
| one transfer, one record | 46% of Ridge Racer | - |
| strips joined, down and right | Ridge Racer 97.5% | its 64x32 strips |
| ...up and left too | Ridge Racer 99.4% | an image a row at a time from the bottom |
| patches inside a record | Ridge Racer 99.2%, no more spans | font glyphs changed one at a time |
| a partial upload keeps what it leaves | Ridge Racer 100% | a new image over a sheet's last rows |
| records cut, not retired | Captain Tsubasa 37% to 100%, Xenogears 13% to 98%, Gran Turismo 2 49% to 100%, Vagrant Story 91% to 100% | VRAM uploaded whole, then the framebuffer drawn into it |
| composites | Castlevania 4% to 100% (with tiled palettes), Wipeout 53% to 100% | backdrops in 64x128 tiles |
| a triangle a texel short | Wipeout's last spans, Xenogears 100% | the fill rule's far edges |
| palettes to VRAM's edge | Metal Gear Solid's "wraps" gone | 8-bit palettes at x 928 |
| zeros count as clear | Einhander 83% to 100% | planes reading whole pages |
| copies carry records | Silent Hill 57% to 100%, Area 51 92% to 100% | palettes copied into the framebuffer |

**The 26 discs, at the end:**

- **100%**: Ace Combat 3, Air Combat, Area 51, Bomberman, Captain Tsubasa J, Castlevania SOTN,
  Chrono Cross, Driver 2, Einhander, Final Fantasy VII, Final Fantasy VIII, Gran Turismo 2,
  Legend of Mana, Ridge Racer, Silent Hill, Spyro 3, Tekken 3, Vagrant Story, Valkyrie Profile,
  Wild Arms, Wild Arms 2, Wipeout, Xenogears.
- **Crash 3 95.8%, Vandal Hearts 93.8%, Metal Gear Solid 63.5%**: the rest textures the GPU drew
  - Metal Gear Solid reading its own framebuffer for screen effects, Crash and Vandal Hearts
  textures drawn into VRAM. These stay native by design.
- **All 26: 95.2% of 3.69 billion textured pixels.** The 6,000 frames are mostly intros, logos and
  first scenes; phase 2's review plays further.
- **Dumped**: 4,313 PNGs, 17.7 MB. Spyro's and Valkyrie Profile's are canvases of half or all of
  VRAM with a few cells filled (0.1% and 3%); most others are 30-100% filled.

**The dumps reviewed** (Valkyrie Profile, Castlevania SOTN, Crash 3, Spyro 3, Final Fantasy VII):
the right images in the right colours - Spyro's tiles, Castlevania's composite backdrops and
logo whole, Crash's textures, Valkyrie Profile's character sprites, Final Fantasy VII's credits.
Two things to know:
- Valkyrie Profile's backgrounds come out in bands, the cells its draws read.
- Many of Crash's texels have bit 15 set, so they dump at alpha 128 and look faded. The bit
  only matters to semi-transparent polygons, but a dump cannot tell which those will be.

**The machine unchanged**: the twelve-disc set, 3,000 frames on the interpreter, without the
tracker and with `--texture-dump`. Every line of every report is the same - instructions, CD
sectors, checksums, every count - except two that are the host's, not the machine's:
- the barriers the machine thread found the rasteriser busy at, 1-2 more on most discs and 17 to
  32 on Ridge Racer
- the rasteriser thread's own time

**Cost**: on the rasteriser's thread, where the tracker runs. Ridge Racer's 1.28 million jobs took
it 3.30 s without the tracker and 4.51 s with it - about 1 µs a job, most of it the lookups;
Wild Arms 2's 0.88 million, 5.35 s and 6.24 s. Phase 2 puts the lookup on the app's path and
measures it there; the composite and zero checks run only for jobs nothing simpler names.

### Phase 2, as built (2026-10-17)

**What was built:**

- **`render/texture_pack.h/.cpp`**, in the core: a folder of replacements named as the tracker
  names sources, subfolders too, and `pack.ini` (Replacement packs, above). It is the
  `TextureReplacer` the hardware rasteriser asks (`render/raster.h`, new): the tracker keeps what
  the job being drawn names (`TextureTracker::current()`), and the pack looks it up. Images are
  read on a loader thread of the pack's own as jobs first want them - in the tools, there and
  then - cut into their 8x8-texel blocks, the empty ones dropped, alpha rounded to 0, 128 or 255;
  kept up to a memory budget.
- **`hw_raster/replacement_atlas.h`**: where both rasterisers keep them on the card - a texture
  array of 4096x4096 pages, each holding blocks of one scale, and a table of where each block is
  (Replacement images, above) - and evicts them, longest unused first. Direct3D 11 writes pages
  and table with `UpdateSubresource`, Direct3D 12 with copies on its list; a page array or table
  grown means new descriptors, so Direct3D 12 waits for the card first. Everything batched is
  drawn before a page is written, so nothing still to be drawn reads a block written over.
- **The shader** (`raster_common.h`): a sixth `uint4` on every vertex, p[20-23], the image's
  words; `PsDraw` takes a replaced texel from the image where it has one - nearest, or bilinear
  over the four pixels round the sample point - and the game's own where it has not. A replaced
  texel is drawn at eight bits and not dithered; with `RasterOptions::replacements_as_console`, a
  debug switch, it is cut to 15 bits and drawn as the texel it replaces.
- **`render/png_file.h`** reads PNGs too: every colour type and depth, palettes and tRNS, Adam7.
- **Dumping** adds to files already there (Dumping, above), and in the app writes on a thread of
  the tracker's own.
- **The app**: `host::Machine::SetTextures` keeps the tracker and the pack, and the App gives it
  `textures\<serial>\dump` and `replace` as the game's settings say - `texture_dump`,
  `texture_replacement`, per game, and `texture_budget_mb`. Settings > Video > Textures has the
  two switches and a line saying what is named, written and replaced; View > Reload Textures and
  Open Texture Folder. The Machine's report carries the counts.
- **Run-ahead**: the machine loads a state every frame, and a state load makes the tracker forget
  every upload. So it holds its records as the state is saved (`TextureTracker::Hold`), names
  the frames ahead as ever - their pictures are the ones shown - and the load back brings the
  held records back. Ridge Racer with `--run-ahead 1`: 18,525 jobs replaced in 1,500 frames,
  every one of the real frames' 9,254 and the frames ahead's too, the pictures as without.
- **`boot_runner`**: `--texture-pack <dir>`, `--texture-console`, `--texture-pack-scale k`;
  `--frame-log` above 1x logs the sharper picture's checksum too. `hw_raster_test` has a scene
  for replacements (8 checks), `texture_tracker_test` 70 checks.

**What the identity check found first**: the tracker, run under the hardware rasteriser, named
what the software one did only by luck. Three things were the software rasteriser's:

| What | Why it mattered on the card | Now |
|---|---|---|
| a job that put no pixels down changed nothing | the hardware rasteriser counts no pixels, so nothing ever cut a record: Ridge Racer's "0 drawn over" | a job draws if it can: a triangle with area and within the console's size, anything inside the drawing area |
| a VRAM-to-VRAM copy's record hashed from native VRAM | the card copies; native VRAM has the copy only once something reads it there | a copied record keeps its own texels, taken before the copy, and its pieces and palettes are read from them |
| texels no record covers count as clear where native VRAM is zero | where the card drew, native VRAM may still say anything | where the GPU drew, only what is known to be zeros: a fill of zeros, a black rectangle, a copy of clear texels |

The third cost Metal Gear Solid a fifth of what it had named (63.5% to 51.2%) until "known to
be zeros" was worked out as closely as the console does it: it clears all of VRAM with one black
rectangle rather than a fill, and swaps a bank of palettes back and forth with mask-checked
copies, leaving clear texels between the palettes it uploads. So the tracker now follows what
each copy lands, texel by texel - with the mask checked, the texel already there where its bit
15 is set - wherever both ends are known; and marks drawn only the pixels a triangle covers by
the console's fill rule, not its bounding box. Back to 63.5%.

After them, the software and hardware rasterisers name the same, job for job, on Ridge Racer
and on the twelve discs - every count of every record and every reason, 3,000 frames on the interpreter, on the rules as they ended.

**The identity check** - the twelve-disc set on WARP, 3,000 frames on the interpreter, each disc
dumped at 4x and the dump drawn back as its own pack, cut to the console's colours:

| Disc | 4x | 1x | Jobs replaced | Machine at eight bits |
|---|---|---|---|---|
| Ace Combat 3 | same | same | 188,412 | same |
| Air Combat | same | same | 1,200 | same |
| Area 51 | same | same | 2,540 | same |
| Bomberman | same | same | 159,900 | same |
| Captain Tsubasa J | same | same | 15,467 | same |
| Final Fantasy VII | same | same | 5,554 | same |
| Final Fantasy VIII | same | same | 1,200 | same |
| Legend of Mana | same | same | 1,320 | same |
| Ridge Racer | same | same | 1,040,469 | same |
| Vandal Hearts | same | same | 1,200 | same |
| Wild Arms | same | same | 4,934 | same |
| Wild Arms 2 | same | same | 850,241 | same |

The 3,000 frames are mostly the BIOS's logo, intros and films: Ridge Racer, Wild Arms 2, Ace
Combat 3 and Bomberman draw many textured jobs in them, the rest few - 1,200 on four discs, the
same number each, is most likely the BIOS's own logo.

- **4x**: the dump drawn at 4x, the pack made 4x larger nearest; **1x**: drawn at 1x, the pack
  made 8x larger. "Same" is every frame log line - native VRAM's checksum every 100 frames and,
  at 4x, the sharper picture's - and the final checksum. Direct3D 12 gives the same as 11 on
  Ridge Racer at 1x and 4x. The table was run before the last of the rules for zeros (below);
  on the rules as they ended the 1x check was run again on all twelve - dumped at 1x, drawn back
  8x larger - with the same pictures and the same jobs replaced.
- **The machine**: drawn from the pack at eight bits, as the app draws it, instructions and CD
  sectors are the same as without on every disc. A replaced texel lands in VRAM, so a game that
  reads back a picture it drew could see one; none of the twelve does in these frames.
- **A hand-edited texture**: Ridge Racer's "COMPUTER ENTERTAINMENT" lettering recoloured at 4x
  with a grid between its pixels, drawn by `boot_runner --scale 4` and by the app at 4x, the grid
  visible at the sub-texel scale.

**Coverage, with phase 2's rules** (the software rasteriser, the 26 discs, 6,000 frames on the
recompiler, as phase 1 measured): every disc's share the same as phase 1's, to the tenth - 95.2% in all, 22 discs at 100%, Crash 3 95.8%, Vandal Hearts 93.8%, Metal Gear Solid 63.5%. Naming the same on either rasteriser cost no disc a tenth of a percent.

**Left for later**:
- a state loaded, or a step back while rewinding, forgets every upload: the game's own textures
  until it uploads again (since brought back - Phase 4, as built);
- the budget is psxemu.ini's only;
- dumps in the app are written as each source closes, on a thread of their own - a game that
  closes many at once (Ridge Racer patches its font glyph by glyph) writes many small files;
- the software rasteriser draws no replacements, by design.

### Phase 3, as built (2026-10-17)

**What was built** (the design is [The AI upscaler](#the-ai-upscaler), above, as built):

- **`render/texture_upscale.h`**, in the core: what the upscaler is handed - a source's texels,
  decoded on demand through its palette, and the 8x8-texel cells read so far - and the queue it
  is handed them through. The texture tracker hands a source over each time draws read more of
  it, on the rasteriser's thread, and whether or not it is dumping.
- **`graphics/upscale/texture/ai_textures.h/.cpp`**, in the front end: the queue, its thread and
  its device. Tiles are made ready (holes filled from the nearest drawn texel, the source's edge
  repeated), run three by three through phase 0's `OnnxUpscaler`, put back together into 8x8
  blocks, and handed to the pack (`TexturePack::Provide`) and written as PNGs - zlib's fastest
  level, run lengths only, since an image is mostly empty where a sheet is read only in a corner.
  On the card that draws, a call is one tile beside its coverage, and the thread sleeps so that
  its calls take a share of the time (`texture_ai_share`, 10%).
- **The pack** takes images from the upscaler beside its own: a hand-made one always wins
  (`HandMade`), and `ai\<model>-<k>x\` is read as a second folder.
- **`graphics/upscale/texture/ai_download.h/.cpp`**: the files fetched by the emulator - ONNX
  Runtime and DirectML out of their NuGet packages by HTTP ranges, Real-ESRGAN's two models from
  its release, converted with phase 0's `torch_model` - about 20 MB, each checked. The same files
  as `fetch_onnxruntime.ps1` fetches, byte for byte.
- **The app**: `texture_ai_scale` (Off, 2x, 4x) and `texture_ai_model` (`anime`, the default,
  or `general`, sharper), per game; `texture_ai_share` and `texture_ai_card` in psxemu.ini.
  Settings > Video > Textures has an "AI upscaling" choice, and "Get the AI files..." in its status
  line while any are missing; View > Upscale All Dumped Textures. The status line counts tiles
  done and waiting. With Textures added, Settings > Video was 1,493 lines tall at 200%, nearly
  the whole screen, so it became a page each - Renderer, Rasteriser, NVIDIA DLSS, AMD FSR,
  Textures, On-screen display - chosen from a list down its left: 971 lines. Each game's
  `textures\` folder is in the data folder, `Documents\My Games\PSXEmu\`, with its save states.
- **`boot_runner`**: `--texture-ai`, `--texture-ai-out`, `--texture-ai-scale`,
  `--texture-ai-share`, `--texture-ai-grid`, `--upscale-folder`. `ai_textures_test`, 19 checks,
  with a model that only repeats pixels.

**Full speed** - the app on Ridge Racer (a 3D disc that reads its sheets through 91 palettes),
Castlevania and Valkyrie Profile (2D), at 4x, the upscaler at 4x, 60 seconds
of the attract mode each, the lowest of Show Timings' samples after the first 4 seconds, and
the mean:

| Drawing on | Upscaling on | Ridge Racer | Castlevania | Valkyrie Profile |
|---|---|---|---|---|
| Radeon 780M | nothing | 100%, 100.4% | | 25%, 93.3% |
| Radeon 780M | RTX 4060, all its time (the default) | 99%, 100.5% | 99%, 99.9% | 57%, 98.1% |
| RTX 4060 | nothing | | 99%, 99.9% | |
| RTX 4060 | RTX 4060, 10% (the default) | 98%, 100.4% | 99%, 100% | 98%, 99.9% |
| RTX 4060 | RTX 4060, 25% | 83-85%, 99.8-99.9% | 93%, 99.7% | |
| Radeon 780M | Radeon 780M, 10% | 88-92%, 99.5-100.4% | | |
| Radeon 780M | Radeon 780M, 25% | 84-86%, 99.5-99.7% | | |

Valkyrie Profile's dips on the Radeon are its own: they come at the same moment of its attract
mode with the upscaler off, and lower. One run of Castlevania on the 4060 at 10% fell to 31%
once and did not again in three more. So with two cards the queue costs nothing, and on the
4060 alone a tenth of its time costs nothing either; on the Radeon alone even a tenth shows as
dips of a few percent while the queue is running - the first minute of a game, until its
textures are done, and none at all on the next run, which reads them back. A share of 25% is
too much on either card. The 780M draws Ridge Racer at 4x with little time left over, and a
call of 80-180 ms cannot be cut finer than a tile.

`boot_runner --texture-ai` on Ridge Racer, 3,000 frames at 4x on the Radeon: 651,731 jobs drawn
with the upscaler's images.

**Upscale All on the 26 discs' dumps** (phase 1's, 6,000 frames each) on the Radeon, at 4x:
4,313 dumps, 4,303 images made - the other ten are clear to the last texel - from 16,567 tiles
in 2,711 calls: 280 seconds of calls in 601, and 1.1 GB of PNGs. Most discs take a few seconds;
Valkyrie Profile (1,204 dumps) 177 seconds, Metal Gear Solid (1,356) 118. Spyro 3's 388 dumps are
2048x512 sheets read only in places, and each image is 8192x2048 at 4x: 113 of its 122 seconds
went outside the model until the PNGs were written with run lengths only - 82 seconds, and 43
MB rather than 122.

**Reviewed**, on all 26 discs: for each, the eight textures with the most cut-out edge and the
most detail, a 64x64-texel corner of each, the dump nearest beside the upscale over a
checkerboard - and the cut-out edges closer, over magenta too. No dark fringe on any: what was
clear takes the colour of the drawn texel next to it before the model sees it, so what the
model smooths across a cut-out's edge is the sprite's own colour. Valkyrie Profile's sprites,
outlined in light grey, keep that outline as a thin smooth rim; Vandal Hearts' monsters, Crash
3's lettering and the BIOS's logo are clean-edged. What the model does badly is what it was not
trained on: tiny pixel lettering (Einhänder's fine print, Wild Arms 2's font) comes out softer
and fainter, and dithered noise (Xenogears' title) is smoothed away.

**Palette animation**: 57 of the 4,303 images are read through more than eight palettes, 41 of
them Valkyrie Profile's; each palette is an image, done once.

**Left for later**:
- on one card alone the queue shows as small dips while it runs (above); a half-float model, or
  the Radeon's NPU through Windows ML, may make a call short enough not to;
- Real-ESRGAN softens pixel-art lettering and HUDs; there is no default list of what never to
  upscale - `pack.ini`'s `[never]` does it per game - and a pixel-art model whose licence allows
  it would do better;
- tiles go first come, first served, not nearest the middle of the screen first;
- a 2x model, rather than the 4x one halved.

### Phase 4, as built (2026-10-17)

**What was built:**

- **RTX Video as a backend of the upscaler** (`MakeRtxVideoBackend`, `ai_textures.h`): phase 0's
  `rtx_video.h` on a Direct3D 12 device of its own, asked for 2x or 4x as it is rather than 4x
  halved, at quality 4. Everything else - tiles, holes filled, coverage through it, the cache
  folder - is the ONNX backend's.
- **Built in where the SDK is**: the front end's project, `build_tools.bat` and `boot_runner`
  define `PSXEMU_RTX_VIDEO` and link NGX only when `Temp\rtxvideo\` holds NVIDIA's SDK, and copy
  its `rel` `nvngx_vsr.dll` beside the executable; anywhere else `rtx_video.cpp` is a backend that
  says it is not there.
- **In the AI upscaling list**, "2x/4x, NVIDIA RTX Video (RTX cards)", where it can run - a build
  with NVIDIA's SDK, and an NVIDIA card - or while psxemu.ini sets it: `texture_ai_model =
  rtx_video`, per game like the other models. It runs on the NVIDIA card with the most memory,
  into `textures\<game>\ai\rtx_video-<k>x\`. "Get the AI files..." says what it needs rather
  than offering a download - NVIDIA's SDK cannot be fetched for the user.
- **`boot_runner --texture-ai rtx-video`**, `--texture-ai-quality 1-4`.

**What it costs** - Upscale All over the 26 discs' dumps put together (4,047, those the discs
share once), 15,955 tiles in 2,545 calls, each the same three by three tiles:

| Backend | Card | A call | Calls, in all | The whole run |
|---|---|---|---|---|
| RTX Video, quality 4 | RTX 4060 | 23.6 ms | 60 s | 282 s |
| Real-ESRGAN animevideo-v3 | RTX 4060 | 35.6 ms | 91 s | 289 s |
| Real-ESRGAN general-x4v3 | Radeon 780M | 148.4 ms | 378 s | 584 s |

RTX Video is 1.5 times as fast as the anime model on the same card, not the four times phase 0
measured on one 256x256 picture: a call also carries the picture to the card and back, which at
this size is much of what it costs. And the whole run hardly changes, being mostly the PNGs.

**How it looks** - sheets of the dump nearest beside each upscale at 4x, for the textures with
the most cut-out edge and the most detail, on twelve discs against the anime model and on four
against both models (`Temp\texcmp\sheets\`, not committed):

- **RTX Video keeps the texture as it was, softened**: grain, noise and dither stay - Xenogears'
  speckled title, Castlevania's stone - and edges are a sharpened blur, like a good bicubic. On
  phase 0's whole frames that read as natural; on textures it reads as soft.
- **The anime model** draws the cleanest sprites and edges: Vandal Hearts' monsters, Valkyrie
  Profile's outlines, FF7's credits lettering, with dither smoothed away.
- **The general model** is the sharpest and keeps the most detail: Vandal Hearts' carved panel and
  Wipeout's small print are the most legible of the four.

RTX Video is best at nothing here. Its one advantage, speed, buys little: with two cards the
upscaler already costs the game nothing, and on the RTX 4060 alone the anime model at a tenth of
the card held full speed (phase 3). So it was to stay in psxemu.ini, as decided before it was
measured; the user wanted the choice on the menu all the same, and it is there on NVIDIA cards,
the Real-ESRGAN models first.

**Checked in the app**: Ridge Racer for a minute at 4x on the Radeon, RTX Video on the 4060 - 617
tiles upscaled, nearly a million draws replaced, the game at 100% throughout.

**Upscale All without asking**: the game's dumps not yet upscaled now go in the background each
time the upscaler starts (The AI upscaler, above). Ridge Racer's 279 dumps in the app, the
upscaler on the 4060: all done within 20 seconds of starting, the game at 100%; the next start
queued none.

**Cells read but clear were upscaled every run**: a tile was empty only if nothing in it, its 16
texels round included, was drawn. Clear cells beside drawn ones done before went through the
model and gave nothing - and, leaving nothing in the image, were not known to be done next run.
Now a tile is empty if no cell it gives has a drawn texel. Ridge Racer, 2,000 frames at 4x run
twice: the second run 0 calls (it had been 20 tiles in the app), and 293,314 jobs drawn with
the upscaler's images against the first run's 206,714, every one there from the first frame.

**Save states and rewind**: a load made the tracker forget every upload (phase 2's left for
later), so after loading a state, or at each step back while rewinding, the game's own textures
were drawn until it uploaded them again - for a texture loaded once, a font or a menu, the rest
of the session. Uploads are not machine state, and stay out of the save state:

- **Within a run** the tracker remembers every record it has made - where, and each hash it has
  had there - and after a load brings back each that VRAM still holds, the largest first where
  they overlap, so a joined upload comes back as itself and not as its strips. A load leaves
  native VRAM all of VRAM - a hardware rasteriser is given it afresh - so what was drawn need not
  be known.
- **Across runs**, saving a state writes `<state>.textures` beside it: the live uploads, 24 bytes
  each. Loading it restores exactly those, each checked against VRAM first.

Ridge Racer, a state saved at frame 1,500 with the upscaler's images all made, loaded in a new
run: 193 uploads restored, and all of the first 60 frames' 1,020 textured jobs drawn upscaled -
none without the file. In the app the same: 1,661 draws upscaled in the first second after the
load, none in three seconds without. `--rewind-test 60`: the 193 records brought back by the
step back, and the machine the frame kept aside, byte for byte, as before. `texture_tracker_test`
73 checks.

**Not done**: RTX Video's quality levels 1-3, set beside 4 on the same textures.

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

- **Coverage, measured in phase 1: 95.2% of textured pixels on the 26 discs.** The first rules
  named 3.8% of Castlevania's and 13% of Xenogears'; strips joined, records cut rather than
  retired, composites, zeros and copies brought 22 of the 26 to 100%. What is left is textures the
  GPU drew itself (Metal Gear Solid's screen effects most of it), which the design leaves native.
  The fallback identity - texture page and CLUT hashed together - is not needed.
- **AI output on tiled 2D.** A tile map upscaled as one sheet can still show seams where two tiles
  that sit side by side on screen are far apart on the sheet. `pack.ini` can exclude a sheet, and
  the binary-alpha filter is the alternative for it.
- **Fonts and HUDs** often look worse upscaled by a photographic model. They can be excluded by
  name, and a pixel-art model handles them better; phase 3's review found them softened
  (Einhänder, Wild Arms 2) and left the list to `pack.ini`'s `[never]`, per game.
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
