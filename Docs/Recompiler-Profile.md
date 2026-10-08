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

## Re-profile after both, and the next one: stores and RAM mirrors (same day)

Self time on the machine thread after the two changes above, summed by group, over
Wild Arms, FF7, Ace Combat 3 and Ridge Racer:

| Group | Share |
|---|---|
| Compiled memory calling out (`Access`, `Cpu::Load`, `Cpu::Store`, `NoteStoreRange`, `AddressTranslation`) | 17-21% |
| Dispatcher (`BlockCache::Find` - an `unordered_map` -, both `Step`s, `StepImpl`, the `load_in_flight` `std::function`, `CauseRegister`) | 12-25% |
| Interpreter fallback (`lambda_2`, `ExecuteInstruction`) | ~15% |
| Waiting on the raster thread at vblank | 5% (Wild Arms) to 24% (FF7) |
| SPU | 4-10% |

The raster thread itself is idle 56-85% of the time: the wait is the frame's drawing
arriving in a burst just before vblank and then being waited for, a serial latency that
only costs speed when the machine runs uncapped.

A histogram of what called out (1,500 frames): **RAM stores 16-40M, never direct**;
and **loads and stores through RAM's mirrors** - Wild Arms makes 28.8M mirror loads and
14.9M mirror stores beside 58M direct loads, Ace Combat 3 10.8M and 5.4M - because
the direct path stopped at the first 2 MB. Scratchpad is nearly unused (8,000 loads
in Ace Combat 3), I/O loads are 2.3-10M and have to call out.

**Done:** compiled stores write RAM themselves, and loads and stores reach the
mirrors (`HostInterface::ram_window_bytes`, 8 MB, masked down to the 2 MB):

- A store tests `BlockState::ram_store` (null while PGXP is on, the cache is isolated,
  the debugger watches, the write queue is modelled, **or an address is watched**
  - `Cpu::RamStoreIsPlain`), the segment, the window, the alignment, and then the
  page's bit in the block cache's code-page bitmap (`bt [r10], r9`). A store to a
  page with compiled code in it takes the callback, which writes it and throws the
  blocks away - so invalidation is exactly what it was. The bitmap is now a fixed
  2,048 words covering all 512 MB so compiled code can hold its address.
- Skipped, because unobservable: `icache.InvalidateLine`, which only ever writes
  0xFFFFFFFF into an array that already holds it.
- `ShiftRegImm` did not emit REX, so shifting R8-R15 would have shifted the low
  register instead; it does now (nothing used it with a high register before).

Back to back with the previous commit, `--recompiler`, 1,500 frames:

| Run | Before | After | |
|---|---|---|---|
| Wild Arms | 8.8x | 10.9x | +24% |
| Final Fantasy VII | 8.4x | 9.4x | +12% |
| Ridge Racer | 9.2x | 10.9x | +19% |

Twelve-disc table, `--recompiler`, 3,000 frames, alternated: **81.4 s -> 70.7 s (13%
less), every disc faster, all 36 checkpoint pictures and all 12 sector counts
identical** - stores cost nothing and a mirror load owes the same stall, so unlike
the batching change nothing moves. The 40 hardware test programs draw the same
pictures; `--watch-ram` finds the same writes (13, 13 and 4 at three addresses);
Final Fantasy VII with PGXP on the hardware rasteriser still draws 25,854 of 44,978
vertices from shadows with no load read directly, and the same checksum. `rec_test`
941 -> 966 (the differential suite now runs its self-modifying programs through
direct stores); with the code-page check removed 29 of them fail, with the alignment
test removed one does, and both come back.

## The dispatcher, and why cleaning it up bought almost nothing (same day)

Done: `BlockCache::Find` has a 4,096-slot direct-mapped table in front of the
`unordered_map` (a hit is one compare; `Remove` and `Clear` empty the slots they
take out), `Recompiler::Step` no longer copies the `Block`, no longer divides to count
direct reads (`stats()` does), and asks `load_in_flight` through a function pointer
instead of a `std::function`; `System::StepImpl` works the Cause bits out inline
instead of calling `Cpu::CauseRegister`. Output identical (twelve-disc table: all
pictures and sector counts; `rec_test` 966/0).

**Measured: +0-3%**, back to back (Wild Arms 11.06x -> 11.17x, Ridge Racer 10.85x ->
11.13x, FF7 9.5x -> 9.6x), inside the noise of one run. The per-step cost was never the
problem; **the number of steps is.** Wild Arms takes 22.6M steps in 1,500 frames, 11.9M
compiled chains and 10.7M single interpreted instructions - 47% of the steps for 2.4% of
the instructions, each one a full trip through `StepImpl`, the bridge and the engine.

What those 10.7M are (a histogram of why `Interpret` was called):

- **About 4.4M are the BIOS boot**, identical in every game (150 frames of it): a loop of
  `sll`/`bne`/`lw` the compiler declines for a reason I have not chased, because it is paid
  once per boot and not per frame.
- **In play, per 1,500 frames, Wild Arms:** `add` 1.2M, `addi` 0.7M, `mult`/`multu`/
  `div`/`divu`/`mfhi`/`mflo` ~1.5M, then the instruction after each of those, since an
  interpreted load or a refused instruction makes the next step interpret too. Ridge
  Racer and Ace Combat 3 add `mult` (0.3M), `mfhi` and the GTE's `cop2` (0.9-1.0M).
- Branches refused for their delay slot: a few thousand. Not a factor.

So the next dispatcher gain is **compiling `add`/`addi` (trapping on overflow) and the
multiply/divide family** - fewer steps, not cheaper ones - and the GTE moves after that.

## Done: add, addi and the multiply/divide unit are compiled (same day)

The histogram above said what the steps were; this removes the biggest part of them. `add`, `addi`,
`mult`, `multu`, `div`, `divu`, `mfhi`, `mflo`, `mthi` and `mtlo` compile, when the host says it can answer
for them (`HostInterface::overflow` and `::hilo`; without both they stay the interpreter's, as every
`rec_test` harness other than the new tests still has it).

- **`add`/`addi`**: an x86 `add` and `jno`; the overflow path calls out to raise the exception exactly as a
  faulting memory access does, and leaves by the same fault exit. A load in flight is written to its register
  on that path only, so the block that carries on still lands it after the add. Not compiled in a branch's
  delay slot - the exception there is the branch's, with its address and the BD bit, which only the
  interpreter does - and the branch before such a slot stays the interpreter's with it.
- **The unit** (`BlockState::special`, one entry because the compiled code's one-byte offsets run out at 128
  bytes): HI and LO stay the CPU's; compiled code passes the operands and gets a value back. What makes this
  more than a call is the clock. The interpreter ticks inside each instruction and `mfhi` waits for the last
  multiply by comparing the machine's cycle count with when the unit finishes; compiled code ticks nothing
  as it goes. So the engine tells the host how far the chain has got - `(budget spent) + (extra cycles owed)
  + (index in the block)`, added to the clock as the chain began - and the host answers with what the
  instruction costs beyond its one cycle (`Cpu::CompiledHiLo`), which the chain is charged with the rest. The
  interpreter's `MULT`, `MULTU`, `DIV` and `DIVU` now call the same `Cpu::MulDiv`, so the arithmetic and the
  cost bands are in one place.

Back to back with the previous commit, `--recompiler`:

| Run | Before | After | | Interpreted instructions |
|---|---|---|---|---|
| BIOS boot, 600 frames | 8.1x | 9.8x | +21% | 8.6M -> 4.2M |
| Wild Arms, 1,500 frames | 10.4x | 11.8x | +13% | 10.7M -> 6.8M |
| Ridge Racer | 10.7x | 11.8x | +10% | 11.2M -> 6.8M |

Twelve-disc table, `--recompiler`: 89.7 s -> 75.0 s, **all 36 pictures identical**; Area 51 has read 3 more
sectors by frame 3,000 (5,691 for 5,688), pacing, the same kind as before. Wild Arms and Ridge Racer's
1,500-frame checksums are unchanged. The BIOS shell differs from the previous recompiler only at frame 600
(100-500 identical), by a handful of draw commands - the interpreter itself lands on a third answer there.
The 40 hardware test programs draw the same pictures; amidog's `psxtest_cpu` - the one that checks the traps
and the unit's timing column - gives the same results screen as the interpreter and the previous recompiler;
Final Fantasy VII with PGXP on the hardware rasteriser still draws 25,854 of 44,978 vertices from shadows with
the same checksum. `rec_test` 966 -> 985, `cpu_test` 297, `timing_test` 38 and `timer_test` 80 unchanged.

**What the tests check against.** `RunReference` in `rec_test` is the interpreter's `ADD`, `ADDI`, `MULT`,
`MULTU`, `DIV`, `DIVU`, `MFHI` and `MFLO` written out again with its own clock, not the compiler's logic. Over
four operations, sixteen operand pairs (each of multiply's three cost bands either side of zero, divide by zero,
`INT_MIN / -1`) and seven gaps before the result is read - the unit busy, then not - compiled code agrees on
every register, HI, LO and the cycles. A second test runs the same program as two blocks entered one at a
time and as a chain of linked blocks and requires the same total either way. Each of the three guards was
removed in turn - the index in the clock, the delay-slot rule, the load written out on the trap - and one or
two checks fail each time.

What is left of the steps: Wild Arms now takes 6.8M interpreted steps in 1,500 frames, of which about 4.4M is
the BIOS boot. The remainder is `lwl`/`lwr`/`swl`/`swr`, the coprocessor moves and the GTE.

## Done: the GTE is compiled, and compiled code keeps the machine's clock (same day)

The re-profile on 3D games (Ridge Racer, 3,000 frames): 63.8M interpreted instructions, 37.7M of them
the GTE and its neighbours - `mfc2` 5.0M, `mtc2` 7.7M, `ctc2` 4.3M, `cfc2` 0.5M, `lwc2` 4.0M, `swc2` 5.3M,
the commands ~4M - and 26M more behind them (the instruction after an interpreted load is interpreted too).
Cop0 is a few thousand and is not compiled: a compiled `mtc0` could isolate the cache or move the interrupt
mask in the middle of a chain, which the loads and stores assume cannot happen.

- **Commands, `mfc2`/`cfc2`, `mtc2`/`ctc2`, `lwc2`, `swc2`** go to `BlockState::special` with the GTE
  operations (`kSpecialGte*`, `HostInterface::gte`, `Cpu::CompiledGte`). A command and a register read wait for
  the command before them by the machine's clock, as `Cpu::COP2` holds (one cycle past the end), which the
  host works out from `elapsed` exactly as for the multiply/divide unit. `mfc2` and `cfc2` deliver an
  instruction late like a load, so they arm the load delay the same way, need a follower in the block, and
  count as a load in flight for the rule that keeps a memory access's address register out of the way.
  `lwc2` and `swc2` are the existing compiled load and store with the GTE's register in place of one of the
  CPU's. While the host tracks PGXP the moves, `lwc2` and `swc2` stay the interpreter's - it carries a shadow
  with each - and commands still compile. An address error on `lwc2` stops the block before the register is
  written; the interpreter writes the zero `Load` returned. Nothing else differs.
- **The first version failed amidog's TIMING group**, every command, while the flag and value columns were
  right. That test times a command by reading a root counter before and after it. Until now each GTE command
  ended a chain, which charged the cycles so far to the machine; with the commands compiled the whole loop is
  one chain, and a counter read in the middle of it sees the clock where the chain began. It was an old,
  silent limit of compiled code - any compiled loop that times itself with a counter reads the same time twice -
  that the interpreted GTE had been hiding. **Fixed in general, not for the GTE:** before a load or store past
  RAM (counters, GPU, CD, DMA, the BIOS, everything with a clock) the engine brings the machine up to the
  instruction making it (`HostInterface::sync` -> `Cpu::TickCycles`), working out how far into its block that
  is from `BlockState::block_pc`, which a block with a memory access now writes when it starts; the chain is
  charged only what has not been synced. RAM and its mirrors are skipped.

Ridge Racer, 3,000 frames, back to back with the previous commit: **8.3x -> 10.5x (+26%)**, interpreted
instructions 63.8M -> 16.9M. Wild Arms +3%, Ace Combat 3 and FF7 within noise (their remaining steps are
elsewhere - see below). Final checksums unchanged on all four.

Twelve-disc table, `--recompiler`: every picture identical at all 36 checkpoints, 77.6 s -> 71.3 s. **Area
51 has read 5,651 sectors by frame 3,000** where the previous recompiler had read 5,691 - and 5,651 is what the
interpreter reads: the recompiled run's checkpoints (picture, MDEC, CD sectors and GP0 words at frames 1,000,
2,000 and 3,000, and the CD command count) now equal the interpreter's exactly. The sync is why - events land by
the clock the interpreter would have had at the access - and it is the first disc on which the two CPUs agree to
the sector (the other eleven still differ by a few, as the recompiler's flat cycle per instruction is not the
interpreter's). The 42 hardware
test programs (cpu, dma, gpu, mdec, gte, gte-fuzz, cop) draw the same pictures; `psxtest_cpu` gives the
interpreter's results screen; FF7 with PGXP on the hardware rasteriser still draws 25,854 of 44,978 vertices from
shadows with the same checksum; `cpu_test` 297, `timing_test` 38, `timer_test` 80, `gpu_test` 95, `media_test`
479 unchanged.

**amidog's `psxtest_gte`, through the compiled path, passes**: run for 558,200 frames with the start button
pressed at frame 1,000 (it waits at its menu otherwise - a first comparison of two menus proved nothing and
was thrown away), the final results screen is byte-identical to the previous recompiler's, with every
group's X, F, V and T green in the TOTAL row, and at frame 40,000 the three builds' pictures agree exactly
once the sync is in (the build without it differs). The suite runs 43% faster, 8.97x to 12.84x.

`rec_test` 985 -> 1031: a reference written from `Cpu::COP2` and the load pipeline (its own clock, a
load in flight and one a stage behind) against compiled code over four command costs, eight gaps and two
readers, with the delay slot's old value and the next instruction's new one; `lwc2` and `swc2` with RAM direct
and called, and a misaligned `lwc2` faulting; the pending-load rule for `mfc2`; PGXP gating; an
admission sweep; a read waiting for a command by the chain's clock, across linked blocks as across entries; and
the sync's exact cycle counts. Three guards were removed in turn - the load delay on `mfc2`, the in-flight rule,
the follower rule - and one or two checks fail each time; the sync's two halves, the subtraction and
`block_pc`, likewise.

**What is left**, from the same histogram: on 2D games nearly all of the remaining interpreted steps (FF7:
6.6M of 7.1M in 3,000 frames) are not uncompilable instructions at all. They are loops whose back edge is a
branch with a **load in its delay slot**: the load lands after the instruction at the branch's target, which is
in another block, so the compiler refuses the load, and with it the branch, and the three instructions around
the loop's edge cost a full dispatcher trip each iteration (one loop in FF7: 1.2M iterations in 1,000 frames).
Both targets of a conditional branch are known when it is compiled, so the load can be written out at the end of
the block when neither first instruction reads or writes its register. After that: `lwl`/`lwr`/`swl`/`swr`
(1.8M each in Ridge Racer), and every BIOS call's entry at 0xB0, which stays the interpreter's by design.

## Method and caveats

- `boot_runner` built `/O2 /Zi /DEBUG /INCREMENTAL:NO` (no incremental-link
  thunks); scratch sampler suspends each thread and reads RIP plus a stack walk
  (~120 samples/s here). JIT frames have no unwind info, so inclusive numbers for
  helpers called from compiled code are rooted at the helper and the share
  attributed to "[jit]" is understated; self numbers are reliable.
- Percentages are of one thread's samples including start-up and the waits.
- Speed numbers: one run at a time, A/B alternated (see the memory note on
  wall-clock noise). Discs from the share, not run in parallel.
