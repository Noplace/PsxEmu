# Gaps

Hardware and features still missing, ordered by how likely each is to stop a
game working. See [Roadmap.md](Roadmap.md) for the phase each belongs to and
[Bugs-Found.md](Bugs-Found.md) for what has already been fixed.

Last audited against the tree after bug 26.

---

## Blocking a game right now

Nothing known. The last entry here was XA-ADPCM, and it is implemented - see
below.

## Silently wrong rather than absent

These do not stop anything, which is what makes them worth listing: a game
runs at the wrong speed or draws slightly wrong and nothing reports an error.

### Root counters - clock sources right, everything else approximate

Counters 0-2 count, compare against their targets, raise their interrupts, and
take their clock from the source the mode register selects. The hblank rate was
measured against the GPU and is exactly 263 per frame, which is right.

What is not done, in rough order of how likely it is to bite:

- **Sync modes are ignored.** `mode.syncmode` is decoded into the struct and
  never read, so a counter asked to pause during blanking, or to reset at the
  start of one, free-runs instead.
- **A target of zero never fires.** `Tick` guards with `target > 0`, so a
  counter set to interrupt at 0 - which is how software asks for a wrap-only
  interrupt - is silent.
- **Everything is quantised to 32 CPU cycles.** `IOInterface::Tick`
  accumulates and only advances the world once 32 cycles have gone by, so no
  counter can be read with finer resolution than that, and an interrupt can be
  up to 32 cycles late.
- **The dot clock divider is hardcoded to 10.** It should follow the horizontal
  resolution: 10, 8, 5, 4 and 7 for 256, 320, 512, 640 and 368 pixels. A game
  that switches to 640-wide gets a counter running at half the rate it asked
  for.
- **`mode.intreq` is only restored on a mode write**, so one-shot mode can stay
  latched longer than the hardware would.

### Cycle timing - approximate

`Cpu::Load` charges a region-dependent stall (3 cycles for RAM, 0 for the
scratchpad, 3 for a hardware register, 5 for the BIOS ROM) on top of the
per-instruction cost. That was enough to stop the BIOS giving up on VSync - see
bug 16 - but it is a model, not a measurement.

DMA transfers complete instantaneously. A game that expects a transfer to take
time, or that races a transfer against an interrupt, will see a machine that is
faster than the hardware.

### CD audio is resampled linearly

The SPU's *voice* path uses the hardware's Gaussian table (`kGauss`). The
CD-audio path, which resamples a 44100 Hz track to the output rate, uses linear
interpolation rather than the hardware's seven-point filter. The code says so
where it does it. The difference is a slight softening of the top end, not a
wrong pitch or a click.

### The instruction and data caches are not modelled

`ICache`/`ICache2` exist in `cpu.h` and every call site is commented out,
deliberately: routing data loads through an *instruction* cache corrupted every
read once the BIOS enabled it, and a cache modelled wrongly is worse than no
cache at all.

The cost is timing fidelity. It would also matter to a recompiler, which wants
the cache-control write at `0xFFFE0130` as its signal that code has changed -
see [Recompiler-Plan.md](Recompiler-Plan.md).

### Load delay slots are not modelled

A load's result is written to the register immediately rather than one
instruction later. Real code almost never depends on reading the old value, and
compilers fill the slot, so this is forgiving in practice - but it is more
permissive than the hardware, so software that would fault on a console will
run here.

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

### Controllers - the digital pad and nothing else, now fed from an XInput pad too

`Sio` answers `0x5A41` and two button halfwords - still only the digital pad's
protocol, not DualShock's. There is no analog mode, no stick axes reported to
software, no rumble, no mode switching, no multitap, and no lightgun - the
last of which also needs the GPU's scanline position latched on trigger.

What changed is where the button state comes from. `PSXEmu.Win32/gamepad.h`
polls XInput - the same slot search-and-latch and deadzone mechanism GBAEmu's
`GamepadInputDevice` already used, generic Windows plumbing with nothing
GBA-specific about it - and maps the four face buttons by position (Xbox A at
the bottom to Cross, and round from there), the shoulders to L1/R1, and the
analog triggers to L2/R2 past XInput's own held threshold, since the emulated
pad has no analog value for them to become. Port 1 takes the keyboard or a
pad, whichever is pressed; port 2 takes a second pad if one is present, with
no keyboard fallback - two controllers need two physical pads, matching the
console.

Some games require an analog pad; most do not.

### CD-ROM - 23 commands of 28

Missing: `04` Forward and `05` Backward (CD-audio scan), `12` SetSession
(multi-session discs), `1C` Reset, `1D` GetQ (subchannel Q).

None of these has been asked for by anything tested so far.

### Disc images - a bare image has no track layout

`OpenImage` assumes one data track covering the whole file. That is right for a
single-track game and wrong for any disc with CD-DA: the audio tracks are read
as data, `GetTN`/`GetTD` report one track where there are twelve, and a game
that plays a music track gets nothing. A cue sheet supplies the layout, which
is why `.cue` works and a bare image of the same disc does not. `.ccd` is not
read at all, and no compressed container is supported.

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
  modulation, reverb and CD-audio input, with `spu_test` covering it.
- **`psx/emu.h` and `psx/emu.cpp`** are an earlier iteration superseded by
  `system.*`. They are kept in the tree but built by neither the solution nor
  the harnesses.
- **`utilities/cdrom/cdrom.cpp`** is the old host-CD read, superseded by
  `psx/disc.cpp`.
