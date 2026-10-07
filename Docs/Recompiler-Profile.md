# Where the time goes with the recompiler on, 2026-10-07

Sampling profile of `boot_runner --recompiler` (master at 9388ee3), six discs at
3,000 frames plus a BIOS boot, one run at a time. Question: DuckStation is
sometimes ten times faster - what is the rest of the machine spending its time on?

## The result in one line

**Compiled guest code is about 7% of the machine thread.** The recompiler is not
the bottleneck any more; everything around it is.

| Run | Speed | Compiled code (self) |
|---|---|---|
| Wild Arms | 4.4x real time, 253 fps | 6.2% |
| Final Fantasy VII | 5.4x | 7.0% |
| Ace Combat 3 | 4.3x | 8.2% |
| Area 51 | 4.9x | 6.6% |
| Ridge Racer | 3.4x | 4.5% |
| Legend of Mana | 4.2x | 8.3% |
| BIOS boot, 600 frames | 5.0x | 3.6% |

(Speeds are under the sampler, which costs a few percent; unsampled Wild Arms is
4.8-5.7x.) A recompiler that ran every guest instruction for free would still only
be ~14x. DuckStation's lead is in everything between the instructions.

## Where the rest goes (machine thread, % of wall, range over the six discs)

| Cost | Share | What it is |
|---|---|---|
| **Frame resolve** | 8-14% (BIOS 24%) | `Gpu::ResolveFramebuffer` converts the display area 15-bit to 32-bit, one `VramAt` + three `From5Bit` per pixel, 76,800 px a frame, on the machine thread |
| **Waiting for the raster thread** | 7-9% (BIOS 19%) | `Gpu::SyncRaster` from `ResolveFramebuffer` at vblank. The raster thread itself is ~88% idle (only 10-12% busy) - this is latency, not load |
| **Device batching** | ~45-50% inclusive (`IOInterface::RunPending`) | Every 32 cycles all devices are ticked: GPU, three root counters, CD, SIO, SIO1, SPU, DMA. A compiled chain is capped at 64 instructions and `Cpu::TickCycles` re-splits it into 32-cycle pieces, so ~30,000 `RunPending` calls a frame at ~100 ns each |
| **Compiled loads/stores calling out** | 12-20% inclusive (`RecompilerBridge::Access`) | Loads from RAM read directly now; **stores always call out**: thunk -> `Access` -> `AddressTranslation` -> `Cpu::Store` (watch, isolation, queue, decode) -> observer -> `NoteStoreRange`. Loads from I/O, scratchpad and mirrors also call out, and `Cpu::Load` ticks the machine once per stall cycle |
| **Dispatcher** | ~10% (`System::StepImpl`, `RecompilerBridge::Step`, `Recompiler::Step`, `BlockCache::Find`) | Every chain returns to `StepImpl`: interrupt/Cause checks, BIOS-vector check, pgxp/ram flags, then a hash lookup. ~50M entries + 41M interpreted steps per 3,000 frames |
| **Interpreter fallback** | 11-19% inclusive | 4% of instructions but ~15% of the time: each one is a full dispatcher round trip plus `ExecuteInstruction`'s per-cycle ticking |
| **SPU** | 5-11% | `Spu::GenerateFrame` ~3-4.5% self, 24 voices x 44.1 kHz |
| **DMA/MDEC/CD** | 3-6% / up to 6% in FMV | `Dma::Tick`; `Mdec::InverseDct` etc. in Legend of Mana / Area 51 |
| Software raster | ~12% on its own thread | not on the critical path |

## Experiment: batching alone (Wild Arms, 1,500 frames, alternated, same checksum)

Constants only: `IOInterface::kBatchCycles`, the 32 in `Cpu::TickCycles`, and
`RecompilerBridge::kBudget`.

| Build | Speed |
|---|---|
| master (32 / 32 / 64) | 5.0, 5.2, 5.7x |
| 256 / 256 / 64 | 6.3, 6.3x |
| 1024 / 1024 / 64 | 6.1-6.2x |
| 256 / 256 / 256 | 6.0-6.6x |
| 1024 / 1024 / 1024 | 6.7-6.8x |

About **+25-30%** from three constants, with Wild Arms' framebuffer checksum and
instruction counts unchanged. Wall clock here is noisy (~10% between repeats); the
direction is not. Not yet checked on the twelve-disc table - a coarser batch moves
timing, so that table is the gate. Profiling the 1024 build shows what is left:
`IOInterface::Tick` 6.7% self (the per-stall-cycle `Tick()` loop in `Cpu::Load`),
`StepImpl` 11% self, ResolveFramebuffer unchanged at 12%.

## What the interpreter fallback is made of

Histogram of `RecompilerBridge::Interpret` by opcode (Wild Arms, FF7, Ace Combat
3, first 2,000 frames). Not GTE in these scenes - plain instructions the compiler
declines, plus whatever follows them:

- `add` / `addi` (they trap on overflow): 3.8M and 3.4M in Wild Arms
- `mult`/`multu`/`div`/`divu`/`mfhi`/`mflo`: ~2M in FF7 and Ace Combat 3 (the
  dynamic-cost case the decoder already flags)
- `lwl`/`lwr`/`swl`/`swr`: ~0.9M in Ace Combat 3
- `sll`, `beq`, `bne`, `lw`: mostly consequences - a branch whose delay slot is
  uncompilable is refused whole, and an interpreted load leaves a load in flight
  so the next step interprets too (`Recompiler::Step`'s `load_in_flight` check)
- GTE (`cop2`, `lwc2`/`swc2`) is not in the top of these early scenes but is
  never compiled; a 3D scene will show it.

## Ranked, by what the profile says is recoverable

1. **Take the frame resolve and the raster sync off the machine thread** -
   ~16-23% of wall on every disc, 43% in the BIOS. Have the raster thread finish
   the frame's queue and do the 15->32-bit conversion itself (or SIMD it - an
   SSE2/AVX2 pack is ~10x faster than the scalar loop), double-buffered, and
   publish; the machine thread then never waits at vblank. Native-VRAM readers
   (checksums, `--ppm`) can still force a sync.
2. **Schedule devices by event, not by 32 cycles** - measured +25-30% from the
   constants alone; the real fix is `NextEventCycles` (already written for
   `--exact-timing`) driving both the batch and the compiled chain's budget, so a
   chain runs until the next event instead of 64 instructions, and devices tick
   only when due. Also removes most of the dispatcher's share.
3. **Inline RAM stores in compiled code** (and scratchpad loads/stores) with the
   code-bitmap check inline, as loads already do - up to ~10%.
4. **Widen the compiled subset**: add/addi, mult/div/mfhi/mflo/mthi/mtlo,
   lwl/lwr/swl/swr, bltzal/bgezal, then GTE moves and commands called directly -
   ~10-15%, and it removes the dispatcher round trips they cause.
5. **SPU**: skip voices that are off/silent, generate in blocks - a few percent.

Estimate, not measured: 1-3 together remove roughly half the machine thread's
time, so about 2x (5x -> ~10x) before any of the compiled code itself changes.

## Done: the frame resolve, SSE2 (same day)

`Gpu::ResolveFramebuffer`'s 15-bit path now converts eight pixels at a time (SSE2,
the same `(c << 3) | (c >> 2)` widening in 16-bit lanes), when the row does not wrap
past VRAM's right edge; the 24-bit path and wrapped rows stay scalar. Back to back,
alternated, `--recompiler`, 1,500 frames (600 for the BIOS):

| Run | Before | After | |
|---|---|---|---|
| BIOS boot | 5.71x | 6.89x | +21% |
| Wild Arms | 5.69x | 6.93x | +22% |
| Final Fantasy VII | 5.82x | 6.96x | +20% |
| Ridge Racer | 6.55x | 7.51x | +15% |
| Area 51 | 5.93x | 6.87x | +16% |

Identical output: every `--frame-log 100` checkpoint (picture checksum, non-black
count, MDEC/CD/GP0 counts) matches the build before it at all 30 checkpoints of
3,000 frames on FF7, Ace Combat 3, Area 51, Ridge Racer and Legend of Mana, and the
final checksum of Wild Arms and the BIOS boot; `gpu_test` 95 checks, 0 failures.
That is more than the 8-14% the profile attributed to it: the scalar loop was
also stalling on lines the raster thread had just written. What remains of this
item is the vblank wait for the raster thread (7-9%), which needs the resolve to
move onto the raster thread and the front end's picture copy to follow it.

## Done: device batching follows the next event (same day)

With the recompiler on, `IOInterface` no longer ticks every device every 32 cycles.
The batch ends at the soonest event any device has scheduled
(`NextEventCycles`, the code `--exact-timing` already used), never sooner than 32
cycles and never later than 1,024; `Cpu::TickCycles` hands a chain over in one
piece instead of re-splitting it into 32s; and the chain's budget is the cycles the
current batch has left (`IOInterface::CyclesToBatch`), never less than the 64 it
was. A register write shortens a long batch back to 32, since it can schedule
something sooner than the batch was computed for. The interpreter is untouched:
every one of its baselines stays byte-identical, because none of this runs without
`--recompiler`.

Back to back with the previous commit, `--recompiler`, 1,500 frames:

| Run | Before | After | |
|---|---|---|---|
| Wild Arms | 7.09x | 8.51x | +20% |
| Final Fantasy VII | 7.01x | 8.41x | +20% |
| Wild Arms, `--exact-timing` | 6.08x | 8.77x | +44% |
| Final Fantasy VII, `--exact-timing` | 6.08x | 8.32x | +37% |

Twelve-disc table, `--recompiler`, 3,000 frames, checkpoints 1,000/2,000/3,000, one
disc at a time, alternated: **97.8 s -> 81.1 s, 17% less, every disc faster.**
Pictures identical at all 36 checkpoints except Ace Combat 3's frame 3,000, which
is the same movie scene a few macroblocks on (79,268 -> 79,002 decoded, 2,229 ->
2,225 sectors), checked by eye. Pacing moved a little elsewhere, as it did with the
recompiler change of 2026-10-07: Area 51 has read 3,259 and 5,688 sectors by frames
2,000 and 3,000 where it had read 3,246 and 5,650 (the same pictures and MDEC
counts). That is the point of the change - a device event lands when it is due,
rather than a chain and a batch late, so the CD gets a little more done per frame.
The 40 hardware test programs under `test/test suite` (cpu, dma, gpu, mdec) draw
the same picture, with `--recompiler`, before and after; `rec_test` 941, `cpu_test`
297, `timer_test` 80 pass.

## Method and caveats

- `boot_runner` built `/O2 /Zi /DEBUG /INCREMENTAL:NO` (no incremental-link
  thunks); scratch sampler suspends each thread and reads RIP plus a stack walk
  (~120 samples/s here). JIT frames have no unwind info, so inclusive numbers for
  helpers called from compiled code are rooted at the helper and the share
  attributed to "[jit]" is understated; self numbers are reliable.
- Percentages are of one thread's samples including start-up and the waits.
- Speed numbers: one run at a time, A/B alternated (see the memory note on
  wall-clock noise). Discs from the share, not run in parallel.
