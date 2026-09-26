# Gaps

Hardware and features still missing, ordered by how likely each is to stop a
game working. See [Roadmap.md](Roadmap.md) for the phase each belongs to and
[Bugs-Found.md](Bugs-Found.md) for what has already been fixed.

Last audited 2026-09-21, after bug 82 (the mouse's three motion modes), with
the GP0 queue entry below, the harness counts and the speed-ceiling entry
brought up to date after bugs 87 to 95 (the GPU's drawing cost, the interlaced
field, the audio resample, phase 7's rasteriser thread, and the transfer and
instruction-cache timing options), and the controllers entry after bugs 96 to
100 (capability replies, ANALOG, multitap types and cards, GunCon). Every
entry below was re-read against the code, not carried forward: each claim was
checked at the line it describes, and what follows is what that reading found.

Four entries were wrong and are corrected below - the GPU's scanline
granularity, the MDEC's control bit 30, the baseline provenance, and the
"sweep the other harnesses" question, which is now answered. Two gaps were
missing entirely and have been added: the GPU's absent drawing time (which
Test-Suite.md was already linking to) and the emulation-speed ceiling. The
rest verified as written.

---

## Can crash a game

Nothing known. No game is blocked, and the one path that ended in a crash
regardless of the game - an MDEC-in transfer that moved every word inside the
CHCR write - is bug 56.

### The MDEC's own timing is the DMA channel's, not a decode model

Channel 0 now feeds the decoder one block at a time (bug 56), waiting the
block's bus time plus 2,688 cycles for each macroblock it completed -
DuckStation's figure for six 8x8 blocks, not a measurement, and charged as a
delay before the next block rather than modelled inside the MDEC. The decoder
itself still decodes a whole macroblock the instant its last word arrives and
holds its output in a buffer of its own rather than a 768-word FIFO, so:

- **The data-in request is reported but not obeyed.** `Mdec::Status` raises
  STAT's request bit (28) when control bit 30 is set and the decoder wants
  data, so software watching it sees something sensible - but `Dma::Dma0`
  never consults it. The channel's own timer decides when to feed, and a game
  that cleared bit 30 mid-transfer would be fed anyway. (This entry used to
  say bit 30 was not modelled at all, which stopped being true.)
- **Output appears all at once** when a macroblock finishes, where hardware
  copies it out over the same 2,688 cycles.

Games that stream film see the right throughput and the right ordering. A game
that watches STAT's request bits closely rather than using DMA would not.

## Test health

### Every harness is green

cpu 297, gte 106, timer 79, sio 203, spu 144, gpu 73, mdec 85, media 378, mc 97, debug 174 - 1,636
checks, no failures, and 2,290 across all eighteen harnesses (re-run
2026-09-26, after bugs 78-112 - with the rasteriser threaded, now the default).
`host_test`'s two real-speed checks fail now and then on a busy host, before a
change as well as after it; see Test-Suite.md. The two that were failing when this document was last
audited are bugs 58 (the CD peak meter's own test played silence) and 59 (the
top-left rule's vertical test was inverted, which the half-open raster loops
turned from a wrong owner into a gap).

None of them caught bug 60, which was live in every frame of every game at the
time: the harnesses test what a unit test can reach, and "the screen is one
column narrower than it should be" is not that. The per-game table in
Test-Suite.md is - it moved Ridge Racer and nothing else.

### The fill rule applies to every triangle now - and shading is not bit-exact

The gate that kept it to semi-transparent triangles is gone (bug 105). It was
checked the way this entry asked: against Wild Arms' field, the scene the seams
were reported in, with no seam either way - and against 20 more of the saved
games' states, JaCzekanski's `gpu/triangle` reference, and the twelve discs.

That reference turned up something else. Away from the edges, this core's
Gouraud shading differs from it by one step of 5-bit colour at about 26,000 of
the triangle's pixels, in a fine regular pattern: the colour is interpolated
here per pixel from barycentric weights, where hardware steps it along each
line in fixed point. `gpu/rectangles` differs at 4,693 pixels the same way.
Nobody would see it; a checksum against another emulator would.

### Baselines - refreshed, and now a real table

First measured at `8c7c694` on 2026-09-16: the BIOS boot row, the
register-access row, and a per-game table of twelve discs at 3,000 frames with
checksums at frames 1000/2000/3000. See [Test-Suite.md](Test-Suite.md). Both
have moved since, each time deliberately and each time with the changed frames
checked by eye: the memory-access work (bugs 76-77) and then the branch cost
(bug 78), which moved ten of the twelve games and took the BIOS boot to
92,082,652 instructions.

The refresh paid for itself immediately: the BIOS boot was drawing 304,803 of
305,920 non-black pixels where the pre-September build drew all of them, which
turned out to be **bug 60** - the drawing area's last column and row discarded
by a half-open loop clipping an inclusive bound. Four days in every frame of
every game, invisible without a reference to compare against.

### The September SPU work - recorded and tested now

`1419d78` replaced the reverb and added volume sweeps without a Bugs-Found
entry or a test. Both have them now - bugs 101 and 102, and `spu_test`'s
`sweep` and `reverb` groups - and testing them is what found the faults those
entries fix. The GPU half of this entry was settled earlier: the September
12-14 raster commits are what bugs 59 and 60 are about.

### Tests written with the feature they check, and never run against it

Two of these now: `mdec_test`'s `TestOutputDmaStartedFirst`, below, and the CD
Audio Peak Meter test, which shipped with the meter in `209943e` and failed
from the first run - it played silence and asserted a zero peak was a bug in
the drive (bug 58). A test committed alongside its feature is worth running
once before it is believed.

### A written test nobody called - swept, and clean

`mdec_test`'s `TestOutputDmaStartedFirst` - the Area 51 regression, written
with commit `535949b` - was never added to `main()`, so its checks had never
run. Wiring it in (bug 56) found a real bug in the first two of them, and left
the obvious question: how many others are there?

**None.** The 2026-09-21 audit swept all 17 harnesses in
`PSXEmu.Core/tools`, comparing each `void Test...()` definition against
whether anything references it. All 214 of them are wired in - by a direct
call, or by a group table in the three that take a group name (`cpu_test`,
`gte_test` and `spu_test`, whose entries read `{ "arithmetic",
TestArithmetic }` rather than as calls, which is exactly what a naive grep for
`TestArithmetic(` misses and what made them look uncalled on the first pass).
Three harnesses - `host_test`, `timing_test` and `frame_limiter_test` - name
their sections differently (`DoorbellChecks()` and the like) and were swept the
same way against their own naming. Also clean.

### The twelve discs do not cover the mask bit

Bug 83 was a GPU rule missing outright - a texture's bit 15 never reached the
framebuffer, so mask-checking protected nothing - and every one of the twelve
discs came out byte-identical across the fix. None of them uses mask-checking
in its first 3,000 frames, so that table could not have found the bug and
cannot catch a regression in it. Silent Hill is the only disc here known to
exercise it, and it is not on the table; `gpu_test`'s new checks are what
stands in for it. The general point is worth keeping in view: a checksum table
covers what its games happen to do, and the quiet ones are the gaps.

## Silently wrong rather than absent

These do not stop anything, which is what makes them worth listing: a game
runs at the wrong speed or sounds or draws slightly wrong and nothing reports
an error.

### Voice and main volumes came out at half - fixed

Bug 57. `Spu::VolumeOf` doubled a fixed-level volume into the range the mixer
multiplies by and shifted the doubling straight back out again, so 3FFFh -
unity - mixed at half, at both the voice and the main stage. `audio_volume`
defaulted to 2.0 and cancelled one of the two, which is why it read as taste
rather than as a correction; the default is now 1.0, the hardware's own level.
An existing `psxemu.ini` still says 2.0 and will be twice as loud until it is
changed in the menu.

What is left here is the same question one level up: with the mix right, FF7
peaks at 87% of full scale rather than the 22% this document used to cite as
"a PlayStation is quiet by modern standards". That claim was the bug talking.

### Reverb and volume sweeps - match DuckStation, never heard against a console

Both used to be listed as missing. Commit `1419d78` implemented them, and
bugs 101 and 102 corrected them:

- **Sweeps:** `Spu::StepSweep` ramps a voice or main volume with bit 15 set -
  linear or exponential, up or down, at the register's rate - and `VolumeOf`
  returns the running level instead of full scale.
- **Reverb:** `Spu::ProcessReverb` now runs the documented network - input
  volumes, same- and different-side reflections with the wall and IIR
  coefficients, the comb and all-pass stages - off the 32 reverb registers at
  22,050 Hz, where it used to be a two-tap delay using two of them.

Both are now DuckStation's models, and a scratch comparison ran them against a
transcription of DuckStation's code - every sweep register from eleven starting
levels, and forty random reverb configurations of 20,000 samples - with no
difference in any output, in sound RAM or in the reverb address. Both are in
`spu_test`. What is still not done is hearing them against a console, or any
measurement DuckStation did not already make. One difference is left on
purpose: a voice's volume sweep keeps stepping while the voice is silent, where
DuckStation's stops unless the SPU interrupt is enabled. `boot_runner`'s `spu requests` and `spu modes` lines show
whether a game uses either: all twelve regression discs use the reverb, and
none uses a sweep in its first 3,000 frames.

### CD audio is resampled linearly

The voice path uses the hardware's Gaussian table (`kGauss`); the CD-audio
path, which resamples 44,100 Hz to the output rate, interpolates linearly
rather than with the hardware's seven-point filter. The code says so where it
does it. A slight softening of the top end, not a wrong pitch or a click.

### Cause's interrupt-pending bits - fixed

Bug 84. `Cpu::RaiseException` used to set `Cause` bits 8-15 from `SR`'s
interrupt mask (`cause |= (sr & 0xFF00)`, the code's own `//todo : set ip
flags correctly`) rather than from the lines actually pending. The
BIOS's handler computes `cause & sr & 0xFF00`, gets a non-zero answer, and
works either way, which is why this survived - but software reading `Cause`
to find out *which* line is pending got the mask instead.

Now: `Cpu::CauseRegister` composes what software reads. Bit 10 - the one line
the PSX wires its interrupt controller to - is `(I_STAT & I_MASK) != 0` at the
moment of the read rather than a copy taken at the last exception; bits 11-15
(IP3-IP7 on an R3000A) read zero, because nothing is attached to them on this
machine; bits 8-9 are software's own, written by `MTC0` and honoured by the
dispatch, so a software interrupt is delivered like any other. An exception
writes the code and BD and leaves the rest alone, and `MTC0` to `Cause` can
reach nothing but bits 8-9 - both the same masks DuckStation uses.

What is left is not a gap so much as the hardware: IP3-IP7 have nothing to
report because the console attaches nothing to them.

### Cycle timing - partly measured, memory regions still modelled

[CPU-Timing-Plan.md](CPU-Timing-Plan.md) tracks this. Done: multiply and
divide charge psx-spx's measured 6/9/13/36 cycles (bug 43), every GTE command
charges its documented cost with the hardware's stall when the next one comes
too soon (bug 42), and a branch costs a cycle whether or not it is taken, with
a GTE hold costing one more to restart (bug 78). amidog's `psxtest_gte` TIMING
group passes for all 22 opcodes, and timers.exe's delay loops read within 2
cycles of a real console's at every length.

Measured for loads: memory access costs (bugs 76-77). `timing_test` runs
JaCzekanski's `cpu/access-time` against the table it recorded on a real
console, and 42 of 51 cells match.
- **Fixed costs:** RAM, the scratchpad, the on-die registers and the cache
  control register.
- **From the memory-control registers:** the BIOS ROM and the expansion,
  CD-ROM and SPU buses, by psx-spx's formula and the width of the read.
- **Still off by default:**
  - the CD-ROM by one cycle, the SPU and expansion 2 by 3 or 4: that is the
    formula's own error, not fitted over;
  - a partial `lwl`/`lwr` from a narrow bus, charged a whole word where the
    console reads only what it needs.

  Emulation > Timing Accuracy > Measured Bus Timing (bug 111) fixes both, and
  all 51 cells match with it on. It is off by default because the rule is
  fitted to the same table it matches - one register setting per region - and
  nothing independent checks it.
- **Not modelled:** a slow load overlapping the instructions after it.
- **Unmeasured:** stores. Write Queue Timing (bug 111) models psx-spx's
  four-deep write queue: a store is free until the queue is full, and a load
  waits for it to empty. But each entry's drain time is a guess (a read's
  cost), nothing here can measure it, and it is interpreter-only. So it is an
  estimate, off by default.

### DMA data moves eagerly; only the completion is paced

A transfer bills about one cycle per word (plus page and linked-list node
costs, DuckStation's model rather than a measurement), and since bug 38 its
busy bit and interrupt wait for that many cycles of ordinary execution. The
data itself still moves all at once when the transfer starts - except channel
1 in request mode, which waits for MDEC output (the Area 51 fix). A device
whose readiness depends on partial progress mid-transfer is otherwise not
modelled; channel 0's version of this is the crash path at the top.

By default the CPU also runs on through the transfer's time, where a console
stops it. Emulation > Timing Accuracy > DMA Stops the CPU (bug 111) makes it wait
the transfer out before its next instruction, the rest of the machine running
through it. That leaves the eager data movement unobservable by the program,
since it cannot run until the transfer is over. Other devices can still see it:
the SPU gets a whole upload at once rather than word by word. Off by default,
because it slows every game that moves a lot of data, as a console is slowed.

Chopping - CHCR bit 8, which gives the CPU windows between slices of a burst -
is not modelled: a chopped transfer takes what an unchopped one does, where
JaCzekanski's `dma/chopping` measures 16,693 cycles and up on a console. No game
on the regression table uses it. (A burst started without its trigger used to
never run at all; it now runs when its device is asking, bug 112.)

### Root counters - correct, with coarse edges

The three counters count their real clock sources, honour their sync modes and
match targets as the hardware does (`timer_test`, 79 checks; bugs 27-31).
Approximate rather than wrong:

- **Interrupts can be up to 32 CPU cycles late.** `IOInterface::Tick` batches
  32 cycles before advancing the world. Counter *reads* are exact -
  `RunPending()` runs the batch early on any counter register access.
- **Hblanks are counted per completed scanline**, the right number attributed
  to the end of the line rather than the moment the beam leaves the window.
- **Vblank lands on a scanline boundary.** `Gpu::Tick` consumes whole
  scanlines and evaluates vblank, the vsync interrupt and the field flip once
  per line, so a game cannot observe the beam crossing into vblank mid-line.
  The beam's position *within* a line is tracked, though - `dot_accumulator_`
  keeps the remainder and `in_hblank()` reads it against the display window
  from GP1(06), so the hblank gate a counter sees is sub-scanline. (This used
  to say the whole thing moved a scanline at a time, which the dot-clock work
  had already made untrue.)
- **Everything above still moves in 32-cycle steps**, which is the real floor:
  `IOInterface::Tick` batches, and a counter read runs the batch early.

Emulation > Timing Accuracy > Exact Event Timing (bug 111) removes the first
and last of these. A batch ends at the next event any device has scheduled
instead of every 32 cycles:
- a counter's target or wrap
- either edge of hblank and the end of each scanline
- a DMA or a CD response falling due
- an SIO transfer, an SPU sample
- the rasteriser running dry

So an interrupt lands on its cycle, and counter 1 counts an hblank as the beam
enters it. `timer_test` checks both, off and on. Off by default, because every
game's timing moves with it and it costs speed in proportion to how often events
come. Vblank landing on a scanline boundary is not an approximation: that is
where it starts.

### The GP0 queue and drawing time - built, with four simplifications left

**Drawing takes time now (bug 85).** Every primitive is charged in GPU clocks -
a setup cost by shape, then a per-pixel cost that doubles for a texture and
rises again for blending or mask-checking - and `Gpu::Tick` burns it down.
GPUSTAT bit 28, ready to receive a DMA block, drops while the rasteriser owes
time, and the DMA request line drops with it. So a game that watches the GPU's
load now sees one. The constants are DuckStation's, which are community
measurements rather than anything Sony published.

**And the queue is real (bug 86).** GP0 words wait in it rather than being
acted on where they land, the port reports full at the 16 words hardware
holds - bits 26 and 28, and the DMA request line with them - and DMA channel 2
stops when it is full and picks up when the rasteriser has made room, with
MADR and BCR describing the remainder so a paused transfer needs nothing
remembered on the side.

**And the cost is charged on what actually rasterises (bug 87).** A primitive's
area is taken after clamping it to the drawing area, not before. Charging the
whole of it billed a 3D game for geometry the drawing area threw away - Silent
Hill was charged 2.9 frames of drawing for every frame and ran at a third
speed - so the rasteriser could never catch up and channel 2 spent the frame
waiting.

**And only the active field is charged (bug 88).** In 480-line interlace with
drawing to the display area prohibited, hardware puts down half the lines, so
the per-pixel cost of triangles, rectangles and lines halves - GPUSTAT bits 19
and 22 set with bit 10 clear, which is DuckStation's condition. Setup costs and
fills are not halved, which is also what it does.

**And the pixels follow it (bug 89).** The rasteriser leaves the displayed
field's rows alone rather than drawing every line and being charged for half,
which is what bug 88 left inconsistent. Primitives and fills skip; a
CPU-to-VRAM transfer and a VRAM-to-VRAM copy do not, which is where DuckStation
draws the line as well. `Gpu::Stats::field_skipped` counts the pixels left
alone and stays zero outside 480i.

Four simplifications are left, and all four are deliberate:



- **The store is deeper than the 16 words it reports.** Nothing here can make
  a CPU write wait, so a game that ignores the ready bits and writes anyway
  would lose words if the store were exactly 16; hardware would have stalled
  its CPU instead. The depth is what software is told; the capacity is what is
  kept, and `Gpu::Stats::queue_overflows` counts anything lost beyond it.
- **A paused transfer stops at a node or block boundary**, not mid-node, so a
  single linked-list node of up to 255 words still goes over in one piece.
- **A partly-offscreen primitive is estimated by clamping its corners**, which
  undershoots where intersecting its edges with the drawing area would be
  exact. DuckStation documents the same approximation and takes it.
- **Transfers are not charged, unless asked (bug 93).** By default a CPU-to-VRAM
  or VRAM-to-CPU blit still costs no GPU time. Emulation > Charge GPU Time for
  VRAM Transfers charges one tick a pixel - a figure derived from the
  VRAM-to-VRAM copy cost, since DuckStation charges nothing here and nobody
  measured it - which is why it is off. Either way the words flow past a busy
  rasteriser rather than queueing behind it, because the blitter is a separate
  piece of the chip and holding them back would deadlock a game that uploads a
  texture between two primitives.

Reading GPUREAD also forces whatever is queued to run first, drawing time
given away rather than answering from a stale latch - the one way the queue
could have turned into a wrong picture rather than a slower one.

This entry was missing until the 2026-09-21 audit, while Test-Suite.md's
`gpu_test` section had been pointing at it by name.

### The instruction cache - a timing model, off by default and unproven

By default every instruction fetch costs one cycle wherever it comes from,
which is the same as assuming it always hits the cache - including the BIOS
running uncached out of ROM. Emulation > Instruction Cache Timing (bug 94)
models the cache: DuckStation's 256-line layout, a refill to the end of the line
on a miss, the full bus cost for every uncached fetch. It is a timing model only
- instructions still come from memory, never from the cache - so it cannot run
stale code, which is how the earlier `ICache2` corrupted every read.

Three limits:

- **Interpreter only.** With the recompiler on it does nothing, because compiled
  blocks do not fetch. Modelling it there means tag checks in generated code, on
  top of the recompiler's own unproven timing.
- **Not shown to be more accurate.** Against JaCzekanski's access-time test and
  the timers test's console log it is neutral in steady state; see bug 94 for
  the numbers and for the comparison that briefly looked better and was not.
- **The cache-enable bit in `0xFFFE0130` is not consulted**, as DuckStation does
  not, and a loaded state starts with a cold cache.

**There is no data cache to add.** The R3000A's data cache is the PlayStation's
1 KB scratchpad at 0x1F800000, which has always been implemented.

### GTE - values and flags agree with hardware; one matrix is guessed

All 22 commands pass amidog's `psxtest_gte` REG, COMPLEX, TIMING and OPCODE
groups - TIMING since bug 78, OPCODE since bug 79, which is where the 44-bit
accumulator's mid-sum overflow and RTPS's IR0 were found - and games issue
tens of thousands of commands with none unrecognised. The MVMVA garbage matrix
(matrix select 3) is written from the description, not measured.

## Present but incomplete

### Disc images

- **CHD, but not ECM or PBP** ([Disc-Formats-Plan.md](Disc-Formats-Plan.md)).
  CHDs mount through libchdr with chdman's default codecs (bug 104); one
  compressed with zstd is refused with a message, since zstd is not built in.
  Nothing here has read a CHD that chdman made - there is no chdman on this
  machine, so the ones tested were written by `tools/chd_writer.h` to its
  format.
- **A CloneCD `.sub` answers the drive's position, and nothing else yet** (bug
  110). Where it sits beside a `.ccd`, GetlocP and the position reports sent
  while CD audio plays come from each sector's own Q subchannel, which is how
  Tomb Raider's pregap - listed nowhere else in its dump - came to be seen.
  Reading data sectors, GetQ (which
  synthesises its answer from the track table, see CD-ROM above) and
  copy-protection checks that read the subchannel some other way still do not
  use it. There is no `.sbi` support either, the patch files that carry a
  LibCrypt disc's altered subchannel beside a `.cue`.
- **A scrambled `.ccd` is refused rather than descrambled.**
  `DataTracksScrambled=1` means the image holds the raw channel, not sectors.
  Both the descriptor and its image are refused, deliberately - mounting one
  would feed the controller noise shaped like a disc. None of the dumps here
  are scrambled.
- **A bare image cannot know its music track boundaries.** Data sectors carry
  a sync pattern and audio does not, so `OpenImage` finds the end of the data
  track and calls everything after it one audio track; a game asking for track
  5 gets nothing. A `.cue` or `.mds` beside the image is used automatically.
- **Rips with zeroed XA subheaders** play their films silent. The Captain
  Tsubasa J `.bin` on the share has all eight subheader bytes zero on every
  stream sector (its `.mdf` does not), so XA audio sectors are not recognised as
  audio and reach the CPU as data. Nothing detects this or offers the
  descriptor-backed image instead.

### Serial port (SIO1) - a port with nothing plugged into it

`1F801050`-`1F80105F` is decoded and behaves as a port with no cable
(`psx/sio1.h`, bug 80): the registers keep what software writes and read back
what hardware would, the status reports no device on /DSR or CTS and an empty
receive FIFO, the baud-rate timer counts, and a transmit with the transmitter
enabled raises IRQ8 if it is armed. `sio1_to_console` copies what it transmits
into the BIOS console, which is how homebrew that prints over the port is read
here.

What is not there is anything on the other end:

- **No link cable.** Two emulator instances cannot be joined, so the handful of
  games with a link mode (Doom, Ridge Racer Revolution, Destruction Derby and
  a couple of dozen more) see an unplugged port - which is what they see on
  one console anyway. DuckStation does not emulate a link either.
- **No host serial port**, so PC-side tools cannot talk to the machine.
- **A byte is transmitted instantly** rather than taking its ten or so bit
  periods at the programmed baud rate. With nothing receiving, the only
  difference is how soon the transmit interrupt arrives.
- **Unverified against hardware.** JaCzekanski's suite has no SIO1 test, so
  `sio_test`'s `sio1` group checks this against psx-spx and DuckStation, not
  against a console. The unplugged /DSR and CTS levels are where the two
  disagree: DuckStation reports both asserted, this reports neither.

### Physical drives - data tracks only

A mounted drive letter reads data sectors. Audio tracks are not read and no
subchannel is available, so a physical disc cannot play its music.

### CD-ROM - every command answers, some approximately

All 28 commands are handled (bug 37). Approximate: the Forward/Backward scan
rate is plausible rather than measured, SetSession assumes one session, and
GetQ synthesises its Q bytes from the track table rather than a real
subchannel.

### Memory cards - done, apart from a few edges

Each disc gets its own `card1.mcr`/`card2.mcr` under
`Documents\My Games\PSXEmu\memcards\<disc>\`, created formatted. Cards are
held in memory and written whole, atomically, a second after a game stops
writing (and on pause, eject, cold boot and exit). File > Memory Cards inserts,
creates and ejects per slot while a game runs, and the Memory Card Editor
lists both cards with icons and titles and deletes, undeletes, exports and
imports, copies between slots and formats (bug 69,
[Memory-Cards-Plan.md](Memory-Cards-Plan.md)). Import takes single saves
(`.mcs`, or a raw headerless save) and the saves on a whole card from another
tool: raw, DexDrive `.gme`, VGS `.mem`/`.vgs` or `.psx` (bug 106). What is left:

- **A card from another tool cannot be inserted as it is**, only imported onto
  one of PSXEmu's. Insert says so and why: a card is written back to its own
  file as a plain 128 KB image, which would drop the tool's header.
- **Card contents are not in save states.** Loading a state leaves the cards
  as they are, which is the usual choice, but a state saved before a game
  wrote its save and loaded after it does not undo the save.
- **A card write that fails is counted, not shown.** `MC::flush_failures()`
  records it; the front end does not tell anyone.
- **No per-slot Recent list**, which the plan suggested.

### Controllers

Digital pad, Dual Analog, DualShock (with the real analog/rumble negotiation,
its capability replies, and an ANALOG button), mouse, multitap (a type per
player, and four memory card slots), GunCon and "nothing plugged in" are
implemented and covered by `sio_test` - see Bugs-Found 96-100. Missing or
unproven:

- **The GunCon has never met a GunCon game.** Its reply is DuckStation's byte
  for byte, and where it says the beam is uses DuckStation's arithmetic, but no
  disc on the share supports it. Point Blank or Time Crisis would settle it.
- **No Konami Justifier** (Hyper Blaster). It works differently - the GPU raises
  IRQ10 as the beam passes the gun. Area 51, the one light-gun disc on the
  share, is a Justifier game, though it also takes the mouse.
- **The ANALOG button leaves out DuckStation's 00h status byte**, which tells a
  game its mode changed behind its back and needs a game database to be safe
  (bug 97). A game that only notices through that byte will not notice.
- **The mouse has never met a mouse-aware game.** Its reply follows psx-spx,
  and how much a hand movement is worth is now a choice rather than the
  guessed divisor it was (bug 82): Input > Mouse > Motion offers the desktop
  pointer's own accelerated movement, Windows' curve reapplied to raw counts,
  or linear counts scaled for a 1994 ball mouse. What is still unsettled is
  which of them is *right*, and that needs a game to judge in - `Populous -
  The Beginning` and `Lemmings & Oh No! More Lemmings` are on the share and
  both claim mouse support. Two of the three modes also rest on numbers
  nobody published: the console mouse's ~200 CPI, and the constants in
  Windows' undocumented ballistics.

## Barely started

- **Parallel / expansion port** - a readable buffer with nothing behind it.
- **DMA channel 5 (PIO)** - accepts register writes and raises its interrupt;
  transfers nothing.

## Blocking use rather than correctness

### Settings cover little

`psxemu.ini` holds `audio_volume`, `audio_backend` (WASAPI or DirectSound),
`graphics_backend` (D3D11, D3D12, OpenGL or Vulkan), `video_filter`, controller type and input
source per port, the multitap player sources and types, `frame_limiter`,
`cdrom_mechanical_timing`, `skip_bios_intro`, `recompiler`, `gpu_thread`,
`gpu_transfer_timing`, `icache_timing`, `exact_event_timing`, `dma_stops_cpu`,
`measured_bus_timing`, `write_queue_timing`, `bios_file`,
`emulation_speed`, `pause_in_menus`, `show_timings`, `show_bios_console`,
`sio1_to_console`, `mouse_motion` and `mouse_dpi` - twenty-four keys, which is
every field `StoreConfig` writes.
Beside those, the front end keeps its own keys in the same file: the eight
most recent discs (`recent_disc_1`..`8`, File > Recent Discs) and the controller
bindings - Port 1's keys as `key_up`, `key_cross` and so on (bug 70), and every
other port and device as one `bind_<slot>_<device>` line, written only when it
differs from the defaults (bug 107).

`bios_file` is a filename rather than a path: the images live in
`Documents\My Games\PSXEmu\bios`, which Settings > BIOS lists (anything in it
of exactly 512 KB, which is what the core accepts) and the front end creates on
first run. The command line still wins over it, and a name that has since been
deleted falls back to the old search beside the executable rather than refusing
to boot.

### The front end is minimal

A window, menus for disc, reset, pause, volume, video filter, controllers,
which BIOS to boot and how fast to run (50-300%), D3D11, D3D12, OpenGL and Vulkan presenters
(all of them in a window or borderless full screen, Alt+Enter or F11),
keyboard/XInput/mouse input, and a speed readout in the title bar (bug 49) that
Emulation > Show Timings expands into where each frame's time went, and a BIOS
console window (Emulation > BIOS Console, bug 66) showing what software prints
through the BIOS, a memory card editor (bug 69), recent discs, controller
bindings (bug 107, which replaced bug 70's keyboard-only list), and a CPU
debugger (Emulation > Debugger, bugs 71-75) with memory, watchpoints, a BIOS
call log, a call stack, labels and device panes (see
[Debugger-Plan.md](Debugger-Plan.md); PsyQ `.SYM` symbol files are not read yet) -
and no settings dialog, deliberately:
every setting is already in the menus. Keys and pad controls are rebound per
port and per device. Only XInput pads are read: a DualShock 4 or DualSense
plugged in on its own, with nothing presenting it as an XInput pad, is not seen.
Output a program sends to the serial port is
shown when Emulation > Serial Port to Console is ticked, which puts it in the
BIOS console window beside what the BIOS itself printed (bug 80). What a
program sends to the expansion port's DUART directly is still not shown: those
registers trap.

It *can* be driven from an agent session after all - launched, sent
`WM_COMMAND`s, and read back through its title bar (Test-Suite.md's host_test
section) - which is how the threading work was checked end to end. What still
needs a person is what a frame looks like and what anything sounds like.

**It is no longer single-threaded** (2026-09-18). The machine, video, audio and
input each have a thread and the UI thread only answers the window, so a menu or
a drag no longer stops the game and a long frame no longer stops the window -
[Threading-Plan.md](Threading-Plan.md) has what that cost and bought. Whether
the game keeps running under an open menu is a setting: Emulation > Pause While
in Menus, off by default. Phase 7, the rasteriser's own thread, is done too
(bug 91), so the plan is complete.

### The emulation speed ceiling is the scene, not the front end

Emulation > Speed offers 50 to 300% (bug 81). What actually happens, measured
2026-09-23 on the BIOS shell with `show_timings` on, driving a scratch copy of
the front end with its own `psxemu.ini`:

| Speed asked | Recompiler | Interpreter |
|---|---|---|
| 100% | 59.3 fps (100%) | 59.3 fps (100%) |
| 150% | 88.9 fps (150%) | 63.9-65.9 fps (108-111%) |
| 200% | 96.9-100.4 fps (163-169%) | - |
| 300% | 93.9-100.0 fps (158-169%) | 64.5-66.5 fps (109-112%) |

So on the BIOS shell the ceiling is about **1.65-1.70x with the recompiler** and
about **1.10x with the interpreter**: 150% is exact recompiled, 200% and above
clamp. The shell is the worst case, though - see the per-scene table below, where
a real game recompiled reaches 3-4x and 300% is real.

**This entry used to say the ceiling was about 110% and blamed the front end** -
"a presenter, an audio device that consumes in real time, and the thread
hand-offs between them, plus the minimised window". All of that was wrong, and
the numbers behind it were taken with the interpreter, which is the 110% column
above.

What the per-frame accounting says at the ceiling: **hand-off 0.11-0.15 ms,
idle 0.0 ms, present ~2 ms on the video thread's own time.** The machine thread
is spending every millisecond it has inside `RunOneFrame` and none of it
waiting for anything. A visible window measures the same as a minimised one, so
that factor is ruled out too. `SampleRing::Write` drops what does not fit rather
than blocking, so the audio device cannot hold the machine back either.

The ceiling is simply what a frame of emulation costs, against a 16.86 ms real
frame - **and that is per scene, not per emulator, which is what makes the
table above misleading on its own.** The BIOS shell is the most expensive thing
in the test set: 640x480 interlaced, drawing heavily. Marginal cost per frame,
measured with `boot_runner`:

| Scene | Interpreted | Recompiled | Recompiled + `gpu_thread` |
|---|---|---|---|
| BIOS shell | 18.00 ms (0.94x) | 8.24 ms (2.05x) | 6.90 ms (2.44x) |
| Ridge Racer | - | 4.07 ms (4.14x) | 3.60 ms (4.68x) |
| Wild Arms | 13.06 ms (1.29x) | 3.79 ms (4.45x) | 3.22 ms (5.24x) |

The recompiled column moved down after bugs 89 and 91 - the interlaced field
skip halved the shell's pixel work, and the rasteriser moved to a thread - so
the figures here are lower than the ones this entry first recorded.

So **in a real game with the recompiler, 200% and 300% do work** - Wild Arms has
headroom for 5x and Ridge Racer for 4.7x with the rasteriser on its own thread,
and 4.5x and 4.1x without. The shell clamps at about 165% because
the shell is slow, not because the front end or the speed setting is. The 4.09 ms
here matches Threading-Plan.md's own 4.4 ms for the same game, measured
independently.

Interpreted, a game sits near 1.3x, so 150% is roughly the honest top of the
range on that CPU.

**And the "`boot_runner` reaches 1.68x, so it is not the emulation" argument was
a measurement mistake worth recording.** That 1.68x is `boot_runner`'s *first*
400 frames - the boot animation, 11.37 ms a frame. The steady shell menu, which
is what the front end was showing, costs 18.00 ms a frame interpreted and 10.54
ms recompiled. Compared like with like the front end is *marginally faster* than
the harness, not 35% slower: there is no front-end overhead to find. Marginal
cost per frame, not total time over a run that includes the boot, is the figure
to use.

**One real defect this turned up, now fixed (bugs 90 and 92).** Sound was resampled by
the speed *asked for* rather than the speed achieved, so at 300% on a host good
for 165% the device was handed about 55% of the samples it needed and the rest
was silence. The ratio is now the rate the machine is actually managing, and the
ring's trim has the authority to refill after a shortfall. At every speed the
host can reach - 50, 100, 150% - sound is gapless.

Bug 90 applied that everywhere and made the pitch slide after every unpause and
warble in every game. Bug 92 confines it to a host that is actually falling
behind - half a second of the limiter with nothing to sleep off - and is
otherwise back to resampling by the setting, as it always was. At a speed the
host cannot reach, the only gap left is that first half second, before the
shortfall is recognised: about 5,500 short frames once, then none.

### Never run against the reference

- **The recompiler against the game table - run now, and it agrees.** On
  2026-09-25 all twelve discs gave the same picture compiled as interpreted at
  every one of the 36 checkpoints, with no recompiler faults (Test-Suite.md).
  The two CPUs are still not equivalent - interrupts land at block boundaries,
  and compiled code charges its cycles more coarsely, so it runs 14-19% more
  instructions in the same frames and three discs read a CD sector more or fewer
  by a checkpoint - but no game in the table shows it. See
  [Recompiler-Plan.md](Recompiler-Plan.md).

## Not gaps

Things that look missing and are not, so they are not re-investigated:

- **Air Combat's and Captain Tsubasa J's intro films.** Both play. They went
  black on their first frame because a DMA flag that outlived its enable fired
  the stream library's sector callback early - bug 55. DICR bit 31 is now the
  master enable with *any* flag, not only enabled ones, as DuckStation has it.
- **The colourful "noise" behind MEMORY CARD and CD PLAYER.** The shell's own
  paint-splatter art, identical on a real SCPH1001
  (`playstation-scph1001-menu.png`, drew1440.com BIOS survey), uploaded whole
  from VRAM (896,0)-(956,59).
- **CD audio (CD-DA) playback and the BIOS CD player.** Both work - bugs 34-36.
  When CD music seems missing the cause is usually the track layout (disc
  images above). The `media_test` peak-meter failure above is new and is not
  this.
- **XA-ADPCM.** `Cdrom::DecodeXaAdpcm` handles 4/8-bit, mono/stereo, 37,800 and
  18,900 Hz with filter history across sectors, honouring `Setfilter`; Wild Arms'
  opening film decodes to 50 seconds of clean audio. Silence from a particular
  image is more likely zeroed subheaders (disc images above).
- **The MDEC.** Implemented, `mdec_test` 85 checks - including both DMA
  channels' behaviour, which is where bugs 55 and 56 landed; films play.
- **The SPU voice path.** 24 ADPCM voices, ADSR, the Gaussian table, noise,
  pitch modulation, CD input, and loop addresses surviving key-on (bug 39).
  The quirks left are the entries above.
- **Load delay slots.** Modelled, including a write in the slot beating the load
  and lwl/lwr forwarding (`cpu_test` `loaddelay`, bug 32).
- **Ace Combat 3 input and Wild Arms after "press start".** Fixed (bug 46; bugs
  25-26). Their plan documents predate the fixes.
- **`System::BootDisc` and auto-boot.** The BIOS boots discs itself and the
  front end lets it; `BootDisc` and `--auto-boot` remain for the harness (bug
  19).
- **A bare `.img`, `.mds`/`.mdf`, `.ccd`/`.img`.** All mount. CloneCD's
  descriptor is read for its table of contents, and an `.img` opened on its
  own finds the `.ccd` beside it the way it already found a `.cue` or an
  `.mds`. The gap left is the track layout a bare image with no descriptor at
  all cannot know, above.
- **`psx/emu.h`/`emu.cpp`** are superseded by `system.*` and built by nothing;
  **`utilities/cdrom/cdrom.cpp`** is superseded by `psx/disc.cpp`.
