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

## Phase 1 - Devices the boot needs — DONE

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
- [x] **DMA channels 2, 3 and 6**: linked-list, block and burst for the GPU,
      CD-ROM to RAM, and the ordering-table clear.
- [x] **SIO0** controller port with the digital pad protocol, and an empty slot
      that correctly reports itself empty.
- [x] Fifteen correctness bugs in the revived CPU, DMA, GPU and I/O - see
      [Bugs-Found.md](Bugs-Found.md).
- [x] `gte_test`: 99 checks over the geometry coprocessor. All passing.
- [x] `cpu_test`: 181 checks over the instruction set, the memory map,
      exceptions and the interrupt path. All passing.
- [x] `media_test`: 103 protocol-level checks over the disc layer, the
      controller, ISO9660 and the disc boot. All passing.
- [x] **The BIOS boots and renders its whole intro** - the Sony diamond, "SONY"
      above it and "COMPUTER ENTERTAINMENT" below, fading in - and then reaches
      the shell menu, polls the controller port and issues CD-ROM commands.

## Phase 2 - The GTE — DONE, pending a real 3D workload

- [x] **All 22 commands**: RTPS/RTPT, NCLIP, AVSZ3/4, MVMVA, NCDS/NCDT,
      CC/CDP, DPCS/DPCT, INTPL, DCPL, SQR, OP, GPF/GPL, NCS/NCT, NCCS/NCCT.
- [x] **The full register file** - 32 data and 32 control registers, with the
      screen, depth and colour FIFOs, the IRGB/ORGB packing, LZCS/LZCR, and
      the read-back quirks (H sign-extends, ORGB and LZCR are read-only, SXYP
      pushes rather than stores).
- [x] **Saturation and the FLAG register**, including the derived error bit and
      the RTPS IR3 quirk where the flag is judged on a differently shifted
      value from the one stored.
- [x] **The unsigned Newton-Raphson divide** off its 257-entry table,
      including the overflow at H >= 2*SZ3.
- [x] `MFC2`/`MTC2`/`CFC2`/`CTC2` in `COP2`, and `LWC2`/`SWC2`, which were
      `UNKNOWN` in the opcode table.
- [x] `gte_test`: 99 checks over the register file, saturation, and every
      command with an independently derived expected value. All passing.
- [ ] **Validate against a real 3D workload.** The BIOS shell issues *zero*
      GTE commands - it is entirely 2D - so nothing here has been exercised by
      real software yet. amidog's GTE suite, or any 3D game, is the next check.
- [ ] MVMVA's garbage matrix (matrix select 3) is implemented from the
      description rather than from measurement; it is not something software
      uses deliberately.

## Phase 3 - Making games boot

- [x] **ISO9660**: the primary volume descriptor, directory walk, and file
      lookup by path in every form software writes it - bare name, leading
      slash of either kind, `cdrom:` prefix, `;1` version suffix, any case.
- [x] **SYSTEM.CNF**: the BOOT line parsed out and the executable it names
      loaded, with `PSX.EXE` as the fallback for discs that omit the file.
- [x] `System::BootDisc` reports *which step* failed rather than just failing,
      through `DiscBootInfo`.
- [x] `boot_runner --boot-disc` and a **Boot disc** menu item in the front end.
- [x] 47 more checks in `media_test`, over a synthetic ISO9660 image the test
      builds itself.
- [ ] Boot through the BIOS rather than around it. `BootDisc` side-loads the
      executable directly; the BIOS's own boot path needs more of the CD-ROM
      drive than is implemented.
- [ ] Directories are walked but untested beyond the root - no PlayStation
      disc puts its executable in a subdirectory, but the code path exists.
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

- [ ] amidog's CPU suite on top of `cpu_test`, which covers the instruction
      set but not its timing.
- [ ] Load delay slots. Currently not modelled: `LB`/`LW` write the register
      immediately, with the delay commented out.
- [ ] Instruction cache as a real cache, or not at all. The current `ICache2` is
      a broken half-model that was actively corrupting data reads until it was
      taken out of that path.
- [ ] Per-instruction cycle counts, and memory access penalties.
- [ ] DMA transfer timing rather than instant completion.

## Phase 6 - Settings, save states, memory cards, more front ends

Three of these now have plans of their own, written against the tree as it
actually is rather than from memory:

- [ ] **[MDEC-Plan.md](MDEC-Plan.md)** - the motion decoder. The last large
      missing component; blocks full-motion video and nothing else. Six steps,
      each landable on its own.
- [ ] **[Save-States-Plan.md](Save-States-Plan.md)** - what a state has to
      hold, what it must not, and the serialiser that has to exist first. The
      test is a checksum: save at frame 600, run to 900, and compare against
      loading that state and running 300.
- [ ] **[Memory-Cards-Plan.md](Memory-Cards-Plan.md)** - swapping first, then
      the editor. `mc.h` already declares the whole on-card format; nothing
      yet understands it.
- [ ] `psx/emuconfig.h` and `psx/settings.h`, copied from GBAEmu more or less
      verbatim. The BIOS path and the disc path become settings.
- [ ] ImGui in the Win32 front end, for a debugger and settings UI.
- [ ] `PSXEmu.Interop` and `PSXEmu.WinUI`, if wanted - the design in GBAEmu's
      `Docs/WinUI-Interop.md` transfers whole.

---

## Where it stands

The BIOS boots and renders its whole intro: the Sony diamond, "SONY" above it
and "COMPUTER ENTERTAINMENT" below, fading in, and then the shell menu with its
"MEMORY CARD" and "CD PLAYER" entries. The controller port is polled and the
CD-ROM is issued commands.

What is still visibly wrong, in order:

1. **A rainbow smear behind the two menu entries.** Investigated, narrowed,
   not yet fixed. What is now known, all of it from `boot_runner`:

   - Every pixel of that VRAM page is written by command `A0` - a plain
     CPU-to-VRAM upload. Nothing renders into it, so the rasteriser is not
     what produces it. (`--watch-vram 896,0,128,56`)
   - The upload completes: 3600 of 3600 pixels, and so does its partner at
     (704,0). Nothing is truncated.
   - The draws are command `2D` with the raw texpage attribute `010B` and
     `010E`, which decode to texture page 11 and 14 at 15-bit direct colour
     with no CLUT - and there is a 60x60 upload at exactly those addresses.
     So the BIOS really is asking for a 15-bit texture there, and the
     sampling depth is not wrong.
   - Not the mask bit: zero pixels are mask-rejected in a whole run.
   - Not texture disable: honouring it changed nothing.

   So the bytes the BIOS computed and uploaded are themselves wrong, which
   points back at the CPU or the DMA rather than at the GPU. The next probe is
   to capture the RAM the (704,0) upload reads from and compare it with the
   (896,0) one - at frame 340 the first renders as a clean smooth gradient and
   the second, later, does not.
2. **The GTE**, still one command out of about thirty. It has not stopped the
   intro, because that geometry is 2D, but nothing with real 3D will work
   until it is done.

Phase 2 is still the next big piece, and it needs amidog's GTE suite alongside
it from the first commit - thirty commands, and a wrong one shows up as "the
picture looks a bit off" and nothing more.

How the boot was unstuck, for the record: the Cop0 status history in
`boot_runner` showed the second vertical blank being entered and never returned
from. Following that back gave bug 12 - a finished DMA transfer leaving its
interrupt permanently asserted, which the BIOS had no handler for, so it took
its unhandled-exception path and unwound with a longjmp instead of an `rfe`,
leaving the status register stuck. Then bug 13, a GPUSTAT ready bit reported
only mid-transfer, was the last spin loop in the way.

And the intro text arrived with bug 14: DMA channel 2 block mode had been a
`BREAKPOINT` stub, so every texture the BIOS uploaded that way never reached
VRAM. The primitives still drew - sampling an empty page, where every texel
reads as transparent - which is why a run could report 1923 primitives and 73
million plotted pixels with no text on screen at all.

## A note on a correct fix making things look worse

Fixing bug 7 - the interrupt setting EPC to the instruction that had already
run - moved every visible number the wrong way at the time. Before it: 640x478,
890 GP0 words, 1.5M pixels. After it: 256x240, 3 GP0 words, a black screen.

The fix was kept anyway, because `EPC` must point at the instruction that has
not run yet and the old behaviour was re-executing instructions. The boot had
been getting further *by accident*, on a path where interrupts were silently
dead and the shell fell through its timeouts into code that happened to touch
the GPU.

That judgement turned out to be load-bearing. Bugs 12 and 13, found afterwards,
are what actually made the machine boot - and neither could have been seen at
all while interrupts were dead. Reverting bug 7 to recover a prettier number
would have hidden both of them behind a screen that looked better and worked
less.

This is section 6 of the standards document's point about overfitting, from the
other direction: the number to trust is the one you can explain.
