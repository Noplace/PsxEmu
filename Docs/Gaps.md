# Gaps

Hardware and features still missing, ordered by how likely each is to stop a
game working. See [Roadmap.md](Roadmap.md) for the phase each belongs to and
[Bugs-Found.md](Bugs-Found.md) for what has already been fixed.

Last audited after the DMA busy-bit pacing fix (bug 38).

---

## Blocking a game right now

### Air Combat freezes during its intro FMV

Air Combat (SLUS-00001) reaches its intro film, decodes at least one frame,
and then stops with a black screen - the GP0 word count stops moving around
frame 620 and never resumes. It is stuck in a wait-and-consume function
(`8002933C`, reached through `800242B4`) spinning on a single-slot "frame
ready" flag at `0x800A2BB8` that nothing ever sets again.

This is not the XA/CD audio path (checked against bug 36 - the silence is in
MDEC's video output, not the SPU mix) and it is not DMA busy-bit
observability either: bug 38 was written for exactly this shape of problem,
is a real fix, and the freeze is unchanged by it.

What produces that flag is now known: it is the game's **DMA channel 3
(CD-ROM) completion handler**, and it runs exactly once. The game starts its
FMV correctly (`ReadS` at lba 32501, mode C0) and the drive streams 341 data
sectors to the end of the run, but **DMA3 never runs again after the first
one**, so nothing is ever fetched into RAM and the BIOS's own decoder reports
`MDEC_vlec: invalid VLC ID`. Ruled out on this side: DICR write-one-to-clear,
the halfword I_STAT acknowledge, CD interrupt gating, data-FIFO re-arming,
the drive status byte, and any `ShouldStart` rejection (measured: zero, on
every channel).

The sharpened finding: the film never sets up a CD DMA at all - channel 3's
MADR and BCR are written exactly 161 times each, every one during file
loading. The single channel-3 "completion" that starts the game's frame
pipeline is a **ghost**: DICR's flag bits are write-one-to-clear, so
completions latched during BIOS-era loading survive the film's
`DICR <- 00000000`, and the moment it enables channel 3 the stale flag raises
an interrupt for a transfer that never happened. The producer marks a frame
ready with nothing behind it and the ring is desynchronised from then on.
Interrupt delivery itself is healthy (537 delivered, 147 swallowed). Whether
the stale flags are faithful - psx-spx suggests they might be - is the open
question.

Investigated in detail, including several ruled-out hypotheses, in
[Air-Combat-FMV-Plan.md](Air-Combat-FMV-Plan.md). It is the only game known
to be blocked; the previous entry here was XA-ADPCM, which is implemented -
see below.

## Silently wrong rather than absent

These do not stop anything, which is what makes them worth listing: a game
runs at the wrong speed or draws slightly wrong and nothing reports an error.

### Root counters - implemented properly, timing still quantised

The three counters count their real clock sources, honour their sync modes,
and match their targets the way the hardware does. `timer_test` covers this in
70 checks. What used to be here - sync modes decoded and ignored, a target of
zero silently never matching, the dot clock divided out of CPU cycles by a
hardcoded 10, a counter that could only wrap once however long the step - is
all gone. See bugs 27-31.

Three things about the counters are still approximate rather than wrong:

- **Interrupts can be up to 32 CPU cycles late.** `IOInterface::Tick` is
  called once per cycle and batches to 32 before advancing the world, so an
  interrupt lands at the end of the batch it happened in. Counter *reads* are
  exact - `RunPending()` runs the batch early whenever software reads or
  writes a counter register - so a game timing something short measures it
  correctly; only the interrupt edge is coarse.
- **Hblanks are counted per completed scanline**, which is the right number
  but attributes them to the end of the line rather than to the moment the
  beam leaves the display window. The gate counter 0 pauses on uses the real
  within-line position, so only the count is phase-approximate.
- **`Gpu::Tick` still advances a whole scanline at a time.** Nothing between
  scanlines can be observed, which is why the hblank gate can only change at
  batch granularity.

### Cycle timing - modelled, not measured

`Cpu::Load` charges a region-dependent stall (3 cycles for RAM, 0 for the
scratchpad, 3 for a hardware register, 5 for the BIOS ROM) on top of the
per-instruction cost. That was enough to stop the BIOS giving up on VSync -
see bug 16 - but it is a model, not a measurement, and the per-instruction
costs beneath it are uniform where real ones are not.

DMA transfers now take time rather than completing instantaneously: a channel
bills the machine roughly one cycle per word, plus one per sixteen for the
DRAM page boundary, plus 8 cycles per linked-list node and 5 more for a node
that carries data. The rate is DuckStation's model rather than a measurement
of real silicon.

That billed time is also now observable (bug 38): a channel's busy bit stays
set, and its completion interrupt waits, until that many of the machine's
cycles have actually gone by through the CPU's own ordinary
instruction-by-instruction ticking - not the whole amount resolved
synchronously inside the register write that triggered it. Software that
starts a transfer and then polls to find out when it is done now sometimes
finds it still running, the way real software does. The data itself still
moves eagerly, all at once, the moment the transfer is triggered; only the
*signal* that it is done is paced against the billed cycles. That is a
deliberately smaller change than modelling a real block-by-block bus request -
DuckStation actually transfers each block only as its device asks for it - so
a device whose readiness genuinely depends on partial progress mid-transfer is
still not modelled here.

What is still missing is any comparison against hardware. The right instrument
is a timing test suite run on a console and on this, and nothing like that has
been run. Until then every number here is a plausible shape, not a fact.

The emulator's own speed is at least measured now: `boot_runner` reports
emulated seconds against wall-clock seconds at the end of a run. See
[Recompiler-Plan.md](Recompiler-Plan.md), which argues measurement should come
before any optimisation work - that measurement now exists.

### CD audio is resampled linearly

The SPU's *voice* path uses the hardware's Gaussian table (`kGauss`). The
CD-audio path, which resamples a 44100 Hz track to the output rate, uses linear
interpolation rather than the hardware's seven-point filter. The code says so
where it does it. The difference is a slight softening of the top end, not a
wrong pitch or a click.


### Voice and main volumes come out at half

`Spu::VolumeOf` decodes the sweep-capable volume registers. In fixed-level
mode the stored level is meant to be doubled into the full signed range, and
the code's `(int16_t)(reg << 1) >> 1` does the doubling and then shifts it
straight back out - so a voice or main volume of 3FFFh, which should be about
unity, mixes at about half. It is consistent across every voice and the main
output, so nothing sounds wrong relative to anything else; it is just quiet,
which is part of why the front end defaults its master gain to 2x.

The CD and external *input* volumes were on the wrong side of this entirely -
run through `VolumeOf` when they are a different, plain-signed format - which
silenced CD-DA and XA outright until bug 36. Those now use `InputVolumeOf`.
Fixing the voice/main halving is a separate pass: it doubles every game's
audio and would want the master-gain default dropped to match, so it needs
measuring on its own rather than riding along here.

### Volume sweeps are not implemented

Bit 15 of a voice or main volume register selects a sweep - a ramp, linear or
exponential, up or down, at one of 128 rates - instead of a fixed level.
`Spu::VolumeOf` reads any such register as 3FFFh, full scale, because the
sweep's starting level is not stored in the register and nothing tracks one.

A sequencer uses sweeps for note attack shaping, tremolo and cross-fades
between layered voices, so a game that uses them has its dynamics flattened:
every part plays at the level of the loudest. Nothing about pitch changes,
which makes it hard to hear as a fault rather than as a mix.

`boot_runner`'s `spu requests` line counts sweep writes against level writes,
so whether a given game is affected is now one run rather than a guess. Final
Fantasy VII is not: 0 sweeps against 4006 plain levels over 3600 frames, its
panning done by rewriting levels note by note. That was worth knowing during
bug 39, and it is worth checking before blaming this for anything.

### The reverb is a two-tap delay, not the hardware's network

`Spu::ProcessReverb` runs a two-tap delay out of the reverb work area, with a
single feedback term off the two master reverb volumes. The hardware runs a
comb-and-all-pass network out of the same buffer, driven by the 32 reverb
registers (`1F801DC0h..`) - the all-pass and comb delays, their feedback and
filter coefficients, and the input filters. All 32 are stored and none but the
first two are used.

The shape of the effect is there - a decaying echo at some depth - but not its
response, and the delay length is not even a fixed room: it is derived from
however much sound RAM sits above `reverb_base_`, so a game that allocates a
large work area gets a long slapback rather than a large hall.

This is not academic. Final Fantasy VII routes **all 24 voices** through the
reverb with the master enable set (`reverb FFFFFF`, `control C0B5`, measured by
`boot_runner`'s `spu modes` line), so every note of its music passes through
it. Bug 39 fixed that music's pitch and its waveform; what it sounds like is
still partly this.

Doing it properly means implementing the documented network against the real
register set, which is a pass of its own with `spu_test` coverage to match -
the reverb has none today, and it is the only major SPU path in that position.

### The instruction and data caches are not modelled

`ICache`/`ICache2` exist in `cpu.h` and every call site is commented out,
deliberately: routing data loads through an *instruction* cache corrupted every
read once the BIOS enabled it, and a cache modelled wrongly is worse than no
cache at all.

The cost is timing fidelity. It would also matter to a recompiler, which wants
the cache-control write at `0xFFFE0130` as its signal that code has changed -
see [Recompiler-Plan.md](Recompiler-Plan.md).

### Menu backgrounds draw as rainbow noise

The BIOS menus - MAIN MENU, MEMORY CARD, the CD player - draw coloured noise
behind their highlighted labels where a flat or gently shaded fill belongs.
The shapes and the text are right, so this is a fill reading from somewhere
nothing was written: a texture page or CLUT that is not where the GPU thinks
it is. It has been there all along and nothing measured it, because until the
harness could write a PNG nothing had looked at these screens.

Not investigated. Nothing is known to depend on it, but it is the only known
case of the GPU drawing something visibly wrong.

### Cause's interrupt-pending bits are faked

`RaiseException` sets `Cause` bits 8-15 from `SR`'s interrupt mask rather than
from the actual pending lines. The BIOS's handler computes `cause & sr & 0xFF00`
and gets a non-zero answer, which is why it works - but software that reads
`Cause` to find out *which* line is pending gets the mask instead.

## Present but incomplete

### Memory cards - the format is declared, nothing understands it

`MC` loads and creates 128 KB files, reads and writes 128-byte sectors, and
carries the `flag_` byte the SIO layer reports. Games save and load.

The front end now gives each disc its own pair, automatically: booting a disc
loads or creates `card1.mcr` and `card2.mcr` in
`Documents\My Games\PSXEmu\memcards\<disc filename>\`, named after the disc
image rather than anything read off it, so it works for discs with no
SYSTEM.CNF too. Swapping a disc mid-session leaves the cards alone, which
matches hardware - the memory card slots have nothing to do with the drive.

What is still missing is any comprehension of what is *on* a card: nothing
walks the directory, follows a block chain, decodes a Shift-JIS title or an
icon, or computes a frame checksum. There is still no in-emulator eject for a
running machine - only a cold boot disconnects a card, which is what makes the
auto-load above safe to do unconditionally. And `WriteSector` opens, seeks,
writes and closes the file for every 128 bytes - a game saving one block does
that 64 times, and a crash part-way through leaves a half-written card.

Planned in [Memory-Cards-Plan.md](Memory-Cards-Plan.md).

### Controllers - DualShock now, no multitap or lightgun

`Sio` speaks the real DualShock handshake: a pad boots as a plain digital one
(`5A41h`) and only becomes analog (`5A73h`) if a game actually asks for
it, through the same commands a real one answers to - `0x43` to enter
configuration mode, `0x44` to switch modes and optionally lock the switch,
`0x45` to report which mode it is in, `0x4D` to say which bytes of a poll
reply the two vibration motors listen on. A game that never negotiates any of
this never sees anything different from the plain pad this always was, which
is why every game tested so far - none of which ask for analog input -
produced an identical result before and after this was added.

Two things are approximated rather than measured: commands `0x46` and `0x47`
(capability queries almost nothing exercises) are acknowledged with the right
shape and zero-filled content, and `0x4C` reports a DualShock rather than a
DualShock 2 - pressure-sensitive face buttons are not implemented, so nothing
would read the extra data anyway. Still entirely absent: multitap, and the
lightgun, which also needs the GPU's scanline position latched on trigger.

`PSXEmu.Win32/gamepad.h` is where an XInput pad actually reaches this. The
polling, slot search-and-latch and deadzone handling are the same generic
mechanism GBAEmu's `GamepadInputDevice` already used for Windows/XInput,
nothing GBA-specific about it; the mapping differs because PSX has four face
buttons to GBA's two, so Xbox's four map across by position (A at the bottom
to Cross, and round from there) rather than GBA's compromise of doubling two
Xbox buttons onto one. The triggers become L2/R2, both sticks feed the analog
axes once a game asks for them, and the stick clicks become L3/R3. Port 1
takes the keyboard or a pad, whichever is pressed; port 2 takes a second pad
if one is present, with no keyboard fallback - two controllers need two
physical pads, matching the console. Rumble is read back from `Sio`'s
per-port motor state once a frame and fed to `XInputSetState`.

Mode switching is entirely the game's doing, not the player's - there is no
emulated ANALOG button, and no keyboard or pad shortcut standing in for one.
A real DualShock lets the player force the switch when a game does not ask
for it; this does not. Every real game that wants analog input negotiates it
itself at startup, so this has not yet mattered, but a homebrew disc or a
utility that expects the player to press the button would find nothing does.

Covered by `sio_test` - the handshake, the axis byte order, the pre-DualShock
legacy rumble pattern and the `0x4D`-configured one, and that the two ports do
not leak state into each other.

### CD-ROM - all 28 commands answer

Every command the drive controller accepts is now handled. The last five went
in together (bug 37): `04` Forward and `05` Backward scan the disc during
CD-DA play, skipping a block of sectors per sector time and scanning faster
the more often the command is repeated, ending in ordinary play on a new
`03` Play; `12` SetSession seeks to a session, succeeding for session 1 and
failing for any other on these single-session images; `1C` Reset reboots the
controller to its power-on state; and `1D` GetQ returns one subchannel-Q entry
from the track table. Covered by `media_test`.

What is approximate rather than absent: the scan geometry (how many sectors a
level skips) is a plausible rate, not a measured one; SetSession assumes one
session because the disc formats read here carry only one; and GetQ synthesises
its Q bytes from the track table rather than from a real subchannel, which is
all the track layout it has to work with. Nothing tested needs more than that.
### Disc images - a bare image can only work out so much

A cue sheet gives the full track layout and everything works from it, CD music
included. Without one the layout has to be inferred, and only part of it can
be: a data sector is recognisable by its twelve byte sync pattern and audio is
not, so `OpenImage` finds where the data track ends by binary search and calls
everything after it one audio track. Where one music track ends and the next
begins is recorded nowhere in the data area - it lived in the lead-in, which a
dump does not include - so a disc with twelve music tracks still mounts as two,
and a game that asks for track 5 by number still gets nothing. That needs the
cue sheet, and if one is sitting beside the image it is now used automatically.

Still missing: `.ccd` is not read at all, though it carries a real table of
contents and would give a complete layout; and no compressed container is
supported.

Planned in [Disc-Formats-Plan.md](Disc-Formats-Plan.md).

### Physical drives - data tracks only

A mounted drive letter reads data sectors. Audio tracks are not read through
the drive, and no subchannel data is available, so a physical disc cannot play
its music.

### GTE - exercised now, still not validated

All 22 commands, the register file, saturation, the FLAG register and the
Newton-Raphson divide are implemented, and `gte_test` covers them with 99
checks. Real software now uses it heavily - a game run issues about 60,000
commands with none unrecognised - which is a great deal more confidence than
this entry used to carry.

What is still missing is a *comparison*: nothing has been checked against
hardware output. Until amidog's GTE suite runs, "passing" means "agrees with
the description I read". The MVMVA garbage matrix (matrix select 3) is written
from the description rather than from measurement.

## Barely started

### Serial port (SIO1)

`0x1F801050`-`0x1F80105F` is not decoded at all. Link-cable only; nothing has
ever touched it.

### Parallel / expansion port

A buffer exists and is readable. Nothing is behind it.

### DMA channel 5 (PIO)

Accepts register writes and raises its interrupt. Transfers nothing.

## Blocking use rather than correctness

### Settings exist but cover almost nothing

`psx/emuconfig.h` holds the runtime settings and `psx/settings.h` reads and
writes them as a plain `key = value` file, following GBAEmu's design: unknown
keys are preserved, and every getter takes the current value as its default.
`System::config()` is how a component reaches them, and the front end keeps
`psxemu.ini` beside the executable, written as settings change rather than only
at exit.

There is exactly one setting in it: `audio_volume`. The BIOS path, the disc
path, the key bindings and everything else are still command-line arguments,
menu choices or hardcoded, and are not remembered between runs.

### No save states

There is no serialiser: `psx/state.h` does not exist and no component has a
`Serialise`. Planned in [Save-States-Plan.md](Save-States-Plan.md).

### The front end is minimal

A window, a menu with disc, reset and volume commands, a D3D11 presenter and
keyboard input. No configurable bindings, no debugger, no settings UI beyond
the volume menu, no pause indicator, no speed display -
and that last one matters more than it sounds, because nothing in this project
has ever measured wall-clock speed. See
[Recompiler-Plan.md](Recompiler-Plan.md), which argues that measurement should
come before any optimisation work.

## Not gaps

Things that look missing and are not, so they are not re-investigated:

- **CD audio (CD-DA) playback, and the BIOS CD player.** Both work. `Play`
  seeks, the drive hands raw 2352-byte sectors to the SPU once a sector time,
  and they are mixed through the same CD input XA-ADPCM uses. The BIOS CD
  player lists a disc's tracks, seeks to the one chosen and counts the time up
  while it plays - which needs three things games never touch: the shell-open
  latch that says a disc was swapped, GetlocP's exact eight-byte subchannel
  reply, and `Play` treating a track number of zero as "carry on from here".
  It also needs the CD input volume applied as a plain signed level rather than
  through the voice/main sweep decode, which was silencing it at the mixer -
  see bug 36. The audio reaches the output, verified at the *mixed* peak and
  not just the sector count. See bugs 34, 35 and 36. When CD music appears not
  to work the cause is usually the track layout - see disc images above.
- **Load delay slots.** Modelled. A load reaches its register one instruction
  later than the instruction that issued it, a register write in that slot
  beats the load rather than being overwritten by it, and lwl/lwr forward to
  each other so the usual back-to-back pair works with no gap. Covered by
  `cpu_test`'s `loaddelay` group. See bug 32.
- **`System::BootDisc` and auto-boot.** The BIOS boots discs itself, correctly,
  and that is what the front end does. `BootDisc` remains for the harness's
  `--boot-disc`, and auto-boot for `--auto-boot`, but bug 19 measured the HLE
  shortcut as much worse than letting the BIOS do it and the front end no
  longer uses it.
- **A bare `.img` not loading.** It loads and boots. The gap is the track
  layout above, not the file.
- **XA-ADPCM.** Implemented: `Cdrom::DecodeXaAdpcm` handles 4- and 8-bit, mono
  and stereo, at 37800 or 18900 Hz, with the filter history carried across
  sectors. `LoadSector` routes an audio sector to it and raises no data-ready
  interrupt, so software never sees it in its data stream, and `Setfilter`'s
  file and channel are honoured when the filter bit is set. Covered by
  `spu_test`; verified against Wild Arms, whose opening film decodes to 50
  seconds of audio with a lag-1 autocorrelation of 0.997 and no clipping.
- **The MDEC.** Implemented, with `mdec_test` covering it in 59 checks. Video
  and its audio both work.
- **The SPU.** 24 ADPCM voices, ADSR, the hardware Gaussian table, noise, pitch
  modulation and CD-audio input, with `spu_test` covering it in 107 checks. The
  CD input volume is a plain signed level, distinct from the voice/main sweep
  format (bug 36). Where a voice loops back to now follows the hardware: a
  repeat address written by software survives the key-on that follows it, which
  is what Final Fantasy VII's music depends on (bug 39). Two known quirks
  remain, both gaps above: the voice and main volumes come out at half, and
  **volume sweeps are not implemented at all** - `VolumeOf` reads any sweep
  register as full scale. FF7 turns out not to use them (0 sweeps against 4006
  plain levels, measured), but a game that does will have its dynamics
  flattened. The reverb is its own gap, above.
- **`psx/emu.h` and `psx/emu.cpp`** are an earlier iteration superseded by
  `system.*`. They are kept in the tree but built by neither the solution nor
  the harnesses.
- **`utilities/cdrom/cdrom.cpp`** is the old host-CD read, superseded by
  `psx/disc.cpp`.
