# Docs

Working notes for anything spanning more than one sitting. Status per document.

| Document | Status | Purpose |
|---|---|---|
| [Emulator-Project-Standards.md](Emulator-Project-Standards.md) | reference | The structure and working practices this project is being rebuilt to, copied from GBAEmu |
| [Roadmap.md](Roadmap.md) | live | The step-by-step plan, phase by phase, what is done and what is next, and where the boot is currently stuck |
| [Project-Layout.md](Project-Layout.md) | live | How the projects split, include conventions, how to build, how to run |
| [Test-Suite.md](Test-Suite.md) | live | The two harnesses, and the baselines to check after any change |
| [Gaps.md](Gaps.md) | live | Hardware and features still missing, ordered by impact. Also what is deliberately not done |
| [Bugs-Found.md](Bugs-Found.md) | live | Bugs fixed in the revived code, with the symptom each produced |

## Where things stand

A PlayStation 1 emulator revived from a 2012-2014 codebase.

**Working:**

- Builds clean under MSVC 14.51 (`v145`), `/std:c++20 /permissive-`, all four
  of Debug/Release x Win32/x64, plus two headless harnesses.
- A software GPU that owns VRAM and produces a framebuffer.
- A CD-ROM controller and a disc layer that reads `.cue`, `.bin`, `.img`,
  `.iso` and a physical drive. 56 protocol-level checks, all passing.
- A controller port with the digital pad protocol.
- A Win32 front end that presents the framebuffer through Direct3D 11, mounts
  discs and takes keyboard input.

**Not working:** the BIOS boot does not reach a picture. It runs into the shell
and takes a vertical blank, but interrupts then stop being delivered. See
"Where it is stuck" in [Roadmap.md](Roadmap.md) for the two measurements that
narrow it down, and [Gaps.md](Gaps.md) for everything still missing.
