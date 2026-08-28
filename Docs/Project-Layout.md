# Project layout

Follows section 1 of [Emulator-Project-Standards.md](Emulator-Project-Standards.md).

```
PsxEmu/
  PSXEmu.sln
  PSXEmu.Core/                 static library - all emulation, no UI framework
    PSXEmu.Core.vcxproj
    platform/                  stands in for the WinCore library this repo does not have
      types.h                  fixed-width integer names
      timer.h                  steady_clock behind the old utilities::Timer shape
      util.h                   SafeDelete / SafeDeleteArray / SafeRelease
    psx/                       the machine
      psx.h                    aggregate header - order is load-bearing
      system.h/.cpp            owns every component, and the run loop
      cpu.h/.cpp               MIPS R3000A
      cpu_context.h            register file
      gte.h/.cpp               geometry coprocessor  (one command implemented)
      gpu_core.h               the interface the core talks to the GPU through
      gpu.h/.cpp               software GPU: VRAM, GP0/GP1, rasteriser
      cdrom.h/.cpp             CD-ROM controller: FIFOs, commands, interrupts
      disc.h/.cpp              disc images: cue, bin, img, iso, physical drive
      sio.h/.cpp               controller / memory card port
      spu.h/.cpp               sound registers  (no mixer yet)
      dma.h/.cpp               DMA channels
      io_interface.h/.cpp      memory map and hardware registers
      root_counter.h/.cpp      timers
      mc.h/.cpp                memory card file format
      kernel.h/.cpp            BIOS call logging
      debug.h                  BREAKPOINT, and the trap counter behind it
      debug_assist.h/.cpp      _DEBUG-only CSV instruction logger
      emu.h/.cpp               superseded by system.*; kept, not built
    utilities/
      cdrom/iso9660.h          ISO9660 structures
      cdrom/cdrom.cpp          old host CD read, superseded by disc.cpp; not built
      lean/hash_table.h
    tools/                     headless harnesses, built by a .bat, not the solution
      build_tools.bat
      boot_runner.cpp
      media_test.cpp
      disasm.h
  PSXEmu.Win32/                front end: a window, D3D11, input
    PSXEmu.Win32.vcxproj
    main.cpp                   window, menus, message loop, the frame loop
    d3d11_presenter.h/.cpp     uploads the core framebuffer and draws it
  bios/                        the user's BIOS dump
  Docs/
  Build/                       MSBuild output
  Temp/                        harness output, not in the solution
```

## The rule that matters

**The core never knows which front end is running.** `GpuCore` deliberately has
no `HWND` on it; the core owns VRAM and hands out a framebuffer, and a front end
reads it. That boundary is what lets `boot_runner` render and checksum frames
with no window, no device and no input.

This was not true of the code this was revived from: the GPU was a subclass
holding a D3D11 context, and drew straight to the swap chain with no VRAM at
all.

## Include conventions

`PSXEmu.Core/` is the include root. Everything - core and consumers alike - uses
paths relative to it:

```cpp
#include "psx/psx.h"
#include "platform/timer.h"
#include "tools/disasm.h"
```

`psx/psx.h` is the one aggregate header, and it includes the rest **in
dependency order**. A type used inline in another header must be included
before it: putting `gpu.h` after `system.h` will not compile, because `System`
holds a `Gpu` by value.

Front ends put `$(ProjectDir);$(SolutionDir)PSXEmu.Core` on their include path,
so their own headers resolve first and core headers second.

## Building

### The solution

```
msbuild PSXEmu.sln -p:Configuration=Release -p:Platform=x64
```

`v145` (MSVC 14.51), `WindowsTargetPlatformVersion` `10.0` meaning "latest
installed", `/std:c++20`, `/permissive-`. All four of
Debug/Release x Win32/x64 build clean.

Output goes to `Build\$(Platform)\$(Configuration)\` for **both** platforms.
That is deliberate: MSBuild's own default puts x64 under `$(SolutionDir)x64\`
but Win32 straight into `$(SolutionDir)` with no platform folder, and anything
computing a path to the binary then has to special-case it.

`VcpkgEnabled` is `false` on the core, which references no packages. Leaving it
on makes a project install variants it never uses, and some of those do not
exist for x86.

### The harnesses

```
PSXEmu.Core\tools\build_tools.bat
```

Compiles the core sources directly rather than through MSBuild, so they stay
independent of the solution configuration, and drops binaries in `Temp\tools\`.
Edit the `vcvars64.bat` path at the top if your Visual Studio install differs.

This is also the fastest way to get a compile error out of the core.

## Character set

The core is built Unicode, and calls the `A`-suffixed Win32 entry points
explicitly (`OutputDebugStringA`, `CreateFileA`) where it wants narrow strings.
Being explicit rather than relying on the project's character set is what keeps
the MSBuild build and the `cl`-driven harness build agreeing with each other.

## Where Direct3D fits

**Direct3D presents, and does nothing else.** Every PlayStation pixel is
rasterised on the CPU inside `PSXEmu.Core`, which owns VRAM;
`d3d11_presenter.cpp` uploads the finished frame into a dynamic texture and
stretches it over the window with a full-screen triangle. Nothing about the
PlayStation's drawing is expressed in shaders.

That boundary is the point. It is why `boot_runner` can render and checksum
frames with no graphics device at all, and why swapping the presenter for a
D3D12 or Vulkan one would change nothing else in the project.

The shaders are compiled from a string at startup rather than loaded from
`.cso` files beside the executable - which is how the 2012 front end did it,
with absolute paths baked in from somebody else's machine.

## Running the front end

    PSXEmu.Win32.exe [bios.bin] [disc]

With no arguments it looks for `bios\SCPH1001.BIN` beside the executable, then
`SCPH1001.BIN`, then `..\..\..\bios\SCPH1001.BIN` so it works when run straight
out of the build directory.

File > Open disc mounts a `.cue`, `.bin`, `.img` or `.iso`; a drive letter can
be passed on the command line. Keyboard maps to a digital pad in slot 1: arrows
for the d-pad, X/Z/S/A for cross/square/circle/triangle, Q/W and 1/2 for the
shoulders, Enter for start and Shift for select. Space pauses, Escape quits.
