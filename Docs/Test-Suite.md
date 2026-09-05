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

**Current: 194 checks, 0 failures.**

| Group | Covers |
|---|---|
| `arithmetic` | add/sub wraparound, sign vs zero extension of immediates, the logical ops, signed vs unsigned compares |
| `shifts` | arithmetic vs logical right shifts, variable shifts masking the amount to five bits |
| `muldiv` | signed and unsigned multiply, HI/LO, division by zero, and the most negative value divided by -1 |
| `branches` | every conditional, taken and not, at negative/zero/positive and at the extremes; that the delay slot runs either way; that a branch never writes its own operand |
| `jumps` | j/jal/jr/jalr, where the link register points, and that the linking branches write it even when not taken |
| `loadstore` | sign vs zero extension on byte and halfword loads, and that partial stores leave their neighbours alone |
| `unaligned` | lwl/lwr/swl/swr at all four alignments, and the pairs used together to move an unaligned word |
| `loaddelay` | a load's value landing one instruction late, a write in the delay slot beating it, a second load to the same register discarding the first, and the pairing surviving a branch delay slot |
| `gtedelay` | MFC2 having the same one-instruction load delay as an ordinary load (bug 42, the same shapes as `loaddelay` aimed at MFC2), and that a GTE register read right after a command waits out its busy time rather than skipping it |
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

**The important caveat:** the BIOS shell issues *zero* GTE commands, so nothing
here has been checked against real software. These tests say the implementation
agrees with the description; they do not yet say it agrees with the hardware.
That is what amidog's suite is for, and it is the next thing to run.

## gpu_test

    gpu_test

Register-level tests for the GPU's command and status handling. No BIOS, no
window: commands go straight to GP0/GP1 the way the memory-mapped registers
would, and GPUSTAT and I_STAT are read back.

**Current: 13 checks, 0 failures.**

This is a starting set, not full coverage - the rasteriser is exercised
indirectly by every `boot_runner` run and the framebuffer checksums below, so
what is here is register behaviour nothing else drives: GP0(1Fh) setting
GPUSTAT.24 and raising I_STAT's GPU line, GP1(02h) acknowledging it and
allowing a fresh edge, a repeated request while unacknowledged raising no
second I_STAT edge, and GP1(00h) reset clearing both. It also pins down, as a
fact about the current code rather than an assumption a future change
discovers the hard way, that the three GPUSTAT readiness bits report ready
unconditionally - there is no GP0 FIFO or drawing-time model yet. See bug 40
in [Bugs-Found.md](Bugs-Found.md) and "no drawing time" in [Gaps.md](Gaps.md).

## media_test

    media_test [work-directory]

Protocol-level tests for the disc layer and the CD-ROM controller. No BIOS, no
window, no disc of its own - it writes the images it needs into the work
directory and deletes them afterwards. Exit code 0 if everything passed.

**Current: 103 checks, 0 failures.**

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
- **The controller with an empty tray**: Getstat answers, GetID reports "no
  disc" as an INT5 rather than silence, and an unknown command still answers
- **The controller with a disc**: GetID reports a licensed region, GetTN
  reports the track count, and Setloc + ReadN delivers the sector that was
  actually asked for
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

**Current: 107 checks, 0 failures.**

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
| `--exe <file>` | Side-load a PS-EXE. Alone, immediately - before the BIOS has run at all. With `--auto-boot`, deferred to pc=80030000 instead: the BIOS clearing BEV and Isolate Cache and setting up a video mode first, which a standalone test program can assume the same way it could on real hardware |
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

## Baselines

Check these after any change to the CPU, timing, or the renderer - not just the
part being worked on.

### BIOS boot, SCPH1001

    boot_runner bios/SCPH1001.BIN --frames 400 --quiet

| Measure | Value |
|---|---|
| instructions | 185,794,454 |
| resolution | 640x478 |
| framebuffer checksum | `d357591479cbd199` |
| non-black (visible) | 305,920 of 305,920 |
| unimplemented paths | 0 |
| GTE commands | 0 - the shell menu is entirely 2D |
| RFEs executed | 1,023 |
| interrupts taken | 1,009 |
| final I_STAT / I_MASK / SR | `00000001` / `0000004D` / `40000401` |
| GP0 words / GP1 words | 38,320 / 2,694 |
| primitives / pixels | 2,234 / 106,401,520 |
| texels 4-bit / 15-bit | 3,945,208 / 2,139,264 |
| CD-ROM commands | 3 |

**These are baselines, not targets.** The BIOS boots and draws its intro: the
blue radial gradient fills the frame correctly. The logo geometry on top of it
is wrong, because the GTE is unimplemented - the 56 unimplemented paths are
`COP2` being reached and trapping.

The 12 GPU primitives-per-frame and the pixel count are the numbers most
sensitive to a renderer change; the checksum is sensitive to everything.

**Earlier baselines, kept so the progression is not lost:**

| When | Result |
|---|---|
| Before the interrupt fix (bug 7) | 640x478, `7f931a8558291383`, 890 GP0 words, 1 interrupt - got there by accident, on a path where interrupts were dead |
| After bug 7, before the DMA fix (bug 12) | 256x240, `aedac3154f8a0383`, 3 GP0 words, 2 interrupts, black screen |
| After bugs 12 and 13 | 640x478, `f0afceabcd797b57`, 18,224 GP0 words, 1,923 primitives - boots and draws, but no intro text |
| After bug 14 (DMA block mode) | 640x478, `e9ea0b3d07bd3b89`, 16 unimplemented paths - the full intro renders |
| After the GTE (Phase 2) | the table above; the 16 unimplemented paths were COP2 register moves, now handled |

### Register access, same run

| Register | Meaning | Expected |
|---|---|---|
| `1F801000-1020` | memory control | 1 write each |
| `1F801040/44/4A` | controller port | polled, ~728 reads |
| `1F801070` | I_STAT | 39,509 reads, 2,429 acknowledges |
| `1F801074` | I_MASK | 25,196 reads, 13 writes |
| `1F801100-1128` | root counters | written |
| `1F801800-1803` | CD-ROM | 3 commands issued |
| `1F801810/14` | GP0 / GPUSTAT | 18,224 / 2,694 writes |
| `1F801D80-DFE` | SPU register file | written |
| `1F802041` | POST (boot progress) | 15 writes |

Every device is now reached. A device dropping off this list is a regression
even when the checksum has not moved.

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

- **amidog's GTE suite has now run** (`test/psxtest_gte/` - see bug 41 for how
  to reach it, `--auto-boot --exe` or the Win32 front end's Boot PSX-EXE menu
  command). Value and flags agree with hardware outright; timing does not -
  see bug 42, still open, and "GTE" in Gaps.md.
- **amidog's CPU suite** on top of `cpu_test`, which covers the instruction set
  but not its timing. `test/psxtest_cpu/` is present and runs to a results
  screen unattended - see bug 41 - and nobody has read it yet.
- **Memory card round trips** in `media_test`, once cards exist. Per the
  standards document, the *wipe* is the point: write, wipe, read back, or a
  `serialize()` that stores nothing still appears to work.
