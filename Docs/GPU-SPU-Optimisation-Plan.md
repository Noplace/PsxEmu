# Profiling and optimising the rasteriser and the SPU

## Measured, 2026-09-16: neither of them is the bottleneck

Step 1 has run. Four `boot_runner` builds - baseline, `Gpu::PlotPixel`
stubbed, `Spu::GenerateFrame` stubbed, both stubbed - run one at a time, best
wall-clock of the repeats:

| Workload | Baseline | Rasteriser | SPU | Both | Everything else |
|---|---|---|---|---|---|
| BIOS boot, 400 frames | 3.99s | 0.51s (12.8%) | 0.15s (3.8%) | 0.66s (16.5%) | 3.33s (83.5%) |
| Wild Arms, 1500 frames | 15.03s | 0.76s (5.1%) | 0.42s (2.8%) | 1.28s (8.5%) | 13.75s (91.5%) |
| Area 51, 1500 frames | 14.06s | 0.57s (4.1%) | 0.42s (3.0%) | 1.01s (7.2%) | 13.05s (92.8%) |

**So the answer is no.** This document's own criterion was "if either is a
third of the time, it is far cheaper to fix than a recompiler" - the
rasteriser is 4-13% and the SPU 3-4%, and on a real game *both together* are
under 9%. Rewriting the rasteriser perfectly, at a week of work and with bug
60 as the standing warning about what a one-line change to it can cost, would
buy about 5% on a game.

The remaining 90% is everything that is not those two: the interpreter's
dispatch and its memory accesses, DMA, the CD-ROM, the MDEC and the timers. A
sampling profiler is what splits that further, and it is worth doing before the
recompiler rather than after, because "the interpreter" and "the bus" are very
different answers.

**The BIOS boot is the outlier and it is misleading**: 12.8% there is the
shell's full-screen gradient being redrawn with nothing else happening. Games
spend proportionally far less. Do not use the BIOS boot to decide this.

### The methodology correction that came with it

The 0.94-1.2x real-time figures this project has been quoting for disc runs -
including in [Recompiler-Plan.md](Recompiler-Plan.md)'s justification - were
measured with **three runs going at once**, against disc images on a network
share. One run at a time, the same discs give **1.69x** (Wild Arms) and
**1.73x** (Captain Tsubasa), at about 99 fps. Wall-clock is the one number in
this project that is not deterministic, and it must be measured alone.

## Why this comes before the recompiler

The interpreter runs the BIOS boot at 1.57x real time and the heavier discs at
roughly 0.94-1.2x. Something in that run is the bottleneck, and nobody has
established that it is the CPU. Two candidates are scalar C++ doing a great
deal of per-element work:

- **The rasteriser** plots about 84 million pixels in a 400-frame BIOS boot -
  a `Plot()` call each, with a clip test, a dither lookup, a 16-bit read for
  the mask test and semi-transparency, and a write.
- **The SPU** generates one sample at a time, 44,100 a second, and each one
  walks 24 voices: ADPCM decode, a four-point Gaussian interpolation, the ADSR
  envelope, the sweeps, the reverb network.

If either is a third of the time, it is far cheaper to fix than a recompiler is
to write, and the fix is ordinary optimisation rather than a new execution
engine. This document is how to find out, and what to do about each answer.

## Step 1: attribute the time, cheaply

No profiler needed for the first cut. `boot_runner` already reports wall-clock
against emulated time, so the measurement is a subtraction:

1. Baseline: `boot_runner bios/SCPH1001.BIN --frames 400 --quiet` and a disc
   run, wall-clock noted. Three runs each, since this is the one number in the
   project that is not deterministic.
2. Build a variant with `Gpu::Plot` returning immediately. The frame comes out
   black and every checksum is wrong - that is fine, this build is a stopwatch,
   not an emulator - and the delta is what the rasteriser costs.
3. Build a variant with `Spu::GenerateFrame` returning silence. Same idea.
4. Build a variant with both.

Deltas, not absolutes: "the rasteriser is 40% of the run" is what decides
whether to spend a week on it. A scratch copy of the file with one `return`
added is enough; nothing needs committing.

**Then** a real profiler on whichever won, because the cheap test says *which
component* and not *which line*. Visual Studio's sampling profiler on
`boot_runner` is sufficient - it is a console program with no window to
confuse it.

## If the rasteriser is the cost

In roughly the order of payoff per unit of risk:

- **Hoist the per-pixel branches out of the loop.** `Plot` decides dithering,
  masking, semi-transparency and depth on every pixel, from state that cannot
  change inside a primitive. A templated inner loop instantiated on those flags
  - the shape DuckStation's `gpu_sw_rasterizer.inl` uses - lets the compiler
  delete the branches entirely.
- **Walk spans, not bounding boxes.** `RasterTriangle` tests every pixel of the
  bounding box against three edge functions; a triangle covers about half of
  its box, so half the work is rejected pixels. Computing the span ends per
  scanline removes both the edge tests and the rejects from the inner loop.
- **Incremental interpolation.** The barycentric weights, and the colour and UV
  derived from them, are linear in x - they can be stepped rather than
  recomputed per pixel.
- **Then, and only then, SIMD.** Four or eight pixels at a time is a large
  rewrite and it wants the loop to be simple first. This is where DuckStation
  ends up, and it is the last step, not the first.

Bug 60 is the cautionary tale for all of it: a one-line change to the loop
bounds lost the last column and row of every frame and nothing noticed for four
days. Every one of these changes must leave the twelve-disc table in
[Test-Suite.md](Test-Suite.md) byte-identical, and that is the whole safety
net.

## If the SPU is the cost

- **The per-sample voice loop is the hot part**, and most voices are usually
  off. An early-out on the voices that are keyed off, or a compacted list of
  active ones, is cheap and safe.
- **Generate in blocks.** `GenerateFrame` is called once per sample from
  `Tick`; a block of 32 or 64 samples amortises the per-call overhead and gives
  the compiler something to vectorise. The SPU's own timing is per-sample, so
  the block size has to divide evenly into everything that observes it - the
  IRQ address check in particular.
- **The reverb network** runs its comb and all-pass stages per sample through
  sound RAM. It is the newest code here (the September commit), it has no test
  coverage, and it is the first thing to measure rather than assume.
- **The Gaussian interpolation** is four multiplies and a table lookup per
  voice per sample. It is correct and it is not obviously reducible; leave it
  until the profiler says otherwise.

## What must not move

Any change here is a change to what the machine produces. The instruments:

- **The twelve-disc table and the BIOS boot row** in Test-Suite.md, byte for
  byte. A rasteriser optimisation that moves a checksum has changed a picture.
- **`gpu_test` and `spu_test`**, which cover the register behaviour and the
  mixer respectively.
- For audio specifically, `boot_runner --wav` and the `spu` summary line: the
  peak and the key-on count are a cheap check that the mix did not change
  shape, and the WAV is there when it did.

## The honest expectation

A rasteriser rewrite is a week of work for a machine that already runs at
roughly real time, and the reason to do it is the same as the recompiler's: the
150% and 200% speed settings have nowhere to get the headroom from. If the
profiling says the rasteriser is 40% of the run, fixing it is a far better
first move than a JIT - it is smaller, it is testable against existing
baselines, and it cannot break timing. If it says the CPU interpreter is 80% of
the run, this document is finished and
[Recompiler-Plan.md](Recompiler-Plan.md) is the answer.
