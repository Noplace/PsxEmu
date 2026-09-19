# Test suite and regression baselines

Per section 6 of
[Emulator-Project-Standards.md](Emulator-Project-Standards.md).

Build the harnesses with `PSXEmu.Core\tools\build_tools.bat`; they land in
`Temp\tools\`.

## cpu_test

    cpu_test [group]

Unit tests for the R3000A, the memory map, exceptions and the interrupt path.
Each test assembles a handful of MIPS instructions into RAM, runs them through
the real CPU, and checks what came out - the same path a game takes. No BIOS,
no window. A group name runs only that group.

**Current: 251 checks, 0 failures.**

| Group | Covers |
|---|---|
| `arithmetic` | add/sub wraparound, sign vs zero extension of immediates, the logical ops, signed vs unsigned compares |
| `shifts` | arithmetic vs logical right shifts, variable shifts masking the amount to five bits |
| `muldiv` | signed and unsigned multiply, HI/LO, division by zero, and the most negative value divided by -1 |
| `muldelay` | mult/multu's 6/9/13-cycle cost by rs magnitude, div/divu's fixed 36, and reading hi/lo before the operation finishes waiting out the busy window instead of skipping it (bug 43, the same hazard shape as `gtedelay`'s GTE check) |
| `branches` | every conditional, taken and not, at negative/zero/positive and at the extremes; that the delay slot runs either way; that a branch never writes its own operand; that a taken branch and its delay slot cost 1 cycle together and a not-taken branch costs 1 cycle on its own (bug 43) |
| `jumps` | j/jal/jr/jalr, where the link register points, and that the linking branches write it even when not taken |
| `loadstore` | sign vs zero extension on byte and halfword loads, and that partial stores leave their neighbours alone |
| `unaligned` | lwl/lwr/swl/swr at all four alignments, and the pairs used together to move an unaligned word |
| `loaddelay` | a load's value landing one instruction late, a write in the delay slot beating it, a second load to the same register discarding the first, and the pairing surviving a branch delay slot |
| `gtedelay` | MFC2 having the same one-instruction load delay as an ordinary load (bug 42, the same shapes as `loaddelay` aimed at MFC2), and that a GTE register read right after a command waits out its busy time rather than skipping it |
| `sqrloop` | reconstructs bug 42's own psxtest_gte SQR loop and measures this core's cycles directly: exactly 9 cycles/iteration, matching the hardware-recovered value (bug 43) - a regression guard on the GTE-busy-wait/branch-cost interaction that answered CPU-Timing-Plan.md's phase 2 question |
| `memory` | RAM through KUSEG/KSEG0/KSEG1, RAM mirroring, the scratchpad, hardware registers through all three windows, the BIOS being read-only, and $zero staying zero |
| `exceptions` | syscall and break vectoring, the Cop0 status stack pushing and popping, mfc0/mtc0 |
| `interrupts` | I_STAT acknowledge semantics, the three gates that can block an interrupt, and that EPC points at the instruction that has *not* run |

Two of these are worth reading twice, because both encode a bug that cost real
time to find the hard way:

- **A branch never writes its own operand.** Bug 1 was `blez` written with an
  assignment where a comparison was meant, so it zeroed the register it was
  testing. Three lines of test, an afternoon of BIOS disassembly.
- **EPC points at the instruction that has not run.** Pointing it at the one
  that just finished makes it run twice on return, and when that instruction is
  an `rfe` the status stack is popped twice and interrupts never come back.
  That is bug 7.

## gte_test

    gte_test [group]

Unit tests for the geometry coprocessor. No BIOS, no window. Registers are
loaded, a command word is executed, and the results are checked - the same path
a game takes, through the same MFC2/MTC2/CFC2/CTC2 semantics.

**Current: 99 checks, 0 failures.**

| Group | Covers |
|---|---|
| `registers` | the 16-bit packing, which registers sign-extend on read, the read-only ones, SXYP pushing the FIFO rather than storing, IRGB/ORGB packing and clamping, LZCS/LZCR |
| `flags` | FLAG cleared at the start of every command, IR saturation and its flag bit, the lm bit choosing 0 or -8000 as the floor, the derived error bit, MAC0 overflow |
| `rtps` | the identity transform, the translation vector, the depth FIFO push, the screen offset, the perspective divide and its overflow, IR0 from DQA/DQB, and RTPT doing all three vertices |
| `nclip` | the signed area, both windings, and a degenerate triangle |
| `averagez` | AVSZ3 over the newest three depths, AVSZ4 over all four, and OTZ saturating |
| `arithmetic` | SQR, OP as a cross product, GPF scaling by IR0, GPL adding to the accumulator |
| `mvmva` | each matrix, each vector including IR, each translation, and the documented broken far-colour case |
| `colour` | the lighting chain, CODE passing through the colour FIFO untouched, the FIFO shifting, and component saturation |
| `unknown` | an unrecognised command being counted rather than silently ignored |

Expected values are derived from the hardware description, not from this
implementation, so a failure means the code is wrong rather than that it
changed. NCLIP's area, OP's cross product and AVSZ's weighted sum are each
computed by hand in the test.

These tests say the implementation agrees with the description. That it agrees
with the hardware is amidog's `psxtest_gte` (`test/psxtest_gte/`): its REG and
COMPLEX groups pass for all 22 commands. Its TIMING group is bug 42 and
[CPU-Timing-Plan.md](CPU-Timing-Plan.md). The BIOS shell issues zero GTE
commands, so the BIOS baseline below says nothing about the GTE; the game table
does.

## gpu_test

    gpu_test

Register-level tests for the GPU's command and status handling. No BIOS, no
window: commands go straight to GP0/GP1 the way the memory-mapped registers
would, and GPUSTAT and I_STAT are read back.

**Current: 31 checks, 0 failures.**

This is a starting set, not full coverage - the rasteriser is exercised
indirectly by every `boot_runner` run and the framebuffer checksums below, so
what is here is register behaviour nothing else drives, plus one rasteriser
check that earned its place by catching a real bug: GP0(1Fh) setting
GPUSTAT.24 and raising I_STAT's GPU line, GP1(02h) acknowledging it and
allowing a fresh edge, a repeated request while unacknowledged raising no
second I_STAT edge, GP1(00h) reset clearing both, and a polyline's
terminator word not being drawn as a bogus final vertex (bug 45 - it was).
It also pins down, as a fact about the current code rather than an
assumption a future change discovers the hard way, that the three GPUSTAT
readiness bits report ready
unconditionally - there is no GP0 FIFO or drawing-time model yet. See bug 40
in [Bugs-Found.md](Bugs-Found.md) and "no drawing time" in [Gaps.md](Gaps.md).

It also covers the display side, where the same two-registers-read-as-one
mistake was possible: the visible width is `GP1(06h)`'s window divided by
`GP1(08h)`'s dot clock, not the mode width itself. Each of the five modes
still produces its nominal width from the standard 512..3072 window, 368 mode
produces 365 from it, Metal Gear Solid's codec registers produce 318 rather
than 368 (bug 50 - the extra 50 columns were VRAM past the framebuffer), and
a window wider than the mode, or an inverted one, falls back rather than
sampling off the end.

## media_test

    media_test [work-directory]

Protocol-level tests for the disc layer and the CD-ROM controller. No BIOS, no
window, no disc of its own - it writes the images it needs into the work
directory and deletes them afterwards. Exit code 0 if everything passed.

**Current: 259 checks, 0 failures.**

A second argument of `keep` leaves the generated images behind, which is how
`boot_runner --boot-disc` gets a disc to point at without a game.

Covers, in the order it runs:

- **MSF/BCD round trips**, including the awkward boundaries (74, 75, 76 frames;
  4499, 4500, 4501 sectors)
- **A cooked 2048-byte ISO**: track table, the 150-sector lead-in, the right
  sector coming back for a given address, a synthesised sync pattern and
  header, and reads off both ends failing rather than returning stale bytes
- **A cue sheet with two tracks**: track starts, lengths and types, and a
  sector inside track 2 resolving through that track's own offset
- **An Alcohol `.mds` and its `.mdf`**: the 2448-byte stride these dumps use,
  which no file length can reveal; the pregap that gives a track a disc
  address 150 sectors ahead of its position in the file; that pregap reading
  as silence rather than as an error; and the descriptor being picked up when
  the image beside it is what was opened
- **The controller with an empty tray**: Getstat answers, GetID reports "no
  disc" as an INT5 rather than silence, and an unknown command still answers
- **The controller with a disc**: GetID reports a licensed region, GetTN
  reports the track count, Setloc + ReadN delivers the sector that was
  actually asked for, and GetlocL answers with that sector's header - eight
  bytes with no status byte in front (bug 51); Getparam reads back the mode
  and the Setfilter file and channel, with the always-zero byte between them
  (bug 64)
- **An ISO9660 filesystem** the test builds itself: the volume descriptor, the
  root directory, and finding a file by every form software writes - bare
  name, either slash, a `cdrom:` prefix, a `;1` suffix, the wrong case - plus
  reading one back at its exact size rather than rounded up to a sector
- **SYSTEM.CNF parsing**: the ordinary form, the spacing and line endings that
  vary by publisher, BOOT appearing partway down, and files with no BOOT line
  failing rather than guessing
- **Booting a disc end to end**: mount, read SYSTEM.CNF, resolve the
  executable, load it, and check both that the payload reached its load address
  and that the pc points at the entry point - then that a disc with no
  filesystem and an empty drive each fail *with a reason*

Every one of these is a silent failure otherwise. A sector reader off by the
150-sector lead-in returns perfectly valid data from the wrong place; a
controller that ignores a command it does not know hangs whatever sent it.
Neither says anything except "the game did not boot".

## spu_test

    spu_test [group]

Unit tests for the sound unit. No BIOS, no window, no audio device. Sample
data is written into sound RAM, voices are keyed on through their real
registers, and the frames that come out are checked.

**Current: 108 checks, 0 failures.**

| Group | Covers |
|---|---|
| `registers` | the voice register file, per-voice addressing, the read-only ones |
| `keyonoff` | key-on and key-off being edge-triggered, ENDX set and cleared |
| `adpcm` | block decode, the shift and filter, the end and repeat flags |
| `loopaddr` | where a voice loops back to: a repeat address written before key-on surviving it, the loop-start flag setting it when software has not, software outranking the flag until the next key-on |
| `envelope` | the attack ramping rather than starting at full, the level being readable, silence without a key-on |
| `mixer` | per-voice and main volume, left and right kept separate |
| `timing` | one frame per 768 cycles, and the frame count over a known run |
| `noiseirq` | the noise generator running, the IRQ address compare |
| `cdvolume` | the CD input volume as a plain signed level, not a sweep register (bug 36) |
| `xaparams` `xacounts` `xashift` `xastereo` `xafilter` `xasat` | XA-ADPCM: parameter offsets, frame counts, silence and shift, mono and stereo, the filter carrying across sectors, saturation |

`loopaddr` is the one worth reading twice. Four of its eight checks fail
against the implementation as it stood before bug 39 (measured, by putting the
old behaviour back and running the group against it) - while all 99 checks that
existed then passed. Nothing asserted where a voice loops back to, only that it
was still making a noise afterwards, and a voice looping over three times as
much sample as it should makes a noise perfectly happily. "Still audible" and
"audible and correct" are not the same measurement.

## boot_runner

    boot_runner <bios.bin> [options]

| Option | Effect |
|---|---|
| `--disc <path>` | Mount a disc: a `.cue`, an image file, or a drive letter |
| `--boot-disc` | Read SYSTEM.CNF from the mounted disc and start its executable |
| `--auto-boot` | Let the BIOS run for real, then take over at pc=80030000 - the point it would hand a game control at |
| `--exe <file>` | Side-load a PS-EXE. Alone, immediately - before the BIOS has run at all. With `--auto-boot`, deferred until the BIOS writes POST code 7 (kernel initialised): BEV and Isolate Cache cleared and the kernel's tables set up, which a standalone test program can assume, and the shell not yet copied over the program's uninitialised data (bug 65) |
| `--frames <n>` | Run for n frames, then stop (default 300) |
| `--ppm <file>` | Write the final visible frame as a PPM |
| `--vram <file>` | Write the whole 1024x512 of VRAM as a PPM |
| `--trace <n>` | Disassemble n instructions as they execute |
| `--trace-skip <n>` | Start tracing only after n instructions |
| `--trace-at <hex>` | Start tracing when the pc first reaches an address |
|  `--trace-irq` | Start tracing when the first hardware interrupt is taken |
| `--hot <n>` | Print the n most-executed addresses |
| `--dis <hex>:<n>` | Disassemble n instructions from an address (RAM or BIOS) |
| `--watch-vram x,y,w,h` | Report which GP0 command wrote each pixel into a VRAM area |
| `--wav <file>` | Write everything the SPU produced as a 44100 Hz stereo WAV |
| `--press b@f[+h]` | Press a button at frame f, holding h frames |
| `--cd-mechanical` | Charge the CD-ROM for spin-up, seek distance and rotational latency (`EmuConfig::cdrom_mechanical_timing`). Off by default, exactly as in the front end - every baseline in this document is a flag-off number, and none of them hold with it on |
| `--load-state <file>` | Resume from a save state instead of booting - skips `--disc`/`--boot-disc`/`--auto-boot`/`--exe` entirely |
| `--save-state <file>` | Write a save state after the run finishes |
| `--quiet` | Suppress the per-100-frame progress lines |

Exit code is 0 if anything was drawn, 1 if the final frame was entirely black.
A run that draws nothing is a failure, not a pass with a boring picture.

Every run prints, unprompted: a framebuffer checksum, the non-black pixel
count, the `BREAKPOINT` trap count, how many RFEs executed, interrupt counters,
CD-ROM tallies, GTE and GPU tallies, GP0/GP1 and GTE command histograms, the
setup of the first textured primitives, every CPU-to-VRAM transfer, the Cop0
status history, and every hardware register touched with read and write counts.

The **Cop0 status history** is a ring of the last 64 exceptions, RFEs and writes
to the status register, with the pc and the before/after status each time. With
only a handful of exceptions in a whole run, the order they happened in says far
more than any counter - it is what turned "the boot hangs somewhere" into "the
second vertical blank is entered and never returned from".

The register list is the single most useful thing in that output. It answers
"has the machine got as far as X yet" without any tracing at all.

The **GPU counters** answer the other question that costs the most time: a
primitive that was never issued, one that was issued and drew nothing, and one
that drew the wrong thing all look identical on screen. Between them they
separate every case:

- the **command histogram** says whether the primitive was issued at all
- the **rejection counts** - clipped, mask-rejected, transparent - say why its
  pixels went nowhere
- the **texel-depth split** says whether its texture was read at the depth it
  was meant to be
- the **textured setup log** prints the raw texpage and CLUT attribute words
  next to what they decoded to, so a decode bug is visible without a trace
- the **transfer log** shows every CPU-to-VRAM upload and whether all of its
  pixels arrived
- **`--watch-vram`** names the GP0 command behind every write into a chosen
  VRAM rectangle, which answers "what is this region and who made it"

Bug 14 was found from two of those numbers sitting next to each other:
textured quads were being issued, and 5.19 million texels were being rejected
as transparent with nothing clipped and nothing mask-rejected.

## wav_pitch

    wav_pitch <file.wav> [options]

Reads what `boot_runner --wav` wrote and prints what note is in it, window by
window: time, RMS, peak, frequency, the nearest equal-tempered note and how far
off it is in cents.

| Option | Effect |
|---|---|
| `--from <sec>` / `--to <sec>` | The range to analyse |
| `--window <n>` / `--hop <n>` | Analysis window and step, in samples (2048 / 1024) |
| `--min-rms <n>` | Skip windows quieter than this (default 300) |
| `--min-hz <n>` / `--max-hz <n>` | Search range (default 60 / 5000) |
| `--summary` | One line per note instead of one per window |

It exists because audio is the one part of this machine with no equivalent of a
framebuffer checksum. "The notes sound low" cannot be diffed, put in a bug
report, or checked again after a change; `F5 698 Hz, +0.6 cents` can.

Pitch is detected with YIN's cumulative mean normalised difference rather than
plain autocorrelation. That is not a detail: autocorrelation's characteristic
failure mode is reporting a note an octave out, and octaves are exactly what
this tool gets pointed at. It depends on nothing, not even the core.

## frame_limiter_test

    frame_limiter_test

Eight checks on `platform/frame_limiter.h`: that a loop is held to 59.29 Hz and
to PAL's 49.76 rather than run flat out, that work done inside the frame comes
out of the wait rather than on top of it, that a host too slow to make the
deadline runs slow instead of sprinting to catch up, and that `Reset` and a
rate of zero both do what they say.

And, since bug 62, that the frames are **evenly spaced** - the 5th and 95th
percentile intervals within 3 ms of the period, and no single frame half a frame
late. The six rate checks all passed while frames were arriving 0 to 30 ms
apart, because the overshoots and the catch-ups averaged out; the audio pump,
fed once a frame, was the thing that noticed.

It exists because **a harness has neither a monitor nor a sound device**, and
those were what the emulator's speed used to be set by. This checks the one
piece that takes the decision away from both of them. See bug 49; the emulator
ran at 2.8x on a 165 Hz display for as long as it did partly because no test
could have caught it.

The speed itself is no longer beyond reach: the built emulator can be launched,
sent a `WM_COMMAND` to boot, and read back through its title bar - which is how
the threading work was checked, and how the numbers in Threading-Plan.md were
taken. What a frame looks like and what it sounds like are still out of reach.

Timing-sensitive by nature, so it is the one harness that can fail on a
heavily loaded machine without anything being wrong. The bounds are wide
enough that only a real regression should cross them.

## host_test

    host_test [bios]

Thirty-two checks on `PSXEmu.Core/host/` - the channels the front end's threads
talk through, and the threads themselves (Docs/Threading-Plan.md). Every other
harness here is single-threaded by construction, and a checksum cannot see a
race: a lost sample, a frame read while it was being written, a request run out
of order. So each channel is driven the way it will be used, one thread on each
side, with a sequence number in every item.

- **The channels.** Two million frames through the sample ring in random chunk
  sizes, arriving in order with nothing lost, repeated or torn; 200,000 requests
  from four threads, each thread's in its own order; 20,000 frames through the
  mailbox, none of them read half-written, with taken plus dropped accounting
  for every one published; mouse motion adding up exactly across a racing
  publisher and taker; a doorbell that does not lose a ring that came first.
- **The machine's thread.** A threaded BIOS boot lands on **boot_runner's own
  instruction count and checksum** - 97,749,265 and `c7c8db90c5984798` - which
  is the assertion that threading changed nothing about what the machine
  computes. Then again with pause and resume requests thrown at it from another
  thread as fast as it will take them (432 of them, same numbers), and again
  through a state saved at frame 200 and loaded into a fresh machine by request.
- **Stopping.** Forty machines stopped mid-frame, mid-pace and paused; the
  slowest came back in 17 ms. A paused machine answers a request in under a
  millisecond, because it waits on its doorbell rather than polling.
- **All three threads together**, at real speed, against a device that plays at
  exactly 44,100 frames a second: five seconds with nothing short, nothing
  dropped, no underrun and every frame presented - pausing twice in the middle.

The BIOS is `bios/SCPH1001.BIN` unless one is named, and the thread checks are
skipped, loudly, without it.

What it cannot check is the Win32 side: the window, the Direct3D presenter and
the real sound devices. Those were exercised by driving the built emulator with
posted `WM_COMMAND`s and reading the frame rate back out of its title bar - see
the note in Threading-Plan.md - and the last word on how it feels is still the
person using it.

## Baselines

Check these after any change to the CPU, timing, or the renderer - not just the
part being worked on.

**Measured 2026-09-16 at `8c7c694`**, which is what every number below is: one
run each, on this machine, with the build that commit produces. The table this
replaces had been stale since before bug 43 and said so; it is not kept, because
a baseline nobody can reproduce is worse than none - the progression it recorded
is in the "earlier baselines" table further down.

`boot_runner` is deterministic: three runs of one binary on one disc agree to
the instruction. A number here that does not reproduce means the build, the
BIOS or the disc image differs - see the note on the disc table below, since
the most likely answer is the network share rather than the emulator.

### The harnesses, at a glance

| Harness | Checks | | Harness | Checks |
|---|---|---|---|---|
| `cpu_test` | 251 | | `gpu_test` | 31 |
| `gte_test` | 99 | | `mdec_test` | 85 |
| `timer_test` | 70 | | `media_test` | 259 |
| `sio_test` | 105 | | `spu_test` | 108 |

**1,008 checks, 0 failures**, all eight green. Each harness's own section above
says what its groups cover. (`media_test` gained two when the front end's
`pause_in_menus` and `show_timings` settings arrived: every setting in
`EmuConfig` round-trips through the file, and those are settings.)

Four smaller harnesses cover the host-side headers the front end leans on and
are not counted above, since they test no emulation: `letterbox_test` (12
checks, aspect ratio), `frame_limiter_test` (8 checks - the average rate, and since bug 62 the
spacing between frames too), `speed_resampler_test` (11 checks, the audio
arithmetic behind 50-200% speed - the frame counts, that a minute at 150% does
not drift, and that blocks join continuously) and `host_test` (32 checks, the
threads and the channels between them - its own section above).

`rec_test` (460 checks) is not counted either, and for a different reason: it
covers the recompiler in `PSXEmu.Core/rec/`, which sits beside the interpreter
rather than inside it - nothing in `rec/` includes `psx/`, and
`psx/recompiler_bridge.h` is the one file that knows both. Its compiler checks are differential - a block is compiled,
executed, and every register, every byte of memory and the address it says to
continue at are compared against a reference interpreter written from the
instruction set rather than from the compiler. Its engine checks do the same
thing to whole programs: a loop, code that rewrites itself, and a load left in
flight across a block boundary are each run interpreted and then recompiled,
and the two machines compared. Everything that compiles or runs guest code runs
twice, once with the register allocator off and once on. See
`Docs/Recompiler-Plan.md`.

`boot_runner --recompiler` runs the machine on the recompiler instead of the
interpreter, which is how the baselines below get checked against it, and
`--recompiler-toggle N` switches between the two every N frames - the headless
stand-in for the front end's **Emulation > Recompiler** item, which can be
changed while a game is running. The BIOS
boot is identical either way; a game's framebuffer checksum is not yet, because
the cycle model is approximate - see the timing section of
`Docs/Recompiler-Plan.md`. Without the flag nothing about the run changes.

`boot_runner --recompiler-diff` is the differential harness: two whole machines,
one on each CPU, with all 32 registers plus HI, LO and the pc compared at every
block boundary. On a disagreement it prints the block's disassembly, the
registers that differ, and the interrupt counts on both sides - which is what
says whether the CPU computed something wrong or the two machines simply took an
interrupt at different instructions. It runs until the frame count is reached or
something diverges, and exits non-zero for the latter.

`rec_bench` is a benchmark rather than a test, and it asserts nothing:
recompiled against interpreted, the register allocator on against off, and a
sweep of the block length that shows where allocation starts to pay. It takes
an iteration count and a repeat count, both optional. The numbers it produced,
and what they decided, are in the plan's step 6.

### BIOS boot, SCPH1001

    boot_runner bios/SCPH1001.BIN --frames 400 --quiet

| Measure | Value |
|---|---|
| instructions | 97,749,265 |
| resolution | 640x478 |
| framebuffer checksum | `c7c8db90c5984798` |
| non-black (visible) | 305,920 of 305,920 |
| unimplemented paths | 0 |
| GTE commands | 0 - the shell menu is entirely 2D |
| RFEs executed | 919 |
| interrupts taken | 907 (vblank 339, dma 508, cdrom 3, timer2 57) |
| final I_STAT / I_MASK / SR | `00000001` / `0000000D` / `40000401` |
| GP0 words / GP1 words | 16,955 / 2,325 |
| primitives / pixels | 1,157 / 84,641,245 |
| texels 4-bit / 15-bit | 3,159,000 / 0 |
| CD-ROM commands | 3 |
| SPU | 297,483 frames, 64 key-ons, peak 28,461/23,222 |

**What moved since the last refresh, and why.** The instruction count is bug
43's, unchanged - the CPU side has not moved at all. The checksum has, and
building `535949b` (the commit before the September 12-14 GPU work) to compare
against is what turned up bug 60: those commits made the raster loops half-open
without noticing that a primitive's extent and the drawing area are different
kinds of bound, and the drawing area's last column and row stopped being drawn.
Refreshing this table is what found it - 304,803 non-black where the older
build had all 305,920, which is the sort of thing a stale baseline hides.

With bug 60 fixed the frame is full again, and against `535949b` there are now
no missing pixels at all: 131 interior pixels differ, none of them black in
either build. Those are shared columns changing owner under bug 59's corrected
fill rule, which is the intended difference. The remaining pixel-count gap
(84,641,245 plotted against 84,715,353) is the same thing seen from the other
side - a shared column is now covered once rather than twice.

The SPU peak is bug 57's: the mix was coming out at a quarter and
`audio_volume` defaulted to 2.0 to cover it. Both are corrected, so a peak of
28,461 here is the hardware's own level rather than a number to compare against
anything recorded before that bug.

**These are baselines, not targets.** The run reaches the shell menu, with no
unimplemented paths hit.

The primitive and pixel counts are the numbers most sensitive to a renderer
change; the checksum is sensitive to everything.

**Earlier baselines, kept so the progression is not lost:**

| When | Result |
|---|---|
| Before the interrupt fix (bug 7) | 640x478, `7f931a8558291383`, 890 GP0 words, 1 interrupt - got there by accident, on a path where interrupts were dead |
| After bug 7, before the DMA fix (bug 12) | 256x240, `aedac3154f8a0383`, 3 GP0 words, 2 interrupts, black screen |
| After bugs 12 and 13 | 640x478, `f0afceabcd797b57`, 18,224 GP0 words, 1,923 primitives - boots and draws, but no intro text |
| After bug 14 (DMA block mode) | 640x478, `e9ea0b3d07bd3b89`, 16 unimplemented paths - the full intro renders |
| After the GTE (Phase 2) | the table above; the 16 unimplemented paths were COP2 register moves, now handled |

### Real games

What the BIOS boot cannot show: the CD-ROM under a real game's own driver, the
MDEC decoding film, the SPU mixing, and the parts of the GPU a shell menu never
reaches. Twelve discs, 3,000 frames each from cold with no input:

    boot_runner bios/SCPH1001.BIN --frames 3000 --frame-log 1000 --quiet --disc <image>

Checksums are the visible framebuffer at frames 1000, 2000 and 3000.

| Disc | f1000 | f2000 | f3000 | non-black | res | macroblocks | sectors |
|---|---|---|---|---|---|---|---|
| Air Combat | `a1e228e8a2ee662c` | `db59eb682fe9985e` | `12a6284c62657ea5` | 51,200 | 320x240 | 243,200 | 5,293 |
| Wild Arms | `7daf7515b034bb74` | `9ecc3caaf8731ea6` | `01b3d7eb25290632` | 61,440 | 320x240 | 93,120 | 3,747 |
| Wild Arms 2 (cd1) | `00d5e173b295085a` | `c7a39acab8a692fb` | `22da9010e1a6bfbe` | 76,800 | 320x240 | 0 | 100 |
| Vandal Hearts | `bcb8fe295f5b70db` | `7e2959681f0a6aec` | `94ae6edd29a35858` | 52,652 | 320x240 | 128,400 | 5,260 |
| Legend of Mana | `ec6fe2e3bdb4fd30` | `baf825dc27742faa` | `bbc7cecc82310cd8` | 76,064 | 320x240 | 155,400 | 5,345 |
| Ridge Racer | `2e63ac3574a2a3c3` | `dc337b3bf868b8d8` | `e363f0b4ab4b87eb` | 76,415 | 320x240 | 0 | 1,578 |
| Bomberman Party Ed. | `4a31d7a6c52734a4` | `717a1bbe80c75439` | `ba27f3e0e9823174` | 76,224 | 320x240 | 147,000 | 4,686 |
| Area 51 | `085daca5fb878fff` | `8706d714ea09fec7` | `c20fec6d8f189e8d` | 51,855 | 256x240 | 100,080 | 5,511 |
| Final Fantasy VII | `37991653287d63d1` | `bbbb18dffe854383` | `44eccfde5b859174` | 75,911 | 320x240 | 0 | 668 |
| Final Fantasy VIII | `aedac3154f8a0383` | `f3ee4d06bf3e0383` | `24cffdf4fad5568e` | 3,790 | 640x480 | 0 | 1,187 |
| Ace Combat 3 | `3121874ad83b9ef4` | `afa843f1f90957a1` | `1e2454a46003d966` | 61,189 | 320x240 | 82,992 | 2,291 |
| Captain Tsubasa J | `f0779890ee9b1bb0` | `816d516f2ba1d3f8` | `add4d55f3196ad03` | 76,800 | 320x240 | 59,100 | 3,776 |

Images are the ones under `\\superserverx\D\Games\Sony\PSX\ISO`; Area 51 and
Wild Arms 2 are mounted from their `.ccd`, which gives byte-identical results to
their `.img`. Bug 60 moved Ridge Racer's frames 2000 and 3000 and nothing else
in this table - its primitives reach the drawing area's edge where the other
eleven discs' do not, which is a fair warning about how little a checksum table
proves on its own. Instruction counts are in the run's own output and are not
tabulated - they move for any timing change and say nothing a checksum does not.

**What each one is here for**, since a checksum that moves is only useful if
something says where to look:

- **Air Combat** and **Captain Tsubasa J** - the stream/MDEC path. Both were
  black screens until bug 55; a film that stops decoding shows up here as the
  macroblock count freezing rather than as a wrong checksum.
- **Area 51** - MDEC output ordering (bug: each frame decodes as two halves
  into two buffers).
- **Bomberman Party Edition** - GetlocL and the CD state machine (bug 51), and
  the multitap/SIO work (bugs 52-54).
- **Final Fantasy VII** - the SPU: its prelude is where bug 39's pitch bug and
  bug 57's halved mix both showed. Its frame 3000 is a title screen, so the
  checksum is stable but says nothing about audio; the run's `spu` line does.
- **Wild Arms** - XA audio, and the overworld tile seams of bug 59's gate.
- **Ridge Racer**, **Vandal Hearts**, **Legend of Mana**, **Ace Combat 3** -
  ordinary 2D/3D rendering under real drivers, at three points each.
- **Final Fantasy VIII** - newly bootable once `.ccd` was read. Its frame 3000
  is a near-black publisher screen (3,790 non-black), so it is a weak signal
  and mostly proves the disc still mounts and runs.

**Two traps, both of which have cost time here.**

*Run at most three of these at once.* The images live on a network share, and
under eight concurrent `boot_runner`s it starves: a run simply stops getting
sectors partway through, ends early, and produces a different checksum for a
binary that did not change. Before believing any difference, compare the
`cdrom ... N sectors` line against the table above - equal sector counts mean
both runs actually read the disc.

*Clear the save files first.* A game that finds a memory card boots
differently, reproducibly. `boot_runner` creates none of its own, but the front
end's cards under `Documents\My Games\PSXEmu\memcards\<disc>\` are the same
files if anything has played that disc.

### Register access, same run

| Register | Meaning | Expected |
|---|---|---|
| `1F801000-1020` | memory control | 1 write each |
| `1F801040/44/4A` | controller port | 288 / 288 / 216 reads |
| `1F801070` | I_STAT | 26,500 reads, 1,974 acknowledges |
| `1F801074` | I_MASK | 24,065 reads, 11 writes |
| `1F801100-1128` | root counters | written; `1F801110` read 512 times |
| `1F801800-1803` | CD-ROM | 3 commands issued |
| `1F801810/14` | GP0 / GPUSTAT | 824 / 2,325 writes, 446,156 GPUSTAT reads |
| `1F801D80-DFE` | SPU register file | 112 registers written |
| `1F802041` | POST (boot progress) | 18 writes |

Most GP0 traffic never touches `1F801810`: it arrives by DMA, which is why the
port's 824 writes and the GPU's own 16,955 GP0 words are both right.

Every device is now reached. A device dropping off this list is a regression
even when the checksum has not moved.

### Save states

Not a checksum table - a repeatable procedure, per bug 44. Run after any
change that touches a `Serialise` (or anything a `Serialise` reads, which in
practice means most of the core):

    boot_runner bios/SCPH1001.BIN --frames 900 --ppm a.ppm
    boot_runner bios/SCPH1001.BIN --frames 600 --save-state s.st
    boot_runner bios/SCPH1001.BIN --load-state s.st --frames 300 --ppm b.ppm

`a.ppm` and `b.ppm` must be byte-identical. Do it again with `--disc
Temp/disctest.iso --boot-disc` added to all three (a `media_test`-generated
test disc works) - the disc mount/reopen path is the one most likely to
silently diverge and the BIOS-only run alone will not catch it. Then:

    boot_runner bios/SCPH1001.BIN --load-state s.st --frames 0 --save-state s2.st

`s.st` and `s2.st` must be byte-identical (catches a field saved on the way
out but not restored on the way in). Finally, confirm a state made against
one BIOS is refused against another, and a state whose disc image has moved
is refused rather than silently run with a closed file.

## Traps to remember

**Line counts are not failure counts.** When the amidog suites go in, count the
actual failure marker, not output lines.

**Clear every save file before checksumming a game.** A game that finds a save
boots differently and gives a different, perfectly reproducible checksum. When
memory cards exist, clear them.

**Do not overfit.** Section 6 of the standards document records tuning one GBA
suite by 577 results while breaking 74 in two others. If a fix cannot be
justified from documented hardware behaviour, record it as a guess or leave it.
Bug 7 is the case in point here: the correct fix moved every visible number the
wrong way, and it stays anyway.

## Still to build

- **amidog's GTE suite has run** (`test/psxtest_gte/` - see bug 41 for how
  to reach it, `--auto-boot --exe` or the Win32 front end's Boot PSX-EXE menu
  command). Values and flags agree with hardware outright, and bug 42 made
  every GTE command's own cost match. Its TIMING column is still red, because
  the test's loop also measures the ordinary CPU instructions around each
  command - [CPU-Timing-Plan.md](CPU-Timing-Plan.md) phases 0 and 3.
- **amidog's CPU suite** on top of `cpu_test`, which covers the instruction set
  and, since bug 43, multiply/divide and branch costs. `test/psxtest_cpu/` is
  present and runs to a results screen unattended - see bug 41 - and its
  results have not been read precisely yet (CPU-Timing-Plan.md phase 0).
- **Memory card round trips.** Cards exist now, and nothing tests them. Per
  the standards document, the *wipe* is the point: write, wipe, read back, or
  a `Serialise()` that stores nothing still appears to work.
  [Memory-Cards-Plan.md](Memory-Cards-Plan.md) sketches an `mc_test`.
- **Reverb and volume sweeps** in `spu_test` - implemented without a check
  (Gaps.md).
