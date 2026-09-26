# Docs

Working notes for anything spanning more than one sitting. Status per document.

**Where to look first:** [Gaps.md](Gaps.md) for what is missing now,
[Test-Suite.md](Test-Suite.md) for the numbers to check after a change, and
[Bugs-Found.md](Bugs-Found.md) for why things are the way they are.

| Document | Status | Purpose |
|---|---|---|
| [Emulator-Project-Standards.md](Emulator-Project-Standards.md) | reference | The structure and working practices this project is being rebuilt to, copied from GBAEmu |
| [Project-Layout.md](Project-Layout.md) | live | How the projects split, include conventions, how to build, how to run |
| [Test-Suite.md](Test-Suite.md) | live | Every harness, and the baselines to check after any change |
| [Gaps.md](Gaps.md) | live | Hardware and features still missing, ordered by impact. Also what is deliberately not done. The most recently audited status document |
| [Bugs-Found.md](Bugs-Found.md) | live | Bugs fixed in the revived code, with the symptom each produced |
| [Roadmap.md](Roadmap.md) | history | The plan phase by phase. Phases 0-3 and 6 are accurate; for current status trust Gaps.md |
| [Threading-Plan.md](Threading-Plan.md) | built | **Built 2026-09-18**: a thread each for the window, the machine, video, audio and input, with the channels between them in `PSXEmu.Core/host/` and `host_test` holding them. The standard it follows is DuckStation's, PCSX2's and Dolphin's; phase 7, a thread for the rasteriser, is the part not done |
| [Recompiler-Plan.md](Recompiler-Plan.md) | live | Dynamic recompilation, on the **Settings > Emulation > Recompiler** menu and off by default. 3.0-3.9x real time against the interpreter's 1.5-1.7x; BIOS boot identical. The differential harness has both CPUs agreeing exactly for millions of instructions - what is left is cycle accounting, not correctness |
| [CPU-Timing-Plan.md](CPU-Timing-Plan.md) | live | Real per-instruction cycle costs. Multiply/divide and branches done (bug 43), and amidog's `psxtest_cpu` passes in full (bug 68); memory-region costs (phase 3) are not |
| [Memory-Cards-Plan.md](Memory-Cards-Plan.md) | built | Per-disc cards, an in-memory card written whole once a game stops writing, insert/create/eject per slot while running, and the Memory Card Editor. Bug 69 |
| [Debugger-Plan.md](Debugger-Plan.md) | done | An in-app CPU debugger: breakpoints, stepping, memory and watchpoints, halting mid-frame without changing what the machine computes. **Emulation > Debugger**: disassembly, registers, breakpoints, step into/over/out, run to cursor, and `boot_runner --break`. A memory pane that reads hardware registers without side effects, and memory and register editing. Read and write watchpoints, CPU and DMA, with `boot_runner --watchpoint`. A BIOS call log with breaks on a call, an approximate call stack, labels, and device panes. PsyQ `.SYM` files are not read |
| [Disc-Formats-Plan.md](Disc-Formats-Plan.md) | live | `.mds`/`.mdf`, `.ccd`/`.img` and `.chd` done; ECM and PBP not started |
| [Emulation-Speed-Plan.md](Emulation-Speed-Plan.md) | built | 50/100/150/200% speed, and why the audio path was the whole job |
| [GPU-SPU-Optimisation-Plan.md](GPU-SPU-Optimisation-Plan.md) | measured | Whether the rasteriser or the SPU is the bottleneck. Neither is: 4-13% and 3-4% of a run |
| [Save-States-Plan.md](Save-States-Plan.md) | built | `StateIO`, a `Serialise` on every component, F1-F8 slots. Bug 44 |
| [MDEC-Plan.md](MDEC-Plan.md) | built | The motion decoder. Bug 23 - and its step 3 described work the hardware does not do |
| [Wild-Arms-Press-Start-Plan.md](Wild-Arms-Press-Start-Plan.md) | fixed | Blank after "press start". Bugs 25-26 |
| [Ace-Combat-3-Input-Plan.md](Ace-Combat-3-Input-Plan.md) | fixed | Input never reaching the game. Bug 46 |
| [Air-Combat-FMV-Plan.md](Air-Combat-FMV-Plan.md) | fixed | Intro film decoding one cycle and stopping. Bug 55 |
| [FF7-Prelude-Pitch-Plan.md](FF7-Prelude-Pitch-Plan.md) | fixed | The prelude a twelfth flat. Bug 39 |

The "fixed" and "built" plans are kept as they were written, with a status line
at the top; the investigation in each is still the record of how the answer was
found.

## Where things stand

A PlayStation 1 emulator revived from a 2012-2014 codebase.

**Working:**

- Builds clean under MSVC 14.51 (`v145`), `/std:c++20 /permissive-`, all four
  of Debug/Release x Win32/x64, plus the headless harnesses.
- **1,297 checks across the ten emulation harnesses, 0 failures**, and more
  harnesses beside them - the recompiler's 460, the threads' 33, and the three
  around `platform/`. [Test-Suite.md](Test-Suite.md) has the table.
- The CPU with load delay slots and measured multiply/divide/branch costs; the
  GTE, passing amidog's `psxtest_gte` values and flags; a software GPU; the
  CD-ROM with CD-DA and XA-ADPCM; the MDEC; the SPU with reverb; DMA, timers,
  and the controller port with the digital pad, DualShock, mouse and multitap.
- Disc images: `.cue`, `.chd`, `.mds`/`.mdf`, `.ccd`/`.img`, `.bin`, `.img`,
  `.iso`, and a physical drive.
- Save states, per-disc memory cards, and 50-300% emulation speed.
- A dynamic recompiler, off by default, at 3.0-3.9x real time.
- A Win32 front end on five threads - window, machine, video, audio, input -
  presenting through Direct3D 11 or 12, with WASAPI or DirectSound.

**Games boot and play through the BIOS**, not around it: twelve discs are in the
baseline table, with intro films, XA audio and CD music. No game is known to be
blocked.

**What is still wrong** is timing rather than function: memory-region access
costs are modelled rather than measured, and the recompiler charges a flat
cycle per instruction, so a game's checksum differs between the two CPUs.
amidog's CPU suite now passes in full, TIMING column included (bug 68). See
[Gaps.md](Gaps.md) and [CPU-Timing-Plan.md](CPU-Timing-Plan.md).
