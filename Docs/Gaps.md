# Gaps

Hardware and features still missing, ordered by how likely each is to stop a
game working. See [Roadmap.md](Roadmap.md) for the phase each belongs to and
[Bugs-Found.md](Bugs-Found.md) for what has already been fixed.

Last audited 2026-09-15, after bug 55 (DICR master flag). Every entry below
was checked against the code at that point, not carried forward from the
previous audit (which predated bug 39 and the September 12-14 commits).
Harness counts and the recompiler and threading entries refreshed 2026-09-18.

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

cpu 287, gte 106, timer 70, sio 146, spu 108, gpu 31, mdec 85, media 271, mc 77, debug 174 - 1,355
checks, no failures (re-run 2026-09-21, after bugs 78-82). The two that were failing when this document was last
audited are bugs 58 (the CD peak meter's own test played silence) and 59 (the
top-left rule's vertical test was inverted, which the half-open raster loops
turned from a wrong owner into a gap).

None of them caught bug 60, which was live in every frame of every game at the
time: the harnesses test what a unit test can reach, and "the screen is one
column narrower than it should be" is not that. The per-game table in
Test-Suite.md is - it moved Ridge Racer and nothing else.

### The fill rule is still gated on semi-transparency

`RasterTriangle` applies the top-left bias only when `state.semi_transparent`
is set. Hardware's fill rule does not know what blending is, and with bug 59's
polarity fix the gate should be unnecessary - but un-gating it moves which
texel wins on every adjacent opaque tile edge, which is what put seams through
Wild Arms' overworld when the rule was upside down. It wants checking against
that game, not against a unit test.

### Baselines - refreshed, and now a real table

Measured at `8c7c694` on 2026-09-16: the BIOS boot row, the register-access
row, and a new per-game table of twelve discs at 3,000 frames with checksums at
frames 1000/2000/3000. See [Test-Suite.md](Test-Suite.md).

The refresh paid for itself immediately: the BIOS boot was drawing 304,803 of
305,920 non-black pixels where the pre-September build drew all of them, which
turned out to be **bug 60** - the drawing area's last column and row discarded
by a half-open loop clipping an inclusive bound. Four days in every frame of
every game, invisible without a reference to compare against.

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
- **Still off:**
  - the CD-ROM by one cycle, the SPU and expansion 2 by 3 or 4: that is the
    formula's own error, not fitted over;
  - a partial `lwl`/`lwr` from a narrow bus, charged a whole word where the
    console seems to read only what it needs.
- **Not modelled:** a slow load overlapping the instructions after it.
- **Unmeasured:** stores.

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

All 22 commands pass amidog's `psxtest_gte` REG, COMPLEX, TIMING and OPCODE
groups - TIMING since bug 78, OPCODE since bug 79, which is where the 44-bit
accumulator's mid-sum overflow and RTPS's IR0 were found - and games issue
tens of thousands of commands with none unrecognised. The MVMVA garbage matrix
(matrix select 3) is written from the description, not measured.

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
imports `.mcs`, copies between slots and formats (bug 69,
[Memory-Cards-Plan.md](Memory-Cards-Plan.md)). What is left:

- **Only `.mcs` single saves import.** No whole-card formats other than the
  raw 128 KB (`.gme`, `.vgs`, `.psx` from other tools), and no raw
  headerless saves.
- **Card contents are not in save states.** Loading a state leaves the cards
  as they are, which is the usual choice, but a state saved before a game
  wrote its save and loaded after it does not undo the save.
- **A card write that fails is counted, not shown.** `MC::flush_failures()`
  records it; the front end does not tell anyone.
- **No per-slot Recent list**, which the plan suggested.

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
`graphics_backend` (D3D11 or D3D12), `video_filter`, controller type and input
source per port, the multitap player sources, `frame_limiter`,
`cdrom_mechanical_timing`, `skip_bios_intro`, `recompiler`, `bios_file`,
`emulation_speed`, `pause_in_menus`, `show_timings`, `show_bios_console` and
`sio1_to_console`, `mouse_motion` and `mouse_dpi`.
Beside those, the front end keeps its own keys in the same file: the eight
most recent discs (`recent_disc_1`..`8`, File > Recent Discs) and the keyboard
bindings (`key_up`, `key_cross` and so on, Settings > Input > Keyboard
Bindings) - bug 70.

`bios_file` is a filename rather than a path: the images live in
`Documents\My Games\PSXEmu\bios`, which Settings > BIOS lists (anything in it
of exactly 512 KB, which is what the core accepts) and the front end creates on
first run. The command line still wins over it, and a name that has since been
deleted falls back to the old search beside the executable rather than refusing
to boot.

### The front end is minimal

A window, menus for disc, reset, pause, volume, video filter, controllers,
which BIOS to boot and how fast to run (50-300%), D3D11 and D3D12 presenters,
keyboard/XInput/mouse input, and a speed readout in the title bar (bug 49) that
Emulation > Show Timings expands into where each frame's time went, and a BIOS
console window (Emulation > BIOS Console, bug 66) showing what software prints
through the BIOS, a memory card editor (bug 69), recent discs and a keyboard
binding editor (bug 70), and a CPU debugger (Emulation > Debugger, bugs 71-75) with
memory, watchpoints, a BIOS call log, a call stack, labels and device panes (see
[Debugger-Plan.md](Debugger-Plan.md); PsyQ `.SYM` symbol files are not read yet) -
and no settings dialog, deliberately:
every setting is already in the menus. Only the keyboard is rebindable; an
XInput pad's layout is fixed. Output a program sends to the serial port is
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
in Menus, off by default. What is left of that plan is its phase 7, a thread for
the rasteriser, which is worth about 12% with the recompiler on and nothing like
a priority.

### Never run against the reference

- **The recompiler against the game table.** It is built and runs the BIOS
  boot identically, but a game's checksum differs between the two CPUs -
  interrupts land at block boundaries rather than instruction boundaries, and
  compiled code charges one cycle an instruction flat. No run of the
  twelve-disc table with `--recompiler` is recorded. See
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
