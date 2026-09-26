# Project layout

Follows section 1 of [Emulator-Project-Standards.md](Emulator-Project-Standards.md).

```
PsxEmu/
  PSXEmu.sln
  PSXEmu.Core/                 static library - all emulation, no UI framework
    PSXEmu.Core.vcxproj
    platform/                  stands in for the WinCore library this repo does not have
      types.h                  fixed-width integer names
      util.h                   SafeDelete / SafeDeleteArray / SafeRelease
      frame_limiter.h          holds a front end's loop to the machine's frame rate
      speed_resampler.h        stretches the SPU's output to the emulation speed
      mouse_scaling.h          what a host mouse's movement is worth to a PSX mouse
      input_bindings.h         a gamepad's controls as bits, and a binding map as arithmetic and text
    host/                      the only thread-aware part of Core - Docs/Threading-Plan.md
      doorbell.h               one per thread; everything that gives it work rings it
      request_queue.h          the one door into something another thread owns
      sample_ring.h            sound, machine thread to audio thread, lock-free
      frame_mailbox.h          finished frames, machine thread to video thread
      input_exchange.h         the host's pads and mouse in, the pads' motors out
      machine.h/.cpp           the machine's thread: requests, a frame, hand-off, pacing
      audio_output.h/.cpp      the audio thread: keeps the device fed from the ring
      video_output.h/.cpp      the video thread: shows the newest frame
    audio/                     sound devices, driven by host/audio_output
      iaudioengine.h           what one has to do: wait, say how much room, take it
      wasapiaudioengine.h/.cpp WASAPI, shared and event-driven
      dsoundaudioengine.h/.cpp DirectSound, a looping buffer topped up every 5 ms
    psx/                       the machine
      psx.h                    aggregate header - order is load-bearing
      system.h/.cpp            owns every component, and runs them an instruction at a time
      component.h              the base every device derives from
      emuconfig.h              EmuConfig - every runtime setting, held by value
      settings.h               psxemu.ini: load and store EmuConfig
      state.h/.cpp             StateIO - save states, read and written through one path
      cpu.h/.cpp               MIPS R3000A interpreter
      cpu_context.h            register file
      recompiler_bridge.h      the one file that knows both cpu.h and rec/
      gte.h/.cpp               geometry coprocessor: all 22 commands
      gpu_core.h               the interface the core talks to the GPU through
      gpu.h/.cpp               software GPU: VRAM, GP0/GP1, rasteriser
      cdrom.h/.cpp             CD-ROM controller: FIFOs, all 28 commands, CD-DA and XA-ADPCM
      disc.h/.cpp              disc images: cue, chd, mds/mdf, ccd/img, bin, iso, physical drive
      iso9660.h/.cpp           the filesystem: volume descriptor, directories, file lookup
      mdec.h/.cpp              motion decoder
      sio.h/.cpp               controller / memory card port: pad, DualShock, mouse, multitap
      sio1.h/.cpp              serial port: the registers of a port with nothing plugged in
      spu.h/.cpp               sound: 24 voices, ADSR, reverb, sweeps, CD input
      dma.h/.cpp               DMA channels
      io_interface.h/.cpp      memory map and hardware registers
      root_counter.h/.cpp      timers
      mc.h/.cpp                a memory card slot: the card in memory, written to its file whole
      mc_directory.h/.cpp      what is on a card: list, delete, undelete, export, import, format
      kernel.h/.cpp            BIOS call logging
      debugger.h/.cpp          breakpoints and stepping: halts the machine before an instruction
      disasm.h                 MIPS disassembler - boot_runner and the debugger window
      bios_calls.h/.cpp        the A0h/B0h/C0h functions by name, for the debugger's call log
      debug.h                  BREAKPOINT, and the trap counter behind it
      debug_assist.h/.cpp      _DEBUG-only CSV instruction logger
      emu.h/.cpp               superseded by system.*; kept, not built
    rec/                       the recompiler - includes nothing from psx/ (Docs/Recompiler-Plan.md)
      recompiler.h             the engine: HostInterface, dispatch, invalidation
      block_decoder.h          guest code to a block
      block_compiler.h         a block to x64, with the register allocator
      block_cache.h            compiled blocks by address, and the page bitmap
      runtime.h                what compiled code calls back into
      emitter.h                executable memory and a byte cursor
      x86_extras.h             the instruction encodings
    lib/reccore/               the RecCore emitter, vendored and since replaced; built by nothing
    lib/libchdr/ lib/lzma/ lib/zlib/ lib/zstd_stub/
                               CHD reading, vendored unchanged and built as C (lib/README-chd.md)
    utilities/
      cdrom/iso9660.h          ISO9660 structures
      cdrom/cdrom.cpp          old host CD read, superseded by disc.cpp; not built
      lean/hash_table.h
    tools/                     headless harnesses, built by a .bat, not the solution
      build_tools.bat
      boot_runner.cpp          boots a BIOS or disc, reports everything; the checksum baselines
      cpu_test.cpp  gte_test.cpp  gpu_test.cpp  mdec_test.cpp
      timer_test.cpp  sio_test.cpp  spu_test.cpp  media_test.cpp  mc_test.cpp  debug_test.cpp
                               the ten emulation harnesses (Docs/Test-Suite.md)
      rec_test.cpp  rec_bench.cpp   the recompiler's tests, and its benchmark
      timing_test.cpp          bus timing against a real console (cpu/access-time)
      host_test.cpp            the threads and channels in host/
      frame_limiter_test.cpp  speed_resampler_test.cpp  letterbox_test.cpp
      bindings_test.cpp        keys and pad controls onto PSX buttons, and the settings they live in
      wav_pitch.cpp            the note in a WAV boot_runner wrote
      make_test_disc.cpp       writes a synthetic disc image
      make_chd.cpp  chd_writer.h   any mountable image written out as a CHD, chdman's format
      letterbox.h
  PSXEmu.Win32/                front end: a window, four renderers, input
    PSXEmu.Win32.vcxproj       and .filters, which shows these folders in Solution Explorer
    app/                       the application: the UI thread and what it is made of
      main.cpp                 wWinMain, and nothing else
      framework.h              the include set every file here opens with
      const.h                  every constant: window names, menu ids, the choice tables
      app.h/.cpp               class App - the UI thread: window, menus, settings, full screen,
                               and the four threads everything else runs on
      menu.h/.cpp              builds the menu bar; ticks an item against a value
      engine_factory.h/.cpp    brings up a graphics and an audio engine, each with a fallback
      win32_paths.h/.cpp       command line, BIOS, settings file, data root, disc-derived names
      win32_dialogs.h/.cpp     the file pickers and the message boxes
      app_icon.h               the application icon, for every window class
    graphics/                  the renderers, all behind one interface
      igraphicsengine.h        what a presenter has to be able to do
      video_presenter.h/.cpp   what the video thread draws with, and the menus' asks of it
      d3d11_presenter.h/.cpp   uploads the core framebuffer and draws it; no filters
      d3d12_graphics_engine.h/.cpp   the same, plus the ported pixel-shader filters
      d3dx12.h                 Microsoft's D3D12 helpers, vendored as they come
      opengl_engine.h/.cpp     the same again in OpenGL 3.3, on a child window of its own
      gl_functions.h           the OpenGL past 1.1 the engine asks the driver for
      vulkan_engine.h/.cpp     the same again in Vulkan 1.0, on a child window of its own
      vk_functions.h           the Vulkan the engine uses, declared here; vulkan-1.dll is loaded
                               at run time, nothing is linked
    input/                     the host's devices, and what they press
      input_thread.h/.cpp      the input thread: pads, keyboard, raw mouse at 1 kHz
      keyboard.h               key names for psxemu.ini, and the default keyboard and pad maps
      gamepad.h                one XInput slot: its controls, both sticks, both motors
      mouse.h                  the host mouse, raw or captured, as a PSX mouse's counts
      controller_bindings.h    every binding, per port and device: defaults, psxemu.ini, mapping
    ui/                        the tool windows
      controller_bindings_window.h/.cpp  Settings > Input > Controllers: each port's type and
                               source, and the drawn pad to bind
      emulation_settings_window.h/.cpp  Settings > Emulation: CPU, timing, GPU and CD-ROM
                               switches, and the Accuracy and Performance presets
      key_bindings_window.h/.cpp  the older keyboard-only list, kept but off the menu
      memcard_editor.h/.cpp    File > Memory Cards > Memory Card Editor, both slots side by side
      console_window.h/.cpp    Emulation > BIOS Console: the BIOS's putchar/puts/printf output
      debugger_window.h/.cpp   Emulation > Debugger: disassembly, registers, breakpoints, stepping
    Resource/                  the filters' HLSL, the icon, and psxemu.rc + resource.h, which
                               compile the icon into the exe
    shaders/                   filter shaders, compiled into headers by the build, and
                               glsl_filters.h, the same filters in GLSL for OpenGL and Vulkan,
                               spirv_filters.h, that GLSL compiled for Vulkan (generated by
                               make_spirv.cpp through build_spirv.bat - see Bugs-Found 109)
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
#include "platform/frame_limiter.h"
#include "psx/disasm.h"
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

**Build the solution, never the project on its own.** `msbuild
PSXEmu.Win32.vcxproj` leaves `$(SolutionDir)` pointing at the project's own
folder, so `$(SolutionDir)PSXEmu.Core` on the include path resolves to nothing
and every core include fails with `C1083`. The errors name `psx/psx.h` and look
like a missing file rather than a missing macro.

Each `.hlsl` in `Resource\` carries its `ShaderType`/`ShaderModel` per
configuration, and a configuration with no block of its own gets FXC's default
of `vs_2_0` - which fails on a pixel shader with `X4502`. All four
configurations need their own entries; `Debug|x64` was missing both for a while
and only that configuration failed.

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

## Formatting

`PSXEmu.Win32/.clang-format` describes that project's style: Google with
four-space scopes and a hundred columns. Running `clang-format -i` over the
directory is a no-op on a clean tree, so it is safe to run before committing.

It deliberately does **not** sit at the solution root, because `PSXEmu.Core` is
two-space Google and a root config would claim it too. `.clang-format-ignore`
beside it excludes `d3dx12.h` (Microsoft's, vendored verbatim) and the
generated `shaders/*.h`.

Data tables whose layout carries meaning - the menu choice tables in `const.h`,
whose order has to line up with the command-id runs - are wrapped in
`// clang-format off`, because the formatter packs braced initialisers onto
shared lines and that correspondence stops being checkable at a glance.

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
frames with no graphics device at all, and it is what made the D3D12 engine
possible without touching anything else: both sit behind `IGraphicsEngine`,
`engine_factory.cpp` is the only file that names either, and Settings > Video >
Renderer swaps which one is running under a live machine. A Vulkan one would go
in the same way. Sound has the same arrangement - `IAudioEngine`, with WASAPI and
DirectSound behind it and Settings > Audio > Output switching between them
while a game runs.

The filters are the one exception to "does nothing else", and they are the
front end's own: they run on the finished frame on its way to the screen, not
on anything the PlayStation drew. Only the D3D12 engine has them; the D3D11
one's `SetPixelShader` is a no-op and the Filter menu greys itself out when it
is the one running.

The shaders are compiled from a string at startup rather than loaded from
`.cso` files beside the executable - which is how the 2012 front end did it,
with absolute paths baked in from somebody else's machine.

## Running the front end

    PSXEmu.Win32.exe [bios.bin] [disc]

With no arguments it looks for `bios\SCPH1001.BIN` beside the executable, then
`SCPH1001.BIN`, then `..\..\..\bios\SCPH1001.BIN` so it works when run straight
out of the build directory.

File > Boot disc mounts a `.cue`, a `.mds`, or a `.bin`, `.img`, `.iso`
or `.mdf` image; a drive letter can
be passed on the command line.

Each of the two controller ports holds whichever controller Settings > Input >
Controllers says, and is fed from the device chosen there - the keyboard, or one
of the four XInput pads. Which key or pad control presses which button is set in
the same window, separately
for each port, each multitap player and each device, and kept in `psxemu.ini`. It
starts as the table in `const.h`: on the keyboard, arrows for the d-pad, X/Z/S/A
for cross/square/circle/triangle, Q/W and 1/2 for the shoulders, Enter for start
and Shift for select; on a pad, the Xbox layout by position (A is cross), the
triggers for L2/R2 and Back for select.

File > Recent Discs lists the last eight discs played, the most recent first.

Space pauses. F1-F8 load a save-state slot and Ctrl+F1-F8 save one; the slot
last used is the one the Emulation menu's own Save State and Load State act on.
