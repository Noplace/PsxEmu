# Gaps

Hardware and features still missing, ordered by impact. See
[Roadmap.md](Roadmap.md) for the phase each belongs to.

---

## Blocking a booting machine

### GTE - implemented, not yet validated against real 3D

All 22 commands, the full register file, saturation, the FLAG register and the
Newton-Raphson divide are implemented, and `gte_test` covers them with 99
checks. What is missing is confidence: **the BIOS shell issues zero GTE
commands**, so no real software has exercised any of it. Until amidog's GTE
suite or a 3D game runs against it, "passing" means "agrees with the hardware
description I read", not "agrees with the hardware".

The MVMVA garbage matrix (matrix select 3) is written from the description
rather than from measurement.

### SPU - registers but no mixer

`psx/spu.cpp` has the full register file and decodes reads and writes
correctly. It produces no audio: no ADPCM decode, no ADSR, no reverb, no SPU
RAM, no voice mixing. There is also no audio interface for a front end to
implement.

### Memory cards

`psx/mc.h` describes the memory card *file format*, and `Sio` has the slot
plumbing, but the `0x81` device is not implemented: a memory card and an empty
slot are indistinguishable to software. Nothing is loaded or saved.

## Blocking correctness

### DMA - partial

Channel 2 (GPU) handles all three sync modes - linked list, block and burst -
in both directions. Channel 3 (CD-ROM) and channel 6 (OTC) work. Channels 0
and 1 (MDEC), 4 (SPU) and 5 (PIO) are unimplemented.

Transfers complete instantly rather than over time, and the busy bit is not
modelled. The interrupt side is now right - `DICR`'s flags are
write-one-to-clear, the master flag is derived, and the CPU interrupt is an
edge - which is what unstuck the boot (bug 12).

### CD-ROM - the common path only

Implemented: the register file and both FIFOs, the interrupt-and-acknowledge
scheme with delayed responses, and Getstat, Setloc, Play, ReadN/ReadS,
MotorOn, Stop, Pause, Init, Mute/Demute, Setfilter, Setmode, Getparam,
GetlocL/GetlocP, GetTN, GetTD, SeekL/SeekP, Test, GetID and GetTOC.

Not implemented: XA-ADPCM and CD-DA audio, the sector filter actually
filtering, the lid-open interrupt, and realistic seek timing (seeks currently
take a fixed number of cycles regardless of distance).

### Physical drives - data tracks only

`Disc::OpenDevice` reads a mounted drive through the filesystem layer, which
gives cooked 2048-byte sectors and one implied data track. A real TOC and
audio tracks need `IOCTL_CDROM_RAW_READ` and the drive's own track map.

### MDEC - absent

No motion decoder at all, so no FMV.

### Load delay slots - not modelled

`LB`, `LH`, `LW`, `LBU`, `LHU`, `LWL`, `LWR` write their destination register
immediately; the delay is commented out in each. Well-written code is
unaffected, because the assembler inserts the `nop`. Code that relies on
reading the *old* value in the delay slot will differ.

### Instruction cache - removed from the data path, not modelled

See bug 2 in [Bugs-Found.md](Bugs-Found.md). `ICache2` was corrupting every
data read and has been taken out of that path. `ICache` and `WBuffer` in
`cpu.h` are unused. Nothing models the cache now, which is correct-but-slow
rather than fast-and-wrong.

### Cause's interrupt-pending bits are faked

`RaiseException` fills `Cause` bits 15:8 by copying `SR`'s interrupt-mask bits
rather than reflecting `I_STAT`. It happens to set IP2 when IM2 is set, which
is enough for the BIOS handler to recognise an interrupt, but it is not what
the hardware does. Software that reads Cause to decide which device interrupted
will get the wrong answer.

### Cycle timing - approximate

`Cpu::Tick` is called once or twice per instruction with no regard to the
actual cost of the instruction or the memory it touched. GPU and CD-ROM timing
are derived from that, so anything depending on precise timing is approximate
at best.

### GPU - complete enough to draw, not to be right

Not implemented: texture caching, the GPU's own drawing time (drawing is
instant), polygon clipping against the drawing area beyond a bounding-box test,
and the interlace field handling is a first approximation.

One known artifact: a rainbow smear behind the BIOS shell's menu entries. It has
been traced as far as "the bytes the BIOS uploaded are themselves wrong" - the
rasteriser never touches that VRAM page. See "Where it stands" in
[Roadmap.md](Roadmap.md) for everything ruled out and the next probe.

## Blocking use

### No settings, no save states

No `emuconfig.h`, no `settings.h`, no `serializer.h`. The BIOS path is a
parameter to `System::Initialize` rather than a setting. Nothing serializes.

When save states go in, section 5 of the standards document applies directly:
audit every `serialize` for emptiness, and audit every object that holds state
but is not in the device list - memory cards, the CD drive's seek state, SPU
voice state, GTE registers.

### The front end is minimal

`PSXEmu.Win32` presents the framebuffer, mounts discs, maps the keyboard to a
digital pad, and resets. There is no settings UI, no debugger, no save-state
UI, no audio, and no gamepad support.

---

## Not gaps

Things that are actually done, listed so they are not re-investigated.

- **The MIPS R3000A instruction set** is complete for the base integer set:
  every arithmetic, logical, shift, branch, jump, load and store opcode,
  including the unaligned `LWL`/`LWR`/`SWL`/`SWR` group and the `HI`/`LO`
  multiply and divide pair. `COP0` handles `MFC0`/`MTC0`/`RFE`.
- **The memory map** is complete and decoded on physical addresses, so KUSEG,
  KSEG0 and KSEG1 all reach the same registers. RAM mirroring, the scratchpad,
  the expansion region and the cache-control register at `0xFFFE0130` are all
  handled, in all three access widths.
- **Exceptions** are raised with the right codes and vectors, and an interrupt
  now sets `EPC` to the instruction that has not yet run.
- **Root counters 0-2** count, compare against their targets and raise their
  interrupts.
- **Vertical blank** comes from the GPU's scanline counter, which is where it
  belongs. An invented fourth root counter used to fake it; that is gone.
- **Disc images** load from `.cue`, `.bin`, `.img`, `.iso` and a drive letter,
  with the sector layout detected rather than assumed, and the 150-sector
  lead-in accounted for. Covered by `media_test`.
- **The BIOS loader** rejects a missing or wrong-sized dump instead of carrying
  on with a buffer full of zeroes.
- **The PS-EXE loader** validates the `PS-X EXE` magic and bounds-checks the
  load address before writing into RAM.
- **`psx/emu.h` and `psx/emu.cpp`** are an earlier iteration superseded by
  `system.*`. They are kept in the tree but built by neither the solution nor
  the harnesses.
- **`utilities/cdrom/cdrom.cpp`** is the old host-CD read, superseded by
  `Disc::OpenDevice`. Kept, not built.
