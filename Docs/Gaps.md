# Gaps

Hardware and features still missing, ordered by how likely each is to stop a
game working. See [Roadmap.md](Roadmap.md) for the phase each belongs to and
[Bugs-Found.md](Bugs-Found.md) for what has already been fixed.

Last audited 2026-09-15, after bug 55 (DICR master flag). Every entry below
was checked against the code at that point, not carried forward from the
previous audit (which predated bug 39 and the September 12-14 commits).

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

- **The data-in request is not modelled.** Real hardware asserts it while the
  input FIFO has room; here the channel's own pacing stands in for it, and
  MDEC control bit 30 (DMA-in enable) does not gate the channel at all.
- **Output appears all at once** when a macroblock finishes, where hardware
  copies it out over the same 2,688 cycles.

Games that stream film see the right throughput and the right ordering. A game
that watches STAT's request bits closely rather than using DMA would not.

## Test health

### Every harness is green

cpu 251, gte 99, timer 70, sio 105, spu 108, gpu 31, mdec 85, media 246 - 995
checks, no failures. The two that were failing when this document was last
audited are bugs 58 (the CD peak meter's own test played silence) and 59 (the
top-left rule's vertical test was inverted, which the half-open raster loops
turned from a wrong owner into a gap).

### The fill rule is still gated on semi-transparency

`RasterTriangle` applies the top-left bias only when `state.semi_transparent`
is set. Hardware's fill rule does not know what blending is, and with bug 59's
polarity fix the gate should be unnecessary - but un-gating it moves which
texel wins on every adjacent opaque tile edge, which is what put seams through
Wild Arms' overworld when the rule was upside down. It wants checking against
that game, not against a unit test.

### Baselines are stale

`Test-Suite.md`'s BIOS boot table says so itself, and no per-game checksum
table exists. Bomberman Party Edition's frame-1300 logo, recorded as
`dedbf5071e81061a` for bug 51, is `757f6dd8b459a0ef` on HEAD with the picture
unchanged - moved by the GPU commits. Until there is a refreshed table the
only trustworthy regression check is an A/B: the same sources built with and
without a change, compared every 100 frames (how bug 55 was checked, over
twelve discs).

### Recent SPU and GPU work is unrecorded and untested

The September 12-14 commits have no Bugs-Found entries. The SPU commit
replaced the reverb and added volume sweeps (see below) without adding a
`spu_test` check.

### Tests written with the feature they check, and never run against it

Two of these now: `mdec_test`'s `TestOutputDmaStartedFirst`, below, and the CD
Audio Peak Meter test, which shipped with the meter in `209943e` and failed
from the first run - it played silence and asserted a zero peak was a bug in
the drive (bug 58). A test committed alongside its feature is worth running
once before it is believed.

### A written test nobody called

`mdec_test`'s `TestOutputDmaStartedFirst` - the Area 51 regression, written
with commit `535949b` - was never added to `main()`, so its checks had never
run. Wiring it in (bug 56) found a real bug in the first two of them. Worth a
sweep of the other harnesses for the same thing: a test is only a test if
something calls it.

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

### Reverb and volume sweeps - implemented, not verified

Both used to be listed as missing. Commit `1419d78` implemented them:

- **Sweeps:** `Spu::StepSweep` ramps a voice or main volume with bit 15 set -
  linear or exponential, up or down, at the register's rate - and `VolumeOf`
  returns the running level instead of full scale.
- **Reverb:** `Spu::ProcessReverb` now runs the documented network - input
  volumes, same- and different-side reflections with the wall and IIR
  coefficients, the comb and all-pass stages - off the 32 reverb registers at
  22,050 Hz, where it used to be a two-tap delay using two of them.

Neither has a `spu_test` check, and neither has been compared against
hardware or another emulator's output. `boot_runner`'s `spu requests` and
`spu modes` lines show whether a game uses them: Final Fantasy VII routes all
24 voices through the reverb (`reverb FFFFFF`) and uses no sweeps.

### CD audio is resampled linearly

The voice path uses the hardware's Gaussian table (`kGauss`); the CD-audio
path, which resamples 44,100 Hz to the output rate, interpolates linearly
rather than with the hardware's seven-point filter. The code says so where it
does it. A slight softening of the top end, not a wrong pitch or a click.

### Cause's interrupt-pending bits are faked

`Cpu::RaiseException` sets `Cause` bits 8-15 from `SR`'s interrupt mask
(`cause |= sr & 0xFF00`) rather than from the lines actually pending. The
BIOS's handler computes `cause & sr & 0xFF00`, gets a non-zero answer, and
works - but software reading `Cause` to find out *which* line is pending gets
the mask instead.

### Cycle timing - partly measured, memory regions still modelled

[CPU-Timing-Plan.md](CPU-Timing-Plan.md) tracks this. Done: multiply and
divide charge psx-spx's measured 6/9/13/36 cycles, a not-taken branch costs a
cycle (bug 43), and every GTE command charges its documented cost with the
hardware's stall when the next one comes too soon (bug 42) - recovered from
inside amidog's own test loop, matching the table for every opcode.

Still modelled: `Cpu::Load`'s per-region stall (3 cycles RAM, 0 scratchpad, 3
I/O, 5 BIOS ROM). Primary sources put hardware at 1 / 5 / 7 and a
*programmable* 27-33 for the ROM, set by a memory-control register this core
does not use as a timing input, with a load's cost partly overlapping the
instructions after it. That is phase 3, deliberately left for its own pass
because bug 16 is what a wrong number here does. amidog's GTE suite's TIMING
column stays red until phase 0 (sampling the column itself) and phase 3 are
done.

### DMA data moves eagerly; only the completion is paced

A transfer bills about one cycle per word (plus page and linked-list node
costs, DuckStation's model rather than a measurement), and since bug 38 its
busy bit and interrupt wait for that many cycles of ordinary execution. The
data itself still moves all at once when the transfer starts - except channel
1 in request mode, which waits for MDEC output (the Area 51 fix). A device
whose readiness depends on partial progress mid-transfer is otherwise not
modelled; channel 0's version of this is the crash path at the top.

### Root counters - correct, with coarse edges

The three counters count their real clock sources, honour their sync modes and
match targets as the hardware does (`timer_test`, 70 checks; bugs 27-31).
Approximate rather than wrong:

- **Interrupts can be up to 32 CPU cycles late.** `IOInterface::Tick` batches
  32 cycles before advancing the world. Counter *reads* are exact -
  `RunPending()` runs the batch early on any counter register access.
- **Hblanks are counted per completed scanline**, the right number attributed
  to the end of the line rather than the moment the beam leaves the window.
- **`Gpu::Tick` advances a whole scanline at a time**, so nothing between
  scanlines is observable and the hblank gate changes at batch granularity.

### The instruction and data caches are not modelled

`ICache`/`ICache2` exist in `cpu.h` with every call site commented out,
deliberately: routing data loads through an *instruction* cache corrupted every
read once the BIOS enabled it. The cost is timing fidelity, and a future
recompiler would want the cache-control write at `0xFFFE0130` as its signal
that code changed - see [Recompiler-Plan.md](Recompiler-Plan.md).

### GTE - values and flags agree with hardware; one matrix is guessed

All 22 commands pass amidog's `psxtest_gte` REG and COMPLEX groups, and games
issue tens of thousands of commands with none unrecognised. The MVMVA garbage
matrix (matrix select 3) is written from the description, not measured. Its
TIMING group is the cycle-timing entry above.

## Present but incomplete

### Disc images

- **No compressed containers** (CHD, ECM, PBP). Planned in
  [Disc-Formats-Plan.md](Disc-Formats-Plan.md).
- **A CloneCD `.sub` is ignored.** `.ccd` is read now - the table of contents,
  and the `.img` beside it - but the 96 bytes of subchannel per sector that
  the third file holds are not. Nothing asks for them yet: GetQ synthesises
  its answer from the track table (see CD-ROM above), and that is where a real
  subchannel would go if anything ever needed one.
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

### Physical drives - data tracks only

A mounted drive letter reads data sectors. Audio tracks are not read and no
subchannel is available, so a physical disc cannot play its music.

### CD-ROM - every command answers, some approximately

All 28 commands are handled (bug 37). Approximate: the Forward/Backward scan
rate is plausible rather than measured, SetSession assumes one session, and
GetQ synthesises its Q bytes from the track table rather than a real
subchannel.

### Memory cards - the format is declared, nothing understands it

Games save and load, and the front end gives each disc its own
`card1.mcr`/`card2.mcr` under `Documents\My Games\PSXEmu\memcards\<disc>\`.
Missing: anything that walks the directory, follows a block chain, decodes a
title or icon, or checks a frame checksum; an eject for a running machine
(only a cold boot disconnects a card); and a sane write path - `WriteSector`
opens, seeks, writes and closes the file for every 128 bytes, so a one-block
save does that 64 times and a crash part-way leaves a half-written card.
Planned in [Memory-Cards-Plan.md](Memory-Cards-Plan.md).

### Controllers

Digital pad, DualShock (with the real analog/rumble negotiation), mouse,
multitap and "nothing plugged in" are implemented and covered by `sio_test`.
Missing or unproven:

- **No lightgun.** It needs the GPU's beam position latched on the trigger.
- **No ANALOG button.** Mode switching is only ever the game's doing; a disc
  that expects the player to press it finds nothing does.
- **The multitap's four memory card slots** (`0x81`-`0x84`). The core side is
  small; the Win32 side hand-duplicates two card slots and would need
  rebuilding for eight.
- **Every multitap player is a DualShock**; there is a per-player source but
  no per-player type.
- **`0x46`/`0x47`** answer with the right shape and zero content, and `0x4C`
  reports a DualShock, not a DualShock 2 (no pressure-sensitive buttons).
- **The mouse has never met a mouse-aware game.** Its reply follows psx-spx,
  and its divide-by-4 on raw input is a guess.

## Barely started

- **Serial port (SIO1)** - `1F801050`-`1F80105F` is not decoded at all.
- **Parallel / expansion port** - a readable buffer with nothing behind it.
- **DMA channel 5 (PIO)** - accepts register writes and raises its interrupt;
  transfers nothing.

## Blocking use rather than correctness

### Settings cover little

`psxemu.ini` holds `audio_volume`, `graphics_backend` (D3D11 or D3D12),
`video_filter`, controller type and input source per port, the multitap player
sources, `frame_limiter`, `cdrom_mechanical_timing`, `skip_bios_intro` and
`bios_file`. The last disc and the key bindings are not remembered; the
bindings are a compiled-in table in `const.h`.

`bios_file` is a filename rather than a path: the images live in
`Documents\My Games\PSXEmu\bios`, which Settings > BIOS lists (anything in it
of exactly 512 KB, which is what the core accepts) and the front end creates on
first run. The command line still wins over it, and a name that has since been
deleted falls back to the old search beside the executable rather than refusing
to boot.

### The front end is minimal

A window, menus for disc, reset, pause, volume, video filter, controllers and
which BIOS to boot, D3D11 and D3D12 presenters, keyboard/XInput/mouse input,
and a speed readout in the title bar (bug 49). No binding editor, no debugger,
no settings dialog. It cannot be run from an agent session, so front-end
changes are verified by hand.

### Never run against the reference

- **amidog's CPU suite** (`test/psxtest_cpu/`) runs to its results screen; the
  results have not been read.
- **A dynamic recompiler** is a plan only - [Recompiler-Plan.md](Recompiler-Plan.md).

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
