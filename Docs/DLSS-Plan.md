# NVIDIA DLSS 4.5 and DLSS 5

**Status: phases 1 and 2 done (2026-09-30) - the plane beside VRAM, with depth from PGXP and
motion from the GTE and 2D matching, handed over beside each shared picture with whether it is new
or starts afresh, and shown by Video > View Depth and View Motion. On the 26 discs, motion never
does worse than none, and halves the warp error where 3D moves; the machine runs exactly as
without it. Phase 3, jitter, next. DLSS 4.5 Super Resolution, DLAA and 2x
Frame Generation are possible on this laptop's RTX 4060 once it is back (it has shown as "Unknown"
since the 0x9F restart). DLSS 5 is not possible yet: there is no public SDK for it, and it runs on
RTX 50 cards only until an RTX 40 update NVIDIA has promised for "later this fall".**

**Decided 2026-09-29:**

- **Scope:** Super Resolution and DLAA, then Frame Generation.
- **DLLs:** NVIDIA's DLLs ship beside the executable, once phase 0 confirms the licence allows it.
- **FSR:** left for later.
- **Pacing:** the machine's own frame limiter keeps pacing.
- **RTX 50 modes:** shown in the menu, untested.
- **DLSS 5:** an enhancement, like PGXP, once its gate opens.

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
- **All of it is an enhancement**, like upscaling and PGXP: the emulator has two goals, accuracy and
  enhancement, and DLSS is off whenever accuracy is what is asked for.
- **DLSS 5 is gated.** Streamline 2.14.1's public headers name a `kFeatureDLSS_NR` and two "uplift"
  colour buffers and nothing else: no options header, no plugin, no guide. Nothing DLSS 5-specific
  gets built until NVIDIA publishes those and ships RTX 40 support. What it is known to take -
  colour and motion vectors - comes from the DLSS 4.5 phases anyway.
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
  sub-pixels) in RG, with a value of its own for "not known"; depth in B; in A whether the last
  thing drawn was opaque or translucent. Written by the same draws as a second render target.
- **Per kind of job:**
  - triangles write it; a precise one's depth from `w`, anything else's depth unknown (far)
  - translucent pixels leave what is under them and say so in A - a blend state on the second
    target alone, since the colour target does its own blending in the shader
  - mask-rejected pixels are discarded, from both
  - rectangles and lines take their motion from sprite matching (below), depth far
  - fills write zero motion, far, opaque
  - VRAM-to-VRAM copies carry the plane along; a copy onto itself (phase 3's CPU path) clears it
  - 15-bit texels carry theirs too (as built: a game drawing its own picture back onto the
    screen), the primitive's own depth first
  - uploads write "not known" and far
- **`ResolveDisplay` copies the displayed area of the plane** into a second shared texture in the
  picture's slot, same serial. `SharedPicture` gains its handle (phase 1); the picture's jitter and
  two flags - *new picture* and *reset* - come with phases 2 and 3.
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
- **Pacing stays the machine's.** Its limiter keeps the console's rate, as now. Reflex is there
  because Frame Generation requires it:
  - its markers are always set
  - `slReflexSleep` is called at each frame's start, as NVIDIA requires, but with no frame limit
    (`frameLimitUs` 0), so it never sets the rate
  - its low-latency mode is off at first; turning it on beside our limiter is measured in phase 5
    before it is offered

  Any speed but 100% turns Frame Generation off.
- **Our overlay is the UI layer.** It draws into its own target, premultiplied, tagged as UI colour
  and alpha. The screen before it is the HUD-less picture. A menu or glass window over the picture
  turns generation off, with its resources kept.
- **The game's own HUD cannot be separated** and will waver between generated frames. An experiment,
  off by default: 2D drawn after the last 3D polygon of a picture, taken as the UI mask.
- **Latency**: about one picture more, 33 ms for a 30 fps game. A per-game setting.
- **Requirements the menu checks**: hardware-accelerated GPU scheduling on, Windows 10 2004 or later,
  v-sync interval 0 or 1.
- **RTX 50's 3x-6x and Dynamic** are in the menu whenever the card reports them
  (`numFramesToGenerateMax`), marked untested, since no card here can run them.

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
- **An enhancement, like PGXP**: off by default and off whenever accuracy is asked for. The machine
  never sees it. It invents detail the game does not have, and at the PlayStation's polygon counts
  it will reinterpret more than it does in a modern game, so its intensity controls and masks
  matter more here than in NBA 2K27.

**The gate:**

- NVIDIA publishes the plugin and its guide.
- RTX 40 support ships, since this machine's only NVIDIA card is a 4060 Laptop.
- A first run shows the 4060 can carry it at 60 fps.

### Settings and the menus

- **Settings > Video > NVIDIA DLSS**:
  - Off, DLAA, Quality, Balanced, Performance, Ultra Performance
  - Preset: Auto, K, L, M
  - Frame Generation: Off, 2x, and on RTX 50 3x-6x and Dynamic, marked untested
- **Greyed with the reason**: not Direct3D 12, not an NVIDIA card, the software rasteriser, GPU
  scheduling off, the DLLs missing.
- **Per game**: `dlss_mode`, `dlss_frame_generation`.
- **Emulation > Show Timings** shows DLSS's milliseconds and whether frames are being generated.
- **The About box** carries NVIDIA's attribution, as the licence requires.

---

## Phases

| # | What | Done when | Card | Rough size |
|---|---|---|---|---|
| 0 | The SDK read, the licence confirmed for shipping the DLLs, `sl_probe` (one run: each card's LUID, `slIsFeatureSupported` for DLSS and Frame Generation, GPU scheduling, driver) | the 4060 back and reporting both features | 4060, once | 1 |
| 1 (**done**) | The plane beside VRAM: second target, the rules per kind of job, carried in the shared picture, depth from `w`, View Depth, `--ppm-depth`; zero motion | the twelve-disc table's instruction and sector counts identical with it on; all harnesses green; depth reviewed on five discs | Radeon, WARP | 2 |
| 2 (**done**) | Motion vectors: picture boundaries, the GTE's two tables, the identity keys measured, sprite matching, flags, resets, `--motion`, the warp check | the warp check beats zero motion on every 3D disc of the 26; the keys chosen by numbers | Radeon, WARP | 3-5 |
| 3 | Jitter | the table identical in the machine's terms; the warp check unchanged with the jitter taken out | Radeon, WARP | 1-2 |
| 4 | Super Resolution and DLAA in the Direct3D 12 renderer through Streamline; the menus; per-game keys | pictures reviewed at the table's checkpoints; ghosting no worse than without on the 26 discs; the cost measured | 4060 | 2-3 |
| 5 | Frame Generation: the swap chain, Reflex on both threads, new-picture presents, the overlay as UI, our pacing kept; the RTX 50 modes in the menu | 30 to 60 and 60 to 120 even (PresentMon); latency measured (Reflex's own stats); the overlay clean | 4060 | 3-4 |
| 6 | DLSS 5 | the gate above | RTX 50, or RTX 40 after its update | unknown |

A usable feature is the end of phase 4. Phases 1-3 are worth having without it: View Motion and the
warp check are the first tools here that can tell whether a game's picture moves smoothly.

### Phase 1, as built

**The plane.**

- **`psx/shared_picture.h`** says what it holds: RGBA16F, VRAM's layout, one texel per sub-pixel:
  - R, G: motion in sub-pixels, `kUnknownMotion` (32768) where it is not known
  - B: `kPlaneDepthScale` (256) over PGXP's depth, 0 where there is none, which reads as
    infinitely far
  - A: 1 where the last thing drawn was opaque, 0 where it was translucent

  `SharedPicture` gains `planes` and `planes_id`, the same area of it beside the picture.
- **`RasterBackend::SetPlanes(keep, view)`** keeps the plane and shows it or not (`PlaneView`:
  picture, depth, motion). **`Gpu::SetPlanes`** remembers the request across boots and changes of
  rasteriser, and `Machine::set_plane_view` is the front end's way in. Nothing to the software
  rasteriser.
- **Made the first time it is asked for, and kept.** Its target, its read copy - 256 MB each at 8x -
  and a blend state that treats the two targets differently, which needs feature level 10.1. A card
  that cannot give them keeps nothing, and the picture is shown as ever.
- **`PsDraw` and `PsCopy` are compiled twice.** With the plane not kept, the shaders are phase 6's
  exactly.

**What each draw leaves in it.** Draws write it as a second target:

- **Depth:** 1/w interpolated with the screen's weights, where PGXP gave every vertex a depth.
- **Fills:** still, opaque, no depth.
- **Motion:** nothing else's is known yet (phase 2).
- **Translucent pixels** leave what was under them: the plane's target is blended by its own
  alpha, and that alpha is stored, saying so.
- **The mask check** discards both targets.
- **Copies** carry the plane. **So do 15-bit texels**, with the primitive's own depth first where
  it has one, because a 15-bit texture is how a game draws a picture it drew itself back onto the
  screen.
- **Uploads** forget it, with `ClearView`. So do state loads and copies onto themselves, which go
  through native VRAM.
- **The read copy** is made current tile by tile with VRAM's, since whatever draws into a tile
  draws into both.

**Handing it over and showing it.**

- **Handed over:** `ShareDisplay` copies the displayed area of the plane into a shared texture
  beside the picture, before the fence, so the presenter's one wait covers both. Only while it is
  kept, not while it is only shown. A picture that is read back carries none.
- **Video > View Depth and View Motion** show the plane in place of the picture (`PsDisplayDepth`,
  `PsDisplayMotion`), one at a time. They are greyed unless the hardware rasteriser draws above 1x,
  the only picture they can replace, and not saved.
  - Depth is brighter nearer, on a logarithmic scale from about 100 to 100,000 GTE units. It is
    dark blue where there is none, and reddened where the last draw was translucent.
  - Motion's hue is its direction and its brightness how far. Dim purple is not known.
- **`boot_runner --planes`, `--view depth|motion`.** The report counts pictures handed over with the
  plane. `--view` with `--ppm` is the plan's `--ppm-depth`.
- **`hw_raster_test`**: a scene of sixteen checks on the plane's rules, read back sub-pixel by
  sub-pixel (`D3D11Raster::ReadPlanes`), and `--planes` to keep it through every other scene.

**Found on the way.** Metal Gear Solid's in-engine intro showed no depth at all, with 88% of its
vertices precise. It draws its frame somewhere else in VRAM and puts it on the screen as 15-bit
sprites, blurring it, and a sprite wrote "not known" over everything. Carrying the plane with
15-bit texels, as copies already did, fixed it: its walls and arches have their depth, reddened
because the blur is drawn translucent over them.

**Verified:**

- **`hw_raster_test`: 61 checks** - 45 as before and sixteen for the plane - every one passing
  plain, and with the plane kept through every scene at 1x, 2x and 4x. Keeping it changes no pixel
  of VRAM.
- **The twelve-disc table on the Radeon, with and without `--planes`** (`--scale 2 --pgxp
  --shared-picture`): every line of every report the same on all twelve discs - instructions,
  checkpoints, sectors, GP0 words, interrupts. The only lines left out are the wall-clock speed
  and the rasteriser thread's count of barriers waited, which varies run to run anyway. Run again
  with the final build, the same. Between 582 and 2,879 of each disc's 3,000 pictures went over
  with the plane beside them.
- **At 4x with the console's colours** (`--no-true-color --shared-picture --planes`): 35 of the
  table's 36 checkpoints are the recorded software table's.
  - The 36th is Ridge Racer at frame 3000: `f8a515e892619e84`, where the table says
    `fc77d928fb3c4159`.
  - The software rasteriser gives the same, and so does a build of the last commit (6d89f81)
    without any of this, with the same instructions and sectors. The table's entry was already out
    of date, and is left to be explained on its own.
- **Every harness green**, with the counts Test-Suite records. `gpu_test --hw-raster` 80.
- **The front end:** a scratch copy on the Radeon, Direct3D 12 at 4x.
  - View Depth and View Motion are enabled there, each ticks and turns the other off, and a second
    choice turns it off.
  - 59.3 fps throughout, and it closes cleanly.
- **Depth, reviewed by eye:**
  - **Ridge Racer**, frame 3000:
    - the road brightest near and fading smoothly into the distance
    - buildings, hills and palms each at their own depth
    - the sky, drawn without the GTE, with none
    - the car's lights translucent
  - **Spyro 3**, frame 4500: scenery, castle and egg have depth. Spyro himself and some of the
    ground have none - only 58% of vertices were precise by then, the rest ones PGXP does not
    follow.
  - **Crash 3**, frame 4500: black in colour, mid-fade, and the plane shows the 3D title logo under
    a translucent full-screen fade - the rule for translucency, seen whole.
  - **Metal Gear Solid**, frame 9000: above.
  - **Tekken 3** was in films at the frames tried, which are shown at native size and have no
    plane.
- **Speed**, Ridge Racer 3,000 frames on the Radeon, with the app's own path (`--pgxp
  --shared-picture --shown-only`), one run at a time:

  | Scale | Plane off | Plane on |
  |---|---|---|
  | 4x | 1.69x-1.70x real time | 1.69x |
  | 6x | 1.66x | 1.44x |
  | 8x | 1.59x | 1.01x |

  Refreshing its read copy costs nothing measurable (8x without it: 1.00x). The cost is writing
  eight more bytes a sub-pixel through a blend, on an integrated card's shared memory. DLSS on this
  laptop's 2560x1600 screen would not draw above 6x anyway, and the RTX 4060 has memory of its own.
  If it matters there, batches of primitives that cannot blend could write the plane without
  reading it.

**What it shows for phase 2:** depth is only where PGXP followed every vertex of a polygon. Spyro
himself is the plainest case. Motion will ride on the same tracking, so the vertices phase 2 teaches
PGXP to follow - halfword stores, most likely - are depth gained as well.

### Phase 2, as built

**Where things were.**

- **`psx/vertex_motion.h`**: two tables, this picture's and the last's. Things are remembered under
  a key with where they were. The last picture's nearest under the same key wins, within a reach -
  128 screen pixels by default.
- **The GTE**, with PGXP on, looks each vertex it projects up in the last picture:
  - The position is taken from OFX and OFY, which some games move with the buffer they draw into.
  - First by the vertex's **model coordinates**, V0-V2's x, y and z.
  - Then by **where its x and y were loaded from**, when known. PGXP's register shadows now keep
    the word a register was loaded from, by LW and register moves (and in the recompiler's
    bridge), and MTC2 and LWC2 hand it to the GTE.
  - Then, if the last two pictures projected lists of the same length, by its **place in the
    list**, within 32 pixels. That is for a mesh the CPU works out itself.
  - `--motion-key` puts the address or the place first instead.
- **`PreciseVertex`** carries the motion found. So the registers, RAM, DMA and the GTE's value cache
  carry it, as they carry the unrounded position, and the GPU copies it into each vertex.
- **The GPU** matches 2D primitives with the last picture's:
  - rectangles by texture, CLUT, texel, size and flips, or colour and size untextured
  - polygons with no precise vertex by texture and texture coordinates, or colour and shape
    untextured, nearest by their middle
  - each moved whole
- **New pictures and fresh starts** (`Gpu::NextPicture`, at vblank):
  - A picture is new when the display moves to another buffer, or at every vblank for a game that
    has not moved it for six.
  - It starts afresh at a cut, when under a quarter of 64 or more vertices were found. That resets
    this picture and the next, since the work is shown now by a single-buffered game and a flip
    later by a double-buffered one.
  - It also starts afresh on a change of display size or depth, on a load, reset or new
    rasteriser, and at every 480-line interlaced picture: half new lines and half the last field's,
    which no motion describes.
  - `SharedPicture` says both, `new_picture` and `reset`, for phase 4. The rasteriser is told
    (`RasterBackend::NewPicture`).
- **The rasteriser** takes each corner's motion in four more words a vertex, in 64ths of a pixel,
  and writes it into the plane in sub-pixels:
  - interpolated in perspective where there is depth, as a texture is
  - unknown if any corner's is
  - a rectangle's whole
  - a 15-bit texel's own motion carried when the primitive has none
- **Changes nothing the machine does**, and none of it is saved: a load starts afresh.

**The warp check** (`boot_runner --motion`) scores motion with no DLSS and no NVIDIA card:

- At each new picture the rasteriser reads back its colour and plane, moves the last new picture by
  the motion (nearest pixel) and compares, against the last new picture left still.
- A ratio below 1 is motion helping. Pictures that start afresh are not compared.
- Also: `--motion-log`, a line per new picture; `--motion-key`; `--motion-reach`.
- It first blended between pixels. That scored the right motion of Ridge Racer's waving flag, all
  under a pixel, as worse than none (1.071): the blend blurs sharp edges more than the motion
  saves. Nearest pixel scores it 1.026, which is what motion that small can show.

**Choosing the keys, by the numbers:**

- **Model coordinates against the place in the list**, Ridge Racer's first 3,000 frames: warp ratio
  0.546 against 0.993. The place finds as many vertices but the wrong ones wherever the list
  changes.
- **Reach 128 against 64**, on Ridge Racer's fast attract-mode camera swing, from a saved state:
  90% of vertices found against 83%, 86% of pixels with motion against 72%, fresh starts 9
  against 20, ratio 0.496 against 0.518.
- **The address against the coordinates**, the same swing: 0.503 against 0.496. Kept second in
  line, where it costs nothing. It does not find Crash 3's animated models (65.5% of vertices
  either way) - their coordinates are worked out in registers, not loaded.
- **The steady list**, for Ridge Racer's waving title flag: its coordinates change every frame and
  come from nowhere in RAM. 5.1% of its vertices found without it, 99.4% with, and a trace shows
  each match exact.
- **A polygon's shape in the 2D key**, tried to keep out 3D that Air Combat and Ace Combat 3 draw
  from their own arithmetic, and taken out again:
  - it changed nothing there - their trouble was interlacing, below
  - it lost Spyro 3 the matches for Spyro himself, drawn from words PGXP cannot follow: 0.889 to
    0.958

**Verified:**

- **The 26 discs, 6,000 frames each at 2x on the Radeon** (`--pgxp --shared-picture --motion`):

  | Disc | Warp ratio | Pixels with motion |
  |---|---|---|
  | Wild Arms 2 | 0.290 | 99% |
  | Valkyrie Profile | 0.299 | 100% |
  | Ridge Racer | 0.467 | 84% |
  | Vandal Hearts | 0.533 | 100% |
  | Crash 3 | 0.779 | 91% |
  | Captain Tsubasa J | 0.841 | 100% |
  | Spyro 3 | 0.852 | 83% |
  | Metal Gear Solid | 0.890 | 73% |
  | Einhander | 0.986 | 100% |

  - **1.000**, pictures standing still - menus, title screens, 2D not moving: Legend of Mana,
    Bomberman, Area 51, Final Fantasy VII, Ace Combat 3, Silent Hill, Vagrant Story, Castlevania
    SOTN, Chrono Cross, Wipeout and Xenogears.
  - **No picture to compare**, all films or 480i in their first 6,000 frames: Air Combat, Wild Arms,
    Final Fantasy VIII, Gran Turismo 2, Tekken 3 and Driver 2.
  - **None worse than no motion.** The first run had Air Combat at 1.047 and Ace Combat 3 at 1.012:
    their compared pictures were 480i (the BIOS intro, 640x480 title screens), which now start
    afresh.
  - Xenogears mounts by its .bin: its cue names one that has been renamed.
- **The machine, unchanged.** The twelve-disc table on the Radeon, every line of every report the
  same, bar wall-clock speed, the barrier count and the new `motion` line:
  - this build without the plane against phase 1's
  - with the plane and motion against phase 1's
  - under the recompiler, with motion against without

  The software rasteriser's table is the recorded one, bar the Ridge Racer checkpoint already known
  to be stale.
- **Every harness green**: `gte_test` 114 (+8, a `motion` group), `hw_raster_test` 67 (+6, motion
  in the plane), and 67 again at 4x with the plane kept.
- **Cost**, Ridge Racer 3,000 frames with the app's path, one run at a time:
  - 4x: 1.64-1.65x real time without the plane, 1.59x with it and motion
  - 2x: 1.58-1.65x without, 1.59x with

  About 3-4%: the lookups on the machine's thread, and four more words a vertex.

**What is left for later:**

- **Animated models the CPU works out in registers** - Crash 3's characters. A vertex's place
  within its own model, rather than in the whole picture's list, is the next key to try.
- **Pixels with no motion at all** - Ridge Racer's nearest stretch of road, whose vertices are made
  afresh each frame. Filling them from neighbours of the same depth is DLSS's own job, and phase 4
  will see how well it does it.

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

## Risks, and the decisions taken

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

**Decided 2026-09-29:**

1. **Scope: both.** Super Resolution and DLAA first, then Frame Generation. Frame Generation is the
   one thing DLSS gives here that nothing else can: smooth 60 from a 30 fps game.
2. **The licence: ship the DLLs, if the licence allows it.** NVIDIA's RTX SDK licence:
   - forbids using the SDK "in any manner that would cause it to become subject to an open source
     software license" (4(e))
   - requires NVIDIA's marks in the About box and the notice "This software contains source code
     provided by NVIDIA Corporation"
   - allows deployment only for systems with NVIDIA GPUs
   - forbids modifying the binaries

   This project is MIT, which is not copyleft, so shipping NVIDIA's DLLs beside it under NVIDIA's own
   terms should be allowed. Phase 0 reads the licence in full to confirm that before anything ships.
   Still open: whether the binaries live in the repository or are fetched.
3. **FSR: later.** AMD's FSR 3.1 (MIT) takes the same colour, depth, motion and jitter and would
   run on the Radeon. Not planned now.
4. **Pacing: ours.** The machine's limiter keeps the console's rate under Frame Generation. Reflex
   is integrated only as far as Frame Generation requires: markers, and a sleep with no limit.
5. **RTX 50 modes: shown**, marked untested.
6. **DLSS 5: an enhancement.** PSXEmu has two goals - accuracy, and enhancement such as upscaling
   and PGXP. DLSS 5 belongs to the second, like all of DLSS, and gets built once its gate opens.

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
