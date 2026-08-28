# Roadmap

The step-by-step plan to get this from a revived 2012 codebase to a working
PlayStation 1 emulator, following
[Emulator-Project-Standards.md](Emulator-Project-Standards.md).

Phases are ordered so that each one is testable when it lands. The ordering
principle throughout: **nothing is "done" until a harness can show it working
without a human looking at a window.**

---

## Phase 0 - Make it build, and make it testable — DONE

The starting state was a single Win32 application project on `v142`, x86 only,
depending on a `WinCore` library that is not in this repository, with the GPU
implemented directly against D3D11.

- [x] Split into `PSXEmu.Core` (all emulation, no UI framework dependency) and
      `PSXEmu.Win32` (the front end). `git mv` throughout so history survives.
- [x] Replace the missing `WinCore` dependency with `PSXEmu.Core/platform/`.
- [x] Core folder is the include root, one aggregate header (`psx/psx.h`).
- [x] `GpuCore` no longer knows about `HWND`; the core owns VRAM.
- [x] Build clean under `v145` / `/std:c++20` / `/permissive-`, all four of
      Debug/Release x Win32/x64.
- [x] Headless harnesses plus `build_tools.bat`, compiling the core sources
      directly rather than through the solution.
- [x] Make unimplemented paths countable rather than silent (`TrapCounter`).

## Phase 1 - Devices the boot needs — MOSTLY DONE

- [x] **Software GPU**: 1 MB VRAM, GP0/GP1, CPU↔VRAM and VRAM↔VRAM transfers,
      flat/Gouraud/textured triangles, lines, rectangles, semi-transparency,
      dithering, 15- and 24-bit display resolve.
- [x] GPU owns display timing and raises the vertical-blank interrupt,
      replacing an invented fourth root counter that had been faking it.
- [x] **CD-ROM controller** at `0x1F801800-0x1F801803`: command and response
      FIFOs, the interrupt-and-acknowledge scheme, delayed responses, sector
      reading, and the commands the BIOS uses.
- [x] **Disc images**: `.cue` sheets (multi-track, multi-file), `.bin`/`.img`
      raw, `.iso` cooked, and a physical drive by letter. Sector size is
      detected from the file; sync and header are synthesised for images that
      do not store them.
- [x] **DMA channel 3** (CD-ROM to RAM).
- [x] **SIO0** controller port with the digital pad protocol, and an empty slot
      that correctly reports itself empty.
- [x] Seven correctness bugs in the revived CPU and I/O - see
      [Bugs-Found.md](Bugs-Found.md).
- [x] `media_test`: 56 protocol-level checks over the disc layer and the
      controller, with no BIOS and no window. All passing.
- [ ] **The boot still does not reach a picture.** See "Where it is stuck".

## Phase 2 - The GTE

`gte.cpp` implements one command out of about thirty, and that one is
incomplete. Nothing 3D can work until this is real.

- [ ] All 30-odd commands: RTPS/RTPT, NCLIP, AVSZ3/4, MVMVA, NCDS/NCDT, CC/CDP,
      DPCS/DPCT, INTPL, SQR, OP, GPF/GPL, NCS/NCT, NCCS/NCCT.
- [ ] Saturation and the FLAG register, which games read.
- [ ] The unsigned Newton-Raphson divide, including the overflow case.
- [ ] `MFC2`/`MTC2`/`CFC2`/`CTC2`/`LWC2`/`SWC2` in the CPU - `COP2` currently
      dispatches the command form and then unconditionally traps.
- [ ] **amidog's GTE test suite from day one.** Thirty commands, and no game
      will tell you which one is wrong.

## Phase 3 - Making games boot

- [ ] ISO9660 walk to find and parse `SYSTEM.CNF`, and side-load the executable
      it names.
- [ ] CD audio (CD-DA and XA-ADPCM) feeding the SPU mixer.
- [ ] Memory cards: the SIO0 `0x81` device, the file format in `psx/mc.h`, and
      save files on disk.
- [ ] Raw reads from a physical drive (`IOCTL_CDROM_RAW_READ`) so audio tracks
      and a real TOC work, not just data tracks.
- [ ] MDEC, for full-motion video.

## Phase 4 - SPU

`spu.cpp` has the register file but no mixer.

- [ ] 24 voices: ADPCM decode, ADSR envelopes, pitch and interpolation.
- [ ] Reverb, voice on/off edges, the IRQ address.
- [ ] SPU RAM and DMA channel 4.
- [ ] An `IAudioEngine` interface in Core the front ends implement, as GBAEmu
      does - the core must not know which front end is running.

## Phase 5 - Timing and accuracy

- [ ] Load delay slots. Currently not modelled: `LB`/`LW` write the register
      immediately, with the delay commented out.
- [ ] Instruction cache as a real cache, or not at all. The current `ICache2` is
      a broken half-model that was actively corrupting data reads until it was
      taken out of that path.
- [ ] Per-instruction cycle counts, and memory access penalties.
- [ ] DMA transfer timing rather than instant completion.
- [ ] amidog's CPU test suite as the regression gate.

## Phase 6 - Settings, save states, more front ends

- [ ] `psx/emuconfig.h` and `psx/settings.h`, copied from GBAEmu more or less
      verbatim. The BIOS path and the disc path become settings.
- [ ] `psx/serializer.h` and `serialize()` on every device. Per section 5 of the
      standards document, audit every one for emptiness, and audit every object
      holding state that is not in the device list: memory cards, CD seek state,
      SPU voice state, GTE registers.
- [ ] ImGui in the Win32 front end, for a debugger and settings UI.
- [ ] `PSXEmu.Interop` and `PSXEmu.WinUI`, if wanted - the design in GBAEmu's
      `Docs/WinUI-Interop.md` transfers whole.

---

## Where it is stuck

After the interrupt fix (bug 7), the BIOS:

- runs from reset through kernel init into the shell
- takes a vertical-blank interrupt, dispatches it through the BIOS handler
  chain correctly, and delivers the event
- then stops: `SR = 0x404`, meaning the machine believes it is still inside an
  exception, and no further interrupt is ever enabled

Two facts to start from next time, both straight out of `boot_runner`:

- **7 RFEs executed in a 400-frame run.** Exception returns are happening, but
  only a handful, and the last one never happens.
- **`I_STAT` is written once.** The BIOS's vertical-blank handler is reached and
  returns 1 ("mine"), but the acknowledge that should follow never lands, so
  `I_STAT` stays at 9 forever.

The most likely next culprits, in order: the syscall path (`EnterCriticalSection`
/ `ExitCriticalSection` are how the BIOS toggles `SR`, and only 7 exception
returns for the whole run is far too few), and then whether `Cause`'s IP bits
should reflect `I_STAT` rather than being copied out of `SR.IM` as they are now.

## A note on the interrupt fix making things look worse

Fixing bug 7 moved the numbers the wrong way: before it, the boot reached
640x478 with 890 GP0 words and 1.5M pixels plotted; after it, 256x240 with 3 GP0
words and nothing drawn.

The fix is still right - `EPC` must point at the instruction that has not run
yet, and the old behaviour was re-executing instructions. The boot had been
getting further *by accident*, on a path where interrupts were silently dead and
the shell was falling through its timeouts into code that happened to touch the
GPU.

Reverting a correct fix to recover a better-looking number is exactly the
overfitting section 6 of the standards document warns about. It stays, and the
baseline in [Test-Suite.md](Test-Suite.md) records both sets of numbers.
