# NVIDIA DLSS 4.5 and DLSS 5

**Status: plan (2026-09-29), nothing built. DLSS 4.5 Super Resolution, DLAA and 2x Frame
Generation are possible on this laptop's RTX 4060 once it is back (it has shown as "Unknown" since
the 0x9F restart). DLSS 5 is not possible yet: there is no public SDK for it, and it runs on RTX 50
cards only until an RTX 40 update NVIDIA has promised for "later this fall".**

DLSS is not a filter over a finished picture. Every version since 2.0 is *temporal*: it rebuilds each
frame from the last ones, and to do that it needs to know, for every pixel, how far away it is and
where it was a frame ago. A PC game has both for free - a depth buffer and a camera. The PlayStation
has neither: no depth buffer, no camera, and each frame is a fresh list of 2D triangles with nothing
saying which triangle last frame they were. Most of this plan is making those two things, and it is
work on the machine's side, not on NVIDIA's.

---

## The conclusion first

- **One integration, in the Direct3D 12 renderer, through Streamline.** Frame Generation exists only
  on Direct3D 12 and Vulkan; Streamline works with one device in a process; Vulkan's Frame
  Generation has no v-sync. So Super Resolution and Frame Generation both run in the Direct3D 12
  renderer, on the card the rasteriser draws on. The Direct3D 11 rasteriser stays as it is and hands
  over one more texture beside its picture. The other three renderers get no DLSS.
- **The hard part is motion vectors, and it is the core's.** The GTE learns where each vertex it
  projects was in the last picture, and that travels to the GPU the way PGXP's unrounded position
  already does - through the same shadows, DMA and cache. Sprites get theirs by matching.
- **Most of it needs no NVIDIA card.** The planes, the motion vectors and the jitter are rasteriser
  and core work, built and checked on the Radeon and WARP with tools that score motion without DLSS.
  The RTX 4060 is needed from phase 4, and then for a handful of long runs, not loops.
- **The machine never sees any of it**, the rule PGXP follows: with DLSS's inputs on, every disc runs
  the same instructions and reads the same sectors. Jitter gives up the exact sub-pixel, as true
  colour and PGXP already do.
- **DLSS 5 is gated, not planned.** Streamline 2.14.1's public headers name a `kFeatureDLSS_NR` and
  two "uplift" colour buffers and nothing else: no options header, no plugin, no guide. Nothing
  DLSS 5-specific gets built until NVIDIA publishes those and ships RTX 40 support. What it is known
  to take - colour and motion vectors - comes from the DLSS 4.5 phases anyway.
- **Size:** roughly 12-17 sessions to Frame Generation; the motion vectors are about a third of it
  and the one part whose outcome is not certain.

---

## What there is to integrate (2026-09-29)

| Feature | Cards | APIs | Needs from us | Here |
|---|---|---|---|---|
| **Super Resolution** (DLSS 4.5: presets K, L, M) | any RTX | D3D11, D3D12, Vulkan | colour at render size, depth, motion vectors, jitter | yes |
| **DLAA** - Super Resolution at 1:1, as anti-aliasing | any RTX | same | same | yes |
| **Frame Generation 2x** | RTX 40 and 50 | D3D12, Vulkan | the above, plus Reflex, a swap chain Streamline owns, hardware-accelerated GPU scheduling, a HUD-less picture or a UI mask | yes, 4060 |
| **Multi Frame Generation 3x-6x, Dynamic** | RTX 50 | D3D12, Vulkan | as 2x | not testable here |
| **Ray Reconstruction** | any RTX | D3D12, Vulkan | a ray-traced renderer's normals, albedo, roughness, hit distances | not applicable: nothing to trace |
| **DLSS 5** ("neural rendering", `kFeatureDLSS_NR`) | RTX 50; RTX 40 promised this autumn | not published | colour and motion vectors; "trained to recognise" albedo, lighting, normals | not yet |

The presets, from Streamline's `sl_dlss.h`: **K** is the default for DLAA, Quality and Balanced (the
first transformer model); **M**, DLSS 4.5's second-generation transformer, is the default for
Performance; **L**, the same generation and the most expensive, for Ultra Performance. The current
package is Streamline 2.14.1 with NGX 310.9.1.

---

## What the PlayStation gives, and what is missing

| DLSS wants | The PlayStation | Where it comes from here |
|---|---|---|
| Colour at the render size | the picture, 1x-8x | the rasteriser's shared picture, as now |
| Depth | no depth buffer at all | PGXP's `w` - the depth each vertex was projected from - where there is one: 98.7% of Ridge Racer's vertices, 89% of Spyro 3's, 58% of Metal Gear Solid's, none of Ace Combat 3's. 2D has none |
| Motion vectors | nothing | **new**: where each vertex and sprite was in the last picture |
| Jitter | nothing | **new**: a sub-pixel offset per picture in the rasteriser's shader |
| Camera matrices | none - the GTE takes one matrix per model with camera and object combined | identity, with "camera motion included" set: all motion is in the vectors |
| Exposure | 8-bit pictures | fixed, 1.0 |
| A HUD-less picture (Frame Generation) | the game draws its HUD into the same VRAM | none for the game's HUD; **our own overlay** can be separated exactly |
| History resets | nothing marks a cut | **new**: heuristics below |
| One evaluation per game frame | a 30 fps game shows each picture for two vblanks, and `VideoFrame::number` counts vblanks | **new**: a "new picture" mark |

---

## The design

### Where DLSS runs

- **In `D3D12GraphicsEngine`, through Streamline**, loaded only when DLSS is asked for on an NVIDIA
  card: `LoadLibrary` of `sl.interposer.dll`, its signature checked (`sl::security::
  verifyEmbeddedSignature`, full path), `slInit` with `eUseManualHooking`, and only this renderer's
  device and swap chain upgraded (`slUpgradeInterface`). Nothing else in the process - the
  rasteriser's Direct3D 11 device, the other renderers - passes through Streamline. Without the DLLs,
  on the Radeon, or with anything failing, the renderer is exactly today's, and the menu says why.
- **`slIsFeatureSupported` by the card's LUID** decides what the menu offers; the LUIDs are
  `graphics/adapters.h`'s.
- **The rasteriser and the renderer both on the NVIDIA card**, so the hand-off stays on the card
  (phase 6's rule). Settings > Video > Graphics Card already does that. The laptop's screen hangs off
  the Radeon, so Windows copies each presented frame across; measured in phase 4.

### A plane beside VRAM

The planes must be VRAM-shaped, not screen-shaped: a game draws its next frame into one buffer while
showing the other, and what DLSS gets with a picture must be what was drawn into *that* buffer.

- **One more target in the rasteriser, the size of upscaled VRAM, RGBA16F**: motion (x, y, in
  sub-pixels) in RG, depth in B, flags in A - motion unknown, translucent. Written by the same draws
  as a second render target.
- **Per kind of job:**
  - triangles write it; a precise one's depth from `w`, anything else's depth unknown (far)
  - translucent pixels leave what is under them and set the translucent flag - a blend state on the
    second target alone, since the colour target does its own blending in the shader
  - mask-rejected pixels are discarded, from both
  - rectangles and lines take their motion from sprite matching (below), depth far
  - fills write zero motion, far, no flags
  - VRAM-to-VRAM copies carry the plane along; a copy onto itself (phase 3's CPU path) clears it
  - uploads write zero motion, far, and "unknown"
- **`ResolveDisplay` copies the displayed area of the plane** into a second shared texture in the
  picture's slot, same serial. `SharedPicture` gains its handle, the picture's jitter and two flags:
  *new picture* and *reset*.
- **Not machine state.** Native VRAM, downloads and save states are untouched; a loaded state starts
  a new history.

### Motion vectors: where each vertex was

- **The core works out, per vertex, where it was in the last picture**, and `RasterVertex` carries it
  (`px`, `py`, and whether it is known) beside `fx`, `fy` and `w`. The shader interpolates it across
  the triangle - in perspective where there is depth - and writes this position minus that one.
- **A picture ends** when the display start moves (GP1(05h), a double-buffered game's flip), or at
  vblank when the game draws into the area being shown. The GTE keeps this picture's positions and
  the last's.
- **Which vertex is which** - the research. Candidates, to be measured rather than guessed:
  - the vertex as the model holds it - VXY and VZ's values - which a rigid model repeats every frame
  - the RAM address it was loaded from, which LWC2 knows and PGXP's word shadows can carry
  - its place among the picture's projections, which follows the game's object list - usually far
    steadier than GPU order, which is sorted by depth
  - combinations. Each key maps to *all* of the last picture's positions under it, the nearest wins,
    and past a limit none does - so the same model drawn twice, or a vertex shared by polygons, does
    not have to be told apart exactly.
- **It travels as PGXP does.** `PreciseVertex` gains the last position, so MFC2/SWC2, LW/SW, register
  moves, DMA and the GTE's value cache (`Gte::Recall`) carry it with no new paths. So DLSS needs PGXP
  on, and turns it on.
- **Sprites and triangles without a GTE vertex** match on texture page, CLUT, u, v and size, nearest
  last position. A scrolling tile map then matches each tile with the one that moved least, which is
  right while it scrolls less than half a tile a picture.
- **Unknown stays unknown**: zero motion and the flag set. To Super Resolution the flag is the "bias
  current colour" hint - trust this frame here - and to Frame Generation it is the invalid-motion
  value.
- **Reset** on a display mode or size change, a film (24-bit display), a reset or loaded state, the
  rasteriser remade, and a picture in which fewer than some share of vertices matched the last.
- **Tools:**
  - `boot_runner --motion`: vertices matched, pixels with motion
  - **a warp check**: the last picture moved by this one's vectors and compared with this one,
    scored against doing the same with zero motion. That judges motion vectors with no DLSS and no
    NVIDIA card.
  - `--ppm-motion`, `--ppm-depth`
  - Video > View Motion and View Depth in the front end, like View VRAM

### Jitter

- **A Halton (2, 3) offset per picture**, cycling over at least 8 x (output / input)^2 pictures,
  NVIDIA's rule.
- **In `PsDraw`**: triangles, ordinary and precise, are decided and interpolated at each sub-pixel's
  position plus the offset. The exact sub-pixel's integer path is off while jittering, as under true
  colour. Rectangles, lines, fills and copies are not jittered: 2D stays on its grid.
- **The offset a picture was drawn with goes with it**, set at the flip that starts it.
- **Off unless DLSS is on.** Nothing changes otherwise.

### Depth

- **1/w interpolated across the screen, as view depth**, turned by the renderer into the depth DLSS
  takes. Streamline also has a linear-depth buffer type; which of the two DLSS does better with is
  measured, not assumed.
- **Painter's order is fine.** The PlayStation draws last-over-first with no depth test, and the
  plane records the depth of whatever is visible. Unknown depth is far.

### Super Resolution and DLAA

- **In**: the picture, at scale S. **Out**: its rectangle on the screen. The mode picks S: DLSS's
  ratios - Quality 1.5, Balanced 1.72, Performance 2, Ultra Performance 3, DLAA 1 - give an input
  size, and S is the whole scale nearest it, 1-8. Anything between NVIDIA's minimum and maximum
  (`slDLSSGetOptimalSettings`) is accepted.
- **Non-square pixels** (256 or 368 wide on a 4:3 screen): tried with a different ratio each way.
  If DLSS will not take that, it outputs at the input's shape and today's scaling does the aspect.
- **Presets**: Auto (K, M, L by mode, as NVIDIA's defaults), or K, L or M chosen.
- **Once per new picture.** A repeated vblank shows the last output again, so a 30 fps game does not
  feed DLSS the same frame twice.
- **Films and 480i are not upscaled**: shown as now, with the history reset.
- **Filters**: multi-pass chains are already skipped for pictures this wide; single-pass ones apply
  after DLSS.
- **Screenshots** stay the game's picture, as now.

### Frame Generation

- **The swap chain made through Streamline**, and made again whenever Frame Generation turns on or
  off (NVIDIA requires it). The renderer already remakes itself on such changes.
- **Only new pictures are presented.** A 30 fps game gives 30 presents a second and Frame Generation
  makes 60; a 60 fps game gives 120, this laptop's panel rate.
- **Reflex markers**, which Frame Generation requires:
  - on the machine's thread: simulation start and end around a frame, and input sample where the
    pads are read
  - on the video thread: render submit and present, start and end
  - frame ids: the pictures' serials
- **Pacing.** Reflex expects to own the frame rate (`slReflexSleep`, `frameLimitUs`). With Frame
  Generation on, the machine's limiter would hand over to Reflex's at the console's own rate. Any
  speed but 100% turns Frame Generation off.
- **Our overlay is the UI layer.** It draws into its own target, premultiplied, tagged as UI colour
  and alpha. The screen before it is the HUD-less picture. A menu or glass window over the picture
  turns generation off, with its resources kept.
- **The game's own HUD cannot be separated** and will waver between generated frames. An experiment,
  off by default: 2D drawn after the last 3D polygon of a picture, taken as the UI mask.
- **Latency**: about one picture more, 33 ms for a 30 fps game. A per-game setting.
- **Requirements the menu checks**: hardware-accelerated GPU scheduling on, Windows 10 2004 or later,
  v-sync interval 0 or 1.
- **RTX 50's 3x-6x and Dynamic**: offered from `numFramesToGenerateMax`, but not testable on this
  machine.

### DLSS 5

**What is public:**

- It launched on 3 September 2026 in NBA 2K27, on RTX 50 cards, desktop and laptop. RTX 40 support is
  promised for "later this fall".
- NVIDIA says it takes colour and motion vectors, and is trained to recognise albedo, lighting and
  normals.
- It has three models, Structure and Tone intensity controls, and semantic and engine-level masks.
- Integration is through Streamline or Unreal Engine 5.
- Streamline 2.14.1's headers name `kFeatureDLSS_NR` and `kBufferTypeUpliftInputColor` /
  `OutputColor`. No options, no plugin DLL, no guide.

**What it would take, when it can be done:**

- The same place: the Direct3D 12 renderer, Streamline, the planes and motion from phases 1-3.
- **Albedo and lighting planes are nearly free here**, if its guide asks for them. A textured
  PlayStation polygon's colour is exactly the texel times the vertex colour, so the rasteriser can
  write the texel before modulation as albedo and the vertex colour as lighting.
- **Normals** would come from the depth plane's slopes, since most games keep none that reach the
  GPU.
- **An engine mask** would keep 2D, text and films out of it.
- **Opt-in only**: off by default, never a per-game default, and screenshots taken with it on say so.
  It invents detail the game does not have, which is the opposite of what the rest of this emulator
  checks for, and at the PlayStation's polygon counts it will reinterpret more than it does in a
  modern game.

**The gate:**

- NVIDIA publishes the plugin and its guide.
- RTX 40 support ships, since this machine's only NVIDIA card is a 4060 Laptop.
- A first run shows the 4060 can carry it at 60 fps.

### Settings and the menus

- **Settings > Video > NVIDIA DLSS**:
  - Off, DLAA, Quality, Balanced, Performance, Ultra Performance
  - Preset: Auto, K, L, M
  - Frame Generation: Off, 2x, and 3x, 4x and Dynamic on RTX 50
- **Greyed with the reason**: not Direct3D 12, not an NVIDIA card, the software rasteriser, GPU
  scheduling off, the DLLs missing.
- **Per game**: `dlss_mode`, `dlss_frame_generation`.
- **Emulation > Show Timings** shows DLSS's milliseconds and whether frames are being generated.
- **The About box** carries NVIDIA's attribution, as the licence requires.

---

## Phases

| # | What | Done when | Card | Rough size |
|---|---|---|---|---|
| 0 | The SDK read, the licence decided, `sl_probe` (one run: each card's LUID, `slIsFeatureSupported` for DLSS and Frame Generation, GPU scheduling, driver) | the 4060 back and reporting both features | 4060, once | 1 |
| 1 | The plane beside VRAM: second target, the rules per kind of job, carried in the shared picture, depth from `w`, View Depth, `--ppm-depth`; zero motion | the twelve-disc table's instruction and sector counts identical with it on; all harnesses green; depth reviewed on five discs | Radeon, WARP | 2 |
| 2 | Motion vectors: picture boundaries, the GTE's two tables, the identity keys measured, sprite matching, flags, resets, `--motion`, the warp check | the warp check beats zero motion on every 3D disc of the 26; the keys chosen by numbers | Radeon, WARP | 3-5 |
| 3 | Jitter | the table identical in the machine's terms; the warp check unchanged with the jitter taken out | Radeon, WARP | 1-2 |
| 4 | Super Resolution and DLAA in the Direct3D 12 renderer through Streamline; the menus; per-game keys | pictures reviewed at the table's checkpoints; ghosting no worse than without on the 26 discs; the cost measured | 4060 | 2-3 |
| 5 | Frame Generation: the swap chain, Reflex on both threads, new-picture presents, the overlay as UI, pacing | 30 to 60 and 60 to 120 even (PresentMon); latency measured (Reflex's own stats); the overlay clean | 4060 | 3-4 |
| 6 | DLSS 5 | the gate above | RTX 50, or RTX 40 after its update | unknown |

A usable feature is the end of phase 4. Phases 1-3 are worth having without it: View Motion and the
warp check are the first tools here that can tell whether a game's picture moves smoothly.

---

## How it will be verified

- **The machine, unchanged**: the twelve-disc table through `boot_runner --hw-raster --pgxp` with
  DLSS's inputs on, every checkpoint's instructions and sectors the same as with them off; the
  software rasteriser's table byte-identical; every harness green.
- **Motion vectors, scored**: the warp check and coverage on the 26 discs of phase 3's framebuffer
  list, before any DLSS - a number per disc, kept like the table.
- **DLSS's pictures** are not comparable to anything, so by eye: contact sheets at the checkpoints,
  and short clips of motion, where ghosting shows.
- **Frame Generation**: PresentMon for pacing, Reflex's latency stats, and the HUD reviewed game by
  game.
- **On the 4060, gently** (the 0x9F restart): no loops that make and destroy devices, a few long runs
  instead, switches by hand, Afterburner and RivaTuner closed. Everything that can be checked on the
  Radeon or WARP is checked there.

---

## Risks, and the decisions that are yours

- **Motion vectors are heuristic.** Wrong ones smear worse than none, which is why unknown stays
  unknown and why phase 2 is scored before anything is shown. Some games will be better with DLSS
  off, per game.
- **2D games**: sprites stay on their grid, but DLSS softens pixel art. Probably off for them.
- **480i and films**: not upscaled, above.
- **Jitter in framebuffer effects**: a texture drawn under one picture's offset and used under
  another's is off by a fraction of a sub-pixel. Expected to be invisible; the 26 discs will say.
- **The 4060's health**: it has not come back since the restart, and the cause is unknown.
- **The hybrid laptop**: the renderer on the 4060 presents to a screen on the Radeon. Pacing under
  Frame Generation is measured, not assumed.

**Decisions:**

1. **Scope**: Super Resolution and DLAA only, or Frame Generation too. Recommended: both, in that
   order - Frame Generation is the one thing DLSS gives here that nothing else can (smooth 60 from a
   30 fps game), and DLAA alone barely beats drawing at 8x.
2. **The licence.** NVIDIA's RTX SDK licence:
   - forbids using the SDK "in any manner that would cause it to become subject to an open source
     software license" (4(e))
   - requires NVIDIA's marks in the About box and the notice "This software contains source code
     provided by NVIDIA Corporation"
   - allows deployment only for systems with NVIDIA GPUs
   - forbids modifying the binaries

   This project is MIT, which is not copyleft, so shipping NVIDIA's DLLs beside it under NVIDIA's own
   terms is the usual reading. Whether that holds, and whether the binaries live in the repository
   or are fetched, is yours.
3. **FSR on the same inputs.** AMD's FSR 3.1 (MIT) takes the same colour, depth, motion and jitter
   and runs on the Radeon. Either a test stand-in for DLSS while the 4060 is away, or a shipped
   option for every card. Recommended: at least the first.
4. **Pacing under Frame Generation**: Reflex's limiter at the console's rate, or ours kept and
   Reflex's sleep left idle. Recommended: Reflex's, as NVIDIA intends; measured in phase 5.
5. **RTX 50 modes offered untested, or hidden** until someone can try them.
6. **DLSS 5**: whether a mode that invents detail belongs in PSXEmu at all. It is gated either way;
   this is about whether to build it once the gate opens.

---

## Sources

- [Streamline](https://github.com/NVIDIA-RTX/Streamline) (MIT) - `ProgrammingGuide.md`,
  `ProgrammingGuideDLSS.md`, `ProgrammingGuideDLSS_G.md`, `ProgrammingGuideReflex.md`, and the
  headers `sl_dlss.h`, `sl_consts.h` and the buffer and feature ids; releases to 2.14.1
- [NVIDIA DLSS licence](https://github.com/NVIDIA/DLSS/blob/main/LICENSE.txt)
- [NVIDIA: DLSS 4.5](https://developer.nvidia.com/blog/nvidia-dlss-4-5-delivers-super-resolution-upgrades-and-new-dynamic-multi-frame-generation),
  [Dynamic Multi Frame Generation](https://www.nvidia.com/en-us/geforce/news/nvidia-app-dlss-4-5-dynamic-multi-frame-generation-available-now/)
- [NVIDIA: DLSS 5](https://www.nvidia.com/en-us/geforce/news/dlss5-breakthrough-in-visual-fidelity-for-games/),
  [3D-guided neural rendering](https://www.nvidia.com/en-us/geforce/news/dlss-5-3d-guided-neural-rendering/),
  [RTX 40 timing](https://tech-insider.org/nvidia-dlss-5-siggraph-2026/)

Read for what to integrate and how; nothing is copied. The motion vector design is this project's
own, built on PGXP's shadows.
