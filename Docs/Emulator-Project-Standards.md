# Emulator project standards

**This document is meant to be copied into another emulator project.** It
describes the structure and working practices used in GBAEmu so a second
emulator — a PlayStation 1 core, revived from an older repository — can be built
the same way, and so anything already solved here can be pointed at rather than
reinvented.

Reference implementation: `C:\dev\Noplace\GBAEmu`. Where this document says "see
GBAEmu", the file paths are relative to `Solution/`.

---

## 1. Project structure

Four projects, one folder each, so the emulation core is shared rather than
owned by a UI. For a PS1 core the names map directly:

| GBAEmu | PS1 equivalent | What it is |
|---|---|---|
| `GBAEmu.Core` | `PSXEmu.Core` | Static library. All emulation. No UI framework dependency. |
| `GBAEmu.Interop` | `PSXEmu.Interop` | DLL exporting a flat **C ABI** over the core, for managed hosts. |
| `GBAEmu.Win32` | `PSXEmu.Win32` | Win32 + D3D12 + ImGui front end. |
| `GBAEmu.WinUI` | `PSXEmu.WinUI` | WinUI 3 (C#) front end, calling the interop DLL. |

**The rule that matters: the core never knows which front end is running.** It
exposes interfaces the host implements (input, audio) and buffers the host
reads (video). Anything a front end needs from the core goes through those,
never the other way round.

Two consequences worth internalising, both learned the hard way here:

- **Effects that belong to the machine go in the core, not the UI.** GBAEmu
  rotates the framebuffer to convey a tilt sensor; it does that in
  `Kernel::publish_frame`, on the way from the GPU to the buffers the front ends
  read. Doing it in a front end would have meant doing it twice; doing it in the
  GPU's own buffer would have polluted every test baseline.
- **Shared code moves into Core the moment a second front end wants it.** The
  audio engines and the composite/gamepad input devices started in the Win32
  project and moved when WinUI appeared. Core already includes `windows.h`, so
  it is Windows-specific rather than platform-neutral; what it avoids is a
  dependency on a particular *UI framework*.

Also present, and worth keeping:

```
Solution/
  Docs/                     working notes (see section 7)
  <Core>/tools/             headless test harnesses, built by a .bat, not the solution
  Tools/                    one-off utilities, not built by the solution
```

## 2. Upgrading to VS2026

The GBAEmu solution is `Format Version 12.00`, `# Visual Studio Version 18`. An
older PS1 project will likely be on v141 (VS2017) or v142 (VS2019).

Concrete steps for each `.vcxproj`:

| Property | Set to | Note |
|---|---|---|
| `PlatformToolset` | `v145` | VS2026's toolset, backed by MSVC 14.51 |
| `WindowsTargetPlatformVersion` | `10.0` | Means "latest installed", not a pinned SDK |
| `LanguageStandard` | `stdcpp20` | |
| `ConformanceMode` | `true` | |

**Verified:** the GBAEmu core compiles clean under `v145` with no new warnings —
tested with `-p:PlatformToolset=v145`. GBAEmu itself is still on `v143`, so
either works; `v145` is the right target for a project being modernised anyway.

Expect the real work of the upgrade to be *conformance*, not the toolset switch.
`/permissive-` plus C++20 rejects a lot of what compiled in 2017: missing
`typename`, two-phase lookup in templates, `for` loop scoping, implicit
narrowing in braced initialisers, and anything relying on the old `auto_ptr`
family. Fix the code rather than turning conformance off.

For the C# side, GBAEmu targets `net10.0-windows10.0.26100.0` with
`WindowsAppSDK 2.4.0`.

## 3. Include conventions

The core folder is an include root. Everything — core and consumers alike —
uses paths relative to it:

```cpp
#include "emulation/gba.h"        // -> "psx/psx.h"
#include "audio/iaudioengine.h"
#include "framework.h"
```

Front ends put `$(ProjectDir);$(SolutionDir)<Core>` on their include path, so
their own headers resolve first and core headers second.

There is one aggregate header (`emulation/gba.h`) that includes the rest in
dependency order. **Order in that file is load-bearing:** a type used inline in
another header must be included before it. Adding a device header after
`kernel.h` will fail to compile if `Kernel` uses it inline.

## 4. Settings

One struct, one home. In GBAEmu that is `emulation/emuconfig.h`:

- **Compile-time switches** pick an implementation (`GBA_TIMER_CORE_2` selects
  which timer core is built).
- **`struct EmuConfig`** holds runtime settings.

The config is stored **by value on the Bus**, so every component — all of which
already hold a `Bus&` — reads it as `bus_.config().<field>` with no indirection
and no per-class copies.

Three rules that came out of getting this wrong first:

1. **Never let a component keep its own copy of a setting.** Read the config.
2. **`Kernel::Initialize` must not reset the config.** Settings outlive a disc
   or cartridge; resetting throws away the user's file the moment a second game
   is loaded. Default-construct it with the Bus instead.
3. **Emulated register state is not a setting**, even when it looks like a
   boolean. Nor are measured timing constants — those stay `constexpr` beside
   the code they describe, so they cost nothing at runtime.

### Persistence

`emulation/settings.h` — a flat `key = value` file, hand-editable, with
`store_config` / `load_config` helpers. Copy this file more or less verbatim.

Three things it gets right that are easy to miss:

- **Unknown keys are preserved.** A file written by a newer build is not
  stripped by an older one.
- **Every getter takes the current value as its default**, so a missing key
  leaves whatever was already there rather than forcing a hardcoded default.
- **Saving only at exit loses everything to a crash or a kill.** `SaveSettings`
  compares against what is on disk and writes only on a difference, so it is
  safe to call every couple of seconds as well as on shutdown.

Settings live beside the save files (`Documents\My Games\<Emu>\settings.ini`),
so there is one folder to back up.

## 5. Save states

Devices implement `serialize(Serializer&)`, and the kernel walks them. Read
`emulation/serializer.h` — it is about fifty lines and does read and write
through one code path, which is what stops save and load drifting apart.

**The failure mode to design against: a device that silently saves nothing.**
Both of these were live bugs here.

- `EEPROMDevice::serialize` was empty, so cartridge saves were absent from save
  states entirely.
- Cartridge peripherals hang off a GPIO port rather than the bus, so the
  kernel's device loop never reached them, and `GamePakROM::serialize` was
  empty. Anything not on the bus needs an explicit hook.

So, for a PS1 core: **audit every `serialize` for emptiness, and audit every
object that holds state but is not in the device list** — memory cards, the CD
drive's seek state, SPU voice state, GTE registers.

Two more rules:

- **Derived state does not need saving; in-flight state does.** GBAEmu's RTC
  reads the host clock, so the date and time cannot drift and are not saved —
  but the command being shifted in is, because a state saved mid-transfer must
  resume on the same bit.
- **Shared objects are saved once.** Three ROM mirrors each have their own GPIO
  registers (saved three times, correctly) but share the attached devices
  (saved once, by the kernel).

Changing what is serialized changes the format and invalidates existing states.
Say so when it happens.

## 6. Testing

This is the part that made the difference here, and it is worth setting up
before writing much emulation.

### Headless harnesses

`<Core>/tools/` holds console programs that compile the core sources directly
(via a `.bat`, not the solution, so they stay independent of the MSBuild
configuration). They take no window, no input, no audio device.

GBAEmu has three, and the shapes generalise:

| Harness | Shape |
|---|---|
| `suite_runner` | Boots a test ROM, drives its menu with a scripted keypad, dumps the log it writes to save memory. Also a mode that runs a real game and prints a framebuffer checksum every N frames. |
| `video_runner` | For tests that are visual and report nothing: drives the ROM's own "expected vs actual" viewer and compares framebuffers. |
| `peripheral_test` | Pure unit tests — drives devices through their protocols with no ROM at all. |

The PS1 equivalents: amidog's CPU and GTE tests are the established suites, and
**PSX-SPX** is the hardware reference (the equivalent of GBATEK).

### Regression discipline

Keep a `Docs/Test-Suite.md` with **baselines**: per-suite failure counts, and
framebuffer checksums for a handful of real games at a fixed frame number. Check
both after every change to the CPU, timing or the renderer — not just the part
being worked on.

Two traps this caught, both of which cost real time:

- **Line counts are not failure counts.** Some suites log every result. Count
  the actual failure marker.
- **Clear every save file before checksumming a game.** A game that finds a save
  boots differently and gives a different, perfectly reproducible checksum. In
  GBAEmu that meant clearing `*.eep` *and* `*.sav` — clearing only one produced
  a phantom regression and a wrong diagnosis before the real cause turned up.

### When you cannot test against real software

Write the test at the protocol level instead, and **test the direction, not just
the magnitude**. A sign error in a rotation or an axis is easy to make and
invisible without the one game that uses it. Where a device is written, wiped
and read back, the *wipe* is the point — without it, a `serialize` that stores
nothing still appears to round trip.

### Do not overfit

The GBA timer suite has 95 failures that look like a constant begging to be
tuned. One attempt improved that suite by 577 results while breaking 74 in two
others. If a fix cannot be justified from the hardware's behaviour, it is a
guess — record it as a guess or leave it.

## 7. Documentation

`Solution/Docs/` holds working notes for anything spanning more than one
sitting, with a `README.md` indexing them and a status per document. The set
here, which maps directly:

| Document | Purpose |
|---|---|
| `Project-Layout.md` | How the projects split, include conventions, settings |
| `Test-Suite.md` | Scores, what each remaining failure is, regression baselines |
| `Gaps.md` | Features and hardware still missing, ordered by impact |
| `<Feature>.md` | Plan and status for anything half-finished |

Two conventions worth keeping:

- **Record what was tried and failed**, with the numbers. The timer overfitting
  note above exists so nobody rediscovers it.
- **Record what is deliberately not done**, and why. A "not gaps" section listing
  the things that are actually complete stops them being re-investigated.

## 8. Build gotchas

Every one of these cost time here.

- **`Win32` output has no platform folder.** MSBuild puts x64 in
  `$(SolutionDir)x64\$(Configuration)\` but Win32 in
  `$(SolutionDir)$(Configuration)\`. Anything computing a path to a native
  binary must special-case it.
- **vcpkg manifests are per-project.** A project that references none of the
  packages should set `VcpkgEnabled=false`. Leaving it on made two projects
  install an ImGui variant they never used, which then failed to build for x86
  because that variant does not exist there.
- **C# needs `RuntimeIdentifiers` (plural).** Restore only writes assets for the
  RIDs listed, so with one RID, switching platform fails with `NETSDK1047`.
- **A native DLL must join the publish set, not just be copied.** The .NET SDK
  builds publish output from the `ResolvedFileToPublish` item group; a file
  copied in a target that runs `AfterTargets="Publish"` is simply absent. The
  publish *succeeds* and the app then dies at startup with `DllNotFoundException`.
- **`AnyCPU` should mean the same platform everywhere.** One project mapping it
  differently silently sent publishes to a 32-bit build.

## 9. The interop layer

If the PS1 project also wants a WinUI front end, the design here transfers
whole — see `Docs/WinUI-Interop.md`. The essentials:

- **C# cannot link a C++ static library.** A thin DLL exporting a flat C ABI is
  the least-work bridge, and leaves the core and the Win32 app untouched.
- **Implement the core's callback interfaces natively, inside the DLL.** Do not
  pass function pointers back into managed code. Audio is called from the
  emulation thread, so a GC pause there stalls emulation.
- **Hand the host a copy of the framebuffer, not a pointer.** The pointer's
  validity depends on a lock the managed side would have to hold across the
  boundary. A 150 KB copy at 60 Hz does not show up in a profile.

## 10. What differs for PS1

Structure and practices transfer; the machine does not. Expect these to be new
work rather than ports:

- **MIPS R3000A** instead of ARM7TDMI, with a GTE coprocessor that has no GBA
  analogue and needs its own test suite from day one.
- **A BIOS is required**, not optional. GBAEmu embeds a GBA BIOS in a header and
  has a half-finished HLE replacement (`Docs/HLE-BIOS.md`); for PS1, plan on the
  user supplying a dump and wire the path to a setting from the start.
- **CD-ROM instead of a cartridge.** Disc images, seek timing and streaming
  replace the gamepak bus and its prefetch. There is no cartridge-peripheral
  equivalent, but memory cards need the same save-file and save-state care that
  section 5 describes.
- **The GPU renders 3D.** GBAEmu's PPU is a scanline renderer producing a fixed
  240x160 buffer; keeping the same "core owns the framebuffer, front end just
  presents it" boundary is still right, but what fills the buffer is a different
  problem.

What does transfer nearly unchanged: the project split, `settings.h`, the
`Serializer`, the headless harness pattern, the docs layout, and every gotcha in
section 8.
