# Higher internal resolution and PGXP: a hardware rasteriser

**Status: all six phases done (2026-09-29) - a Direct3D 11 rasteriser, chosen at Settings >
Video > Rasteriser, draws at 1x to 8x the console's resolution, with true colour, and with PGXP
draws polygons at their unrounded positions with perspective-correct textures, under the
interpreter or the recompiler; it hands its picture to every renderer on the graphics card, with
nothing read back. At native size it is identical to the software rasteriser, and at every scale,
PGXP on or off, what the machine sees still is.**

The PlayStation draws at 256-640 pixels across, with integer vertex coordinates
and textures mapped without perspective. On a modern display that shows as three
different problems, and they are fixed by three different things:

| What you see | Why | What fixes it |
|---|---|---|
| Blocky, low-resolution 3D | the GPU draws into 1 MB of VRAM at native size | **drawing at 2x-8x internal resolution** on the host's GPU |
| Polygons wobbling and seams opening as the camera moves | the GTE rounds every projected vertex to a whole pixel | **PGXP**: keep the unrounded position and draw with it |
| Textures swimming and bending across big polygons | the GPU interpolates texture coordinates linearly in screen space | **PGXP's depth**: perspective-correct texturing |

Upscaling without PGXP gives sharper wobble; PGXP without upscaling helps less
than it could. They are built as one piece of work, upscaling first.

---

## The conclusion first

- **A second rasteriser, not a second GPU.** The part of `Gpu` that *is* the
  console's GPU - GP0/GP1, GPUSTAT, the command queue, DMA handshaking, display
  timing, what every draw costs - stays one piece of code, shared. What becomes
  selectable is the part that turns a parsed draw into pixels. The reason is below
  under **What is separate and what is shared**; it is the one decision here worth
  arguing about.
- **Direct3D 11 first, on a device and thread of its own.** One API done well
  before four. Frames reach whichever presenter is running by read-back at first,
  then by a shared texture.
- **Two copies of VRAM**: the upscaled one the host GPU draws into, and the
  native 1024x512 one the rest of the machine reads. Most of the work, and most of
  the bugs in every emulator that has done this, is keeping them honest with each
  other.
- **PGXP** is a float shadow of the GTE's screen coordinates, carried through RAM
  alongside the values it describes, and looked up when those values reach the
  GPU. Interpreter only at first.
- **Save states do not change.** They hold native VRAM, as now, so a state moves
  freely between the two rasterisers and `kStateVersion` stays 8.
- **Verified against the software rasteriser**, which stays the reference: at 1x
  the new one should draw the twelve-disc table close to pixel for pixel, and the
  game's timing - instruction counts, CD sectors - must be identical, because it is
  the same code.

---

## What the machine does today

The seam is already there, from the rasteriser's own thread (phase 7 of
[Threading-Plan.md](Threading-Plan.md), bug 91):

- **The machine thread** parses every GP0 command, charges its time, and turns it
  into a fixed-size `Gpu::DrawJob` - a triangle, line, rectangle, fill or
  VRAM-to-VRAM copy - with a snapshot of everything it depends on (`DrawEnv`: the
  drawing area, texture window, mask rules, field).
- **The rasteriser** - inline, or on its own thread - applies jobs to `vram_`
  (`ApplyJob`). Anything that reads VRAM waits for it first (`SyncRaster`), which
  is why a threaded run is byte-identical to an inline one.
- **What stays on the machine thread**: CPU-to-VRAM and VRAM-to-CPU transfers,
  GPUSTAT, the display registers, and resolving the displayed area into 32-bit
  pixels for the front end (`framebuffer()`).
- **`System` holds a concrete `Gpu`**, and about thirty callers - the debugger,
  boot_runner, the harnesses, the App - use its display accessors
  (`frame_count`, `refresh_hz`, the beam position, `stats`). Keeping `Gpu` as the
  shared half means none of them changes.

Two facts PGXP needs, both already true:

- DMA channel 2 knows the RAM address of every word it hands the GPU
  (`Dma` reads `ram.u32[address >> 2]` at the call).
- The GTE's screen-coordinate FIFO is three `int16` pairs (`Gte::sxy_`), easy to
  shadow.

---

## The design

### What is separate and what is shared

You asked for a separate GPU core. What this plan makes separate is the
**rasteriser** - everything that makes pixels - behind an interface in the core,
with two implementations:

    Gpu (shared)                       RasterBackend (one of)
      GP0/GP1, GPUSTAT, queue            SoftwareRaster - today's code, unchanged
      DMA handshake, costing     --->    HardwareRaster - Direct3D 11, upscaled
      display timing, DrawJob            (Win32 front end)
      transfers' bookkeeping

**Why not a whole second `GpuCore`** with its own command parsing and timing:

- **Timing would diverge.** The draw costs, the queue depth, GPUSTAT's ready bits
  and the display timing took bugs 78-93 to get right, and every baseline in
  [Test-Suite.md](Test-Suite.md) depends on them. A second copy would drift, and a
  game's behaviour would change with the renderer - a bug report nobody could
  reproduce on the other one.
- **Every GPU fix would be made twice** - or once and forgotten on the other.
- **The seam already exists.** `DrawJob` is exactly "a draw, parsed and costed";
  the software rasteriser is already a consumer of a queue of them.
- **It is what the proven designs do.** DuckStation's hardware and software
  renderers share its GPU's command and timing code for the same reasons (read
  for the design only; nothing is copied - see the end).

If you still want a fully separate core - its own parser and timing, perhaps to
experiment freely - it is possible: `GpuCore` is already an interface. It costs
the points above, and `System` would need to hold a `GpuCore*` rather than a
`Gpu`. I would not recommend it.

### Where the code goes

The core never knows which front end runs it
([Emulator-Project-Standards.md](Emulator-Project-Standards.md)), so the core
cannot hold a Direct3D device:

- **`PSXEmu.Core/psx/raster_backend.h`** - the interface: apply a batch of
  `DrawJob`s, write a rectangle of VRAM, read one back, resolve the display area,
  sync, and save or load native VRAM.
- **`PSXEmu.Core/psx/software_raster.*`** - today's rasteriser, moved behind it.
- **`PSXEmu.Win32/graphics/hw_raster/`** - the Direct3D 11 implementation and its
  shaders.
- **The front end hands the machine a factory** for the hardware one, as it
  hands `VideoOutput` a presenter factory today. With none - boot_runner, the
  harnesses - there is only software.
- **A small tool, `hw_raster_test`**, links the Direct3D one with WARP -
  Windows' software Direct3D device, deterministic and needing no graphics card -
  so the comparisons below can run headless.

### The renderer's thread and device

- The hardware rasteriser **takes the software one's place on the rasteriser
  thread**, and owns its own Direct3D 11 device there. It never shares a context
  with the presenters, so neither can stall the other.
- **Jobs are batched.** Consecutive triangles and rectangles with the same state
  (texture page, CLUT, blend mode, mask rules, drawing area) go into one vertex
  buffer and one draw call. A change of state, a VRAM write that overlaps a
  texture in use, or a sync flushes the batch.
- **The machine's timing never waits for the host GPU.** Costs are charged when
  jobs are parsed, as now. The machine only waits where it waits today - for
  anything that reads VRAM - and that is where the copies below get downloaded.

### VRAM: two copies, and which is right

| Copy | Size | Written by | Read by |
|---|---|---|---|
| Upscaled VRAM (render target) | 1024xS by 512xS, RGBA8 or R16 | every draw, fill and copy; uploads, scaled up | the display, and texture sampling |
| Native VRAM (CPU copy) | 1024x512x16 | uploads; downloads of what draws changed | VRAM-to-CPU transfers, save states, the debugger, 24-bit display |

Rules:

- **CPU-to-VRAM** writes both copies. The upscaled one gets the pixels repeated
  SxS, so an uploaded image looks exactly as it would at native size.
- **Drawing** changes only the upscaled copy, and marks the rectangle it covered
  as *dirty*: newer on the host GPU than in the native copy.
- **Anything that reads native VRAM** - a VRAM-to-CPU transfer, a save state, the
  debugger, View VRAM - downloads just the dirty rectangles it needs first,
  taking one sample of each SxS block.
- **Sampling a texture from a region being drawn to** is a feedback loop Direct3D
  does not allow. The renderer keeps a second texture, the *read copy*, refreshed
  from the target only in the rectangles that are both dirty and about to be
  sampled.
- **The mask bit** (bit 15), which "set mask" and "check mask" read and write, is
  kept in the depth-stencil buffer, so the checks are a depth or stencil test and
  cost nothing extra.

### Drawing everything the software rasteriser draws

Each `DrawJob` kind, and the console behaviour that needs a shader rather than
fixed-function state:

- **Triangles**: flat or Gouraud, textured or not, raw or colour-modulated.
- **Textures**: 4- and 8-bit through a CLUT, and 15-bit direct, all decoded in the
  pixel shader from VRAM. A paletted texture is always sampled at its exact texel
  - an index cannot be blended - so filtering is only offered on 15-bit ones.
- **The texture window** - the mask and offset - applied in the shader.
- **Semi-transparency**: the four modes - half and half, add, subtract, add a
  quarter - as blend states. On textured primitives only texels with bit 15 set
  blend, so those draw in two passes: opaque texels, then translucent ones.
- **Dithering**: at 1x as the console does. At higher scales it is optional
  ("true colour"), since the 4x4 pattern stretched to 16x16 looks worse than no
  dither at all.
- **Lines and polylines**: one quad per segment, drawn to the console's
  rasterisation rules at 1x and thickened by the scale above it.
- **Rectangles (sprites)**: the easiest to get wrong once upscaled - neighbouring
  texels bleed in at the edges - so they sample with the texel centre clamped
  inside their own rectangle.
- **Fills** ignore the drawing area and the mask, as on the console.
- **VRAM-to-VRAM copies** on the host GPU, with the mask rules.
- **Interlace**: the field-skip rule from bug 89 in the shader.
- **The 24-bit display mode** (films) reads native VRAM, since the MDEC's output
  arrives by CPU upload and is native-sized anyway.

### Upscaling

- **Internal resolution 1x-8x** (maybe 16x, measured on real hardware first).
  Every vertex is multiplied by the scale; texture coordinates are not, since they
  address native texels.
- **The display area** is resolved from the upscaled VRAM at S times its native
  size, so a 320x240 game at 4x reaches the presenter as 1280x960.
- **Filtering** - none (sharp), or bilinear on 15-bit textures - is a later,
  separate choice. The presenters' own filters (xBRZ and the rest) still apply to
  the final picture.

### Getting frames to the screen

- **First, read-back:** the resolved display area is copied to the CPU and
  published through the `FrameMailbox`, like the software rasteriser's frame. All
  four presenters work unchanged. At 4x that is about 5 MB a frame, 300 MB a
  second - measured before anything cleverer is built.
- **Then, a shared texture:** Direct3D 11 can share a texture with another device
  by an NT handle. The Direct3D 11 and 12 presenters open it directly, and Vulkan
  and OpenGL through their Windows external-memory extensions. That removes the
  copy and a frame of latency.

### Save states

- **They hold native VRAM, as now.** Saving downloads whatever is dirty first.
- **Loading uploads it to both copies.** The upscaled picture is blocky until the
  game draws again, usually the next frame.
- A state saved under one rasteriser loads under the other. No format change.

### PGXP

The GTE projects a vertex (`RTPS`, `RTPT`) to a fixed-point screen position and
rounds it into `SXY` as two `int16`s. Games then move those words about - store
them to RAM, copy them into a display list - and DMA hands them to the GPU. PGXP
keeps the unrounded position alongside, and gets it back at the end.

- **GTE:** alongside `sxy_`, a float `x, y` and the projection's depth for each FIFO
  entry, computed from the same values before the rounding.
- **CPU:** a *shadow* for every register and every word of RAM - 2 MB is 512K
  words, 8 MB of shadow at 16 bytes each. Each shadow holds a precise vertex, and
  the 32-bit value it was made for:
  - `MFC2`/`SWC2` of `SXY` copy the precise vertex into the register or RAM
    word's shadow
  - `LW`/`SW` and register moves copy shadows along with values
  - any other write to a word invalidates its shadow
- **GPU:** when a polygon's vertex word arrives, DMA passes its RAM address with
  it. If that word's shadow is valid and still describes the same 32-bit value,
  the precise position is used; otherwise the integer one, as now. A stale shadow
  can never be used, because the value check fails.
- **Perspective-correct texturing:** each precise vertex carries a `w` from the
  projection depth. The hardware rasteriser interpolates texture coordinates with
  it, and falls back to affine for any vertex without one.
- **Options:** vertices only; plus texture correction; plus precise culling (using
  the float positions for `NCLIP` - it fixes some missing polygons and breaks a few
  games, so it is off by default).
- **The recompiler** does not track shadows, so PGXP runs the interpreter at
  first, the way DuckStation's "CPU mode" does. Emitting the shadow copies from
  compiled loads and stores is a later phase, measured for its cost.
- **Not in save states.** The shadows rebuild as the game runs, within a frame
  or two.
- **The software rasteriser ignores PGXP.** It draws at native size, where the
  fraction would only move pixels.

### Settings and the menus

- **Settings > Video > Renderer** gains a second axis: **Software** (today) or
  **Hardware**. The presenter API (Direct3D 11/12, OpenGL, Vulkan) stays its own
  choice.
- **Hardware options:** internal resolution 1x-8x; true colour; PGXP - vertices,
  texture correction, culling.
- **Per game:** `EmuConfig` gets `gpu_rasteriser`, `resolution_scale`, `true_color`,
  `pgxp_vertices`, `pgxp_textures` and `pgxp_culling`. They join
  `GameSettingKeys` (bug 116), so a game that the hardware rasteriser gets wrong
  can be kept on software.
- **Switching** between rasterisers is safe mid-game through the same path as a
  save state: download native VRAM, swap, upload.

---

## Phases

| # | What | Done when | Rough size |
|---|---|---|---|
| 0 | `RasterBackend` extracted; today's code behind it as `SoftwareRaster` | every harness and the twelve-disc table byte-identical | 1-2 sessions |
| 1 | Direct3D 11 skeleton: device and thread, both VRAM copies, uploads, fills, copies, untextured triangles, display resolve by read-back; 1x | all twelve discs boot and run with the hardware rasteriser; menus show their shapes | 2-3 |
| 2 | Everything the software rasteriser draws: textures, CLUTs, texture window, semi-transparency, mask, dithering, lines, sprites, interlace | the 1x comparison below within tolerance on all twelve discs and `gpu_test`'s scenes | 3-5 |
| 3 | VRAM coherence: dirty rectangles, downloads, the read copy, save states, View VRAM, the debugger | save states cross between rasterisers; framebuffer-effect games right | 2 |
| 4 | Upscaling 2x-8x, true colour, sprite edges, the Settings UI, per-game keys | screenshots at each scale reviewed disc by disc | 2-3 |
| 5 | PGXP: GTE shadow, CPU and RAM shadows, DMA addresses, the lookup, perspective correction, options | A/B screenshots of wobble and texture warp on moving scenes; nothing breaks with it off | 3-4 |
| 6 | Shared-texture hand-off to each presenter; PGXP under the recompiler | read-back gone; PGXP's cost measured | 2-3 |

A usable upscaler is the end of phase 4; everything after makes it better.

### Phase 0, as built

- **`psx/raster.h`**: the shared types - `RasterVertex`, `RasterState`, `RasterEnv`,
  `DrawJob`, `RasterCounters` - and `RasterBackend`:
  - `Apply` draws one job
  - `PrepareRead` comes before the machine reads native VRAM (a VRAM-to-CPU
    transfer, the display, a save state, `vram()`)
  - `Written` comes after a CPU-to-VRAM transfer completes
  - `Reloaded` comes after a state loads
  - and the counters and the watch rectangle
- **`psx/software_raster.*`**: the triangle, line, rectangle, fill and copy code,
  `PlotPixel`, `SampleTexture` and the blending, moved out of `gpu.cpp` a line at a
  time with only its members renamed. For it, the three VRAM calls are empty: it
  draws into `Gpu`'s own VRAM.
- **`Gpu`** keeps everything else. It makes the backend in `Initialize`, before
  the rasteriser's thread starts, and names the shared types with its old
  typedefs, so none of its command parsing changed.
- **One quirk kept on purpose:** a CPU-to-VRAM transfer's writes into a watched
  rectangle (`--watch-vram`) are still attributed to the last drawing command,
  as they always were.
- **Verified:**
  - all twelve discs, inline and threaded: every line of `boot_runner`'s report
    - pixels plotted, clipped, mask-rejected and field-skipped, texels by depth,
    every transfer, every CD event - matches a build from just before, as does
    each frame-3000 picture. Only the wall-clock speed line, and in threaded runs
    how many barriers waited, are left out, since they vary run to run anyway.
  - all nineteen harnesses green, 2,433 checks.

### Phase 1, as built

- **`PSXEmu.Win32/graphics/hw_raster/d3d11_raster.*`**: `D3D11Raster`, a
  `RasterBackend` with a Direct3D 11 device of its own - the graphics card's, or
  WARP's.
  - **VRAM on the card** is a 1024x512 RGBA8 render target. Every colour is cut
    to five bits in the pixel shader and widened back the way the software
    rasteriser widens a VRAM pixel, so the card only ever holds exact 15-bit
    values, and alpha carries the mask bit. A download is a shift.
  - **Triangles** are batched, with the drawing area as the scissor and the
    vertices half a pixel in, so Direct3D's pixel centres fall where the software
    rasteriser samples. Flat or Gouraud, dithered, the displayed field skipped,
    the mask bit forced by GP0(E6h). Textured ones are drawn in their colour.
  - **Rectangles** are two triangles with their edges between pixel centres.
  - **Lines** are worked out pixel by pixel on the CPU, the console's rule, and
    drawn as points, so they match software exactly.
  - **Fills** ignore the drawing area and the mask and clip at VRAM's edge.
  - **Copies** snapshot their source (and their destination, when the mask is
    checked) into a second texture and draw from it. Each is read whole before
    any is written, so a copy onto itself shifted right or down does not smear
    as it does on the console - phase 3's.
  - **Native VRAM** is kept in step in 32x32 tiles. A draw marks the tiles it
    covers dirty; `PrepareRead` downloads the dirty ones a reader needs;
    `Written` uploads a CPU transfer; `Reloaded` uploads everything.
- **`Gpu`**:
  - `ChooseRasteriser` makes the one `EmuConfig::gpu_rasteriser` asks for, and
    falls back to software, saying why, if the front end has no factory or the
    device cannot be made. Called by `Initialize`, and by
    `Machine::ApplyConfig` when the setting changes: the old rasteriser's
    pictures are brought into native VRAM and the new one starts from them, the
    same path a save state takes. It switches mid-game.
  - A CPU-to-VRAM transfer now brings its rectangle up to date before writing
    into it, so a state saved halfway through one holds the upload rather than
    what the card had drawn there; a transfer a reset cuts short is reported as
    written.
  - Both are nothing to the software rasteriser.
- **The front end** hands the machine a factory for `D3D11Raster`, and
  Settings > Video > Rasteriser picks Software or "Hardware (Direct3D 11,
  experimental)". The menu ticks what is drawing, not what was asked for.
- **`boot_runner --hw-raster [--warp]`** links the same code, and **`ppm_diff`**
  compares two pictures - where the plan had a separate `hw_raster_test`.
- **Not yet:** textures, semi-transparency, the mask check on draws, and the
  rasteriser's pixel counters, which read zero - phase 2. The debugger's
  VRAM watch counts nothing under it.
- **Verified:**
  - all twelve discs, 3,000 frames on WARP: every report line matches software
    but the pictures' and the rasteriser's own counters - instructions, every
    CD event, transfer and interrupt.
  - seven of the twelve frame-3000 pictures identical to the pixel (films and
    uploads); the other five are textured scenes, drawn as flat shapes.
  - the BIOS's Gouraud logo identical to the pixel at frames 120 and 200; its
    menu background at frame 600 off by one 5-bit step in 80 pixels.
  - threaded and inline hardware runs identical; save states cross between the
    rasterisers both ways with the same instruction counts.
  - the software rasteriser still byte-identical to phase 0 on all twelve discs.
  - all nineteen harnesses green, 2,433 checks.

### Phase 2, as built

Everything the software rasteriser draws, drawn the same to the pixel at native
size. That is more than the plan asked - "within tolerance" - and it came from
changing how, after a first version built as planned was measured.

- **The first version** used the output merger: dual-source blending for the
  semi-transparency modes, a reverse subtraction for B-F, a stencil mirroring the
  mask bits for the mask check, and colours and texture coordinates interpolated
  by the rasteriser. Against the software rasteriser it was one 5-bit step off
  in 12% of blended pixels - a blend stored in the target is not a widened 5-bit
  value, so the next blend on top reads a different B - and about 0.2% of
  textured pixels took the neighbouring texel, where an interpolated coordinate
  lands a hair either side of a whole number.
- **As built, the pixel shader is the software rasteriser's arithmetic in
  integers:**
  - a triangle is drawn as its bounding box, and the shader decides each pixel
    with the same edge functions and fill rule, then interpolates its colour and
    texture coordinates with the same integer division by twice the area. Every
    vertex carries the whole triangle, flat, for it
  - a rectangle carries its corner, so each pixel's texel is the corner's plus
    its offset, flipped or not
  - blending and the mask check read the pixel underneath from the *read copy*
    and do the software rasteriser's integer blend
  - textures are decoded from the read copy too: 4- and 8-bit through the CLUT,
    15-bit direct, through the texture window, modulated or raw, with bit 15
    deciding what blends and what sets the mask
  - dithering, the displayed-field skip and the forced mask bit as before
- **The read copy** is a second texture of VRAM, refreshed from the target a
  row of 16x16 tiles at a time, only where something has drawn since and only
  when a primitive is about to read there - its texture page and CLUT, or the
  pixels under it if it blends or checks the mask. A batch ends only when what it
  drew is what the next primitive reads; that is what makes drawing into a
  texture and then using it, and a translucent primitive over one just drawn,
  come out right.
- **What it costs:** a primitive that blends over one drawn in the same batch
  ends the batch. The output merger only writes, so nothing about blend or
  stencil state changes between batches.
- **At higher resolutions** (phase 4) the edge functions are evaluated at each
  sub-pixel's position rather than at a whole pixel, which the same shader can do
  with the coordinates scaled.
- **Self-sampling is left alone.** A primitive whose texture is the pixels it is
  drawing gets them in drawing order from the software rasteriser and as they were
  before it from the card, and the console's texture cache is a third answer.
  None of the twelve discs does it at a checkpoint.
- **`tools/hw_raster_test`** (new, 26 checks): two machines, one per rasteriser,
  fed the same random GP0 words scene by scene - untextured, textured in every
  depth, every blend mode, sampling what was drawn, the texture window, the mask
  rules, rectangles flipped and not, lines and polylines, fills and copies with
  wrapping, 480i - with all of VRAM compared after each. `--bisect` stops a scene
  at the first primitive to differ.
- **`gpu_test --hw-raster`** runs gpu_test's scenes - the fill rule, texture bit
  15 as the mask, the shared edge blended once, the displayed field - through it.
- **Verified:**
  - `hw_raster_test`: every scene identical to the pixel, on thirteen seeds -
    about half a million random primitives.
  - `gpu_test --hw-raster`: 73 of 73.
  - all twelve discs, 3,000 frames on WARP: all 36 checkpoint checksums and
    every frame-3000 picture identical to software, and every line of every
    report but the rasteriser's own pixel, clip and texel counters.
  - all twenty harnesses green, 2,462 checks, and gpu_test's 73 again through
    the hardware rasteriser.
- **Speed, on this machine's graphics card, 3,000 frames with the rasteriser's
  thread:** 2-10% slower than software at native size - Ridge Racer 1.14x real
  time against 1.27x, Wild Arms 2 1.23x against 1.26x, Captain Tsubasa J 1.26x
  against 1.29x. At 1x the software rasteriser has little to do, and the card
  costs a read-back every frame, which waits for it to finish, plus six 56-byte
  vertices a primitive. The read-back goes in phase 6; the vertices could be one
  instance a primitive if it matters once upscaling makes the card worth having.

### Phase 3, as built

Most of VRAM coherence came with phases 1 and 2: the dirty tiles and their
downloads, the read copy, save states through native VRAM, View VRAM and the
display through `Gpu::vram()` and `PrepareRead` on the machine thread. Phase 3
closed what was left and checked it.

- **Copies onto themselves.** A VRAM-to-VRAM copy whose source and destination
  share a pixel - wrapping included - is done as the console does it, pixel by
  pixel on native VRAM: both rectangles are brought up to date, the software
  rasteriser's loop runs, and the destination goes back to the card. The console
  smears such a copy, and now so does the card's rasteriser. Any other copy is
  still drawn on the card.
- **VRAM-to-CPU transfers** ask `PrepareRead` again for every word they read. It
  now answers at once when nothing has been drawn since it last made the same
  rectangle current, rather than looking at every tile of it each time.
- **A lost graphics card.** A driver reset, an update or a removed card fails
  every Direct3D call from then on, so downloads stop and the picture would
  freeze while the game ran on. `D3D11Raster` notices when a map fails and the
  device says it has been removed, and `RasterBackend::lost()` says why. `Gpu`
  looks once a frame and carries on with the software rasteriser from native
  VRAM as it was last read back; the machine's report tells the front end, which
  says so and ticks Software.
- **The front end** now takes which rasteriser is drawing from the machine's
  report, so the menu stays right after a boot or a reset remakes the machine.
- **`boot_runner`** refuses `--watch-vram` with `--hw-raster` - which command
  wrote each pixel is the software rasteriser's own accounting - and says in its
  report that the hardware rasteriser keeps no pixel counts.
- **Not done here:** per-pixel write attribution and the pixel counters under the
  hardware rasteriser. Nothing but the report and `--watch-vram` reads them.
- **Verified:**
  - **Framebuffer effects:** 26 discs - the twelve of the table and fourteen more
    picked for translucency, fog, masks, blur and in-engine intros (Silent Hill,
    Metal Gear Solid, Vagrant Story, Castlevania SOTN, Chrono Cross, Einhander,
    Spyro 3, Gran Turismo 2, Crash 3, Wipeout, Xenogears, Tekken 3, Driver 2,
    Valkyrie Profile) - 6,000 frames each with a checksum every 100, software
    against hardware on WARP: all 1,560 checkpoints identical, CD sectors
    included, and every frame-6000 picture the same to the pixel.
  - **Save states cross:** Legend of Mana saved at frame 3000 under each
    rasteriser and loaded under the other ran 600 more frames identical at every
    checkpoint, to the instruction, and to the pixel. The two states themselves
    differ only in 68 bytes that also differ between two software runs - not the
    rasteriser's.
  - **`hw_raster_test`**, 39 checks: the new scenes - draws, uploads and
    VRAM-to-CPU reads interleaved, with copies onto themselves (619 reads, every
    word the same), and the lost card - identical on eight seeds, and `--bisect`
    finds no primitive to differ even for a moment.
  - **The front end, on the graphics card:** View VRAM under the hardware
    rasteriser shows what software's does, and a reset keeps the menu ticking
    Hardware.
  - **All twenty harnesses green, 2,475 checks,** and gpu_test's 73 again
    through the hardware rasteriser.

### Phase 4, as built

- **Internal resolution, 1x to 8x** (`EmuConfig::resolution_scale`: 1-6 and 8).
  VRAM on the card is that many times its size each way, and every primitive is
  drawn over the sub-pixels it covers.
- **A console pixel's top-left sub-pixel is its own sample point.** There the
  edge functions are the software rasteriser's times the scale squared, exactly,
  so the shader gives that one sub-pixel the software rasteriser's integer
  arithmetic: its coverage, colour, texel and blend. Native VRAM is downloaded
  from those sub-pixels alone, by a pass into a native-sized target. So **what
  the machine sees is the same at every scale** - native VRAM, and so timing,
  save states and anything a game reads back - and the sub-pixels around each
  are the detail, interpolated in floating point between the same vertices.
- **Textures:** a paletted texture and its CLUT are read at their pixels' own
  sub-pixels, since an index cannot be finer. A 15-bit one - which may itself
  have been drawn at this scale, a framebuffer effect - is read at the
  sub-texel under each sub-pixel, and a sprite maps its sub-pixels onto its
  texels' one for one. Point-sampled throughout, so nothing bleeds in from
  outside a sprite; the plan's "sprite edges" worry belongs to filtering, which
  is not here.
- **True colour** (`EmuConfig::true_color`, on by default, above 1x only): no
  dithering, and eight bits a channel kept, the pixel underneath a blend
  included. It gives up the exact sub-pixel - with it, native VRAM is undithered.
- **Lines** are drawn as a box of sub-pixels per console pixel, so a line is as
  thick at 8x as at 1x.
- **Uploads** go to a native-sized texture on the card and are expanded over
  their sub-pixels by a pass, so an uploaded picture looks just as at native
  size. A copy onto itself (phase 3) does the same after running on native VRAM.
- **The shaders are compiled for their scale**, `SCALE` a constant, so at 1x they
  are the phase-3 shaders and nothing more.
- **Showing it:** `RasterBackend::ResolveDisplay` hands `Gpu` the display area at
  full size, drawn by a pass into a B8G8R8A8 texture so its rows are already the
  presenters' words, and read back a frame behind - this frame's is copied out,
  last frame's handed over - so it never waits for the card. One frame later on
  the screen, above 1x only. `Gpu::picture()` is what the front end publishes;
  `framebuffer()` stays the native picture for everything that measures the
  machine, and a front end that never shows it says so
  (`set_native_picture(false)`), which spares another wait a frame. 24-bit
  mode - films, uploaded at native size - is shown native.
- **Filters:** a multi-pass chain multiplies its picture's size, so all three
  engines skip one for a picture wider than 1024 - only the rasteriser's are.
  Single-pass filters still apply.
- **The graphics card:** Windows' default, the one driving the screen. Tried: the
  high-performance one. On this laptop - a Radeon 780M beside an RTX 4060 Laptop
  GPU - Ridge Racer at 4x ran at 1.29x real time on the 780M and 0.57x on the
  4060, whose every picture crosses PCIe to come back. Phase 6's shared texture
  wants the presenter's card anyway. `boot_runner` names the card it drew on.
- **The menu:** Settings > Video > Rasteriser lists 1x (Native) to 8x and True
  Colour, greyed while the software rasteriser draws. A change remakes the
  rasteriser between frames, keeping VRAM, as a change of rasteriser does.
- **Per game:** `gpu_rasteriser`, `resolution_scale` and `true_color` are
  `GameSettingKeys`, so a game with its own settings keeps its own.
- **`boot_runner --scale n [--no-true-color] [--shown-only]`**, and
  **`hw_raster_test --scale n`**.
- **Verified:**
  - **Exact at every scale:** `hw_raster_test --scale n`, true colour off, every
    scene identical to software's VRAM at 1x-6x and 8x, on four seeds at 2x-4x.
  - **The table at 4x on the graphics card**, true colour off: all 36 checkpoint
    checksums, non-black counts and sector counts identical to software's - the
    exactness holds on real hardware, not only on WARP.
  - **Pictures reviewed** side by side at 1x and 4x (the detail sheets): Ridge
    Racer's cars and road, Spyro 3's castle and dragon, Metal Gear Solid's
    geometry and its dithering gone, and no seams or cracks between polygons.
    Pre-drawn 2D (Valkyrie Profile, Wild Arms 2's text) is the same at any scale,
    and films stay native.
  - **Speed**, Ridge Racer 3,000 frames with the rasteriser's thread, on the 780M
    - measured together, the laptop by then running about 30% slower than
    earlier: software 1.25x real time; 2x 1.20x, 4x 1.02x, 6x 0.84x, 8x 0.72x.
    Without the picture's read-back 8x ran at 1.30x - the read-back is the cost
    above 4x, which phase 6 removes. The front end adds a copy of the picture
    and its upload on top: on the test disc 4x held about 58 fps and 8x about 26.
  - **All twenty harnesses green, 2,478 checks,** and gpu_test's 73 again
    through the hardware rasteriser.

### Phase 5, as built

- **`psx/pgxp.h`**: a `PreciseVertex` - the unrounded x and y, the depth, and the
  32-bit SXY word it was made for - beside every CPU register, every word of RAM
  (8 MB, allocated only while PGXP is on) and of the scratchpad.
- **A shadow is only used while its word still holds the value it was made for.**
  So only the instructions that move an SXY word carry shadows along, and nothing
  else has to clear them: any other write changes the value, which makes the shadow
  stale at no cost. That is what keeps it cheap enough to leave in the
  interpreter's hot paths behind one flag.
- **The GTE** keeps each projected vertex's position before `>> 16` rounded it, and
  its SZ3, beside the SXY FIFO (`Gte::Precise`); MTC2/LWC2 to an SXY register bring
  a matching shadow in.
- **The CPU** carries shadows on MFC2 and SWC2 (out of the GTE), LW and SW (with
  RAM and the scratchpad), and register moves (ADDU/OR with `$zero`, ADDIU/ORI with
  0). A store to GP0 hands its shadow to the GPU.
- **DMA channel 2** hands each word's shadow over with it, list and block mode.
- **The GPU** keeps shadows beside the words in its queue and command FIFO, and a
  polygon's vertex word with one becomes a `RasterVertex` with `fx`, `fy` and `w`.
- **The vertex cache.** Measured on Ridge Racer and Spyro 3, only half the vertices
  arrived with a shadow. The rest were GTE output moved by ways whole-word shadows
  cannot follow - storing x and y as two halfwords, most likely. So the GTE also
  remembers the last vertex it projected to each SXY word (`Gte::Recall`), and a
  vertex word without a shadow takes that one if it was projected this frame or the
  last. It is the same vertex almost always, and the same fraction wherever else it
  appears, so a vertex shared between polygons stays shared. Coverage went from
  48% to 98.7% in Ridge Racer.
- **The hardware rasteriser** draws a triangle with any precise vertex as kind 3:
  float positions, the same fill rule, each edge worked out from its ends in one
  fixed order so the triangle across a shared edge gets exactly the opposite value,
  and texture coordinates interpolated in perspective when every vertex has a
  depth.
- **A triangle flat at whole pixels** is not dropped when its vertices are precise.
  Far off, a strip of road thinner than a pixel rounds flat; at whole pixels its
  neighbours cover its row, but drawn where they really are they leave its band
  open. That was the first picture's horizontal streaks across Ridge Racer's road.
- **Precise culling** (off by default): NCLIP from the unrounded positions, keeping
  a triangle's sign when the fractions say it is not edge-on.
- **The recompiler** keeps no shadows, so `System::StepImpl` runs the interpreter
  while PGXP is on - on only while the setting asks and the hardware rasteriser
  draws.
- **The machine never sees any of it**, precise culling apart: the words, and
  everything done with them, are the rounded ones. With PGXP on, every disc runs
  the same instructions and reads the same sectors as with it off.
- **Settings > Video > Rasteriser:** PGXP: Precise Vertices, Perspective-Correct
  Textures, Precise Culling; per game too. Not in save states.
- **`boot_runner --pgxp [--pgxp-culling] [--no-pgxp-textures]`**, which reports
  how many polygon vertices were drawn precisely.
- **Verified:**
  - **Nothing changes with it off:** the twelve-disc table through the software
    rasteriser, every report line byte-identical to before phase 5.
  - **Nothing the machine does changes with it on:** the twelve discs at 2x with
    `--pgxp`, every checkpoint's instruction and sector counts the same as without.
  - **How much is drawn precisely** (polygon vertices, `boot_runner --pgxp`): Ridge
    Racer 2,206,935 of 2,234,938 (98.7%); Wild Arms 2 191,166 of 196,550 (97%);
    Spyro 3 3,149,300 of 3,530,418 (89%); Metal Gear Solid 7,730,026 of
    13,303,521 (58%); the BIOS logo 25,854 of 31,238. **Ace Combat 3 gets none of
    its own:** 25,854 of 367,530, the logo's alone - the words it sends are not
    ones the GTE projected, so it draws exactly as without PGXP. Most of the other
    discs are still in the logo or a film by frame 3,000.
  - **A/B, PGXP off and on,** Ridge Racer at 4x, the kerb beside the road: the
    road's edge strip, a chain of small polygons, zigzags without it where each
    vertex was pushed to a whole pixel, and is one straight line with it; the joins
    between the kerb's blocks run straight across instead of kinking. Wobble is
    motion, so a still shows only its cause - vertices on whole pixels - and here
    it is gone. Spyro 3 and Metal Gear Solid at 4x reviewed
    with it on: no cracks, streaks or misplaced polygons.
  - **What is left:** a few one-pixel specks near Ridge Racer's horizon, where a
    precise triangle meets one whose vertices arrived without a shadow and were
    drawn at whole pixels. That is PGXP's known limit - every implementation has
    vertices it cannot follow.
  - **Speed**, Ridge Racer 3,000 frames on the 780M, measured together: 4x 0.97x
    real time, 4x with PGXP 1.00x, software 1.23x - no cost measurable. Its cost
    is the interpreter: with the recompiler on, turning PGXP on gives that up.
  - **The front end:** the three PGXP items tick, grey out under the software
    rasteriser, and follow `psxemu.ini`.
  - **All twenty harnesses green, 2,480 checks** - `gte_test` and `cpu_test`
    untouched by the shadows beside their registers - and gpu_test's 73 again
    through the hardware rasteriser.

### Phase 6, as built

**The picture handed over on the card.**

- **`psx/shared_picture.h`**: a `SharedPicture` - an NT handle to a Direct3D 11
  texture, exactly the picture's size, B8G8R8A8 - and the `SharedPictureSource`
  behind it, which a presenter waits on (`WaitReady`) and gives back (`Release`,
  and `Dropped` for a frame never shown). To the core these are handles and
  numbers: `RasterBackend::ResolveDisplay` fills one instead of pixels, `Gpu`
  keeps it (`shared_picture()`), and `Machine` puts it in the `VideoFrame`.
- **The rasteriser** draws the display area into one of five shared textures,
  signals a fence behind it and sends the work to the card; nothing waits. A
  texture is drawn into again once the presenter's card has moved past its picture
  or the frame carrying it was dropped - which `FrameMailbox::Publish` now says.
  With all five still in use, the picture is read back as before, and a card that
  will not share a texture reads back from then on. The source, and with it the
  handles, lives as long as any frame refers to it, past the rasteriser: a frame
  in flight when the rasteriser is remade is still shown.
- **The presenter waits for the fence on its own thread**, a CPU wait with a
  quarter-second limit, rather than on its card: a producer's card that goes
  cannot then hang the presenter's, and the machine's thread never waits for either.
- **Each presenter** takes it its own way, and says which card it draws on
  (`IGraphicsEngine::SharedPictureAdapter`):
  - **Direct3D 12** opens the texture (`OpenSharedHandle`) and copies it on the card
    into the frame's own texture, so filters, chains and the overlay's glass read it
    as they read an upload.
  - **Direct3D 11** opens it (`OpenSharedResource1`) and draws straight from it.
  - **Vulkan** imports it as an image (`VK_KHR_external_memory_win32`, a dedicated
    allocation), takes it over from outside in the general layout, copies it into
    the frame's image and hands it back. The declarations added to `vk_functions.h`
    were checked against the Khronos headers by a scratch build, as the rest were.
  - **OpenGL** opens the texture's memory as a GL memory object
    (`GL_EXT_memory_object_win32`, the Direct3D 11 image handle type), makes a texture
    over it, and draws that into the frame's texture, swapping red and blue: desktop GL
    has no BGRA internal format to declare it with, so it reads Direct3D's B, G, R, A
    as R, G, B, A. The card comes from the extension's own LUID query, so no Direct3D
    device is made on the GL side at all. (Built first on `WGL_NV_DX_interop` and a
    blit, which worked, and leaked - see "After phase 6".)
  - Each hands pictures back once its own fence, query or sync object says the
    frame that read them is done; the picture on the screen stays theirs, so a
    frame drawn again under a moving overlay, or after a renderer switch, still has it.
- **Which card.** The front end tells the rasteriser's factory the renderer's
  adapter (`App::OnPresenterAdapter`), and the rasteriser is made on that card with
  shared pictures on - remade, keeping VRAM, whenever the renderer changes card. A
  renderer that cannot take them gets pixels, as before. On this laptop Direct3D
  11, 12 and OpenGL draw on the Radeon 780M, and Vulkan on the RTX 4060 - so under
  Vulkan the rasteriser now draws on the 4060 too, and without the read-back that
  made it slow there in phase 4.
- **Screenshots and save-state thumbnails** of a shared picture read it back
  through a device of their own (`D3D11Raster::ReadSharedPicture`).
- **Emulation > Show Timings** says `hand-off ... on card` while it is in use.
- **`boot_runner --shared-picture`**, as a presenter that takes each picture at
  once; `--ppm` reads the last one back.

**PGXP under the recompiler.**

- **Every GTE instruction and every COP2 move is interpreted anyway** - the
  recompiler compiles none of them - so the shadows those carry already worked.
  What compiled code did without them was word loads and stores, and register copies.
- **Word loads and stores** call out to `RecompilerBridge` for every access, with
  the instruction's pc; the bridge reads the LW or SW there for its register and
  carries the shadow exactly as the interpreter's LW and SW do. No emitted code
  changes for it.
- **Register copies** - `addu`/`or` with r0, `addiu`/`ori` with 0 - call a new
  `BlockState::move` while PGXP is on (`Recompiler::set_track_moves`), and blocks
  compiled the other way are thrown away when it changes. With it off, not a byte
  is emitted.
- **Found on the way: bug 126.** Compiled loads had always handed their callback
  the wrong pc - R8, where the store's pc goes in R9 - which lost 18% of Ridge
  Racer's precise vertices here, and would have sent a faulting compiled load's
  exception somewhere arbitrary.
- **`System::StepImpl`** no longer gives up the recompiler for PGXP.
- **`boot_runner`'s pgxp line** now says how many of the precise vertices were
  found by value in the GTE's cache (phase 5) rather than arriving with a shadow.

**Verified:**

  - **Shared, the same picture:** Ridge Racer at 4x, the shared picture at frame
    1,500 pixel-identical to the read-back one handed over a frame later (the
    read-back runs a frame behind). `hw_raster_test`'s new scene: the picture on the
    card equals the read-back one, five held pictures make the sixth read back, a
    dropped frame's texture and a released one are drawn into again and the one on
    screen is not, and a picture outlives its rasteriser.
  - **The front end, on all four renderers:** the test disc shows `on card` on
    Direct3D 11 and 12, Vulkan and OpenGL, each switched to live and back, at 1x
    (pixels) to 8x; the pictures correct, and F12's screenshot from Vulkan on the
    4060 byte-identical to Direct3D 12's on the 780M.
  - **Speed, Ridge Racer 3,000 frames** with the front end's path (`--shown-only`),
    one after another on the 780M: software 1.57x real time; 4x read back 1.17x, on
    the card 1.38x; 8x read back 0.61x, on the card 1.34x. In the front end at 4x
    the hand-off went from 1.7 ms to 0.02 ms and a frame's emulation from 14.8 ms to
    9.8 ms. OpenGL's present is the one that grew, 4 to 7-10 ms: the interop's lock.
  - **PGXP under the recompiler, six discs, 3,000 frames at 2x:** every
    checkpoint's instructions, sectors and GP0 words the same as the recompiler
    without PGXP; precise vertices exactly the interpreter's on five, and Spyro 3's
    within 0.01%.
  - **PGXP's cost**, Ridge Racer 3,000 frames at 4x on the card, one after another:
    the interpreter 1.64x real time, with PGXP 1.62x; the recompiler 4.19x, with
    PGXP 3.98x - 1% and 5%. At 8x with PGXP, what phase 5 ran at 0.83x (the
    interpreter, the picture read back) runs at 2.75x.
  - **The twelve-disc table, three ways:** through the software rasteriser, every
    report line the same as phase 5's; with `--recompiler`, all 36 checkpoints'
    pictures the software table's, and the sector counts off by one in the same
    three places as on 2026-09-25 - bug 126's fix changed nothing there; and at 4x
    on the card (`--shared-picture --no-true-color`), every checkpoint's picture and
    sector count the software table's, with 582 to 2,879 of each disc's 3,000
    frames handed over on the card - the rest films, which are shown native, or a
    blank display.
  - **All twenty harnesses green, 2,494 checks** - `rec_test` 467 (the load's pc,
    register copies reported only when asked), `hw_raster_test` 45, `host_test` 34 -
    and gpu_test's 73 again through the hardware rasteriser.

### After phase 6: choosing the graphics card

**Settings > Video > Graphics Card** lists the machine's cards - Automatic, then each by name
with its video memory - and applies to every renderer and to the hardware rasteriser. With
one card it says so and offers nothing. It is saved as `graphics_adapter`, the card's *name*
(a card's LUID changes with every restart); a name that is not there - an external card,
unplugged - is kept and means Automatic until it comes back. It is a setting about the
machine, not a game's, so it is not one of the per-game keys.

- **Automatic is what there was**: each renderer's own choice - Windows' default for Direct3D
  11 and 12, the discrete card for Vulkan, whatever the driver gives OpenGL - and the
  rasteriser on that card, handing its pictures over.
- **A card chosen** is asked of each engine before it starts
  (`IGraphicsEngine::SetPreferredAdapter`): Direct3D 11 and 12 make their device on it,
  Vulkan picks it (by LUID where the driver gives one, by name where it does not), and the
  rasteriser draws on it. The renderer is made again on a change, the rasteriser after it,
  both keeping VRAM. If no renderer will start on the card, Windows' pick is used and it is
  said.
- **OpenGL cannot be told.** On Windows the driver and Windows' own per-app graphics setting
  decide which card a GL context lands on, and nothing an application asks changes that. It
  stays where it is; the rasteriser still goes to the card chosen, its pictures are copied
  across through memory (the read-back of phase 4, so slower on a discrete card), and a
  notice says so.
- **The rasteriser hands its pictures over on the card only when it is on the renderer's**;
  on any other pair they are read back. One that cannot make a device on the card asked
  for draws on Windows' default and reads back, rather than none at all - the presenter
  would otherwise refuse every frame.
- **`graphics/adapters.h`** (header-only, so the tools use it too): the cards by name and LUID,
  `OpenAdapter`, and the LUID as Windows' counters spell it.
- **`boot_runner --gpu <part of a name>`** draws the rasteriser on that card, and
  **`--list-gpus`** lists the cards with the LUIDs Windows' `\GPU Engine` and
  `\GPU Process Memory` counters name them by.

**What testing it found - all in phase 6's own code**, and worth knowing about (bug 129):

- **A livelock.** The rasteriser rotates its pictures through a fixed set of textures, and a
  presenter hands one back only while it is drawing shared pictures. When the set was
  briefly full - the first frames after start - one frame went out as pixels, after which
  nothing was ever handed back and every frame after was read back: 20-29 fps at 8x, for
  good, in about half the Direct3D 11 starts I tried. Every presenter now hands the last
  picture back from a frame of pixels as well; six starts in a row then ran at full speed on
  the card.
- **Too few textures**, five; the presenter releases a few frames behind. With eight
  (`kSharedTextureCount`) none ran out in six starts, where five ran out in 3 to 36 of the
  first 300 frames. The presenters' caches of what they have opened were sized to match: left
  at six they would have closed and reopened textures constantly, and Windows' counter
  read 5.4 GB for Direct3D 11 in the one run made in that state.
- **A real leak in AMD's OpenGL interop, and OpenGL's way of taking a picture changed
  because of it.** The first OpenGL path registered each shared texture with
  `WGL_NV_DX_interop` and blitted it. Every switch to OpenGL then cost about 550 handles and
  18 threads, and - found by watching the whole machine, not the process - about 300 MB of
  RAM at 8x: free memory fell 45.0 → 43.5 GB and committed memory rose 21.9 → 23.5 GB over
  six cycles, scaling with picture size (75 MB a session at 4x). It is the driver: a scratch
  program with none of the emulator in it loses 56 MB for every three textures it registers
  at 2560x1920 - one picture's worth each - however they are unregistered, and a Direct3D
  device it registers them on keeps its ~550 handles. Keeping one device for the whole
  process (tried) stopped the handles and not the memory. Importing the same textures as
  memory objects (`GL_EXT_memory_object_win32`) leaks nothing in the same program, and is
  what OpenGL uses now: over six sessions at 8x free RAM, committed memory and handles are
  flat, and the baseline is about 550 handles lower for having no Direct3D device at all.
  The probe reading every picture back through GL against Direct3D's own read of the same
  texture, with all eight textures in turn and no wait beyond the rasteriser's fence: 800
  pictures, none wrong.
- **Windows' per-process GPU memory counters** - the ones Task Manager shows - climb when a
  second device opens shared textures, and do not come back down, though nothing is held:
  free RAM, committed memory, what the card holds across all processes and the process's
  own `QueryVideoMemoryInfo` all stay flat, and a rasteriser made and destroyed in a loop
  returns every one of them to zero. A figure that only ever grows there after switching
  renderer or taking a screenshot is that counter, not a leak.

**Verified**, with Windows' own counters naming the card each process really allocated on,
the test disc at 8x, before the RTX 4060 dropped out (below):

| Renderer | Automatic | Radeon 780M | RTX 4060 |
|---|---|---|---|
| Direct3D 11 | 780M | 780M | 4060, on card |
| Direct3D 12 | 780M | 780M | 4060, on card |
| Vulkan | 4060 (discrete, as before) | 780M | 4060 |
| OpenGL | 780M | 780M | rasteriser 4060, GL on the 780M, 28 fps: copied across (measured on OpenGL's first path; the copy across is the ordinary read-back either way) |

and switched live from the menu on Direct3D 12 - 4060, 780M, 4060, 780M, 4060, 780M,
Automatic - each on card and each written to `psxemu.ini`. `boot_runner --gpu` drew Ridge
Racer on each card by name and said so. Since then, on the Radeon alone, the four renderers
each showed the right picture with the right colours on the card, and **all twenty
harnesses are green, 2,497 checks** - `media_test` 389, three more for the card's name
round-tripping through the settings file - with `hw_raster_test` 45, `host_test` 34 and
`rec_test` 467 as before.

**A caution about testing this.** The test laptop restarted in the middle of a run (bugcheck
0x9F, a driver blocked on a power request, 2026-09-29), after a long series of runs that
created and destroyed devices on its discrete card, and the RTX 4060 has not come back as a
Windows display device since. The dump could not be read here, so the cause is not
established: cycling a hybrid laptop's discrete card is the suspect, and MSI Afterburner
with RivaTuner - which hooks every process's graphics calls and polls the cards' power
state - was running on it, and was closed afterward. Runs since have been on the Radeon
only, and with neither running the OpenGL handle growth above still reproduced, so that is
not RivaTuner's.

---

## How it will be verified

- **Timing, identical.** The hardware rasteriser shares every line of costing
  code, so with it selected every disc must run the same instruction count and
  read the same CD sectors at every checkpoint of the twelve-disc table. A
  difference is a bug in the seam, not in the drawing.
- **Pictures at 1x, identical** (since phase 2; the plan asked only for close).
  `boot_runner --hw-raster --warp` runs the table on WARP, and each checkpoint's
  checksum and `ppm_diff` of each frame-3000 picture must match the software
  rasteriser's. The software rasteriser stays the reference: it has been checked
  against real hardware through bugs 58-105.
- **The GPU scenes**: `hw_raster_test` draws random scenes of every kind through
  both and compares all of VRAM; `gpu_test --hw-raster` runs gpu_test's own.
- **Upscaled and PGXP pictures** are not comparable to anything, so they are
  reviewed by eye, disc by disc. The contact sheets used for the Accuracy check
  (2026-09-26) are the tool.
- **Every existing harness stays green** at every phase. Phase 0 is only done
  when the whole table is byte-identical.

---

## Risks, and the decisions that are yours

- **Framebuffer effects.** Games that draw into VRAM and read it back - blurs,
  motion trails, screen transitions, some FMV players - are where every
  hardware renderer has its bugs. The dirty-rectangle rules exist for them. The
  per-game switch back to software is the fallback, and a list of such games gets
  kept like the twelve-disc table.
- **2D games at high resolution** can show seams between sprites. Sprite edge
  clamping handles most; the rest are a per-game setting.
- **Decision 1 - shared front half or a fully separate core.** Recommended:
  shared, for the reasons above.
- **Decision 2 - Direct3D 11 first.** Recommended: it is the most forgiving API,
  works on every machine this runs on, and WARP makes it testable without a
  graphics card. Vulkan would be the alternative if Linux ever mattered.
- **Size.** Roughly 15-20 sessions to phase 6, the largest piece of work in the
  project so far. Phase 4 alone is the usable feature.

---

## A note on sources

DuckStation (CC-BY-NC-ND) and Beetle PSX and PCSX-R's PGXP (GPL) were read for
how they approach this; none of their code is copied. The techniques - two VRAM
copies, depth-buffer mask emulation, two-pass semi-transparency, shadowed vertex
positions - are described publicly and are implemented here from scratch against
this emulator's own `DrawJob`s.
