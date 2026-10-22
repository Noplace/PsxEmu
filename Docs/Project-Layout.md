# Project layout

Follows section 1 of [Emulator-Project-Standards.md](Emulator-Project-Standards.md). Four
projects: two shared libraries that know nothing of the PlayStation
([Shared-Libraries-Plan.md](Shared-Libraries-Plan.md)), and PSXEmu's own two on top of them.
The shared libraries are in their own repository, EmuLib (github.com/Noplace/EmuLib), a git
submodule at `EmuLib/` pinned to the commit PSXEmu was checked with: after cloning, or in a new
worktree, run `git submodule update --init`. How to change them without breaking another
emulator: `EmuLib/Emu.Core/README.md`, "Changing it without breaking another emulator".

| Project | Kind | What | Guide |
|---|---|---|---|
| `Emu.Core` | static library | What any emulator shares: the machine's thread, pacing, save states, rewind, settings, disc images, PNGs, host input, an x64 emitter | `EmuLib/Emu.Core/README.md`, `CLAUDE.md` |
| `Emu.Frontend.Win32` | static library | What any Windows front end shares: the four renderers, filters, DLSS and FSR, the texture upscaler's backends, audio devices, pads and mouse, the overlay, DPI, dialogs | `EmuLib/Emu.Frontend.Win32/README.md`, `CLAUDE.md` |
| `PSXEmu.Core` | static library | The PlayStation: its hardware, the HLE BIOS, the recompiler, its enhancements, and `PsxCore`, which the shared machine runs | this file |
| `PSXEmu.Win32` | executable | PSXEmu's front end: the app, its windows and menus, the hardware rasteriser, the PSX controller bindings | this file |

`Emu.Core` includes nothing of the other three; `Emu.Frontend.Win32` includes `Emu.Core` and
nothing of PSXEmu's; `PSXEmu.Core` includes `Emu.Core`; `PSXEmu.Win32` includes all three.

```
PsxEmu/
  PSXEmu.sln
  .gitmodules                  EmuLib, from github.com/Noplace/EmuLib, branch main
  EmuLib/                      git submodule: the two shared libraries below, and README.md,
                               CLAUDE.md, CHANGELOG.md (what a newer pin breaks)
  EmuLib/Emu.Core/             static library - what any emulator shares, nothing of the PSX;
                               the include root, everything under emucore/
    Emu.Core.vcxproj
    Emu.Core.props             imported by a project that uses it: the include path, $(EmuCoreDir)
    README.md  CLAUDE.md       the developer guide, and instructions for Claude in another project
    emucore/
      core.h                   ICore and MemoryState: what the machine's thread asks of a console
      host/                    the only thread-aware part - Docs/Threading-Plan.md
        machine.h/.cpp         the machine's thread: requests, a frame, hand-off, pacing,
                               run-ahead, rewind, the debugger's halts; MachineConfig
        doorbell.h             one per thread; everything that gives it work rings it
        request_queue.h        the one door into something another thread owns
        sample_ring.h          sound, machine thread to audio thread, lock-free
        frame_mailbox.h        finished frames, machine thread to video thread
        latency_markers.h      where a picture's time goes, for NVIDIA Reflex; the front end's
        input_exchange.h       the host's pads and mouse in, the pads' motors out
        audio_output.h/.cpp    the audio thread: keeps the device fed from the ring
        video_output.h/.cpp    the video thread: shows the newest frame through a Presenter
      audio/iaudioengine.h     what a sound device has to do: wait, say how much room, take it
      timing/
        frame_limiter.h        holds a front end's loop to the machine's frame rate
        display_sync.h         running in step with the display (Frame Pacing)
        speed_resampler.h      stretches the console's sound to the emulation speed
      state/
        state_io.h/.cpp        StateIO - save states, read and written through one path
        rewind_buffer.h/.cpp   rewind's store: reverse deltas of the pages that changed
      config/settings_file.h   a flat, hand-editable key = value settings file
      media/
        disc.h/.cpp            disc images: cue, chd, mds/mdf, ccd/img, bin, iso, physical drive
        ecm.h/.cpp             .ecm-packed images
        iso9660.h/.cpp         the filesystem: volume descriptor, directories, file lookup
      image/
        png_file.h/.cpp        PNGs written and read with the bundled zlib
        shared_picture.h       a picture left on the graphics card, for a renderer to open there
      input/
        input_bindings.h       a gamepad's controls as bits, and a binding map as arithmetic and text
        sony_pad_reports.h     a DualShock 4's and DualSense's input reports read, and their motor reports built
        mouse_scaling.h        what a host mouse's movement is worth to a console's mouse
      rec/                     a recompiler's back end
        emitter.h              executable memory and a byte cursor
        x86_extras.h           the instruction encodings
        x64_asm.h              a general x86-64 encoder
        block_cache.h          compiled blocks by address, and the page bitmap
      lib/libchdr/ lzma/ zlib/ zstd_stub/
                               CHD reading, vendored unchanged and built as C (README-chd.md);
                               zlib also for PNGs, .bk2 movies, rewind and the AI downloads
    tools/                     Emu.Core's own harnesses, built by build_tools.bat [out folder]
      frame_limiter_test.cpp  speed_resampler_test.cpp  mouse_scaling_test.cpp
  EmuLib/Emu.Frontend.Win32/   static library - what any Windows front end shares; the include
                               root, everything under emuwin32/, in emulation::win32
    Emu.Frontend.Win32.vcxproj
    Emu.Frontend.Win32.props   imported by a front end: Emu.Core.props, its own include path,
                               $(EmuFrontendSdkDir) - the hand-fetched SDKs, the solution's Temp\
    Emu.Frontend.Win32.targets imported by an executable: the fetched DLLs and models beside it
    README.md  CLAUDE.md       the developer guide, and instructions for Claude in another project
    emuwin32/
      framework.h              the include set every file here opens with
      present/                 putting the picture on the window: the video thread's engines, each
                               an <Api>GraphicsEngine in <api>_engine.h/.cpp
        igraphicsengine.h      what an engine has to be able to do
        video_presenter.h/.cpp VideoPresenter: what the video thread draws with, and the menus'
                               asks of it
        display_clock.h/.cpp   the display's refresh rate and vblanks, for Frame Pacing
        adapters.h             the graphics cards by name and LUID, and picking one; header-only
        shared_device.h        the Direct3D 12 renderer's device, for a Direct3D 12 rasteriser
                               to draw on too
        letterbox.h            where the picture sits in the window, at its aspect ratio
        d3d11/d3d11_engine.h/.cpp  uploads the core framebuffer and draws it; no filters
        d3d12/d3d12_engine.h/.cpp  the same, plus the ported pixel-shader filters
        d3d12/d3d12_dlss.cpp   ...and its NVIDIA DLSS: Streamline started with it, the plane
                               turned into DLSS's inputs, DLSS's picture drawn in the picture's place
        d3d12/d3d12_fsr.cpp    ...and its AMD FSR: AMD's upscaler made on its device from the same
                               inputs as DLSS, and FSR Frame Generation through AMD's swap chain
        opengl/opengl_engine.h/.cpp  the same again in OpenGL 3.3, on a child window of its own
        opengl/gl_functions.h  the OpenGL past 1.1 the engine asks the driver for
        vulkan/vulkan_engine.h/.cpp  the same again in Vulkan 1.0, on a child window of its own
        vulkan/vk_functions.h  the Vulkan the engine uses, declared here; vulkan-1.dll is loaded
                               at run time, nothing is linked
      filters/                 the picture filters (Settings > Video > Filter)
        filter_chain.h         the custom chain's arithmetic: up to four filters, one after another;
                               RendererHasFilters
        shader_pass.h          one stage of a chain, apart from any engine so it can be tested
        shaders/               the filters' HLSL (and superxbr/), compiled into headers beside them
                               by the build; glsl_filters.h, the same filters in GLSL for OpenGL
                               and Vulkan; spirv_filters.h, that GLSL compiled for Vulkan
                               (generated by make_spirv.cpp through build_spirv.bat, which also
                               builds the overlay's SPIR-V - see Bugs-Found 109)
      upscale/dlss/            NVIDIA DLSS (Docs/DLSS-Plan.md)
        streamline.h/.cpp      Streamline's DLL loaded, its signature checked, started and stopped
        reflex_markers.h/.cpp  the machine's Reflex markers passed to whichever renderer runs
                               Frame Generation (emucore/host/latency_markers.h)
        dlss_choice.h          the modes, and the arithmetic of sizes; header-only, the tools use it
        dlss_timing.h          DLSS's card time and Frame Generation's frames, added up for
                               Emulation > Show Timings
        fetch_streamline.ps1   downloads the Streamline SDK into Temp\streamline\ - its DLLs are
                               NVIDIA's, never committed; the targets file copies them beside the
                               exe, with NVIDIA's licences and Streamline's
      upscale/fsr/             AMD FSR (Docs/FSR-Plan.md)
        fidelityfx.h/.cpp      AMD's loader DLL loaded once, every DLL's AMD signature checked;
                               the versions a card runs; fsr.log
        fsr_choice.h           the modes, and the arithmetic of sizes; header-only, the tools use it
        fsr_download.h/.cpp    AMD's three DLLs downloaded by the emulator itself, each checked
                               against the release and AMD's signature (Get AMD's FSR files...)
        fetch_fidelityfx.ps1   downloads AMD's three signed DLLs into Temp\fidelityfx\ - MIT, but
                               69 MB, so never committed; the targets file copies them beside the
                               exe, with the licence
      upscale/texture/         the AI texture upscaler's backends (Docs/Texture-Upscaling-Plan.md)
        texture_upscaler.h     what a backend is: RGB in, a whole number of times larger out
        onnx_runtime.h/.cpp    ONNX Runtime and DirectML loaded once, Microsoft's signature checked
                               on each DLL; an ONNX upscaler run on a Direct3D 12 device we give it
        torch_model.h/.cpp     a PyTorch .pth upscaler read as data, never run, and written out
                               as ONNX: ESRGAN and Real-ESRGAN's compact model
        rtx_video.h/.cpp       NVIDIA RTX Video Super Resolution as a backend, through NGX; built
                               only where NVIDIA's SDK is unpacked in Temp\rtxvideo\ (never
                               committed), x64 only
        ai_download.h/.cpp     ONNX Runtime, DirectML and Real-ESRGAN's models downloaded by the
                               emulator itself, each checked (Get the AI files...)
        fetch_onnxruntime.ps1  downloads ONNX Runtime and DirectML from nuget.org into
                               Temp\onnxruntime\ - 36 MB, never committed
      audio/                   sound devices, driven by emucore/host/audio_output
        wasapiaudioengine.h/.cpp WASAPI, shared and event-driven
        dsoundaudioengine.h/.cpp DirectSound, a looping buffer topped up every 5 ms
      input/
        gamepad.h              one XInput slot: its controls, both sticks, both motors
        sony_pads.h/.cpp       DualShock 4 and DualSense pads over HID, in the slots XInput leaves
        mouse.h                the host mouse, raw or captured
        key_names.h            a key as a settings file stores it ("X", "F10", "Num4"), and back
      ui/
        dpi.h/.cpp             per-monitor DPI: layouts in 96-DPI pixels scaled to each window's
                               monitor, fonts made for it, and WM_DPICHANGED's rescale
        overlay/               what is drawn over the picture: notifications, the controllers, F9's panel
          overlay.h/.cpp       builds the triangles every renderer draws, on the video thread
          overlay_atlas.h/.cpp the glyphs and icons it draws from
          memory_usage.h/.cpp  the process's RAM and its video memory on each card, for the F9 panel;
                               the Windows headers stay in the .cpp, as `small` is a macro in them
          frame_stats_ring.h   the machine's FrameSamples, for the graphs
          shaders/             the overlay's shaders: HLSL for Direct3D and GLSL for OpenGL
                               (overlay_shaders.h), and overlay.vert/.frag compiled to SPIR-V for
                               Vulkan (spirv_overlay*.h, by filters/shaders/build_spirv.bat)
      app/
        engine_factory.h/.cpp  brings up a graphics and an audio engine, each with a fallback
        win32_dialogs.h/.cpp   the file pickers and the message boxes, titled by SetDialogTitle
        screenshot.h/.cpp      a frame as a 4:3 PNG; save-state thumbnails, 160x120
      third_party/             other people's code, as it came; never reformatted
        d3dx12.h               Microsoft's D3D12 helpers
        streamline/            NVIDIA Streamline's headers, MIT, as they come in the SDK
        fidelityfx/            AMD FidelityFX's API headers, MIT, as they come in the SDK
        onnxruntime/           ONNX Runtime's C API headers and DirectML provider header, MIT
  PSXEmu.Core/                 static library - the PlayStation, no UI framework
    PSXEmu.Core.vcxproj
    psx/                       the console's hardware, and nothing else (namespace emulation::psx,
                               as is everything below)
      psx.h                    aggregate header - order is load-bearing
      system.h/.cpp            owns every component, and runs them an instruction at a time
      component.h              the base every device derives from
      types.h                  forward declarations; Disc and Iso9660 imported from Emu.Core
      state_format.h           what a save-state file holds around StateIO's payload
      cpu.h/.cpp               MIPS R3000A interpreter
      cpu_context.h            register file
      gte.h/.cpp               geometry coprocessor: all 22 commands
      gpu_core.h               the interface the core talks to the GPU through
      gpu.h/.cpp               the GPU: VRAM, GP0/GP1, GPUSTAT, the queue, display timing, draw costs
      cdrom.h/.cpp             CD-ROM controller: FIFOs, all 28 commands, CD-DA and XA-ADPCM
      mdec.h/.cpp              motion decoder
      sio.h/.cpp               controller / memory card port: pad, DualShock, mouse, multitap
      sio1.h/.cpp              serial port: the registers of a port with nothing plugged in
      spu.h/.cpp               sound: 24 voices, ADSR, reverb, sweeps, CD input
      dma.h/.cpp               DMA channels
      io_interface.h/.cpp      memory map and hardware registers
      root_counter.h/.cpp      timers
      mc.h/.cpp                a memory card slot: the card in memory, written to its file whole
    render/                    how the GPU's draws become pixels
      raster.h                 DrawJob and RasterBackend - what puts a draw's pixels down - and
                               RasterObserver, what watches it
      software_raster.h/.cpp   the software rasteriser: every DrawJob into native VRAM
      texture_tracker.h/.cpp   which texture each draw reads, by a name the same every run, and
                               the textures dumped (Docs/Texture-Upscaling-Plan.md, phase 1)
      texture_pack.h/.cpp      a folder of replacements for those names, pack.ini, a loader
                               thread: what the hardware rasteriser asks (phase 2)
      texture_upscale.h        what the AI upscaler is handed - a source's texels and the cells
                               read - and its queue, as the core sees it (phase 3)
    enhance/                   opt-in enhancements on top of the machine
      pgxp.h                   PGXP's shadows: each register's and RAM word's unrounded vertex
      vertex_motion.h          where things were in the last picture, by key - motion for DLSS
    media/
      mc_directory.h/.cpp      what is on a card: list, delete, undelete, export, import, format
    bios/                      what is known of the BIOS
      bios_calls.h/.cpp        the A0h/B0h/C0h functions by name, for the debugger's call log
      kernel.h/.cpp            BIOS call logging
      mips_assembler.h         MIPS assembler, which the HLE BIOS writes its ROM code with
      hle/                     the HLE BIOS (Docs/HLE-BIOS.md)
    debug/                     looking inside the machine
      debugger.h/.cpp          breakpoints and stepping: halts the machine before an instruction
      disasm.h                 MIPS disassembler - boot_runner and the debugger window
      debug.h                  BREAKPOINT, and the trap counter behind it
      debug_assist.h/.cpp      _DEBUG-only CSV instruction logger
    emu/                       the emulator's features around the machine
      psx_core.h/.cpp          PsxCore: the PlayStation as Emu.Core's Machine runs it (ICore), and
                               what the front end asks of it beyond that - textures, DLSS's planes;
                               CoreOf, SystemOf, ApplyConfig, MachineConfigFrom
      emuconfig.h              EmuConfig - every runtime setting, held by value
      settings.h               psxemu.ini: psx::SettingsFile, and loading and storing EmuConfig
      movie.h/.cpp             input movies: a BizHawk .bk2 read, and played into the ports
      cheats.h                 GameShark codes: read, run once a frame, and filed per game
      recompiler_bridge.h      the one file that knows both psx/ and rec/
    rec/                       the R3000A's recompiler - includes nothing from psx/
                               (Docs/Recompiler-Plan.md); its back end is Emu.Core's emucore/rec/
      recompiler.h             the engine: HostInterface, dispatch, invalidation
      block_decoder.h          guest code to a block
      block_compiler.h         a block to x64, with the register allocator
      gte_compiler.h           the GTE's commands emitted as x64
      runtime.h                what compiled code calls back into
    tools/                     headless harnesses, built by a .bat, not the solution
      build_tools.bat          builds every harness, Emu.Core's first
      boot_runner.cpp          boots a BIOS or disc, reports everything; the checksum baselines.
                               Also links the hardware rasterisers, for --hw-raster or --d3d12
                               [--warp]
      ppm_diff.cpp             two of boot_runner's pictures compared, and shown side by side
      hw_raster_test.cpp       the hardware rasteriser against the software one, pixel for
                               pixel, scene by scene on WARP
      cpu_test.cpp  gte_test.cpp  gpu_test.cpp  mdec_test.cpp
      timer_test.cpp  sio_test.cpp  spu_test.cpp  media_test.cpp  mc_test.cpp  debug_test.cpp
                               the ten emulation harnesses (Docs/Test-Suite.md)
      rec_test.cpp  rec_bench.cpp   the recompiler's tests, and its benchmark
      hle_test.cpp             the HLE BIOS, driven by programs the test assembles itself
      timing_test.cpp          bus timing against a real console (cpu/access-time)
      host_test.cpp            the threads and channels in emucore/host/, and the machine running
                               a PsxCore
      letterbox_test.cpp       where the picture sits in the window
      bindings_test.cpp        keys and pad controls onto PSX buttons, and the settings they live in
      cheats_test.cpp          GameShark codes read, every code type run, and the cheat files
      movie_test.cpp           BizHawk .bk2 movies: both cores' logs, the archive, the player, the pad
      dlss_choice_test.cpp     the sizes around DLSS: the rasteriser's scale, the jitter, the output
      sl_probe.cpp             what NVIDIA Streamline says of each card; DLSS's input sizes, the
                               signs of its jitter and motion, and its cost (Docs/DLSS-Plan.md)
      fsr_choice_test.cpp      the sizes around FSR: the rasteriser's scale, the jitter, the output
      ffx_probe.cpp            what AMD's FidelityFX runtime offers on each card; the signs of
                               FSR's jitter and motion, and its cost (Docs/FSR-Plan.md)
      filter_chain_test.cpp    the custom filter chain's passes, and the settings that hold it
      ai_probe.cpp             the AI texture upscaler's models: .pth to ONNX, checked against
                               an author's export, timed on a card, run over pictures
                               (Docs/Texture-Upscaling-Plan.md)
      texture_tracker_test.cpp the texture tracker's rules, dumps read back and added to, PNGs
                               of every kind read
      ai_textures_test.cpp     the AI upscaler's tiles made ready and put back together, and its
                               queue, with a model that only repeats pixels
      wav_pitch.cpp            the note in a WAV boot_runner wrote
      make_test_disc.cpp       writes a synthetic disc image
      make_chd.cpp  chd_writer.h   any mountable image written out as a CHD, chdman's format
      make_hle_font.cpp        the HLE BIOS's character sets, from the Shinonome fonts
  PSXEmu.Win32/                PSXEmu's front end: the app, its windows, the hardware rasteriser
    PSXEmu.Win32.vcxproj       and .filters, which shows these folders in Solution Explorer;
                               imports Emu.Frontend.Win32.targets
    app/                       the application: the UI thread and what it is made of
      main.cpp                 wWinMain: per-monitor DPI awareness, the dialogs' title, then App
      framework.h              the include set every file here opens with, and
                               `using namespace emulation::win32` inside psxemu
      const.h                  every constant: window names, menu ids, the choice tables
      app.h/.cpp               class App - the UI thread: window, menus, settings, full screen,
                               and the four threads everything else runs on
      menu.h/.cpp              builds the menu bar; ticks an item against a value
      disc_set.h               which disc images are one game's discs, from their names
      bios_database.h          the BIOS dumps known, by MD5: model, version, date, region
      win32_paths.h/.cpp       command line, BIOS, settings file, data root, disc-derived names
      app_icon.h               the application icon, for every window class
    graphics/
      hw_raster/               the hardware rasteriser (Docs/Hardware-Renderer-Plan.md)
        hardware_raster.h/.cpp HardwareRaster: what both APIs' rasterisers offer beyond
                               RasterBackend, and the one place either is made
        raster_common.h        what both draw with: the HLSL, its compiler, vertex and constant
                               layouts, colour, position and motion packing, the jitter
        d3d11_raster.h/.cpp    DrawJobs drawn on the graphics card with Direct3D 11, into a copy
                               of VRAM kept there; native VRAM brought up to date in 32x32 tiles
        d3d12_raster.h/.cpp    the same with Direct3D 12 (gpu_rasteriser = hardware_d3d12), on
                               a device of its own or the Direct3D 12 renderer's
        replacement_atlas.h    where both keep replacement textures on the card: pages of
                               8x8-texel blocks, the table saying where each is, eviction
                               (Docs/Texture-Upscaling-Plan.md, phase 2)
      upscale/texture/ai_textures.h/.cpp  the upscaler's thread and queue: tiles made ready
                               (holes filled, coverage as its own picture), run on a card, put back
                               together and handed to the pack; the cache folder; Upscale All
      third_party/mmpx/        the MIT notice of the MMPX texture filter's rules
    input/                     the host's devices, and what they press
      input_thread.h/.cpp      the input thread: pads, keyboard, raw mouse at 1 kHz
      keyboard.h               the default keyboard and pad maps, and the keys the window keeps
      controller_bindings.h    every binding, per port and device: defaults, psxemu.ini, mapping
    ui/                        the tool windows
      controller_bindings_window.h/.cpp  Settings > Input > Controllers: each port's type and
                               source, and the drawn pad to bind
      cheats_window.h/.cpp     Emulation > Cheats: the game's codes, on and off, typed or imported
      emulation_settings_window.h/.cpp  Settings > Emulation: CPU, timing, GPU and CD-ROM
                               switches, and the Accuracy and Performance presets
      video_settings_window.h/.cpp  Settings > Video, a page each, chosen from a list down its
                               left: the renderer, card and filter; the rasteriser, resolution
                               and PGXP; NVIDIA DLSS; AMD FSR; textures; the on-screen display
      game_scope.h             whether those three windows are editing everyone's settings or
                               the running game's own
      key_bindings_window.h/.cpp  the older keyboard-only list, kept but off the menu
      memcard_editor.h/.cpp    File > Memory Cards > Memory Card Editor, both slots side by side
      console_window.h/.cpp    Emulation > BIOS Console: the BIOS's putchar/puts/printf output
      debugger_window.h/.cpp   Emulation > Debugger: disassembly, registers, breakpoints, stepping
    Resource/                  the icon, and psxemu.rc + resource.h, which compile the icon into
                               the exe
  bios/                        the user's BIOS dump
  Docs/                        working notes, indexed by Docs/README.md; solved investigations in Docs/history/
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

**And the shared libraries never know which console is running.** `Emu.Core`
runs any console through `ICore`; `Emu.Frontend.Win32` presents any `VideoFrame`.
Neither includes anything of PSXEmu's: what they need from it, they are handed.

## Include conventions

Each project folder is an include root. The two shared libraries put everything
under one folder of their own, so an include says which library it comes from:

```cpp
#include "psx/psx.h"                          // PSXEmu.Core
#include "debug/disasm.h"                     // PSXEmu.Core
#include "emucore/host/machine.h"             // Emu.Core
#include "emuwin32/present/video_presenter.h" // Emu.Frontend.Win32
```

`psx/psx.h` is the one aggregate header, and it includes the rest **in
dependency order**. A type used inline in another header must be included
before it: putting `gpu.h` after `system.h` will not compile, because `System`
holds a `Gpu` by value.

PSXEmu.Core has `$(ProjectDir)` on its include path and imports
`EmuLib\Emu.Core\Emu.Core.props`, which adds Emu.Core's folder after it;
PSXEmu.Win32 has `$(ProjectDir);$(SolutionDir)PSXEmu.Core` and imports
`EmuLib\Emu.Frontend.Win32\Emu.Frontend.Win32.props`, which adds Emu.Frontend.Win32's and
Emu.Core's after them, so its own headers resolve first.

Namespaces follow: `emulation::psx` for the console, `emulation::host`,
`emulation::state`, `emulation::media` and the rest for Emu.Core, and
`emulation::win32` for Emu.Frontend.Win32. PSXEmu.Win32's own code is `psxemu`,
which takes the front-end library's names through a `using namespace` in
`app/framework.h`; PSXEmu.Core takes `StateIO`, `Disc` and `Iso9660` into
`emulation::psx` with `using` declarations (`psx/state_format.h`, `psx/types.h`).

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

Each `.hlsl` filter (in Emu.Frontend.Win32's `emuwin32/filters/shaders/`) carries its `ShaderType`/`ShaderModel` per
configuration, and a configuration with no block of its own gets FXC's default
of `vs_2_0` - which fails on a pixel shader with `X4502`. All four
configurations need their own entries; `Debug|x64` was missing both for a while
and only that configuration failed.

Output goes to `Build\$(Platform)\$(Configuration)\` for **both** platforms.
That is deliberate: MSBuild's own default puts x64 under `$(SolutionDir)x64\`
but Win32 straight into `$(SolutionDir)` with no platform folder, and anything
computing a path to the binary then has to special-case it.

`VcpkgEnabled` is `false` on every project, none of which references a package. Leaving it
on makes a project install variants it never uses, and some of those do not
exist for x86.

The static libraries reference what they use for build order only
(`LinkLibraryDependencies` false), and the executable references all three, so
it links each once. `PSXEmu.Win32.vcxproj` imports `Emu.Frontend.Win32.targets`
after `Microsoft.Cpp.targets`: that copies the fetched DLLs and models beside the
executable (x64 only).

### The harnesses

```
PSXEmu.Core\tools\build_tools.bat
```

Compiles the sources directly rather than through MSBuild, so they stay
independent of the solution configuration, and drops binaries in `Temp\tools\`.
It calls `EmuLib\Emu.Core\tools\build_tools.bat` first, for Emu.Core's own three, which
also runs alone (into EmuLib's `Temp\tools`) and takes an output folder. Edit the `vcvars64.bat` path at the
top if your Visual Studio install differs. It builds about 36 harnesses one after
another; for a quick build, a copy with `/MP` added to its `cl` lines takes about
five minutes.

This is also the fastest way to get a compile error out of the core.

## Formatting

`PSXEmu.Win32/.clang-format` describes that project's style: Google with
four-space scopes and a hundred columns, and `Emu.Frontend.Win32` has the same,
since its code came from there. Running `clang-format -i` over either is a no-op
on a clean tree, so it is safe to run before committing.

Neither sits at the solution root, because `PSXEmu.Core` and `Emu.Core` are
two-space Google and a root config would claim them too. Each
`.clang-format-ignore` excludes vendored code (`d3dx12.h`, NVIDIA's, AMD's,
ONNX Runtime's, MMPX's notice) and the generated `shaders/*.h`.

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
rasterised inside `PSXEmu.Core`, which owns VRAM - on the CPU, or by the
hardware rasteriser in `PSXEmu.Win32/graphics/hw_raster/`, which draws the same
jobs on the graphics card and hands its picture over there. The presenters in
Emu.Frontend.Win32 (`emuwin32/present/`) put that finished picture on the
window: `d3d11_engine.cpp` uploads it into a dynamic texture and stretches it
over the window with a full-screen triangle. Nothing about the PlayStation's
drawing is in the presenters.

That boundary is the point. It is why `boot_runner` can render and checksum
frames with no graphics device at all, and why the presenters could move into a
library any emulator links. All four engines sit behind `IGraphicsEngine`,
`engine_factory.cpp` is the only file that names any of them, and Settings >
Video > Renderer swaps which one is running under a live machine. Sound has the
same arrangement - `IAudioEngine`, with WASAPI and DirectSound behind it and
Settings > Audio > Output switching between them while a game runs.

The filters are the one exception to "does nothing else", and they are the
front end's own: they run on the finished frame on its way to the screen, not
on anything the PlayStation drew. Direct3D 12, OpenGL and Vulkan have them; the
Direct3D 11 one's `SetPixelShader` is a no-op and the Filter menu greys itself
out when it is the one running (`RendererHasFilters`).

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
