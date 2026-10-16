# Rewind and run-ahead

> **Status: both are in the app, 2026-10-16: run-ahead** (Settings > Emulation > Run-ahead, with
> the recompiler) **and rewind** (Settings > Emulation > Rewind, then hold Backspace). Neither
> has been played for long, watched or listened to yet. Phase 4, the hardware rasteriser's VRAM
> snapshotted on the card, is not built. `System::SaveMemoryState`/`LoadMemoryState` exist, and
> `boot_runner --run-ahead <n>` and `--rewind-test <n>` run on them as the front end does. With buttons pressed, run-ahead 2 leaves the twelve-disc table
> exactly as without it under either CPU: every checkpoint picture, every instruction count and
> each final state byte for byte. Every press shows up 2 frames earlier, or 1 where the game
> itself reacts within a frame. Run-ahead 1 costs two frames plus 0.7-0.9 ms. Getting there found
> that a save state was not the machine exactly (bugs 159 and 161), and that the recompiler's
> course depended on what it had compiled before (bug 160). Phase 0 found that the suspected CD
> problem was the first benchmark's own fault.
> `boot_runner --bench-state <n>` makes every measurement here ("How it was measured"). Every
> phase ends with a harness check that a person does not have to watch.

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

With those three fixed, a save and a load cost about **0.6-1.2 ms together**, recompiled. Phase
1 built that and measured **0.13-0.20 ms a save and 0.46-0.59 ms a load**. On that budget,
run-ahead of one or two frames fits easily in every scene measured. Rewind's
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

### A state in memory, as built (phase 1)

`--bench-state 120`, recompiled, from local copies of the discs, after phase 1. One batch, so
the two games' columns compare with each other but not with the tables above.

| | Ridge Racer | Wild Arms |
|---|---|---|
| A frame | 2.54 ms | 1.94 ms |
| Save as `SaveState` builds it, a fresh vector | 2.05 ms | 1.93 ms |
| `SaveMemoryState` | **0.20 ms** | **0.13 ms** |
| ...with the cards (none inserted here) | 0.16 ms | 0.13 ms |
| Load as `LoadState` does it, minus the file | 1.16 ms | 1.27 ms |
| `LoadMemoryState` | **0.46 ms** | **0.59 ms** |
| The frame after a load: code dropped / all kept / a memory state's load | 1.43 / 0.94 / 0.95 ms | 4.81 / 3.30 / 3.25 ms |
| No run-ahead, per frame shown | 2.63 ms | 1.72 ms |
| Run-ahead 1, loads as `LoadState` | 10.31 ms | 7.62 ms |
| **Run-ahead 1 on memory states** | **5.92 ms** | **4.38 ms** |
| ...its save and load | 0.27 + 0.40 ms | 0.30 + 0.58 ms |
| ...its disc reads per frame shown | 0.07, as with none | 0.10, as with none |
| 120 frames with a memory state every frame | the hash of no load | the hash of no load |

A memory state's load throws away no more code than keeping it all does: the frame after costs
the same. The four read-ahead blocks take the disc reads back to what no run-ahead makes. The
payload grew by about 0.9 KB, the SPU's resampler histories and the device batch's end.

Compressing the pages that changed from one frame's state to the next, as they are, comes to
106-123 KB in 4.0-4.3 ms a frame. That is phase 5's starting point.

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

### A state in memory (phase 1, built)

Beside `SaveState`/`LoadState` in `psx/system.h`:

```cpp
struct MemoryState {
  std::vector<uint8_t> payload;
  bool has_cards = false;                       // taken with the cards: run-ahead's, not rewind's
  MC::Snapshot cards[2][kCardSlotsPerPort];
};
void SaveMemoryState(MemoryState* state, bool with_cards);
std::string LoadMemoryState(const MemoryState& state);   // empty, or why not
```

It has no header: it never leaves the process, the BIOS or the disc set. Its buffers are kept,
so taking another into the same `MemoryState` allocates nothing. Both functions run the same
`Serialise` chain as the file functions (`System::SerialisePayload`), so no new field can be
forgotten. What differs:

- **`StateIO` writes into a buffer it is handed and keeps its capacity** (`ReuseBuffer`), and
  reads from a pointer and size it does not copy (`BeginLoadView`). `SaveState` and `LoadState`
  use the same two paths, through a buffer `System` keeps, which takes about 2 ms off every
  F-key save.
- **The disc is not reopened if it is the disc already open** (`Cdrom::set_reopen_on_load`,
  off only for a memory load). A file load keeps reopening, since the image might have changed
  on disk. `Iso9660` is opened again from the open disc, which reads nothing new.
- **Only code compiled from words that changed is thrown away.** RAM is restored through
  `StateIO::Ram`. On a memory load with the recompiler on, it compares each 4 KB page with the
  one it replaces, and in a page that differs, each word. Each run of words that differ goes to
  `NoteBulkWrite` before the copy, as the CPU's own stores would. The first version did it a
  page at a time, but pages holding code and data change every frame, and a word at a time is
  what the store path does.
- **Four read-ahead blocks, not one.** `Disc` keeps the last four blocks it read and replaces the
  one longest unused, so a load that steps back a frame or three reads nothing. That is about
  300 KB per image file.
- **The debugger is reset, as on any load** (`FinishLoad`). Rewind and run-ahead will be off
  while it is armed, so a memory load never lands under a halt.
- **Not yet: what a load forgets on the GPU.** `Gpu::Serialise` drops the sprite motion
  tracker and PGXP's FIFO shadows, because nothing drawn before a loaded state is related to
  anything after it. Under run-ahead it is: the real frame redraws what the last frame shown
  showed, one frame later. Phase 3 keeps the motion tracker over a run-ahead load, so DLSS and
  FSR keep their 2D motion; `boot_runner --motion` can score it. PGXP's RAM and register shadows
  are validated by value and already survive a load.

**What made a load exact** (bug 159). A save and a load every frame has to leave a run exactly
the run without it, and at first it did not:

- Saving restarted the device batch and recomputed the bus timing, in both directions.
- A load restarted the batch at 32 cycles, where the recompiler had it reaching to the next
  event.
- The SPU's resampler histories weren't saved.
- 4 KB of the state was uninitialised memory.

The batch's end and the histories now go at the end of every state, after bug 157's addition.
`IOInterface::Serialise` derives what it derives on a load only, and the buffer is zeroed.
Phase 0's determinism table was right that the compiled code mattered. It was wrong that "the
state is complete": its scene, late in a run, did not happen to show these.

### Memory cards

A card's contents are deliberately not in a save state (Save-States-Plan.md). Run-ahead
cannot keep that rule. A save written by the speculative frames ran on input the player has
not given yet. If the next real frame's input differs, the game may write something else, or
nothing, and the speculative write would be left on the card.

- **Run-ahead states carry the cards** (`MC::Snapshot`, built in phase 1): every inserted
  card's 128 KB image, its flag, and whether it is due to be written. The card's protocol state
  is already in `Sio::Serialise`. A snapshot goes back only into the card it was taken from; a
  card swapped in since is left alone. A card the restore changes is marked to be written again,
  since what it takes back may already have reached the file. `mc_test`'s `a snapshot for
  run-ahead` group holds all of this.
- **Phase 3 flushes only on the real timeline.** A card goes to disk a second after the last
  write (`MC::OnFrame`, once per frame shown). If that ran between a speculative frame and the
  load after it, the file would hold a speculative write until the restore's rewrite a second
  later. Calling it after the load, not after the frame shown, means it never does.
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

**Built in `boot_runner` (phase 2).** `--run-ahead <n>` does it at each frame boundary, after the
real frame's bookkeeping and before the buttons change for the next. So the frames ahead run on
the input the real frame had, as a front end that has only that input would. The real frame's
sound is drained first, and what the frames ahead make is thrown away. Everything the run
reports - the frame log's pictures, the instruction count, the final state - is the real
frames'. `--shown-log <file>` writes the picture shown each frame: with run-ahead, the one n
frames ahead. Refused with `--movie`, the debugger's options and `--shared-picture`.

The counters in the frame log (flips, CD sectors, GP0 words) and the recompiler's statistics
are diagnostics outside the state. They count the frames run ahead too, so they come out about
n + 1 times as large. The instruction count is the real frames' alone.

The results, twelve discs, 3,000 frames, Start and Cross pressed at frames 1000, 1500, 2000 and
2500 for ten frames each, `--run-ahead 2` against none, both CPUs:

- **Exactly the same machine.** Every checkpoint picture, every instruction count and each final
  state byte for byte, in all 24 pairs. It took bugs 160 and 161 to get there.
- **The press shows 2 frames earlier** in 22 of the 24. In the other two (Wild Arms 2 under both
  CPUs, Captain Tsubasa interpreted) it shows 1 frame earlier: the game reacts on the very next
  frame, and run-ahead cannot show a press before the frame that reads it. It takes away up to n
  frames of the game's own lag, and no more than the game has.
- **Every shown picture is the plain run's n frames on**, except for 0-2 frames a run. Those are
  where the frames ahead ran on the input from before a press that the real frames then had.

What it costs, from local copies, recompiled, alternated, 3,000 frames from a cold boot:

| | Ridge Racer | Wild Arms |
|---|---|---|
| No run-ahead | 4.40 s (1.47 ms a frame) | 3.43 s (1.14 ms a frame) |
| Run-ahead 1 | 11.31 s (3.77 ms) | 9.21 s (3.07 ms) |
| Run-ahead 2 | 15.66 s (5.22 ms) | 12.77 s (4.26 ms) |

Run-ahead 2 still runs over three times faster than real time in both. These averages include
the boot and its loading, where the frames are cheap; the scenes measured above are the ones to
budget by.

**Built in the front end (phase 3).** `host::Machine` does it each frame, after the real frame:
it publishes nothing yet, pumps the real frame's sound, and runs the hooks. Then `RunAhead` keeps
the state, cards and all, runs the frames ahead unarmed, throws their sound away, publishes the
last one's picture and loads back with `continuing` set. The cards go to disk (`MC::OnFrame`)
only after that load, so never with a frame ahead's write in them.

- **Hooks per emulated frame and per frame shown.** `after_each_frame` runs after every frame,
  real or ahead: the front end's cheats. `after_frame` runs once per frame shown, after the real
  frame: the BIOS console's text and the card-write watcher, which would otherwise see a frame
  ahead's text or writes and then the real frame's again. `frame_done` is once per frame shown.
- **Sound**: the real frame's goes to the device as always, then what the frames ahead make is
  drained and dropped. The SPU's output ring is not in a state, so nothing else is needed.
- **The readout**: emulate time includes the frames ahead, and the instruction count is the real
  frames' alone. `MachineReport::run_ahead` says how many frames ahead it is running, 0 when held off.
- **Held off** (`RunAheadFrames`) without the recompiler (with PGXP the interpreter runs, so that
  too), while the debugger is armed, and while a movie plays (the `may_run_ahead` hook, which
  the front end answers from the movie player). The machine checks each frame, so it goes back on
  by itself.
- **Motion**: a run-ahead load (`LoadMemoryState(state, continuing)`) keeps the GPU's sprite
  motion tracker, which any other load forgets. The picture published, the last frame ahead,
  then has its motion against the frame emulated just before it, the one shown a frame earlier,
  as DLSS and FSR want. Not yet scored with `boot_runner --motion`.
- **The setting**: Settings > Emulation > Run-ahead, Off or 1-3 frames (`EmuConfig::run_ahead`,
  `run_ahead` in `psxemu.ini`), a game's own when it has separate settings, not changed by the
  presets. Its hint says when the recompiler is off and run-ahead is waiting for it.

Checked: `host_test` runs a recompiled BIOS boot with run-ahead 2 on the machine's thread to the
same instructions and picture as without, the per-frame hook called 1,200 times for 400 frames
shown, and finds it held off under the interpreter. The app, Ridge Racer from a scratch copy with
its own `psxemu.ini`, kept 100% speed with run-ahead 2. Emulating took 2.5-5.3 ms a frame against
1.0-2.0 ms with it off, and handing over 2.3-2.7 ms against 0.15 ms: the save and the load, and
with the rasteriser on its own thread, likely its catching up at each (`Gpu::Serialise` waits
for it). Not measured apart. The sound buffer held at the same level either way. Not yet done:
an hour of play, or anything listened to.

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

### Rewind (phases 5 and 6, built)

**Capture.** `host::Machine::KeepForRewind` takes a memory state at the end of every
`rewind_interval`th real frame, without the cards, and hands it to the store. It runs after
run-ahead's load back, so never on a frame ahead. That costs the machine the save, 0.13-0.29 ms;
the rest is the store's worker thread's.

**The store, as reverse deltas** (`emu/rewind_buffer.h`). The newest state is kept whole. When a
new one arrives, the worker replaces the previous newest with its difference from the new one:
the 4 KB pages that changed, XORed against the new state's and compressed with zlib at level 1. A
state that changed size (the CD-ROM's queues, a port's device) is kept whole instead. Stepping
back one state is applying one delta to the newest, never a walk forward from a keyframe.
Dropping the oldest when the budget is full frees one delta, which nothing depends on. If the
machine pushes again before the worker has finished the last push, the one waiting is replaced:
the store keeps fewer states, never a broken chain.

Measured with `boot_runner --rewind-test`, a state every frame, recompiled, from local copies:

| | Ridge Racer | Wild Arms |
|---|---|---|
| 3,000 frames held in | 88.9 MB | 166.6 MB |
| A delta, mean | 29.0 KB | 55.5 KB |
| ...made on the worker in | 2.14 ms | 2.49 ms |
| A step back (decompress, XOR) | 1.00 ms | 1.19 ms |
| 600 states back | the frame kept aside, byte for byte | the same |
| ...played on 600 frames | the run's own end, byte for byte | the same |

At a state a frame, 256 MB holds about 150 s of Ridge Racer's first 3,000 frames and 80 s of
Wild Arms'. The averages include the boot, where deltas are small, so a busy scene holds less;
the settings window shows what it holds for the game running.

**Playing back.** While Backspace is held, each frame shown is a step back (`Machine::StepBack`):
the store's newest state is loaded and shown, and the one before becomes the newest. It runs at
the console's pace, so a state every K frames rewinds at K times the speed it was played. At the
oldest it stays. The SPU's output is drained and the device plays silence. On release the
machine plays on from the state shown, as from a resume: paced afresh, its sound starting clean.
The states newer than it are gone.

**What it leaves alone, and when it is off.**

- **The memory cards.** States carry no cards, so a rewind never undoes a save the player made.
- **Off** while the debugger is armed and while a movie plays (`Hooks::may_rewind`).
- **The history is forgotten** (`Machine::ForgetHistory`) at a reset, a boot, a state loaded
  from a file and a movie started.
- **Unlike run-ahead it works under the interpreter too.** It runs no extra frames; it costs only
  the save, and a step back is a load.

**Checked.** `boot_runner --rewind-test 300` and `600` on Ridge Racer (both CPUs) and Wild Arms:
the state 300 or 600 back is the one kept aside, and playing on from it reaches the run's own
end, byte for byte. `host_test` holds Backspace on the machine's thread at frame 300, lets go
after exactly 50 steps back, and finds the whole machine 100 frames shown later as a straight run
to frame 351 has it, and not as at 400. The app, from a scratch copy: after 15 s of Ridge Racer
the store held 16 s. Five seconds of Backspace stepped back from the game into the BIOS intro,
at full speed, muted. It played on at 100% with its sound back, the store now holding 11 s.

**Not done.** Nobody has watched it or listened to it yet. On the hardware rasteriser above 1x,
a stepped-back picture is drawn from native VRAM, at native resolution, until the game redraws.
A rewind across a disc change puts the old disc back, as the state says, whatever the tray holds.

**The movie**: rewind waits until a movie is over. Rerecording, which needs states to carry the
movie's position, is the TAS work in Gaps.md, and builds on this.

### The front end (as built)

- **Settings > Emulation > Rewind**: Off, or a state every frame, 2, 3, 5 or 10 frames
  (`rewind_interval`), and 128-1024 MB (`rewind_budget_mb`, 256 by default). The hint under it
  says how many seconds the store holds for the game running. The person's settings, not a
  game's.
- **Settings > Emulation > Run-ahead**: Off or 1-3 frames, per game (phase 3).
- **Backspace**, held, as Tab is held for fast forward: a key of the window, not a controller
  binding. With rewind off, a press says where to turn it on. Letting go, the window losing
  focus, or the save-state picker opening all end it.
- **The overlay**: a "Rewind" badge at the top middle, where fast-forward's is, in its stead
  while both are held.

## Phases

| Phase | What | Done when |
|---|---|---|
| 0 | **Done 2026-10-15.** `boot_runner --bench-state <n>`, on `System::SerialisePayload`/`FinishLoad` and `StateIO::ReuseBuffer`; `Cdrom::set_reopen_on_load`; `Disc::read_stats`. The CD across loads, from the share and a local copy | The numbers repeat; Ridge Racer and Wild Arms cost what their parts add up to, and the reason they seemed not to is known (the first loop). Found on the way: a dropped-code load every frame changes the machine's course |
| 1 | **Done 2026-10-15.** `SaveMemoryState`/`LoadMemoryState`: reused buffers, no copy in, no reopen, only code from changed words thrown away, four read-ahead blocks, cards in run-ahead states (`MC::Snapshot`); `SaveState`/`LoadState` on the same buffer. Bug 159 on the way: a save moved the recompiler's path, and the SPU's resampler histories and the device batch's end were not saved | `boot_runner --memory-state-every 1` on the twelve discs, recompiled and interpreted: every report line as without it, and each final state the same byte for byte. Two identical runs save identical states. A memory-state load costs 0.46-0.59 ms recompiled. `mc_test` 112, `spu_test` 199 |
| 2 | **Done 2026-10-16.** Run-ahead in `boot_runner --run-ahead <n>`, and `--shown-log` for the picture shown. Bug 160 on the way (a recompiled chain ended where its links did, so its course depended on what was compiled before) and bug 161 (the GP0 queue and drawing debt not in a state) | Twelve discs, both CPUs, four presses each, `--run-ahead 2` against none: every checkpoint picture, instruction count and final state the same. Each press shows 2 frames earlier in 22 runs of 24, 1 earlier in the two where the game reacts within one frame. Every shown picture is the plain run's 2 frames on, except 0-2 frames a run where the frames ahead ran on the input before a press |
| 3 | **Built 2026-10-16, not yet played.** Run-ahead in the front end: the hook split (`after_each_frame`, `may_run_ahead`), the sound, the cards flushed after the load, the motion tracker kept, Settings > Emulation > Run-ahead, per game | `host_test`: a recompiled boot with run-ahead 2 on the machine's thread lands where one without does, and the interpreter holds it off. The app at 100% speed with run-ahead 2 in Ridge Racer. Still to do: an hour of play, and listening |
| 4 | Hardware rasteriser snapshots on the card | A run-ahead load at 4x costs what it does at 1x, and `hw_raster_test`'s scenes are identical with run-ahead on |
| 5 | **Done 2026-10-16.** Rewind's store (`emu/rewind_buffer.h`): reverse deltas made on a worker thread, the budget | `boot_runner --rewind-test`: 300 and 600 states back are the frames kept aside, byte for byte, and play on to the run's own end. Deltas 29-56 KB in 2.1-2.5 ms on the worker |
| 6 | **Built 2026-10-16, not yet watched.** Rewind in the front end: Backspace held, the step back, the badge, Settings > Emulation > Rewind, the history forgotten at a jump | `host_test`: 50 steps back on the machine's thread, let go, and the machine is where a straight run to that frame has it. The app stepped back 5 s at full speed, muted, and played on |
| later | The unchanged-input shortcut for run-ahead | Phase 2's test still passes with it, and a held button costs one frame |

The phase 1 test is the one that guards everything after it. A load of a state just saved
must change nothing the machine computes. If any field is missing from a `Serialise`, it fails
at once, on every disc. Run it with the final states compared byte for byte, not just the
checksums: in phase 1 every picture matched while the states, and the recompiler's path,
did not.

## Not done yet (2026-10-16)

Left for later, in no particular order. All of it is on the `rewind-run-ahead` branch, which is
not merged into master.

1. **Phase 4: the hardware rasteriser's VRAM snapshotted on the card.** Above 1x, run-ahead's
   load costs about 5 ms more a frame and loses the upscaled detail every frame. A rewound
   picture is native resolution until the game redraws it. At 1x, or on the software
   rasteriser, neither applies.
2. **Played, watched and listened to.** Nothing here has had an hour of play. Nobody has looked
   at a rewind on screen, listened for a click when Backspace is let go, or judged how run-ahead
   feels.
3. **DLSS and FSR motion under run-ahead, scored.** The motion tracker is kept over run-ahead's
   load as designed, but not measured with `boot_runner --motion`.
4. **Run-ahead's cheaper second step.** When this frame's input equals the last, keep the frame
   computed ahead instead of loading and running it again. A frame with no change in input
   would then cost about one frame, not N + 1. Its sound must be held back, not dropped, until
   the next input shows whether it was real (see "Run-ahead" above).
5. **Backspace and Tab bindable**, in Settings > Input, rather than fixed keys of the window.
6. **Merging the branch** into master.

Found on the way and older than this work, each with a task waiting:

- `host_test`'s 400-frame baseline went stale on master: the picture is now
  `4a65781bfb98db5a`, so four of its checks fail (Test-Suite.md, host_test).
- Ace Combat 3's frame-3000 picture differs recompiled from interpreted, on master too
  (Gaps.md, "Never run against the reference").

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

Each was built as recommended, and can be changed:

- **Rewind and memory cards**: left alone. Rewinding them with the machine would undo a save the
  player meant to keep.
- **Defaults**: rewind off, 256 MB when on; run-ahead off.
- **The Rewind key**: Backspace, fixed as Tab is. Making both bindable is its own change.
- **Run-ahead under the interpreter**: held off, the setting saying why. Rewind is allowed there,
  since it runs no extra frames.

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
