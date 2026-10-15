# Rewind and run-ahead

> **Status: phase 0 done, 2026-10-15; nothing a player can use is built.** `boot_runner
> --bench-state <n>` makes every measurement here (see "How it was measured"). Phase 0 found
> that the suspected CD problem was the first benchmark's own fault, and that throwing compiled
> code away on a load changes what the machine computes. Phase 1 has to keep it for that
> reason, not only for speed. Every phase ends with a harness check that a person does not have
> to watch.

Two features built on one thing: a save state that stays in memory, fast enough to take and
restore every frame.

- **Rewind** keeps a state every frame or few, and while a key is held, steps back through
  them with the picture showing where it is. Releasing the key resumes from that point.
- **Run-ahead** removes the frames of lag a game builds in between reading the pad and showing
  the result. Each frame the machine runs the frame the input belongs to, keeps a state, runs
  N frames further and shows the last of them, then goes back to the kept state. The picture
  is N frames ahead of the machine's real timeline. The game sees exactly what it always
  would.

RetroArch (run-ahead, with one instance or a second hidden one), DuckStation and BizHawk
(rewind) all offer these. Gaps.md lists them under "Features other emulators have, absent
here".

## The conclusion first

**A state already fits in a frame.** The payload is 4.35 MB, and serialising it into a buffer
kept from the last save takes **0.13-0.29 ms**. That is close to the 0.13 ms `memcpy` floor
and under a tenth of a frame. What makes `SaveState` cost 1.7-3.3 ms is building a fresh
vector every time, not the serialising.

**Loading is where the time goes, and almost none of it is the machine.** A load as
`LoadState` does it, minus the file, costs 0.5-2.3 ms. Its three expensive parts are things a
load within one session does not need:

1. **The disc image is reopened** from its path every load: 0.57-0.70 ms on the share, and a
   full read-through for an `.ecm`, whose format has no index.
2. **Every compiled block is thrown away** (`NoteBulkWrite(0, kRamSize)`), so the next frame
   recompiles everything it runs. That adds **0.8-1.7 ms** to the frame after a load. A state
   from one frame earlier has, almost always, exactly the same code bytes. **It also changes
   what the machine computes**: a save and a load after every frame, recompiled, ends 120
   frames later somewhere else. Keep the compiled code and it ends exactly where no load
   would (phase 0, below). The interpreter is exact either way.
3. **The payload is copied** into `StateIO` before it is read (0.13-0.38 ms).

With those three fixed, a save and a load cost about **0.6-1.2 ms together**, recompiled. On
that budget, run-ahead of one or two frames fits easily in every scene measured. Rewind's
capture costs only the save plus storage, and the storage is the real design question: what
changes between consecutive states compresses to **19-116 KB** a frame.

With the disc kept open and the compiled code kept, run-ahead 1 costs **5.6-6.2 ms per frame
shown** in Ridge Racer and Wild Arms, against 2.2-2.4 ms with none. That is two frames, a
save and a load, with nothing left over (phase 0).

**The hardware rasteriser is not cheap yet, and gets a phase of its own.** A state is native
VRAM, so a save reads it back from the card and a load uploads it and marks every upscaled
pixel stale. At 4x on the Radeon 780M a load costs 4.9 ms, and the upscaled detail is lost each
time. Run-ahead needs snapshots of the card's own VRAM, kept on the card.

**The disc is not a problem.** The first measurement made it look like one: Ridge Racer and
Wild Arms, which read the disc as they run, slowed by 2-4x under run-ahead. Phase 0 found that
loop was at fault, not the disc. Measured fairly, the disc reads cost 0.04-0.16 ms per frame
shown, from the network share as from a local copy.

**Run-ahead needs the recompiler.** Interpreted, Ridge Racer's frame alone was 24 ms in this
batch, so even run-ahead 1 (two frames per frame shown) cannot keep up. The setting should
say so rather than quietly drop frames.

## How fast an in-memory state is today

Measured 2026-10-15 on this laptop (the RTX 4060 was not used; the hardware rows are on the
Radeon 780M). The
software rasteriser unless marked, 120 iterations each, each scene booted cold from the share
by `boot_runner` with SCPH1001: Ridge Racer and Wild Arms at frame 3000, FF7 cd1 at frame 3000,
the BIOS shell at frame 1500. Wall-clock times on this laptop swing by up to 45% between
batches (Test-Suite.md); the *ratios* within a scene are what to trust. Where two variants
are compared, they were alternated within one loop.

### Saving and loading, recompiled

| | Ridge Racer | Wild Arms | FF7 | BIOS shell |
|---|---|---|---|---|
| A frame | 3.44 ms | 2.27 ms | 1.84 ms | 2.22 ms |
| Save as `SaveState` builds it (a fresh vector) | 2.12 ms | 3.30 ms | 2.17 ms | 1.71 ms |
| **Save into a reused buffer** | **0.19 ms** | **0.29 ms** | **0.15 ms** | **0.13 ms** |
| `memcpy` of the payload, the floor | 0.13-0.58 ms | 0.54 ms | 0.13 ms | 0.13 ms |
| Load as `LoadState` does it, minus the file | 1.58 ms | 2.29 ms | 1.51 ms | 0.54 ms |
| ...of which reopening the disc image | 0.65 ms | 0.67 ms | 0.57 ms | (no disc) |
| ...of which copying the payload in | 0.15 ms | 0.38 ms | 0.13 ms | 0.13 ms |
| **Load without the reopen** (alternated with one with it) | **0.52 ms** | **0.88 ms** | **0.43 ms** | **0.47 ms** |
| A load's next frame, compiled code thrown away | 2.07 ms | 3.17 ms | 4.53 ms | 4.85 ms |
| ...the same frame, compiled code kept | 1.28 ms | 1.72 ms | 2.85 ms | 3.80 ms |
| **Recompiling after a load** | **+0.8 ms** | **+1.4 ms** | **+1.7 ms** | **+1.1 ms** |

The payload is 4,347,538-4,347,643 bytes: 2 MB of RAM, 1 MB of VRAM, 512 KB of sound RAM, the
64 KB expansion buffer, the instruction cache and the registers. Saving it into a reused buffer
splits about 0.09 ms for `IOInterface` (RAM and the devices), 0.04 ms for the GPU, 0.03 ms
for the SPU, and nothing measurable for the CPU and GTE.

**The interpreter is the control for the recompiling row.** Interpreted, Ridge Racer's
frame after a load cost 17.76 ms with the flush and 17.92 ms without: the same, because there
is nothing compiled to lose. So the recompiled difference is recompiling, not the load.

### Run-ahead of one frame (phase 0)

Per frame shown: one frame, a save, a second frame, a load. Every variant runs the same 30
frames shown from one starting state, the variants taking turns, four times over: 120 frames
shown each. Ridge Racer and Wild Arms at frame 3000, recompiled, from a local copy of the
disc and from the share.

| | Ridge Racer, local | Ridge Racer, share | Wild Arms, local | Wild Arms, share |
|---|---|---|---|---|
| No run-ahead | 2.43 ms | 3.74 ms | 2.29 ms | 2.18 ms |
| Run-ahead 1, a load as `LoadState` does | 9.43 ms | 16.35 ms | 9.07 ms | 9.11 ms |
| ...the disc kept open | 8.90 ms | 15.02 ms | 8.32 ms | 7.63 ms |
| ...and the compiled code kept | **6.17 ms** | **9.52 ms** | **5.96 ms** | **5.60 ms** |
| Disc reads per frame shown, and their time | 2.00, 0.05 ms | 2.00, 0.09-0.16 ms | 2.00, 0.05-0.07 ms | 2.00, 0.04-0.11 ms |
| Steps per frame shown, code dropped / kept | 9,541 / 8,856 | the same | 21,017 / 20,714 | the same |

(The Ridge Racer share column ran in a slower moment of the laptop. Its no-run-ahead frame is
3.74 ms against 2.43 ms locally, and the ratios match the local column.)

With the disc kept open and the code kept, run-ahead 1 costs what its parts add up to: two
frames, a 0.3-0.4 ms save and a 0.8-1.1 ms load. Dropping the compiled code costs 2.4-3.4 ms
more per frame shown, mostly in the first frame after each load. The extra steps are the
recompiler compiling, and running instructions one at a time until it has. The reopen costs
0.5-0.8 ms of the load.

**The disc reads are not the problem.** There are two per frame shown, because `Disc`
caches one 32-sector block (`Source::ahead`) and a load steps back out of it. But each is
served from the operating system's cache in 0.02-0.08 ms, from the share as from local disk.

**Why the first measurement blamed the disc.** The first loop ran its four variants one after
another on a single timeline that kept moving. Each variant's frames were a different stretch
of the game from the last one's. Each "plain" pair also ran straight after another variant's
load had thrown the compiled code away, so it paid for the recompiling. The run-ahead
variants also changed the timeline itself, as below. The per-variant breakdown made this
visible: the variants ran 4,622 to 5,037 steps a frame for what should have been the same
frames.

One thing in that first loop was the share's own: some of its reads took 1.8-4.3 ms each,
where the fair loop's took 0.02-0.08 ms. The share's speed varies from one moment to the next,
and two block reads per frame shown are an exposure to it. Phase 1 keeps the last few blocks, so a
step back of a frame or three reads nothing.

### Does a load change what the machine computes? (phase 0)

From one saved state, 120 frames run on five ways, then the whole state is hashed. A load that
is invisible ends with the same hash as no load at all.

| 120 frames after... | Ridge Racer, recompiled | Ridge Racer, interpreted | BIOS shell, recompiled | BIOS shell, interpreted |
|---|---|---|---|---|
| no load | `26df14101bea0edb` | `c79bcb891a6d9caf` | `60f5f355a6b08f18` | `742db76e20526553` |
| a load, the disc reopened | the same | the same | the same | the same |
| a load, the disc kept open | the same | the same | the same | the same |
| a save and load after every frame, as `LoadState` | **`efeffc07052bb49b`** | the same | **`ea804d37339be28c`** | the same |
| ...that, the compiled code kept | the same | the same | the same | the same |

Wild Arms, recompiled, behaves the same way: one load is invisible, and a load after every
frame is not.

So the state is complete, and keeping the disc open is exact. What changes the course is
**throwing the compiled code away** while the recompiler is on. The likely mechanism, not yet
traced instruction by instruction: an interrupt can be taken only at a block boundary
(Recompiler-Plan.md). Blocks compiled afresh from wherever the machine stands after a load can
have different boundaries from the ones they replaced, so an interrupt lands a few
instructions earlier or later. One load did not show it in these 120 frames. A load every
frame drifts.

For run-ahead that is the difference between a game that plays as it would without it and
one that does not. It also means `boot_runner --run-ahead` could never pass phase 2's test.
Phase 1's "only code that changed is thrown away" is a correctness requirement, not an
optimisation.

### The hardware rasteriser

Ridge Racer, Direct3D 12 on the Radeon 780M, 100 iterations.

| | 1x | 4x, true colour |
|---|---|---|
| A frame | 3.30 ms | 4.58 ms |
| Save after a frame was drawn (VRAM read back first) | 1.15 ms | 2.04 ms |
| Load as today | 2.47 ms | 4.89 ms |
| Load, the disc not reopened | 2.40 ms | 3.18 ms |

A save calls `PrepareRead` over all of VRAM, which waits for the card and reads it back; a
load calls `Reloaded`, which uploads native VRAM and marks every upscaled pixel stale. At 4x a
round trip costs about 5 ms more than in software, and everything drawn upscaled before the
load comes back at native resolution until it is drawn again. For a state loaded once, that
is fine. Run-ahead loads every frame, so it would cost 5 ms a frame and the upscaled picture
would never last.

### What changes from one frame's state to the next

What rewind has to store. Each state XORed against the one a frame before, then compressed with
zlib at level 1 (the vendored zlib; there is no LZ4 or zstd in the tree).

| | Ridge Racer | Wild Arms | FF7 | BIOS shell |
|---|---|---|---|---|
| Bytes that differ | 67,788 | 109,669 | 8,810 | 254 |
| 4 KB pages that differ, of 1,062 | 84.9 | 88.1 | 35.6 | 23.4 |
| The XOR, zlib-1 | 62.6 KB | 115.7 KB | 28.7 KB | 19.5 KB |
| ...time, over the whole 4.3 MB XOR | 12.3 ms | 14.1 ms | 10.7 ms | 10.3 ms |
| A whole state, zlib-1 | 1.71 MB | 1.12 MB | 1.52 MB | 0.46 MB |
| ...time | 90 ms | 57 ms | 80 ms | 30 ms |

At 60 states a second, 256 MB holds about **68 s of Ridge Racer, 37 s of Wild Arms, 150 s of
FF7's field and 220 s of the shell**. Compressing the whole XOR, 10-14 ms a frame, is too slow
for the machine's thread, and wasteful: only 2-8% of the pages changed.

Phase 0's `--bench-state` compresses only the changed pages instead, as they are, not XORed.
In Ridge Racer that is 105.6 KB in 4.9 ms a frame, and in Wild Arms 128.9 KB in 3.7 ms: three
times faster than the whole XOR, but up to 1.7 times bigger. XORing just those pages before
compressing them should keep the speed and win back the size. That, on a thread of its own, is
the design below, and phase 5's first measurement.

## The design

### A state in memory

New beside `SaveState`/`LoadState` in `psx/system.h`:

```cpp
// A state kept in memory for rewind and run-ahead. No header: it never leaves this process,
// this BIOS or this disc. Its buffer is kept and reused, so taking one allocates nothing.
struct MemoryState {
  std::vector<uint8_t> payload;
  std::vector<uint8_t> cards;      // the inserted cards' images - see Memory cards, below
  uint64_t frame = 0;              // gpu().frame_count() when taken
};
void SaveMemoryState(MemoryState* state);
void LoadMemoryState(const MemoryState& state);
```

They run the same `Serialise` chain as the file functions, so no new field can be forgotten.
What differs:

- **`StateIO` writes into a buffer it is handed and keeps its capacity**, and reads from a
  pointer and size it does not copy. `SaveState` and `LoadState` use the same two paths, which
  takes 2 ms off every F-key save as a side effect.
- **The disc is not reopened if it is the disc already open.** `Cdrom::Serialise` reopens only
  when the saved path differs from `disc_.path()`, on a memory load. A file load keeps
  reopening, since the image might have changed on disk since. `Iso9660` is likewise left
  alone when the disc is the same.
- **Only code that actually changed is thrown away.** RAM is restored through a new
  `StateIO::Ram` that, on load, compares each 4 KB page with the page it is about to replace.
  It only does that for pages the block cache has compiled code from (`IsCodePage`, a bitmap
  lookup), and calls `NoteBulkWrite` for those that differ before copying. On FF7 that is
  1.7 ms a load back. More importantly, it is what keeps a recompiled machine on the course it
  would take with no load, which a load every frame needs (phase 0's determinism table). The
  file load can use it too.
- **A few read-ahead blocks, not one.** `Disc` keeps the last four blocks it read, so a load
  that steps back a frame or three reads nothing. That is about 300 KB.
- **The debugger is not reset.** Rewind and run-ahead are off while the debugger is armed, so
  a memory load never lands under a halt.
- **What a load forgets on the GPU is kept for run-ahead.** `Gpu::Serialise` drops the sprite
  motion tracker and PGXP's FIFO shadows, because nothing drawn before a loaded state is
  related to anything after it. Under run-ahead it is related: the real frame redraws what the
  last shown frame showed, one frame later. A run-ahead load keeps the motion tracker, so DLSS
  and FSR keep their 2D motion. PGXP's RAM and register shadows are validated by value and
  already survive a load.

### Memory cards

A card's contents are deliberately not in a save state (Save-States-Plan.md). Run-ahead
cannot keep that rule. A save written by the speculative frames ran on input the player has
not given yet. If the next real frame's input differs, the game may write something else, or
nothing, and the speculative write would be left on the card.

- **Run-ahead states carry the cards**: every inserted card's 128 KB image and its `flag_`,
  about 0.03 ms for two cards. The card's protocol state is already in `Sio::Serialise`. The
  card goes to disk a second after the last write (`MC::OnFrame`, called once per frame shown),
  so it always writes the real timeline's card, never a speculative one.
- **Rewind leaves the cards alone**, as loading a state does. Rewinding past a save does not
  undo it, and a rewind never loses a save the player made. A rewind that lands partway through
  a save is safe: the card already holds the finished save, and the game writes the same
  sectors again.

### Run-ahead

Each frame shown, with N frames of run-ahead (1-3) and a kept state S:

1. Take the input once (`apply_input`), as now.
2. If S exists, load it. The machine is back at the start of the real frame.
3. Run the **real frame**. Its sound is pumped to the device, its cheats applied. Its picture
   is not published.
4. Save S. This is where the machine actually is.
5. Run N **speculative frames** with the same input. Their sound is drained and dropped; the
   last one's picture is published.

Cost: N + 1 frames, a save and a load. On the improved load that is 2 frames plus about 1 ms
for run-ahead 1 in every clean scene above, 4-9 ms a frame shown against a 16.7 ms budget.

What has to split in `host::Machine`:

- **Hooks per emulated frame and per frame shown.** Cheats run after every emulated frame.
  The BIOS console's text, the card-write watcher and `frame_done` run once per frame shown,
  after the real frame. Text a speculative frame printed would otherwise print twice: once
  speculatively, and once again for real.
- **Sound**: `Spu::ReadSamples` after a speculative frame goes to a scratch buffer that is
  thrown away. The SPU's output ring is not in a state, so nothing else is needed.
- **Pacing and the readout**: emulate time includes every frame run, and the instruction count
  is the real timeline's. The speed the machine is "managing" is frames shown per second.
- **Off while** the debugger is armed, a movie plays (a movie's input is by frame number, and
  states stop it anyway), and the interpreter is on, unless the user insists.

**A cheaper second step**, once the plain version is proven. If this frame's input equals the
last frame's, the speculative frame computed last time *is* the real frame now. Keep it
instead of loading and re-running: run one more speculative frame and save. A frame with no
change in input then costs one frame and a save. The sound a speculative frame made must be
held back rather than dropped, then committed or dropped once the next input shows which.
The cards need the same care.

**Not planned: a second instance.** RetroArch can run the speculative frames on a second copy
of the machine, avoiding the load. Two `System`s means two GPUs and two rasterisers, on a
machine thread that is already the ceiling (Gaps.md, the speed entry). The single instance
fits the budget above, so it is the one built.

### The hardware rasteriser

A new pair on `RasterBackend` (`render/raster.h`): `Snapshot(slot)` copies the backend's own
VRAM texture, at its scale, to a texture kept on the card, and `Restore(slot)` copies it back.
They are a copy on the card, with no read back, no upload and nothing marked stale. A
`MemoryState` taken for run-ahead names its snapshot slot. Loading it restores the slot
instead of calling `Reloaded`, provided nothing has replaced VRAM since.

The size of a snapshot is VRAM at the scale: 1024x512 at 4 bytes a pixel is 2 MB at 1x and
32 MB at 4x. Add the plane beside it when DLSS or FSR keeps one. Run-ahead needs N + 1 slots.
Rewind does not use snapshots; it keeps native VRAM, and a rewound picture is drawn from that.
Rewinding shows native resolution for the moment, until the game redraws, which most 3D
games do every frame.

What phase 4 has to settle first: the native copy in `Gpu::vram_` is what a state holds. A
run-ahead save would still read it back, about 1 ms at 1x and 2 ms at 4x. Either the
backend's dirty tracking (`dirty_`/`stale_`) lets a save skip the readback when the snapshot
covers it, or that millisecond stays in the budget.

### Rewind

**Capture**: a memory state every K frames (Settings: every frame to every 10th), taken at
the end of a frame on the machine's thread. That costs the save, 0.13-0.29 ms. Everything
after is on a thread of its own.

**Storage, as reverse deltas.** The newest state is kept whole. When a new one arrives, the
previous newest is replaced by its difference *from the new one*. Only the pages that changed
are kept, each compressed with zlib-1. Stepping back one state is applying one delta to the
newest, never a walk forward from a keyframe. Dropping the oldest state when the budget fills
is freeing the oldest delta, since nothing depends on it. The budget is in megabytes (default
256), and the seconds it holds are shown beside it for the game running.

**Playing back**: while the Rewind key is held, the machine loads one state back per frame
shown and presents it. Each state is 1-10 frames back, so rewinding runs at 1-10x. `Gpu`
already resolves the picture from VRAM on a load, so no frame needs running to show it. The
sound is muted while rewinding. On release the machine runs on from the state shown, and the
states newer than it are dropped.

**The movie**: a rewind stops a movie, as a state load does. Rerecording, which needs
states to carry the movie's position, is the TAS work in Gaps.md, and builds on this.

### The front end

- **Settings > Emulation > Rewind**: on or off, a state every 1-10 frames, a memory budget,
  and the seconds that buys for the game running.
- **Settings > Emulation > Run-Ahead**: 0-3 frames. Per game, through
  `gamesettings\<serial>.ini` (bug 116), since how many frames of lag a game builds in is the
  game's own. Unavailable with the interpreter, saying why.
- **A Rewind binding**, held, Backspace by default. F1-F12 and Tab are taken. It goes in
  Settings > Input > Controllers beside fast-forward.
- **The overlay**: a "Rewind" badge at the top middle, where fast-forward's is. The
  performance panel's emulate time includes every frame run, which shows run-ahead's cost.

## Phases

| Phase | What | Done when |
|---|---|---|
| 0 | **Done 2026-10-15.** `boot_runner --bench-state <n>`, on `System::SerialisePayload`/`FinishLoad` and `StateIO::ReuseBuffer`; `Cdrom::set_reopen_on_load`; `Disc::read_stats`. The CD across loads, from the share and a local copy | The numbers repeat; Ridge Racer and Wild Arms cost what their parts add up to, and the reason they seemed not to is known (the first loop). Found on the way: a dropped-code load every frame changes the machine's course |
| 1 | `SaveMemoryState`/`LoadMemoryState`: reused buffers, no copy in, no reopen, only changed code thrown away, a few read-ahead blocks, cards in run-ahead states; `SaveState`/`LoadState` on the same paths | `boot_runner --memory-state-every 1`, saving and loading after every frame, gives the plain run's checksums and instruction counts on the twelve discs, recompiled as well as interpreted; a save, load, save gives identical bytes; a load costs under 0.6 ms recompiled |
| 2 | Run-ahead in `boot_runner --run-ahead <n>` | The twelve discs with `--press` input give the same checksums at every checkpoint as without it, the machine's timeline being untouched; with a press at frame F, the first changed picture comes N frames earlier |
| 3 | Run-ahead in the front end: the hook split, the sound, the setting, per game | `host_test` runs a machine with run-ahead to `boot_runner`'s instruction count; then an hour of play |
| 4 | Hardware rasteriser snapshots on the card | A run-ahead load at 4x costs what it does at 1x, and `hw_raster_test`'s scenes are identical with run-ahead on |
| 5 | Rewind's store: reverse deltas on a thread, the budget | Record 600 frames, step back to frame 300, compare with a state saved at frame 300: byte-identical. Changed-page compression measured |
| 6 | Rewind in the front end: the binding, playback, the badge | Rewind to a point, release, and the run from there matches a run loaded from a state saved at that point |
| later | The unchanged-input shortcut for run-ahead | Phase 2's test still passes with it, and a held button costs one frame |

The phase 1 test is the one that guards everything after it. A load of a state just saved
must change nothing the machine computes. If any field is missing from a `Serialise`, it fails
at once, on every disc.

## Traps

- **Do not reuse the file header.** A memory state has no magic, version or BIOS hash. It
  cannot outlive the process, and checking them every frame is waste.
- **A memory load on a different disc must still reopen it.** A disc swap inside rewind's
  window is a state whose path differs. Compare the paths; never skip the reopen blindly.
- **Never call the per-frame-shown hooks after a speculative frame.** The BIOS console, the card
  watcher, the movie and the performance graphs all assume one call per real frame.
- **The SPU's output ring is not in a state.** After a rewind, drain it before resuming, or the
  first frame plays sound from the timeline that was abandoned.
- **`kMaxInstructionsPerFrame` is per emulated frame.** A frame shown under run-ahead runs N + 1
  of them, and the give-up check must not add them together.
- **Measure speed back to back.** These numbers moved by up to 2x between batches on the same
  binary (Test-Suite.md, and the 0.13-0.58 ms `memcpy` above). Any claim that a phase made
  something faster alternates old and new in one loop.

## Decisions that are yours

- **Rewind and memory cards**: leave the cards alone (above, the recommendation), or rewind
  them with the machine, which can undo a save the player meant to keep.
- **Defaults**: rewind off or on (it costs 0.13-0.29 ms a capture and the memory budget),
  256 MB, a state every frame; run-ahead 0.
- **The Rewind key**: Backspace, or another.
- **Run-ahead under the interpreter**: refused (recommended, as measured) or allowed with a
  warning.

## How it was measured

`boot_runner --bench-state <n>` (phase 0). Once the run is over, and after everything else
the run writes, it runs the machine on from where it stopped and prints `bench` lines:

- a frame;
- a save into a fresh vector and into a reused one;
- a load with the disc reopened and kept open;
- the frame after a load with the compiled code dropped and kept;
- run-ahead 1 four ways, each split into its frame, save, frame and load, with its steps;
- the determinism table;
- what changes from one frame's state to the next.

Disc reads are counted by `Disc::read_stats` (reads that went to the image, how many stepped
back, their time). Run one scene at a time, never in parallel:

```
boot_runner bios/SCPH1001.BIN --quiet --frames 3000 --recompiler --disc "<Ridge Racer .cue>" --bench-state 120
```

with `--d3d12 --gpu Radeon --scale 1|4 --shown-only` for the hardware rows.

The first tables (saving and loading, the hardware rasteriser, frame to frame) came from a
scratch build before phase 0. That build forced private members public in every core file
(MSVC puts access into a member function's decorated name, so one file alone does not link)
and patched `cdrom.cpp` to skip the reopen. `--bench-state` repeats them through public calls
instead.

Its first run-ahead loop is described under "Why the first measurement blamed the disc".
Don't build another like it: compare ways of doing a thing over the same frames, from one
saved state.
