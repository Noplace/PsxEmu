# Emu.Frontend.Win32

A static library of what any Windows front end here shares, apart from any one console: putting
the picture on a window through Direct3D 11, Direct3D 12, OpenGL or Vulkan, with picture filters,
NVIDIA DLSS and AMD FSR; the texture upscaler's backends; WASAPI and DirectSound; XInput pads,
DualShock 4 and DualSense pads over HID, and the mouse; an on-screen overlay; per-monitor DPI;
message boxes and file pickers; screenshots.

It is the Windows half of `Emu.Core` (read its guide first: this library plugs into its
`host::Machine`, `VideoOutput` and `AudioOutput`). It was taken out of PSXEmu.Win32
(`Docs/Shared-Libraries-Plan.md`, phase 6), which is its first user and the reference for
everything below: `PSXEmu.Win32/app/app.cpp`, `App::StartThreads`, wires all of it.

## The rules

- **Nothing of any console**, and nothing of any one app: it includes Emu.Core and the Windows
  SDK, and never PSXEmu.Core or PSXEmu.Win32. What it needs from an app (a window, a title, a
  sample rate, a way to post to the UI thread) is handed to it.
- **One include root**: the project folder, with everything under `emuwin32/`, so an include names
  the library: `#include "emuwin32/present/video_presenter.h"`. Every file opens with
  `#include "emuwin32/framework.h"` (`windows.h` with `WIN32_LEAN_AND_MEAN` and `NOMINMAX`, and the
  common standard headers).
- **Namespace `emulation::win32`.**
- **Nothing NVIDIA's, AMD's or Microsoft's runtime is committed.** Their DLLs are fetched into
  `Temp\` by scripts, or by the emulator itself, and copied beside the executable by
  `Emu.Frontend.Win32.targets`. Each DLL's signature is checked before it is loaded.

## Using it from a front end

In the executable's `.vcxproj`:

```xml
<AdditionalIncludeDirectories>$(ProjectDir);$(SolutionDir)Emu.Core;$(SolutionDir)Emu.Frontend.Win32;...</AdditionalIncludeDirectories>

<ProjectReference Include="..\Emu.Core\Emu.Core.vcxproj">
  <Project>{14fceedf-673e-4b07-a60c-2b90c1342f10}</Project>
</ProjectReference>
<ProjectReference Include="..\Emu.Frontend.Win32\Emu.Frontend.Win32.vcxproj">
  <Project>{52395c63-31f3-4f44-8fa1-6daa76b64d9f}</Project>
</ProjectReference>

<!-- after Microsoft.Cpp.targets: the fetched DLLs and models beside the executable -->
<Import Project="..\Emu.Frontend.Win32\Emu.Frontend.Win32.targets" />
```

For NVIDIA RTX Video, the executable also links `Temp\rtxvideo\lib\Windows\x64` where that SDK is
unpacked (see `PSXEmu.Win32.vcxproj`). In the app's own code:

```cpp
// framework.h of the app: take the library's names by their short ones
namespace myemu { using namespace emulation::win32; }

// wWinMain, before anything can fail: every message box is titled with the app's name
emulation::win32::SetDialogTitle(L"MyEmu");
```

## What is in it

| Folder | What | Main types and functions |
|---|---|---|
| `present/` | The video thread's renderers and the presenter that drives one | `IGraphicsEngine`, `D3D11GraphicsEngine`, `D3D12GraphicsEngine`, `OpenGLGraphicsEngine`, `VulkanGraphicsEngine`, `VideoPresenter`, `DisplayClock`, `EnumerateGraphicsAdapters`, `FindGraphicsAdapterLike`, `OpenAdapter`, `SharedD3D12Device`, `ComputeLetterboxRect` |
| `filters/` | Picture filters, single and multi-pass, as HLSL (Direct3D 12) and GLSL (OpenGL, and as SPIR-V for Vulkan) | `ShaderPass`, `PlanFilterChain`, `RendererHasFilters`, `kCustomChainKey`; shaders in `filters/shaders/` |
| `upscale/dlss/` | NVIDIA DLSS Super Resolution, DLAA and Frame Generation through Streamline, and Reflex | `Streamline`, `DlssChoice`, `DlssStatus`, `ParseDlssMode`, `ReflexMarkers`, `DlssTiming` |
| `upscale/fsr/` | AMD FSR upscaling, Native AA and Frame Generation through FidelityFX | `FidelityFx`, `FsrChoice`, `FsrStatus`, `DownloadFsrFiles` |
| `upscale/texture/` | AI texture upscaling backends: a model on the graphics card | `TextureUpscaler`, `OnnxRuntime`, `OnnxUpscaler`, `ReadTorchStateDict` and `ExportUpscalerOnnx` (a `.pth` read as data and written as ONNX), `RtxVideoUpscaler`, `DownloadAiFiles` |
| `audio/` | Sound devices for Emu.Core's `AudioOutput` | `WASAPIAudioEngine`, `DirectSoundAudioEngine` (both `IAudioEngine`) |
| `input/` | The host's pads, mouse and key names | `Gamepad` (XInput), `SonyPads` (DualShock 4 and DualSense over HID), `Mouse` (raw or captured), `KeyName`, `KeyFromName` |
| `ui/overlay/` | The on-screen overlay every engine draws: notifications, controllers, performance graphs, a state picker | `Overlay`, `ToastKind`, `StatsMode`, `OverlayTheme`, `FrameStatsRing`, `OverlayDrawData` |
| `ui/` | Per-monitor DPI | `WindowDpi`, `CreateMessageFont`, `MoveForDpi`, `RescaleForDpi` |
| `app/` | What a front end makes its engines with, and the dialogs | `CreateGraphicsEngine`, `LoadAllFilters`, `CreateAudioEngine`, `GraphicsBackend`, `AudioBackend`, `RenderWindows`, `SetDialogTitle`, `ShowError`, `ShowWarning`, `ChooseFile`, `SaveScreenshotPng`, `SaveThumbnailPng` |
| `third_party/` | Headers as they came: `d3dx12.h`, NVIDIA Streamline's, AMD FidelityFX's, ONNX Runtime's | |

As in Emu.Core, every header opens with what it is for. Read it before using the type.

## Wiring a front end around `Machine`

PSXEmu's `App::StartThreads` (`PSXEmu.Win32/app/app.cpp`) is the worked example. In order:

1. **The window.** OpenGL and Vulkan each draw into a child window of their own, made with a
   window class that is `CS_OWNDC` and answers `WM_NCHITTEST` with `HTTRANSPARENT`
   (`App::CreateRenderSurfaces`). Pass all three in a `RenderWindows { main, opengl, vulkan }`.
2. **Sound.** `emulation::host::AudioOutput` with a factory that calls
   `CreateAudioEngine(backend, &opened, core.sample_rate())`. It falls back from WASAPI to
   DirectSound by itself and says which opened.
3. **Video.** `emulation::host::VideoOutput` with a factory that, on the video thread, makes a
   `VideoPresenter(windows, to_ui, &frame_stats, &dlss_timing)` and calls
   `Open(renderer_key, filter_key)` (`"d3d11"`, `"d3d12"`, `"opengl"`, `"vulkan"`). A Direct3D
   device is made by the thread that uses it and used by no other, which is why it is a factory.
   Later changes (renderer, filter, chain, vsync, card, DLSS, FSR) go to it in a
   `VideoOutput::Post`, as do the overlay's: `static_cast<VideoPresenter*>(video.presenter())->overlay()`.
4. **The machine.** `emulation::host::Machine(&core, config, &video->frames(), &audio->samples(),
   hooks)`; its `hooks.frame_done` pushes into the `FrameStatsRing` the overlay's graphs read.
5. **Input.** A thread of the app's polls `Gamepad`s, `SonyPads` and `Mouse` and publishes a
   `HostInput` into `machine.input()`. Mapping that onto the console's ports is the console's
   (PSXEmu: `input/input_thread` and `input/controller_bindings`, which stay in the app).
6. **Start** input, audio, video, then the machine. **Stop** the machine first, then video (it
   destroys the engine on its own thread, before the window goes), then audio, then input.

## DLSS, FSR and the texture upscaler

- **DLSS and FSR** need what the console's hardware rasteriser keeps beside its picture: a plane
  of depth and motion, jitter, and the picture left on the card as an `image::SharedPicture`
  (Emu.Core). A console without a hardware rasteriser that does that runs neither. DLSS runs only
  in the Direct3D 12 engine on an RTX card; FSR in the Direct3D 12 engine on any card.
- **Files.** DLSS needs NVIDIA's Streamline (`upscale/dlss/fetch_streamline.ps1`, into
  `Temp\streamline`); FSR needs AMD's DLLs (`upscale/fsr/fetch_fidelityfx.ps1`, or
  `DownloadFsrFiles` from the app); the AI upscaler needs ONNX Runtime and DirectML
  (`upscale/texture/fetch_onnxruntime.ps1`, or `DownloadAiFiles`). The targets file copies them
  beside the executable. AMD's loader finds its effects only there.
- **RTX Video** is built in only where NVIDIA's SDK is unpacked by hand into `Temp\rtxvideo` (it is
  behind NVIDIA's sign-in), x64 only, with `PSXEMU_RTX_VIDEO=1`, which the library's project sets.
- **The texture upscaler's queue** (which textures to upscale, and where the results go) is still
  PSXEmu's (`ai_textures`), until a second emulator has textures to replace.

## Shaders

The HLSL filters are compiled by the library's project into headers beside them
(`filters/shaders/*.h`, committed). The GLSL is in `glsl_filters.h`; Vulkan's SPIR-V
(`spirv_filters.h`) and the overlay's (`ui/overlay/shaders/spirv_overlay_*.h`) are rebuilt by
`filters/shaders/build_spirv.bat <glslang.exe>` after a GLSL change. The build stops with a
message if the GLSL and the SPIR-V disagree.

## Still the app's, for now

- `input_thread` and `controller_bindings`: they map the host onto PSX ports. The polling half
  could move here once a second emulator wants it.
- The BIOS console window, the settings windows, the menus.
- The Vulkan and OpenGL child windows: each app makes its own (step 1).
