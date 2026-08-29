# Docs

Working notes for anything spanning more than one sitting. Status per document.

| Document | Status | Purpose |
|---|---|---|
| [Emulator-Project-Standards.md](Emulator-Project-Standards.md) | reference | The structure and working practices this project is being rebuilt to, copied from GBAEmu |
| [Roadmap.md](Roadmap.md) | live | The plan phase by phase, what is done, and exactly where the boot is stuck |
| [Project-Layout.md](Project-Layout.md) | live | How the projects split, include conventions, how to build, how to run |
| [Test-Suite.md](Test-Suite.md) | live | The three harnesses, and the baselines to check after any change |
| [Gaps.md](Gaps.md) | live | Hardware and features still missing, ordered by impact. Also what is deliberately not done |
| [Bugs-Found.md](Bugs-Found.md) | live | Bugs fixed in the revived code, with the symptom each produced |

## Where things stand

A PlayStation 1 emulator revived from a 2012-2014 codebase.

**Working:**

- Builds clean under MSVC 14.51 (`v145`), `/std:c++20 /permissive-`, all four
  of Debug/Release x Win32/x64, plus three headless harnesses.
- `cpu_test`: 181 checks over the instruction set, the memory map, exceptions
  and the interrupt path. All passing.
- `gte_test`: 99 checks over the geometry coprocessor. All passing.
- `media_test`: 103 checks over disc images, the CD-ROM controller, ISO9660,
  SYSTEM.CNF and the disc boot. All passing.
- A software GPU that owns VRAM and produces a framebuffer.
- DMA, the interrupt path, timers and the controller port.
- Disc images: `.cue`, `.bin`, `.img`, `.iso`, and a physical drive.
- A controller port with the digital pad protocol.
- A Win32 front end presenting the framebuffer through Direct3D 11.

**The BIOS boots and renders its whole intro** - the Sony diamond, "SONY" above
it and "COMPUTER ENTERTAINMENT" below, fading in - and then reaches the shell
menu, polls the controller port and issues CD-ROM commands.

**The GTE is implemented** - all 22 commands, the register file, saturation and
the FLAG register - with `gte_test` covering it in 99 checks. It has not been
exercised by real software yet: the BIOS shell issues zero GTE commands.

**Discs boot.** ISO9660 and SYSTEM.CNF are read, and the executable a disc names
is loaded and started - `boot_runner --boot-disc`, or File > Boot disc.

**What is still wrong:** a rainbow smear behind the two menu entries, narrowed
to the uploaded texture data rather than the rasteriser; and the disc boot goes
around the BIOS rather than through it. See "Where it stands" in
[Roadmap.md](Roadmap.md).
