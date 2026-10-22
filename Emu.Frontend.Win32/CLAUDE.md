# Emu.Frontend.Win32 - instructions for Claude

Emu.Frontend.Win32 is the shared Windows front-end library for every emulator in this family: the
Direct3D 11/12, OpenGL and Vulkan presenters with filters, DLSS and FSR, AI texture upscaling
backends, WASAPI/DirectSound, XInput and Sony pads, the mouse, the overlay, DPI and dialogs. It sits
on Emu.Core (read `../Emu.Core/CLAUDE.md` and its README first). `README.md` beside this file is the
developer guide: read it, then the header of each type you touch.

## When you build a new emulator's Windows front end

1. **Reference both libraries and import the targets file** (README, "Using it from a front end").
   The executable links both; the targets file copies NVIDIA's, AMD's and ONNX Runtime's DLLs and
   the models beside it.
2. **Include through `emuwin32/`** and open your own files with your app's `framework.h`, which
   includes `emuwin32/framework.h` and has `namespace yourapp { using namespace emulation::win32; }`
   (PSXEmu.Win32: `app/framework.h`).
3. **Call `SetDialogTitle(L"YourEmu")` first thing in `wWinMain`.**
4. **Wire the threads as `App::StartThreads` does** (`PSXEmu.Win32/app/app.cpp`, and the README's
   six steps): `AudioOutput` with `CreateAudioEngine(backend, &opened, core.sample_rate())`,
   `VideoOutput` whose factory makes a `VideoPresenter` on the video thread, `Machine` with your
   `ICore`, and an input thread publishing `HostInput`. Stop them in the reverse order.
5. **Make the OpenGL and Vulkan child windows yourself** (`App::CreateRenderSurfaces` is the
   model) and pass them in `RenderWindows`.
6. **Anything that changes the presenter or the overlay goes through `VideoOutput::Post`**, on the
   video thread: never call the presenter from another thread.
7. **DLSS and FSR need the console's hardware rasteriser** to hand over its picture on the card with
   depth, motion and jitter (`image::SharedPicture`). Without one, leave them off; the presenter
   says why they cannot run.

## When you change this library

- **Nothing of any console or any one app goes in.** No `psx/`, no `app/`, no `ui/` of an app, no
  console constants. What it needs, it is handed (a window, a title, a sample rate, a callback to
  the UI thread). Check with
  `git grep -nE '#include "(psx|emu|render|bios|app|ui|input|graphics|tools)/' -- 'Emu.Frontend.Win32/*.h' 'Emu.Frontend.Win32/*.cpp'`
  - it must print nothing (`emuwin32/` and `emucore/` paths are fine).
- **Never commit a vendor's runtime.** Streamline, FidelityFX, ONNX Runtime, DirectML and RTX Video
  DLLs live in `Temp\`, fetched by the scripts in `upscale/`, and are copied by the targets file.
  Their headers in `third_party/` are kept as they came: never reformat them.
- **Shaders**: an HLSL change rebuilds its header through the project's `FxCompile` rules; a GLSL
  change needs `filters/shaders/build_spirv.bat <glslang.exe>` for Vulkan's SPIR-V. Commit the
  generated headers with the source.
- **Check in the app, on every renderer you touched**, with a scratch copy of the executable that
  has its own `psxemu.ini` beside it (settings live beside the exe), the sound off, and a uniquely
  named disc so no real save state or memory card is touched; delete what it wrote afterwards. A
  flip-model swap chain captures black with `PrintWindow`: make the window topmost and copy from
  the screen. Keep repeated runs on the integrated Radeon; one run at a time on the RTX 4060.
- **Update `README.md`** when you add a module, a type a front end is meant to call, or a rule.

## Gotchas that cost time here

- Code in the global namespace that forward-declares a vendor's namespace (`namespace sl { ... }`)
  must stay outside `namespace emulation::win32 { }`, or the vendor's headers end up nested in it.
- `IGraphicsEngine` implementations are made by `CreateGraphicsEngine`, which falls back from the
  asked renderer to another and says so in `*warning`; the presenter reports what actually opened.
- A string `"psxemu"` in `onnx_runtime.cpp`, `rtx_video.cpp` or `torch_model.cpp` is a name written
  out (a temp folder, a model's producer), not a namespace: changing it changes behaviour.
- The Bash tool halves backslashes: edit `.vcxproj`, `.targets` and `.bat` files with the Edit tool
  or a script written with the Write tool.
