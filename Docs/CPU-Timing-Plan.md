# Real R3000A instruction timing

## The conclusion first

Bug 42 measured this core's own numbers from inside amidog's failing
`psxtest_gte` TIMING test and found the GTE half exactly right - every
opcode's recovered cost matches its documented figure, no exceptions. What
is not right, and what the same measurement pointed at directly, is
everything else in that test's loop: a register read, a branch, a few ALU
ops - ordinary CPU instructions, each charged the same flat one cycle this
core charges everywhere, magnified 501-fold into the whole result.

That is a CPU-wide project, not a GTE one, and this is its plan. The method
is the one bug 42 already proved out: don't guess a number and hope, put a
counter on both sides of the same instruction real test software brackets
and read what this core actually produces. Two oracles for that already sit
in `test/` - `psxtest_gte` and `psxtest_cpu` - and both were downloaded for
exactly this before this plan existed.

## Why this, and why now

[Gaps.md](Gaps.md) has said the same thing for a while: "Cycle timing is
modelled, not measured... the per-instruction costs beneath it are uniform
where real ones are not." That was true and low-priority right up until bug
42 showed a case where the uniform model is the *entire* remaining
discrepancy in an otherwise-correct, independently-verified subsystem. A
one-cycle error that is invisible in a BIOS boot becomes the whole answer
once a test repeats it 501 times on purpose.

[Recompiler-Plan.md](Recompiler-Plan.md) already argued that a JIT's timing
model has to charge exactly what the interpreter charges, or "every
timing-sensitive thing regresses at once" the moment code compiles. Getting
the interpreter's own instruction costs right is a prerequisite for that
project succeeding, not a parallel concern - it is the same number, read
twice.

## What is already right, and should not be touched without a reason

- **The branch delay slot itself.** One instruction, modelled, `cpu_test`'s
  `branches`/`jumps` groups cover it, `loaddelay` covers the load-and-branch
  interaction. Nothing here questions the *slot* - only what a branch and
  its delay slot together cost in cycles.
- **The GTE's per-command cost and stall** (bug 42). Verified by direct
  measurement, not description. Leave it; it is the working half of the
  example that started this document.
- **`Cpu::Load`'s region-dependent stall** (3 cycles RAM, 0 scratchpad, 3
  hardware register, 5 BIOS ROM) exists and is what stopped the BIOS giving
  up on VSync (bug 16). It is a *model*, though, not a *measurement* - see
  below.
- **DMA's billed time** (bug 33/38), rate taken from DuckStation. Same
  caveat as the GTE table had before bug 42 measured it: plausible, not yet
  checked against this project's own test software the way the GTE now has
  been.

## What is measurably wrong, in the order it is worth fixing

### 1. Multiply and divide

Documented (and cross-check this independently before trusting it further -
this came from search results, not a primary fetch, exactly the kind of
figure bug 42's method exists to confirm or correct):

| Instruction | Cost |
|---|---|
| `MULT`/`MULTU`, `rs` in -0x800..0x7FF (or 0..0x7FF unsigned) | 6 cycles |
| `MULT`/`MULTU`, `rs` one magnitude class wider | 9 cycles |
| `MULT`/`MULTU`, `rs` wider still | 13 cycles |
| `DIV`/`DIVU`, any operands | ~36 cycles, fixed |

This core currently charges these the same one cycle as `ADD`. It is the
single most-repeated-in-real-code instance of the uniform-cost gap: any game
doing fixed-point math, audio mixing, or a software divide leans on these
constantly, and `cpu_test`'s `muldiv` group checks their *results*, not
their *timing* - a wrong cycle count would pass every existing check today.

**Also worth checking while here:** whether the *value-dependent* part is
right at all. A number this specific, tied to operand magnitude rather than
the opcode, is exactly the kind of detail that is easy to half-implement
(e.g. keying off `rt` instead of `rs`, or getting a boundary wrong) and hard
to notice without a targeted test.

### 2. Memory access regions

`Cpu::Load` currently charges 3/0/3/5 for RAM/scratchpad/hardware-register/
BIOS. Independent sources describing real hardware give RAM as 7-ish cycles
and on-die I/O as 5-ish (scratchpad matching this core's 0 extra either
way) - different enough from this project's 3/3 that the two cannot both be
right, and neither has been checked against real timing software yet. This
is bug 16's fix revisited with an instrument bug 16 did not have: something
that can confirm a number instead of merely un-hanging a boot.

Store timing is a separate, currently-unasked question: `Cpu::Load` is named
for loads specifically, and it is worth confirming whether stores get any
region-dependent charge of their own or are assumed uniform.

### 3. Whatever `psxtest_cpu`'s own TIMING column is already pointing at

`test/psxtest_cpu/` has the identical EXCEPTION/FLAG/VALUE/TIMING structure
`psxtest_gte` does, and a first look at its results screen (a visual read,
not yet the pixel-sampled kind of confirmation bug 42's GTE work used - that
is the first thing to redo properly here) shows the same shape: plain
ALU/immediate instructions reading as passing, and TIMING clustering red
around the branch groups (`BRA`, `BRA ADV`, `JMP`) and around `MEM DLY`
(load-delay timing) specifically, rather than everywhere. That is
consistent with #1 and #2 above rather than a third, separate cause - but
it should be read precisely, the way bug 42 read `psxtest_gte`, before
assuming that.

### 4. The specific branch-cost question bug 42's own measurement raised

Independent sources describe the R3000A resolving a branch in the decode
stage specifically so that one delay slot suffices - which reads as "no
extra cost for a taken branch beyond the delay slot instruction itself."
Bug 42's own arithmetic (measured: 9 cycles per loop iteration; this
core's instructions as currently understood sum to 10) has a one-cycle
discrepancy sitting *somewhere* in exactly this loop, and a branch costing
one less than assumed is one candidate among several (an instruction this
plan has not yet re-examined being the true zero-cost one is another). This
is the concrete, numbered example to resolve first, precisely because the
right answer is already known to exist and bracketed to within one cycle -
the same loop that found bug 42 can find this too.

## The method, concretely

Bug 42's instrument, generalised:

1. Pick a real test (`psxtest_gte`, `psxtest_cpu`, or a purpose-built
   snippet like `cpu_test`'s existing ones) whose own code brackets the
   instruction under test with a Root Counter reset and read - these
   programs were built to measure real hardware, so they already do this;
   no new harness feature is required to observe it.
2. Instrument the counter's read/write path (temporarily, as bug 42's was -
   this is a diagnostic technique, not a shipped change) to log
   `context_->cycles` at each reset and read.
3. Solve for the per-instruction cost the same way bug 42 did: known
   iteration count, known loop shape, one unknown per instruction class.
   A loop repeated hundreds of times turns a one-cycle question into an
   arithmetic one.
4. Fix the model. Re-run. Confirm the recovered number now matches what the
   test itself expects - which, per bug 42, is the nocash/psx-spx reference
   figures, not a number this project invented.

## Phases

**Phase 0 - re-read `psxtest_cpu`'s TIMING column properly. Not done.**
Pixel-sample it the way bug 42 did for `psxtest_gte` rather than eyeball it,
and get an exact list of which groups fail. This costs an hour and turns
"looks like branches and load-delay" into a confirmed, specific list -
possibly with entries this plan has not anticipated.

**Phase 1 - multiply and divide. Done (bug 43).** The operand-magnitude
table above was verified against a primary fetch of
`psx-spx.consoledev.net/cpuspecifications/` and matches exactly. Implemented
in `Cpu::MULT`/`MULTU`/`DIV`/`DIVU` via `TickCycles`, plus a busy-wait hazard
for HI/LO reads (`hilo_busy_until_cycles_`, the same shape as the GTE's).
New `cpu_test` group `muldelay` checks the cycle cost at each magnitude
boundary for both directions, `DIV`/`DIVU`'s fixed 36, and the busy-wait.
`psxtest_cpu` itself was not re-run against this phase in isolation (that's
phase 4); `cpu_test`, `gte_test` and all other harnesses stayed green, and
the BIOS boot checksum did not move.

**Phase 2 - the one-cycle branch/loop-overhead question. Answered (bug
43): the discrepancy was never real.** Reconstructing bug 42's exact SQR
loop directly in `cpu_test` (`sqrloop` group) and measuring this core's own
cycles gives exactly 9.000 cycles/iteration, matching the hardware-recovered
value precisely - no branch-timing code changed. The naive flat-count's
extra cycle came from not accounting for two things already true here: the
GTE busy-wait stall already absorbs a register read's cost into the
command's window, and a taken branch already costs 0 beyond its delay
slot's own cycle (which independent sources' "resolved in decode" claim
predicted, and this measurement now confirms directly rather than by
argument). A real, separate bug was found and fixed in the same pass: a
*not-taken* branch charged 0 cycles instead of the uniform 1, in every
conditional branch. `J`/`JAL` still charge 2 for branch+delay-slot where
`JR`/`JALR` and taken conditional branches charge 1 - left alone rather than
"fixed" on inference, since this measurement doesn't reach jumps; a genuine
open question for whoever can measure it next.

**Phase 3 - memory region costs. Not done - still the riskiest phase.**
Verify the RAM/scratchpad/I/O/BIOS
figures against a primary psx-spx fetch (not a search-result summary), check
whether stores need their own table distinct from loads, and re-derive
`Cpu::Load`'s 3/0/3/5 from measurement rather than from bug 16's
un-hang-the-boot motivation. This is the riskiest phase to get wrong: bug
16 exists because getting this region wrong once already hung the BIOS, so
change it with the full regression suite run after every step, not just at
the end.

**A primary fetch done for phase 1 already found the real figures, and two
complications phase 3 will have to design around, not just port in:**

    Scratchpad (1F800000h..)      1 cycle    ;on-chip SRAM, no bus access
    On-die I/O (IRQ/DMA/timers)   5 cycles   ;one shared decoder
    Main RAM (KUSEG/KSEG0/KSEG1)  7 cycles   ;plus occasional DRAM-refresh stalls
    BIOS ROM (1FC00000h..)        27..33     ;8bit ROM, programmable bus delay

Both complications are why this is scoped as its own phase rather than a
drop-in table swap:

- **The BIOS ROM figure is not a constant.** It depends on a memory-control
  register the BIOS itself programs early in boot (this core already has a
  `BusCtrl`/`Config` pair in `CpuContext::ctrl` but doesn't read them as a
  timing input anywhere) - "27..33" is the *range* across real consoles'
  differing bus-delay settings, not a number this project gets to pick once.
- **"Load Shadow"**: a slow load's bus access overlaps independent
  instructions that follow it, so the effective cost depends on what comes
  next, not just which region was read - psx-spx's own measurement shows an
  on-die load costing +4 cycles back-to-back but settling to +2 after four
  independent instructions. A single flat per-region stall - what
  `Cpu::Load` does today, and the simplest thing phase 3 could do too -
  cannot represent this; it's the same kind of "back-to-back
  dependent-instruction hazard beyond the load delay already modelled" the
  "What this will not do" section below deliberately scopes out. Phase 3
  should decide explicitly whether to accept that simplification (a flat
  stall, documented as approximate) or take on modelling the overlap, rather
  than drifting into it unstated.

**Phase 4 - re-run both suites, in full, pixel-sampled.** The goal is not
"green," it is an honest count: which of the 22 GTE opcodes and which
`psxtest_cpu` groups now pass, and a precise description of whatever still
does not - matching how bug 42 itself was written up, not declaring victory
early.

## What this will not do

Full pipeline accuracy - real fetch/decode/execute stage stalls, I-cache
timing, back-to-back dependent-instruction hazards beyond the load delay
already modelled - is a larger, different project, and this plan is
deliberately scoped under it. [Gaps.md](Gaps.md) already lists the
instruction/data caches as "not modelled, deliberately," and that stays true
here. If Phase 4's honest count still shows failures after phases 1-3, the
next question is whether they need that larger project or whether this one
simply is not finished yet - which the pixel-sampled results will show
directly, the same way bug 42's did.

## How to know it is right

The same discipline every other timing change in this project has used:

- **Every existing harness stays green.** `cpu_test`, `gte_test`,
  `media_test`, `spu_test`, `mdec_test`, `timer_test`, `sio_test`,
  `gpu_test` - 0 failures, at every phase, not just the last one.
- **`bios/SCPH1001.BIN`'s framebuffer checksum does not move** unless a
  change is deliberately expected to move it, and if it does, why is written
  down before the change is - bug 7's lesson, restated: the number to trust
  is the one that can be explained.
- **New checks encode the hardware, not the old behaviour.** Per section 6
  of [Emulator-Project-Standards.md](Emulator-Project-Standards.md) and per
  how the `loopaddr` and `gtedelay` groups were built: a test that would also
  pass against the wrong implementation is not testing the thing that
  matters.
- **The two amidog suites are the actual target**, not a proxy for it.
  "Passes `cpu_test`" was already true before this plan and did not catch
  the gap; "passes `psxtest_cpu`'s TIMING column, pixel for pixel" is the
  bar this plan is aimed at.
