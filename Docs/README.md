# Docs

Working notes for anything spanning more than one sitting. Status per document.

| Document | Status | Purpose |
|---|---|---|
| [Emulator-Project-Standards.md](Emulator-Project-Standards.md) | reference | The structure and working practices this project is being rebuilt to, copied from GBAEmu |
| [Roadmap.md](Roadmap.md) | live | The plan phase by phase, what is done, and exactly where the boot is stuck |
| [Project-Layout.md](Project-Layout.md) | live | How the projects split, include conventions, how to build, how to run |
| [Test-Suite.md](Test-Suite.md) | live | Every harness, and the baselines to check after any change |
| [Gaps.md](Gaps.md) | live | Hardware and features still missing, ordered by impact. Also what is deliberately not done |
| [Bugs-Found.md](Bugs-Found.md) | live | Bugs fixed in the revived code, with the symptom each produced |
| [Threading-Plan.md](Threading-Plan.md) | live | **Built 2026-09-18**: a thread each for the window, the machine, video, audio and input, with the channels between them in `PSXEmu.Core/host/` and `host_test` holding them. The standard it follows is DuckStation's, PCSX2's and Dolphin's; phase 7, a thread for the rasteriser, is the part not done |
| [Emulation-Speed-Plan.md](Emulation-Speed-Plan.md) | proposed | 50/100/150/200% speed, and why the audio path is the whole job |
| [Recompiler-Plan.md](Recompiler-Plan.md) | live | Dynamic recompilation, on the **Emulation > Recompiler** menu and off by default. 3.0-3.9x real time against the interpreter's 1.5-1.7x; BIOS boot identical. The differential harness has both CPUs agreeing exactly for millions of instructions - what is left is cycle accounting, not correctness |
| [GPU-SPU-Optimisation-Plan.md](GPU-SPU-Optimisation-Plan.md) | proposed | Finding out whether the rasteriser or the SPU is the bottleneck, and what to do about each |

## Where things stand

A PlayStation 1 emulator revived from a 2012-2014 codebase.

**Working:**

- Builds clean under MSVC 14.51 (`v145`), `/std:c++20 /permissive-`, all four
  of Debug/Release x Win32/x64, plus thirteen headless harnesses.
- **1,002 checks across the eight emulation harnesses, 0 failures**, and five
  more harnesses beside them - the recompiler's 460, the threads' 32, and the
  three around `platform/`. [Test-Suite.md](Test-Suite.md) has the table.
- A software GPU that owns VRAM and produces a framebuffer.
- DMA, the interrupt path, timers and the controller port.
- Disc images: `.cue`, `.mds`/`.mdf`, `.bin`, `.img`, `.iso`, and a physical
  drive.
- A controller port with the digital pad, DualShock, mouse and multitap.
- A Win32 front end on five threads - window, machine, video, audio, input -
  presenting through Direct3D 11 or 12, with WASAPI or DirectSound.

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
