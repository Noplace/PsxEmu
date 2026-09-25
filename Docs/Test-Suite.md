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

**Current: 297 checks, 0 failures.**

| Group | Covers |
|---|---|
| `arithmetic` | add/sub wraparound, sign vs zero extension of immediates, the logical ops, signed vs unsigned compares |
| `shifts` | arithmetic vs logical right shifts, variable shifts masking the amount to five bits |
| `muldiv` | signed and unsigned multiply, HI/LO, division by zero, and the most negative value divided by -1 |
| `muldelay` | mult/multu's 6/9/13-cycle cost by rs magnitude, div/divu's fixed 36, and reading hi/lo before the operation finishes waiting out the busy window instead of skipping it (bug 43, the same hazard shape as `gtedelay`'s GTE check) |
| `branches` | every conditional, taken and not, at negative/zero/positive and at the extremes; that the delay slot runs either way; that a branch never writes its own operand; that a taken branch and its delay slot cost 2 cycles together and a not-taken branch costs 1 cycle on its own (bugs 43 and 78) |
| `jumps` | j/jal/jr/jalr, where the link register points, and that the linking branches write it even when not taken |
| `loadstore` | sign vs zero extension on byte and halfword loads, and that partial stores leave their neighbours alone |
| `unaligned` | lwl/lwr/swl/swr at all four alignments, and the pairs used together to move an unaligned word |
| `loaddelay` | a load's value landing one instruction late, a write in the delay slot beating it, a second load to the same register discarding the first, and the pairing surviving a branch delay slot |
| `gtedelay` | MFC2 having the same one-instruction load delay as an ordinary load (bug 42, the same shapes as `loaddelay` aimed at MFC2), and that a GTE register read right after a command waits out its busy time rather than skipping it |
| `sqrloop` | psxtest_gte's own SQR loop (SQR, CFC2, nop, accumulate, bgtz and its delay slot), measured in this core's cycles: exactly 11 a pass, the figure the test's own check code expects - the GTE hold's restart cycle and a taken branch's own cycle both show up in it (bug 78) |
| `memory` | RAM through KUSEG/KSEG0/KSEG1, RAM mirroring, the scratchpad, hardware registers through all three windows, the BIOS being read-only, and $zero staying zero |
| `exceptions` | syscall and break vectoring, the Cop0 status stack pushing and popping, mfc0/mtc0 |
| `interrupts` | I_STAT acknowledge semantics, the three gates that can block an interrupt, that EPC points at the instruction that has *not* run, and Cop0 Cause's interrupt-pending field read through MFC0 (bug 84): only the line that is actually pending rather than every line SR is listening to, following the lines live instead of reporting the last exception, MTC0 reaching bits 8-9 and nothing else, and a software interrupt being delivered like any other |
| `cacheisolation` | a store with Isolate Cache set not reaching the scratchpad, and an ordinary store still landing |
| `biosconsole` | BIOS putchar/puts calls through the A0h/B0h/C0h vectors reaching the console feed exactly once each, interpreted and then recompiled (bug 66) |
| `cpuedges` | what amidog's psxtest_cpu found (bug 68): sub/addi trapping on signed overflow without writing their destination, sltiu's sign-extended immediate, a misaligned lh/lw faulting without loading, all 32 REGIMM encodings branching (and linking only for 10h/11h, after reading rs), jalr with rd == rs, and a misaligned jump target faulting at the target |

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

**Current: 106 checks, 0 failures.**

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
| `opcode` | what amidog's OPCODE group caught (bug 79): a translated product checking the 44-bit accumulator after *each* partial sum, in MVMVA, RTPS and the lighting chain's background step, and RTPS taking IR0 from the full depth-cue sum rather than from wrapped MAC0 |
| `unknown` | an unrecognised command being counted rather than silently ignored |

Expected values are derived from the hardware description, not from this
implementation, so a failure means the code is wrong rather than that it
changed. NCLIP's area, OP's cross product and AVSZ's weighted sum are each
computed by hand in the test.

These tests say the implementation agrees with the description. That it agrees
with the hardware is amidog's `psxtest_gte` (`test/psxtest_gte/`): its REG,
COMPLEX, TIMING and OPCODE groups pass for all 22 commands - TIMING since bug
78, OPCODE since bug 79, which is what the `opcode` group above holds. The
BIOS shell issues zero GTE
commands, so the BIOS baseline below says nothing about the GTE; the game table
does.

## gpu_test

    gpu_test

Register-level tests for the GPU's command and status handling. No BIOS, no
window: commands go straight to GP0/GP1 the way the memory-mapped registers
would, and GPUSTAT and I_STAT are read back.

**Current: 67 checks, 0 failures.**

This is a starting set, not full coverage - the rasteriser is exercised
indirectly by every `boot_runner` run and the framebuffer checksums below, so
what is here is register behaviour nothing else drives, plus one rasteriser
check that earned its place by catching a real bug: GP0(1Fh) setting
GPUSTAT.24 and raising I_STAT's GPU line, GP1(02h) acknowledging it and
allowing a fresh edge, a repeated request while unacknowledged raising no
second I_STAT edge, GP1(00h) reset clearing both, and a polyline's
terminator word not being drawn as a bogus final vertex (bug 45 - it was);
and what GP0(E6h) writes into a pixel's bit 15 while drawing - a textured
draw hands the texel's own bit 15 through, an untextured one writes zero, and
a later draw with mask-checking on is refused exactly where that bit is set
(bug 83, which is what Silent Hill's pale box around the player was).
It also covers what a primitive costs in GPU time (bug 85): a flat untextured
triangle is its 46 ticks of setup plus one per pixel of area, a
semi-transparent one pays half as much again per pixel, GPUSTAT bit 28 and the
DMA request line drop while that time is owed, and both come back once the
machine has run long enough to pay it. Bits 26 and 27 still report ready
unconditionally, which the same test pins down as a fact about the current
code: there is no command queue to fill. See bug 40 in
[Bugs-Found.md](Bugs-Found.md) and "No GP0 FIFO - drawing time is modelled,
the queue is not" in [Gaps.md](Gaps.md).

It also covers the display side, where the same two-registers-read-as-one
mistake was possible: the visible width is `GP1(06h)`'s window divided by
`GP1(08h)`'s dot clock, not the mode width itself. Each of the five modes
still produces its nominal width from the standard 512..3072 window, 368 mode
produces 365 from it, Metal Gear Solid's codec registers produce 318 rather
than 368 (bug 50 - the extra 50 columns were VRAM past the framebuffer), and
a window wider than the mode, or an inverted one, falls back rather than
sampling off the end.

And the fill rule (bug 105): two opaque triangles sharing a diagonal give the
same pixels whichever is drawn first, every pixel of the diagonal drawn by
exactly one of them; two semi-transparent quads side by side blend their shared
column once; and of two opaque quads side by side, the right-hand one owns the
shared column.

## media_test

    media_test [work-directory]

Protocol-level tests for the disc layer and the CD-ROM controller. No BIOS, no
window, no disc of its own - it writes the images it needs into the work
directory and deletes them afterwards. Exit code 0 if everything passed.

**Current: 350 checks, 0 failures.**

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
- **A CHD** (bug 104), written by `tools/chd_writer.h` since there is no
  chdman here: the same track table and every sector the same as the cue sheet
  it was made from, with each codec forced and with the best per hunk -
  through ECC regeneration and repeated-hunk references; a stored pregap read
  from where the CHD keeps it and an unstored one read as silence; a zstd CHD
  refused with a reason that names zstd, one with no track list refused, and a
  truncated file or one that is not a CHD failing cleanly. libchdr prints `NO
  DSTREAM CREATED!` during the zstd check; that is expected
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

**Current: 144 checks, 0 failures.**

| Group | Covers |
|---|---|
| `registers` | the voice register file, per-voice addressing, the read-only ones |
| `keyonoff` | key-on and key-off being edge-triggered, ENDX set and cleared |
| `adpcm` | block decode, the shift and filter, the end and repeat flags |
| `loopaddr` | where a voice loops back to: a repeat address written before key-on surviving it, the loop-start flag setting it when software has not, software outranking the flag until the next key-on |
| `envelope` | the attack ramping rather than starting at full, the level being readable, silence without a key-on |
| `mixer` | per-voice and main volume, left and right kept separate, and the mute bit leaving CD audio alone (bug 103) |
| `sweep` | volume sweeps (bug 101): a fixed level taking effect at once, linear increase and decrease by the rate's step and stopping at the top and at zero, a slow rate stepping every other sample and a write starting it afresh, an exponential decrease shrinking with the level to zero, an exponential increase slowing above 6000h, the phase bit turning an increase toward -8000h and a decrease up to zero but leaving an exponential decrease alone, rate 7Fh never moving, each voice's current volume reading back, and a voice mixed at its sweep's level |
| `reverb` | the reverb (bug 102), through a network plain enough to follow by hand: silence in and out, a steady tone coming back at the level it went in, the reverb volume at zero giving exactly the dry sound, one halfword step every two samples, a small work area at the top of RAM wrapping without touching the RAM below it, and the master enable off writing nothing but still playing what the work area holds |
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

## mc_test

    mc_test
    mc_test <card.mcr>

The memory card: the on-card format (`psx/mc_directory.h`), the editor's
operations, and the card file underneath (`psx/mc.h`). No BIOS, no window. With
a card's path it lists that card instead - saves, blocks, titles, icons - and
never writes it.

**Current: 97 checks, 0 failures.**

| Group | Covers |
|---|---|
| `format` | a formatted card's header, directory and broken-sector frames, their checksums worked by hand ('M' ^ 'C' = 0Eh, A0h ^ FFh ^ FFh = A0h), the write-test frame, fifteen free blocks |
| `import and list` | a three-block save written into free blocks as 51h/52h/53h with its links and checksums, its Shift-JIS title narrowed to ASCII and its icon's palette decoded; a duplicate name, a save too big for the space, and a malformed file each refused without changing the card |
| `export` | a save coming back out as a directory frame and its own blocks |
| `delete and undelete` | A1h/A2h/A3h with the links kept; undelete giving back the card byte for byte; undelete refused once a block has been reused |
| `export everything, format, import it back` | the same saves, names, sizes, titles and bytes after a round trip through a formatted card, with a deleted save's hole in the middle |
| `the card file` | a new card written formatted; a game's write staying in memory until a second passes with none, then flushed atomically; eject saving an unflushed write; the wipe - out of the slot and back in from disk - keeping both; inserting over a card saving the old one; a file that is not 128 KB refused without disturbing the card that is in |
| `other tools' cards and saves` | a DexDrive `.gme`, a VGS `.mem` and a `.psx` each unwrapping to the same card as the raw one, a short `.gme` padded, and a file of no known kind, a too-short one and an unformatted card refused; a card's saves imported past a duplicate, which the report names, and onto a full card refused with the card unchanged; a raw headerless save named after its file and cut to 20 characters, a `.mcs` passed through, and blocks that do not open with "SC" refused (bug 106) |

Mutation-tested when written: a flush that wrote nothing failed six of these,
and a delete that cleared the links (as DuckStation's does) failed the
byte-for-byte undelete. `mc_test <card>` was also run on copies of real saved
cards - Wild Arms, Wild Arms 2, Vandal Hearts, NASCAR Thunder 2004 - and listed
each save with its title, block count and one-to-three-frame icon.

## sio_test

    sio_test

The two serial ports, driven through their registers rather than through any
interface written for the test: SIO0 (`psx/sio.h`), which is the controller and
memory card port, and SIO1 (`psx/sio1.h`), the serial socket on the back. No
BIOS, no window.

**Current: 203 checks, 0 failures.**

| Group | Covers |
|---|---|
| the pad | a digital pad's four-byte poll and its buttons; an empty slot never acknowledging; the acknowledge line as a pulse that releases itself (bug 46) |
| the DualShock handshake | 0x43/0x44/0x45 gated on configuration mode, entering analog and locking it, the status query, the axes, both rumble mappings, a reconnect forgetting the negotiation, and each controller type refusing what it does not have; every byte of the 46h/47h/4Ch capability replies for both queries (bug 96); the ANALOG button flipping the mode, refused by a digital pad and by a locked mode, and held until an exchange in progress is over (bug 97) |
| the mouse | its id, switches and axes, and movement draining across as many polls as it takes |
| the multitap | both addressing methods, per-player state, the escalation rules, and a round trip through a save state; each player a different type, and an empty socket giving no /ACK (bug 98); 81h-84h reaching four different cards, an empty card socket silent, and 82h reaching nothing without a multitap (bug 99) |
| the GunCon | its eight-byte reply, each button bit, X and Y at the middle and corner of a 320x240 picture and the middle of an interlaced one, the off-screen reply, refusing 43h, an unplugged gun silent, and a save state keeping the port a GunCon (bug 100) |
| `sio1` | the serial port with nothing plugged into it: the reset state, an empty receive FIFO reading as the idle line, the registers keeping what is written, the status being read-only, the two strobes not sticking, byte/halfword/word access reaching the right halves, transmitting only when enabled and raising IRQ8 when armed, acknowledging clearing the latch, the baud-rate timer counting down and reloading, the console redirect on and off, and a save-state round trip |

## debug_test

    debug_test

The debugger's core (`psx/debugger.h`, Docs/Debugger-Plan.md phases 0 and 1),
on small hand-assembled programs in RAM - no BIOS, no disc, no window. The window
itself is checked the way the other front-end windows are: driven from outside
the process (bug 72).

**Current: 174 checks, 0 failures.**

| Group | Covers |
|---|---|
| `breakpoints` | halting *before* the instruction at the address runs, taking no cycles; a step while halted doing nothing and not counting the hit again; resume running that instruction rather than halting on it forever; a disabled breakpoint not firing; KSEG0/KSEG1/KUSEG aliases of one address matching; a loop hitting three times |
| `step into` | one instruction a step; a taken branch landing on its target with its delay slot already run |
| `step over` | a `jal` stepped over landing after its delay slot with the function run |
| `step out` | out of a function to its caller, including a function that makes its own call and keeps `ra` in `s0` |
| `run to, break, and the exception vector` | run to an address; a break request; a breakpoint on 80000080h caught by a `syscall`, with EPC pointing at it |
| `the recompiler` | a breakpoint halting at exactly the same instruction with the recompiler on (it is bypassed while the debugger is armed), and compiled code coming back once a plain continue disarms it |
| `the snapshot the window is shown` | registers, pc, halt reason and breakpoint hits; the load delay at each stage - right after a `lw` its value in flight and the register still old, one instruction on landing, then in the register; the disassembly window centred where asked, a `beq zero, zero` shown as `b` with its target, only its delay slot marked; no wrap below address 0, a KSEG1 centre listing KSEG1 addresses, a hardware register not read at all; a state load ending a halt and keeping the breakpoints |
| `memory: reading without side effects, and writing` | RAM bytes across a word boundary and through KUSEG; I_STAT, a timer's mode (without clearing the reached flag a real read clears), GPUSTAT, an SPU register and the cache control register peeked as state; the CD-ROM, GPUREAD, SIO and MDEC, and an address nothing answers at, not read; writes to RAM (only the bytes asked for) and the scratchpad; the BIOS and a hardware register refused whole, with the reason; a write off the end of RAM wrapping into its mirror; the snapshot carrying the memory view |
| `patched code runs as patched` | a loop halted, its add patched, and let go: the new instruction runs, interpreted and with the recompiler - where the block compiled before the patch has to be dropped. Mutation-tested: without the write's `NoteBulkWrite`, the recompiler case fails |
| `editing registers` | a register set while a load to it is in flight keeps the new value (the load is dropped); zero stays zero; hi and lo; a misaligned pc refused; a new pc moving the halt with it, and resuming from there without running what it skipped |
| `watchpoints`, and again with the recompiler on | a store halting after it has happened, on the next instruction, naming the CPU, the store's pc, address, size and value; resuming; a read watchpoint catching an `lw` and an `lb` with the value read, not a store beside it; the bytes either side of a range not tripping it, its last byte through KSEG1 watched through a RAM mirror doing so; `swl` not tripping a read watchpoint (its merge read is the emulator's, not the program's) but tripping a write one; a store in a delay slot reported at the slot and halted at the branch target; an isolated-cache store tripping nothing; DMA channel 6, started by a CPU store, tripping it with the channel and the link it wrote; fifty stores in a loop, fifty halts, and the loop still counting to fifty; a disabled watchpoint disarming. Mutation-tested three ways: without the `swl` guard, without the DMA hook, and with the store check ahead of the isolated-cache return, each fails its checks |
| `the BIOS call log, and breaking on a call` | two calls through B0h logged whether or not anything is armed, with the function, a0 and the return address; `BiosCallName` naming B0h:3Dh and C0h:1Ch without the CSV quotes, and a number with no name; a break on B0h:3Fh passing B0h:3Dh and halting at the vector for 3Fh before that call is logged, which logs once on resuming |
| `the call stack` | main calls A, A calls B (keeping `ra` in `s0`): two frames inside B with the right call sites and targets, both gone back in main; tracking arming the debugger and turning it off disarming. Mutation-tested: returns that never pop fail the second check |
| `labels` | a label on its line and against a `jal` to it; found through KSEG1; an empty name removing it |
| `the device panes` | every section described (DPCR and seven DMA channels, three timers, 24 SPU voices, the GPU, the CD-ROM, the interrupts), a register just written showing in its row, and describing the timers not clearing the flag a mode read would |
| `the disassembler` | `psx/disasm.h`: a `jal` target, REGIMM aliases named by what they do (only rt 10h/11h link), a GTE command and a Cop0 register by name, a GTE control register numbered 32-63 |

Mutation-tested: without the guard that keeps a halted machine halted, a
second `StepInstruction` counted the same breakpoint twice, and the
"does not count the breakpoint again" check failed (2, wanting 1).

## timing_test

    timing_test [bios]

Bus timing against a real console, for [CPU-Timing-Plan.md](CPU-Timing-Plan.md)
phase 3. It boots the BIOS, side-loads JaCzekanski's `cpu/access-time`
(`test/test suite/cpu/access-time/`) and sets each result beside the `psx.log`
the suite ships: the same table, from a real console. It takes a fifth of a
second.

**What the test measures:** for each region and each width it times 100
loads, subtracts 100 nops, and prints what is left per load. That is one load
instruction's cost beyond a nop. It prints `cycles / 100` and `cycles % 100`
either side of a dot, with no leading zero on the remainder, so "5.3" is 5.03
and "12.94" is 12.94. Read as a decimal, a one-digit remainder comes out ten
times too big. The harness's first version did exactly that (see bug 76).

Two questions, kept apart:

- **How close is it?** A cell within a quarter of a cycle of the console counts
  as a match. That is the console's own scatter: its on-die registers share one
  decoder and one cost and read 2.92 to 3.18, and RAM reads 5.03 to 5.21 as its
  refresh lands in some loops and not others. The count is printed, not
  asserted. Raising it is phase 3's work.
- **Has it changed?** The emulator is deterministic, so every row is also
  checked, exactly, against the baseline recorded in the source. Any change to
  bus timing fails it until the baseline is updated alongside. That is
  deliberate: the change should be one somebody meant.

**Current: 19 checks, 0 failures; 42 of 51 cells match the console.**

`timing_test --icache-timing` runs the same test with the instruction-cache model
on (bug 94). The baseline checks are for the default machine and fail with it,
as they should; the console comparison is the point - still 42 of 51, and a
total error of 62.60 cycles against 62.45 without the model.
Cycles per load at 8 / 16 / 32 bits:

| Region | Console | This emulator | Before bugs 76-77 |
|---|---|---|---|
| RAM | 5.21 / 5.03 / 5.14 | 5.01 - matches | 5.01 |
| Scratchpad | 1.05 / 1.01 / 0.94 | 0.99 - matches | 1.99 |
| On-die: DMA, pads, SIO, RAM_SIZE, I_STAT, timers, GPUSTAT, MDEC | 2.92 - 3.18 | 3.00 - matches | 5.00 |
| Cache control | 0.95 / 1.09 / 1.09 | 1.01 - matches | 7.01 |
| BIOS | 7.06 / 12.94 / 24.94 | 7.01 / 13.01 / 25.01 - matches | 7.01 flat |
| Expansion 1 | 6.94 / 13.07 / 25.07 | 7.01 / 13.01 / 25.01 - matches | 7.01 flat |
| Expansion 3 | 6.07 / 6.01 / 9.95 | 6.01 / 6.01 / 10.01 - matches | 7.01 flat |
| CD-ROM | 8.00 / 14.00 / 25.93 | 7.00 / 13.00 / 25.00 - one under | 5.00 flat |
| Expansion 2 | 10.99 / 25.99 / 55.98 | 15.00 / 29.00 / 57.00 | 5.00 flat |
| SPU | 17.99 / 17.99 / 38.94 | 21.00 / 21.00 / 82.00 | 5.00 / 5.00 / 10.00 |

The six slow regions' costs come from the memory-control registers the BIOS
programs, by psx-spx's formula (bug 77). What is left, and why it is left:

- **The formula's own error.** It is exact for the three regions that use no
  recovery or pre-strobe period. For the three that do, it is off by 1 to 4
  cycles, and this core keeps it rather than fitting it. A simpler rule does
  fit all eighteen cells: first access = read delay + 4, then read delay + 2 +
  COM0 + COM2 for each further one. But it comes from one register setting per
  region and these same measurements, and nothing independent could check it.
- **The SPU's 32-bit cell** is not a 32-bit load. 1F801DAA is not
  word-aligned, so the test's read compiles to an `lwl`/`lwr` pair. Each is
  charged a whole word of the 16-bit bus here: 2 x 41. The console takes 39
  for the pair, which looks like one halfword access each, as if a partial
  load reads only the half it needs. It is one data point, so it isn't
  modelled.

The harness was mutation-tested when written: a one-cycle scratchpad stall
added to `Cpu::Load` moved that row up by exactly one cycle, and the baseline
check failed. So a stall change shows one-for-one in the table.

What it does not measure:
- **Stores.** It times only loads.
- **Load overlap.** It doesn't measure how a slow load overlaps the
  instructions after it.
- **Which console.** Its log doesn't say which model it came from, and the ROM
  row depends on how that console's BIOS programmed the bus.

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
| `--break <hex>[,<hex>]` | Execute breakpoints (repeatable). At each hit, before the instruction runs: hit counts, the 32 registers, hi/lo/SR/Cause/EPC and a disassembly around the pc - then it carries on. The run's numbers are those of a run without them (Docs/Debugger-Plan.md) |
| `--watchpoint <hex>[:<len>][:r\|w\|rw]` | A watchpoint (repeatable): stop after a read or write of the range - by the CPU, or a DMA channel's write - print it as a break does, with who made the access and the value, then carry on. Four bytes and writes unless said otherwise. The summary gives each watchpoint's access count |
| `--track-calls` | Keep the debugger's approximate call stack for the whole run (which runs it through the checked step) and print the innermost frames at the end. Every run also prints `bios calls`, the number of A0h/B0h/C0h calls the debugger's log saw |
| `--break-print <n>` | Print only the first n halts, from breakpoints and watchpoints together (default 20); the rest are counted |
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

Thirty-three checks on `PSXEmu.Core/host/` - the channels the front end's threads
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
  instruction count and checksum** - 93,049,815 and `435bad9a6c5e4004` - which
  is the assertion that threading changed nothing about what the machine
  computes. Then again with pause and resume requests thrown at it from another
  thread as fast as it will take them (432 of them, same numbers), and again
  through a state saved at frame 200 and loaded into a fresh machine by request.
  And again with the debugger in it: a breakpoint on the B0 vector from frame
  100, the machine halting mid-frame 60 times (31 at the breakpoint, the rest
  single steps), each halt let go by a request from another thread - same
  numbers.
- **Stopping.** Forty machines stopped mid-frame, mid-pace and paused; the
  slowest came back in 17 ms. A paused machine answers a request in under a
  millisecond, because it waits on its doorbell rather than polling.
- **All three threads together**, at real speed, against a device that plays at
  exactly 44,100 frames a second: five seconds with nothing short, nothing
  dropped, no underrun and every frame presented - pausing twice in the middle.

The BIOS is `bios/SCPH1001.BIN` unless one is named, and the thread checks are
skipped, loudly, without it.

**The real-speed checks need a host with room to spare.** On 2026-09-25 two of
them - the limiter holding 59.29 fps, and five seconds of sound with nothing
short - failed now and then on this machine: once in three runs of a build from
before the change being tested, and once in three after it, alternating, with
the unthrottled boot time the same either way. That is a busy host, not a
regression. When they fail, run the previous build the same way before looking
for a cause in the change.

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
| `cpu_test` | 297 | | `gpu_test` | 67 |
| `gte_test` | 106 | | `mdec_test` | 85 |
| `timer_test` | 70 | | `media_test` | 350 |
| `sio_test` | 203 | | `spu_test` | 144 |
| `mc_test` | 97 | | `debug_test` | 174 |

**1,593 checks, 0 failures**, all ten green. Each harness's own section above
says what its groups cover. (`media_test` gained two when the front end's
`pause_in_menus` and `show_timings` settings arrived, and four more with the
multitap players' types and the GunCon: every setting in `EmuConfig`
round-trips through the file, and those are settings.)

Smaller harnesses cover the host-side headers the front end leans on and
are not counted above, since they test no emulation: `letterbox_test` (12
checks, aspect ratio), `frame_limiter_test` (8 checks - the average rate, and since bug 62 the
spacing between frames too), `speed_resampler_test` (13 checks, the audio
arithmetic behind 50-300% speed - the frame counts, that a minute at 150% does
not drift, and that blocks join continuously), `mouse_scaling_test` (37
checks, the three ways a host mouse's movement becomes a PSX mouse's counts -
the linear scale against a host resolution, the fraction that has to survive
between polls or slow movement vanishes, and the reading of Windows' own
acceleration curve out of the registry bytes this machine holds; bug 82),
`timing_test` (19 checks,
bus timing against a real console - its own section above, and not a
correctness count: it records how far off the timing is) and `host_test` (33 checks, the
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
| instructions | 93,049,815 |
| resolution | 640x478 |
| framebuffer checksum | `435bad9a6c5e4004` |
| non-black (visible) | 305,920 of 305,920 |
| unimplemented paths | 0 |
| GTE commands | 0 - the shell menu is entirely 2D |
| RFEs executed | 920 |
| interrupts taken | 908 (vblank 335, dma 508, cdrom 3, timer2 62) |
| final I_STAT / I_MASK / SR | `00000001` / `0000000D` / `40000401` |
| GP0 words / GP1 words | 16,913 / 2,320 |
| primitives / pixels | 1,153 / 84,637,573 |
| texels 4-bit / 15-bit | 3,159,000 / 0 |
| CD-ROM commands | 3 |
| SPU | 297,483 frames, 64 key-ons, peak 27,547/23,860 |

**The checksum moved to `435bad9a6c5e4004`** with bug 105, the fill rule applied to
opaque triangles too, and nothing else did: the same instructions, the same
305,920 non-black pixels. What changed is 349 pixels on the right-hand edges of
the shell's orange diamond, which lose the one-pixel fringe - and the single
pixel at its apex - that an edge pixel drawn by both sides gave it.

**And then to 93,049,815** (bug 88), with the checksum, every pixel, all 16,913
GP0 words, all 1,153 primitives, the 908 interrupts and every register
unchanged. The shell runs 480-line interlaced, so its per-pixel drawing cost
halves - 89,653,406 GPU clocks to 45,132,122, 25% of a frame's GPU time down to
13% - and it spends 1.1% fewer instructions waiting for it.

The SPU peak in the table above is re-recorded at the same time: it had been
28,461/23,222, which no build in this batch produces, so it went stale at some
earlier point in the same way the Ridge Racer disc row had.

**And then to 94,111,024** (bug 87), checksum and every pixel still unchanged,
along with 42 fewer GP0 words and four fewer primitives. Drawing is charged on
geometry clamped to the drawing area now rather than on the whole primitive, so
the rasteriser keeps up better and frame 400 lands a fraction further into the
shell's list. The interrupt counts and every register above are identical.

**Before that, 94,118,232** (bug 86)
, checksum and every pixel unchanged, with
one more interrupt taken (908, the extra one a DMA completion) for the same
reason the count moved at all: GP0 words now queue behind the rasteriser and
DMA channel 2 waits for room, so the shell's drawing is spread differently
across the frame. Together with bug 85 that is 2.2% more instructions in the
same 400 frames, all of it the BIOS waiting on a GPU that is no longer
instantaneous.

**Before that, 92,367,970** (bug 85), with the checksum, every pixel and the
interrupt counts unchanged. Drawing took GPU time for the first time, and the
BIOS shell waited for it: the extra 285,318 instructions were it spinning on
GPUSTAT bit 28 between primitives.

**Before that, 92,082,652** (bug 78), checksum, console text, RFEs and
interrupts unchanged. Every taken branch and jump now costs its own cycle, so
the same 400 frames hold 6.5% fewer instructions. The disc table moved in ten
games; every changed frame was checked by eye (bug 78).

**Before that, 98,442,368** (bug 77), checksum and console text unchanged
again. The BIOS ROM, the CD-ROM and the SPU now cost what the memory-control
registers set: a word from the 8-bit ROM is 25 cycles where it was 7. This
time the disc table moved in six games; every changed frame was checked by
eye against the old one, and each is the same scene a few frames apart (bug
77 has them).

**It moved again later that day, to 98,464,330** (bug 76), with the checksum
and the console text unchanged. Loads from the on-die registers now cost their
measured 3 cycles rather than 5, so the BIOS's waits on GPUSTAT and I_STAT spin
more times in the same stretch of time. The twelve-disc table moved at one
point only: Ace Combat 3's frame 3000, the same scene of its intro film four
sectors further on.

**The instruction count moved on 2026-09-19**, from 97,749,265 to 97,747,598,
with the checksum and every other number here unchanged. That is bug 67: an
ordering-table clear on DMA channel 6 now finishes before the CPU runs again,
so the BIOS's own wait for it ends 1,667 instructions sooner. The twelve-disc
table below did not move at all.

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
| Air Combat | `a1e228e8a2ee662c` | `51080ad999e88621` | `5109d78c91007c12` | 51,200 | 320x240 | 241,800 | 5,262 |
| Wild Arms | `a94d9bb38769a360` | `e53c89cb43c0075b` | `b828d822ec27badf` | 61,440 | 320x240 | 92,363 | 3,715 |
| Wild Arms 2 (cd1) | `55565300d8dc9411` | `81007d90c767846a` | `1742c42883771622` | 76,800 | 320x240 | 0 | 100 |
| Vandal Hearts | `eac4dfab83da3880` | `7c1297df773e7342` | `fc66e49c14bf8861` | 76,725 | 320x240 | 127,500 | 5,230 |
| Legend of Mana | `e13bb6ec78144cc9` | `9797912c492383e1` | `b16eaf3906c9d6dd` | 76,312 | 320x240 | 154,500 | 5,313 |
| Ridge Racer | `a727da8b232bddfd` | `615989b7725635c2` | `fc77d928fb3c4159` | 76,461 | 320x240 | 0 | 1,578 |
| Bomberman Party Ed. | `4a31d7a6c52734a4` | `45e058b70ed827c2` | `3ba049eea7e64970` | 68,913 | 320x240 | 145,800 | 4,652 |
| Area 51 | `d7e8093204d0085b` | `5b1c23ab7d41b7d0` | `c20fec6d8f189e8d` | 51,855 | 256x240 | 100,080 | 5,490 |
| Final Fantasy VII | `37991653287d63d1` | `bbbb18dffe854383` | `fb1d8340ba2617e0` | 75,943 | 320x240 | 0 | 668 |
| Final Fantasy VIII | `aedac3154f8a0383` | `f3ee4d06bf3e0383` | `c184351a7e528d32` | 4,002 | 640x480 | 0 | 1,187 |
| Ace Combat 3 | `2d039a3114a00858` | `8b98ad87bd86ef11` | `b7d1c35c356ae822` | 54,862 | 320x240 | 80,864 | 2,255 |
| Captain Tsubasa J | `f0779890ee9b1bb0` | `add4d55f3196ad03` | `816d516f2ba1d3f8` | 76,800 | 320x240 | 59,100 | 3,759 |

Re-recorded after bug 105 (the fill rule on opaque triangles too): three
checkpoints moved, and no instruction count - it is a change of pixels only.
Vandal Hearts at frame 1000, the licence screen, and Ridge Racer at frames 2000
and 3000, each by 148 pixels. Checked at 5x: the PlayStation logo's "P" loses a
stray pixel above its top-left corner and a few stair pixels on its slanted top,
and a mountain peak behind Ridge Racer's attract mode loses a one-pixel spike on
its apex - both edge pixels a triangle drew that the rule gives to nothing.
Ridge Racer's frame 3000 has two fewer non-black pixels for it.

**Run with `--recompiler`, 2026-09-25: identical at all 36 checkpoints.** Every
disc gives the same checksum, non-black count and resolution at frames 1000,
2000 and 3000 compiled as interpreted, with no recompiler faults. That is the
first run of this table against the recompiler. The only difference is in the
CD sector counter: Area 51, Bomberman and Captain Tsubasa J read one sector
more or fewer by one checkpoint, which is the compiled code's coarser cycle
accounting showing up as pacing. The compiled runs executed 14-19% more
instructions over the same 3,000 frames - compiled code charges fewer cycles
per instruction - and with every picture the same, the extra presumably went
into the games' waits for the next frame.

Re-checked after bugs 101-103 (the SPU's sweeps, reverb and mute bit): all
twelve identical to the instruction, sectors included. Those changes reach only
what is heard.

Ace Combat 3's frame 2000 re-recorded after bug 96 (the pad's capability
replies): libpad now completes its DualShock setup, which shifts the title
screen's pulsing marker to another point in its pulse. Ace Combat 3, Bomberman,
FF8 and Legend of Mana run a different number of instructions for the same
reason, with no other checkpoint moving.

Re-recorded after bug 89 (the rasteriser skips the field it is displaying),
which moved four frame-1000 checksums and nothing else: Wild Arms, Vandal
Hearts and Ace Combat 3 at frame 1000, Ace Combat 3 at frame 2000 as well.
Every other checkpoint on every disc is byte-identical, CD sector counts
included.

All four are in the 640x480 interlaced boot phase at that point - the licence
screen and the publisher logo, not the game - which is exactly where the skip
applies and where nothing else in this table reaches. Checked by eye at 4x:
the logos are the same, a few pixels further through their fade, with clean
letterforms and no combing. Wild Arms' frame-1000 non-black count moves with
them, 20,002 to 19,933, because a fade caught a moment later has a few more
pixels below the black threshold; its frame-3000 count in the table is
unchanged.

Re-checked after bug 88 (only the active field is charged in interlaced mode):
all twelve are byte-identical at all three checkpoints, CD sector counts
included. That is the expected result rather than a weak one - only Final
Fantasy VIII is in 480i, and it draws to the display area, so hardware would
put down every line and the halving rightly does not apply. The BIOS boot above
is where that change shows.

Re-checked after bug 87 (drawing charged on clipped geometry): eleven of the
twelve are byte-identical at all three checkpoints to the build from before the
GPU timing work, macroblocks and sectors included. Wild Arms reads one CD
sector fewer at frame 2000 with an identical checksum - pacing, not content.
Ridge Racer's frame 3000 moved again, checked by eye: the same attract-mode
shot a fraction of a second later, the car further along the road, with the
same 76,463 non-black pixels. Its row is re-recorded here for a second reason -
the value it held, `0e0cec22ac4c79e3` with 75,957 non-black, matches no build
on this machine, including ones from before bug 78, so it had gone stale at some
earlier point that was not chased down. It remains the one disc in this table
whose primitives reach the drawing area's edge, which is why it is the one that
keeps moving.

Re-recorded after bug 78 (a taken branch costs its own cycle), which moved ten
of the twelve: the CPU does a few percent less per frame, so each game is at a
slightly different point at each checkpoint. Every changed frame 3000 was
checked by eye against the old one, and each is the same scene a moment apart.
Captain Tsubasa J alternates between two frames and has simply swapped them.

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
  command). **The whole suite passes**: BASIC, REG, COMPLEX, the official
  TIMING group (bug 78) and OPCODE (bug 79), with TOTAL green on X, F, V and
  T - sampled from the screen of a build carrying both. OPCODE is slow to
  reach: once it passes it takes 557,805 frames, past boot_runner's
  instruction safety net, and an interpreted run of it is three hours
  (bug 79).
- **amidog's CPU suite passes.** `test/psxtest_cpu/` (reached with
  `--auto-boot --exe`) reports no errors in any group, and its results screen
  is all OK or N/A - sampled by pixel, not by eye, TIMING column included. Bug
  68 fixed what it found; `cpu_test`'s `cpuedges` group holds each fix.
