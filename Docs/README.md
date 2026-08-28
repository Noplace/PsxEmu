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
- `media_test`: 56 checks over disc images and the CD-ROM controller. All
  passing.
- A software GPU that owns VRAM and produces a framebuffer.
- Disc images: `.cue`, `.bin`, `.img`, `.iso`, and a physical drive.
- A controller port with the digital pad protocol.
- A Win32 front end presenting the framebuffer through Direct3D 11.

**Not working:** the BIOS boot does not reach a picture. The exception
machinery works and the first vertical blank is handled cleanly, but the second
one is entered and never returned from. See "Where it is stuck" in
[Roadmap.md](Roadmap.md) - `boot_runner` prints the Cop0 status history that
shows it.
