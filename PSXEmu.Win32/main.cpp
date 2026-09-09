/*****************************************************************************************************************
* Copyright (c) 2012 Khalid Ali Al-Kooheji                                                                       *
*                                                                                                                *
* Permission is hereby granted, free of charge, to any person obtaining a copy of this software and              *
* associated documentation files (the "Software"), to deal in the Software without restriction, including        *
* without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell        *
* copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the       *
* following conditions:                                                                                          *
*                                                                                                                *
* The above copyright notice and this permission notice shall be included in all copies or substantial           *
* portions of the Software.                                                                                      *
*                                                                                                                *
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT          *
* LIMITED TO THE WARRANTIES OF MERCHANTABILITY, * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.          *
* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, * DAMAGES OR OTHER LIABILITY,      *
* WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE            *
* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                                                         *
*****************************************************************************************************************/
//
// PSXEmu.Win32 - the Win32 front end.
//
// Owns a window, a graphics engine and the message loop. It does not own any
// emulation: the core in PSXEmu.Core rasterises every pixel on the CPU and
// this only uploads the finished frame. The one thing that flows the other
// way is input, through the core's SIO device.
//
// The graphics engine is one of two, chosen at startup and switchable live
// from the Video menu: D3D11Presenter (no filter support) or
// D3D12GraphicsEngine (does), both behind IGraphicsEngine so the rest of this
// file never needs to know which one is active.
//
//   PSXEmu.Win32.exe [bios.bin] [disc]
//

#include "psx/psx.h"

#include "d3d11_presenter.h"
#include "d3d12_graphics_engine.h"
#include "gamepad.h"
#include "audio/wasapiaudioengine.h"
#include "audio/dsoundaudioengine.h"
#include "shaders/legacy_shaders.h"
#include "shaders/ps_scanline_filter.h"
#include "shaders/ps_xbrz_filter.h"

#include <commdlg.h>
#include <shlobj.h>   // SHGetFolderPathA
#include <shellapi.h>   // CommandLineToArgvW

#include <array>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")

using emulation::psx::Sio;
using emulation::psx::Spu;
using emulation::psx::System;

namespace {

constexpr wchar_t kWindowClass[] = L"PSXEmuWindow";
constexpr wchar_t kWindowTitle[] = L"PSXEmu";

// Menu command ids.
enum MenuCommand {
  kCommandBootDisc = 1000,
  kCommandSwapDisc,
  kCommandEjectDisc,
  kCommandBootBios,
  kCommandBootExe,
  kCommandOpenMemoryCardSlot1,
  kCommandOpenMemoryCardSlot2,
  kCommandCreateMemoryCardSlot1,
  kCommandCreateMemoryCardSlot2,
  kCommandReset,
  kCommandPause,
  kCommandSaveState,
  kCommandLoadState,
  kCommandVolumeFirst,
  kCommandVolumeLast = kCommandVolumeFirst + 7,
  kCommandRendererFirst,
  kCommandRendererLast = kCommandRendererFirst + 1,   // Direct3D 11, 12
  kCommandFilterFirst,
  kCommandFilterLast = kCommandFilterFirst + 8,       // None + 8 filters
  kCommandViewVram,
  kCommandCdMechanicalTiming,
  kCommandControllerTypeFirst,
  kCommandControllerTypeLast = kCommandControllerTypeFirst + 5,  // 2 ports x 3 types
  kCommandInputSourceFirst,
  kCommandInputSourceLast = kCommandInputSourceFirst + 5,        // 2 ports x 3 sources
  kCommandExit,
};

// ---------------------------------------------------------------------------
// The application
// ---------------------------------------------------------------------------

// Everything the front end owns, in the order it has to be torn down in:
// members are destroyed in reverse, so the machine stops before the audio
// device goes away and both go before the Direct3D device.
//
// This being one object with a destructor is what makes the failure paths in
// wWinMain safe. They used to `return 1` after the presenter and the audio
// device were already up, leaking both.
struct Application {
  // Whichever backend is actually active right now - not necessarily the
  // same as the persisted `graphics_backend` preference, since creating the
  // preferred one can fall back to the other. The Video menu ticks against
  // this, not against the config.
  std::unique_ptr<IGraphicsEngine> graphics;
  std::string current_backend = "d3d12";
  std::string current_filter;   // ditto, for the filter menu

  // Video > View VRAM: shows the whole 1024x512 VRAM instead of the display
  // area, for chasing texture/CLUT corruption that the normal view only
  // shows the symptom of. Not persisted - always starts off.
  bool view_vram = false;
  std::vector<uint32_t> vram_view_scratch;

  std::unique_ptr<IAudioEngine> audio;
  std::unique_ptr<System> system;

  std::string bios_path;

  // User settings, and where they are kept. Written as they are changed
  // rather than only at exit, so a crash or a kill does not lose them.
  emulation::psx::SettingsFile settings;

  // Per-user data, under Documents\My Games\PSXEmu - the same convention
  // GBAEmu uses, so both live in the one place a person would look for either.
  // Empty if Documents could not be resolved, which callers treat as "skip
  // this rather than fail the boot".
  std::string data_root;
  std::string memcards_root;      // data_root\memcards
  std::string savestates_root;    // data_root\savestates - unused until
                                   // save states themselves exist; see
                                   // Docs/Save-States-Plan.md
  std::string settings_path;
  bool running = false;
  bool paused = true;

  // A save or load requested this frame, actioned once at the top of the
  // next frame - never mid-frame, per Docs/Save-States-Plan.md. -1 means
  // nothing pending. The generic Save State/Load State menu items act on
  // last_slot_, which F1-F8 also update, so the two stay in step.
  int pending_save_slot = -1;
  int pending_load_slot = -1;
  int last_slot = 1;   // matches F1, the first of the eight slots

  // Scratch for one frame of audio, sized for the worst case at 30 fps. A
  // member rather than a function-local static so there is one per
  // application rather than one per process.
  std::array<int16_t, Spu::kSampleRate / 30 * 2> audio_scratch = {};

  // Two fixed XInput slots - "Gamepad 1" and "Gamepad 2" in the Input menu,
  // XInput user index 0 and 1 respectively. Which PSX port (if either) each
  // one feeds, and whether a port uses a gamepad at all rather than the
  // keyboard, is decided by EmuConfig::input_source and applied each frame -
  // see the main loop's input block.
  std::array<psxemu::Gamepad, 2> gamepads{ psxemu::Gamepad(0),
                                           psxemu::Gamepad(1) };

  ~Application() {
    if (system != nullptr)
      system->Deinitialize();
    if (audio != nullptr)
      audio->Shutdown();
  }
};

// The window procedure gets at the application through the window's user data,
// set from the CREATESTRUCT before any other message arrives. Messages sent
// during CreateWindowExW itself can still land before that, so every use is
// guarded.
Application* AppFrom(HWND window) {
  return reinterpret_cast<Application*>(
      GetWindowLongPtrW(window, GWLP_USERDATA));
}

// ---------------------------------------------------------------------------
// Graphics
// ---------------------------------------------------------------------------

enum class GraphicsBackend { kD3D11, kD3D12 };

// Tries `preferred` first; if that engine's own device creation fails, tries
// the other one and warns that it did, rather than failing outright - a
// machine that can do one almost always can do the other. Only if both fail
// does this return null, which the caller treats as a hard failure (at
// startup) or a "could not switch, and could not go back either" one (mid
// session, from the Video menu). `*active_backend` is set to whichever
// engine actually ended up running, which the caller uses instead of the
// requested one for menu ticks and persisted state from here on.
std::unique_ptr<IGraphicsEngine> CreateGraphicsEngine(
    GraphicsBackend preferred, HWND window, int width, int height,
    HWND message_owner, std::string* active_backend) {
  auto try_backend =
      [&](GraphicsBackend backend) -> std::unique_ptr<IGraphicsEngine> {
    std::unique_ptr<IGraphicsEngine> engine;
    if (backend == GraphicsBackend::kD3D12)
      engine = std::make_unique<D3D12GraphicsEngine>();
    else
      engine = std::make_unique<psxemu::D3D11Presenter>();
    if (engine->Initialize(window, width, height))
      return engine;
    return nullptr;
  };

  if (std::unique_ptr<IGraphicsEngine> engine = try_backend(preferred)) {
    *active_backend =
        (preferred == GraphicsBackend::kD3D12) ? "d3d12" : "d3d11";
    return engine;
  }

  const GraphicsBackend fallback = (preferred == GraphicsBackend::kD3D12)
                                       ? GraphicsBackend::kD3D11
                                       : GraphicsBackend::kD3D12;
  if (std::unique_ptr<IGraphicsEngine> engine = try_backend(fallback)) {
    *active_backend =
        (fallback == GraphicsBackend::kD3D12) ? "d3d12" : "d3d11";
    const wchar_t* preferred_name =
        (preferred == GraphicsBackend::kD3D12) ? L"Direct3D 12" : L"Direct3D 11";
    const wchar_t* fallback_name =
        (fallback == GraphicsBackend::kD3D12) ? L"Direct3D 12" : L"Direct3D 11";
    std::wstring message = preferred_name;
    message += L" was not available; using ";
    message += fallback_name;
    message += L" instead.";
    MessageBoxW(message_owner, message.c_str(), kWindowTitle,
               MB_OK | MB_ICONWARNING);
    return engine;
  }

  return nullptr;
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

// The volume steps the menu offers, as multiples of the hardware's own level.
// A PlayStation mixes quietly - the discs tested here peak at about a fifth of
// full scale - so the default lifts it rather than being faithful and inaudible.
struct VolumeStep { float value; const wchar_t* label; };

const VolumeStep kVolumeSteps[] = {
  { 0.0f, L"&Mute" },
  { 0.5f, L"&50%%" },
  { 1.0f, L"&100%% (hardware)" },
  { 2.0f, L"&200%%" },
  { 3.0f, L"3&00%%" },
  { 4.0f, L"4&00%%" },
  { 6.0f, L"&600%%" },
  { 8.0f, L"&800%%" },
};

// The two renderer choices, in the order the Video > Renderer menu and
// EmuConfig::kValidGraphicsBackends both list them.
struct BackendChoice { const char* key; const wchar_t* label; };

const BackendChoice kBackendChoices[] = {
  { "d3d11", L"Direct3D &11" },
  { "d3d12", L"Direct3D &12" },
};

// The filter choices - None plus the eight ported from GBAEmu (see
// shaders/), in the order the Video > Filter menu and
// EmuConfig::kValidVideoFilters both list them. Only D3D12 supports these;
// see D3D11Presenter's class comment for why.
struct FilterChoice { const char* key; const wchar_t* label; };

const FilterChoice kFilterChoices[] = {
  { "",            L"&None" },
  { "nearest",     L"&Nearest Neighbor (Legacy)" },
  { "bilinear",    L"&Bilinear" },
  { "crt",         L"CRT (&Legacy)" },
  { "eagle",       L"Super&Eagle" },
  { "hq2x",        L"HQ2X (&Placeholder)" },
  { "xbrz_legacy", L"xBRZ (&Legacy Placeholder)" },
  { "scanline",    L"&Scanline (CRT)" },
  { "xbrz",        L"x&BRZ" },
};

// The three real PS1 controllers a port can hold, in the order
// EmuConfig::kValidControllerTypes and Sio::ControllerType both list them.
struct ControllerTypeChoice { const char* key; const wchar_t* label; };

const ControllerTypeChoice kControllerTypeChoices[] = {
  { "digital",     L"&Original (Digital)" },
  { "dual_analog", L"&Dual Analog (no rumble)" },
  { "dualshock",   L"Dual&Shock" },
};

emulation::psx::Sio::ControllerType ParseControllerType(
    const std::string& key) {
  using emulation::psx::Sio;
  if (key == "digital") return Sio::kDigital;
  if (key == "dual_analog") return Sio::kDualAnalog;
  return Sio::kDualShock;
}

// The three sources a PSX port can be mapped to, in the order
// EmuConfig::kValidInputSources lists them. Front-end-only - Sio has no
// notion of where a port's buttons come from, only what they are.
struct InputSourceChoice { const char* key; const wchar_t* label; };

const InputSourceChoice kInputSourceChoices[] = {
  { "keyboard", L"&Keyboard" },
  { "gamepad1", L"&Gamepad 1" },
  { "gamepad2", L"Gamepad &2" },
};

enum class InputSource { kKeyboard, kGamepad1, kGamepad2 };

InputSource ParseInputSource(const std::string& key) {
  if (key == "gamepad1") return InputSource::kGamepad1;
  if (key == "gamepad2") return InputSource::kGamepad2;
  return InputSource::kKeyboard;
}

// Compiles every ported filter into the engine at once - cheap (startup-cost
// D3DCompile calls, not per-frame work), so there is no reason to defer any
// of them until first selected.
void LoadAllFilters(IGraphicsEngine& engine) {
  engine.LoadPixelShaderFromString("nearest", kLegacyShaders[0]);
  engine.LoadPixelShaderFromString("bilinear", kLegacyShaders[1]);
  engine.LoadPixelShaderFromString("crt", kLegacyShaders[2]);
  engine.LoadPixelShaderFromString("eagle", kLegacyShaders[3]);
  engine.LoadPixelShaderFromString("hq2x", kLegacyShaders[4]);
  engine.LoadPixelShaderFromString("xbrz_legacy", kLegacyShaders[5]);
  engine.LoadCustomPixelShader("scanline", g_ps_scanline_filter, sizeof(g_ps_scanline_filter));
  engine.LoadCustomPixelShader("xbrz", g_ps_xbrz_filter, sizeof(g_ps_xbrz_filter));
}

std::wstring SettingsPathBesideExecutable() {
  wchar_t module[MAX_PATH] = { 0 };
  GetModuleFileNameW(nullptr, module, MAX_PATH);
  std::wstring path = module;
  const size_t slash = path.find_last_of(L"/\\");
  if (slash != std::wstring::npos)
    path.erase(slash + 1);
  return path + L"psxemu.ini";
}

// Writes only when something actually changed, which is what makes it safe to
// call on every edit.
void SaveSettingsIfChanged(Application& app) {
  if (app.system == nullptr || app.settings_path.empty())
    return;
  emulation::psx::SettingsFile updated = app.settings;
  emulation::psx::StoreConfig(updated, app.system->config());
  if (updated.Serialise() == app.settings.Serialise())
    return;
  app.settings = updated;
  app.settings.Save(app.settings_path);
}

// Ticks the step matching the current volume, so the menu shows what is set.
void UpdateVolumeMenu(HWND window, const Application& app) {
  HMENU bar = GetMenu(window);
  if (bar == nullptr || app.system == nullptr)
    return;
  const float current = app.system->config().audio_volume;
  for (size_t i = 0; i < std::size(kVolumeSteps); ++i) {
    const bool on = (current == kVolumeSteps[i].value);
    CheckMenuItem(bar, static_cast<UINT>(kCommandVolumeFirst + i),
                  MF_BYCOMMAND | (on ? MF_CHECKED : MF_UNCHECKED));
  }
}

void SetVolume(Application& app, HWND window, float value) {
  if (app.system == nullptr)
    return;
  app.system->config().audio_volume = value;
  UpdateVolumeMenu(window, app);
  SaveSettingsIfChanged(app);
}

// Ticks the renderer actually running - app.current_backend, not the
// persisted preference, since the two can differ after a fallback.
void UpdateRendererMenu(HWND window, const Application& app) {
  HMENU bar = GetMenu(window);
  if (bar == nullptr)
    return;
  for (size_t i = 0; i < std::size(kBackendChoices); ++i) {
    const bool on = (app.current_backend == kBackendChoices[i].key);
    CheckMenuItem(bar, static_cast<UINT>(kCommandRendererFirst + i),
                  MF_BYCOMMAND | (on ? MF_CHECKED : MF_UNCHECKED));
  }
}

// Ticks the current filter and greys every filter item out when the active
// renderer does not support them - D3D11Presenter's SetPixelShader is a
// no-op, and a menu that silently does nothing on click is worse than one
// that looks unavailable.
void UpdateFilterMenu(HWND window, const Application& app) {
  HMENU bar = GetMenu(window);
  if (bar == nullptr)
    return;
  const bool filters_available = (app.current_backend == "d3d12");
  for (size_t i = 0; i < std::size(kFilterChoices); ++i) {
    const UINT id = static_cast<UINT>(kCommandFilterFirst + i);
    const bool on = filters_available &&
                    (app.current_filter == kFilterChoices[i].key);
    CheckMenuItem(bar, id, MF_BYCOMMAND | (on ? MF_CHECKED : MF_UNCHECKED));
    EnableMenuItem(bar, id,
                   MF_BYCOMMAND | (filters_available ? MF_ENABLED : MF_GRAYED));
  }
}

void SetFilter(Application& app, HWND window, const std::string& key) {
  if (app.current_backend != "d3d12") {
    // Reachable from the settings file (a saved filter with graphics_backend
    // reverted to d3d11) as well as a stray click on a greyed item - either
    // way, say why rather than doing nothing.
    MessageBoxW(window,
                L"Filters require the Direct3D 12 renderer. Switch renderer "
                L"first (Video > Renderer).",
                kWindowTitle, MB_OK | MB_ICONWARNING);
    return;
  }
  if (app.graphics != nullptr)
    app.graphics->SetPixelShader(key);
  app.current_filter = key;
  if (app.system != nullptr)
    app.system->config().video_filter = key;
  UpdateFilterMenu(window, app);
  SaveSettingsIfChanged(app);
}

// Live switch: tears down the active engine and brings up the other one
// against the same window, restoring whichever filter was last saved for
// D3D12 if that is what it switched to. CreateGraphicsEngine's own
// try-then-fallback already covers "the one just picked will not
// initialise"; this only has to handle the (very unlikely, since the engine
// being replaced was working moments ago) case where the fallback fails too.
void SetRenderer(Application& app, HWND window, const std::string& key) {
  if (key == app.current_backend)
    return;

  RECT client;
  GetClientRect(window, &client);
  const int width = client.right - client.left;
  const int height = client.bottom - client.top;

  if (app.graphics != nullptr)
    app.graphics->Shutdown();
  app.graphics.reset();

  const GraphicsBackend preferred =
      (key == "d3d12") ? GraphicsBackend::kD3D12 : GraphicsBackend::kD3D11;
  app.graphics = CreateGraphicsEngine(preferred, window, width, height,
                                      window, &app.current_backend);
  if (app.graphics == nullptr) {
    MessageBoxW(window,
                L"Could not switch renderer, and the previous one could not "
                L"be restored either. Restart the emulator.",
                kWindowTitle, MB_OK | MB_ICONERROR);
    app.current_backend.clear();
    app.current_filter.clear();
    UpdateRendererMenu(window, app);
    UpdateFilterMenu(window, app);
    return;
  }

  app.current_filter.clear();
  if (app.current_backend == "d3d12") {
    LoadAllFilters(*app.graphics);
    const std::string preferred_filter =
        (app.system != nullptr) ? app.system->config().video_filter : "";
    SetFilter(app, window, preferred_filter);   // also saves + updates the menu
  } else {
    UpdateFilterMenu(window, app);
  }

  if (app.system != nullptr)
    app.system->config().graphics_backend = app.current_backend;
  UpdateRendererMenu(window, app);
  SaveSettingsIfChanged(app);
}

// Ticks the controller type actually set on Sio for each port.
void UpdateControllerTypeMenu(HWND window, const Application& app) {
  HMENU bar = GetMenu(window);
  if (bar == nullptr || app.system == nullptr)
    return;
  for (int port = 0; port < 2; ++port) {
    const std::string& current = app.system->config().controller_type[port];
    for (size_t i = 0; i < std::size(kControllerTypeChoices); ++i) {
      const UINT id = static_cast<UINT>(kCommandControllerTypeFirst +
                                        port * 3 + static_cast<int>(i));
      const bool on = (current == kControllerTypeChoices[i].key);
      CheckMenuItem(bar, id, MF_BYCOMMAND | (on ? MF_CHECKED : MF_UNCHECKED));
    }
  }
}

void SetControllerType(Application& app, HWND window, int port,
                       const std::string& key) {
  if (app.system == nullptr)
    return;
  app.system->config().controller_type[port] = key;
  app.system->sio().set_controller_type(port, ParseControllerType(key));
  UpdateControllerTypeMenu(window, app);
  SaveSettingsIfChanged(app);
}

// Ticks which source is mapped to each port.
void UpdateInputSourceMenu(HWND window, const Application& app) {
  HMENU bar = GetMenu(window);
  if (bar == nullptr || app.system == nullptr)
    return;
  for (int port = 0; port < 2; ++port) {
    const std::string& current = app.system->config().input_source[port];
    for (size_t i = 0; i < std::size(kInputSourceChoices); ++i) {
      const UINT id = static_cast<UINT>(kCommandInputSourceFirst +
                                        port * 3 + static_cast<int>(i));
      const bool on = (current == kInputSourceChoices[i].key);
      CheckMenuItem(bar, id, MF_BYCOMMAND | (on ? MF_CHECKED : MF_UNCHECKED));
    }
  }
}

void SetInputSource(Application& app, HWND window, int port,
                    const std::string& key) {
  if (app.system == nullptr)
    return;
  app.system->config().input_source[port] = key;
  UpdateInputSourceMenu(window, app);
  SaveSettingsIfChanged(app);
}

// Ticks whether the drive is being charged for spin-up, seek distance and
// rotational latency. Off is the timing the emulator has always had; on makes
// loading take about as long as a console's, which is most visible on the
// BIOS's "Licensed by SCEA" logo screen - that screen is up for exactly as
// long as the drive takes, and nothing else.
void UpdateCdTimingMenu(HWND window, const Application& app) {
  HMENU bar = GetMenu(window);
  if (bar == nullptr || app.system == nullptr)
    return;
  const bool on = app.system->config().cdrom_mechanical_timing;
  CheckMenuItem(bar, static_cast<UINT>(kCommandCdMechanicalTiming),
                MF_BYCOMMAND | (on ? MF_CHECKED : MF_UNCHECKED));
}

// Takes effect on the next command the drive is given, so there is nothing to
// reset and no reason to make it a cold-boot-only choice - though a boot
// already past its logo screen will not replay it.
void SetCdMechanicalTiming(Application& app, HWND window, bool on) {
  if (app.system == nullptr)
    return;
  app.system->config().cdrom_mechanical_timing = on;
  UpdateCdTimingMenu(window, app);
  SaveSettingsIfChanged(app);
}

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

// Wide to narrow in the codepage the C runtime's fopen expects, which is what
// the core opens files with. Deliberately not UTF-8: on Windows fopen reads a
// char path in the active codepage, so UTF-8 bytes would name the wrong file
// the moment a path stopped being ASCII.
std::string Narrow(const std::wstring& wide) {
  if (wide.empty())
    return std::string();
  const int size = WideCharToMultiByte(CP_ACP, 0, wide.c_str(), -1, nullptr, 0,
                                       nullptr, nullptr);
  if (size <= 1)
    return std::string();
  std::string narrow(static_cast<size_t>(size - 1), '\0');
  WideCharToMultiByte(CP_ACP, 0, wide.c_str(), -1, &narrow[0], size, nullptr,
                      nullptr);
  return narrow;
}

enum class FileDialog { kOpen, kSave };

// One implementation for all four file pickers. There used to be a copy of
// this per dialog, differing only in the filter and two flags.
std::string ChooseFile(HWND window, FileDialog mode, const char* filter,
                       const char* default_extension) {
  char file[MAX_PATH] = { 0 };
  OPENFILENAMEA dialog = {};
  dialog.lStructSize = sizeof(dialog);
  dialog.hwndOwner = window;
  dialog.lpstrFilter = filter;
  dialog.lpstrFile = file;
  dialog.nMaxFile = sizeof(file);
  dialog.lpstrDefExt = default_extension;
  dialog.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
  if (mode == FileDialog::kOpen) {
    dialog.Flags |= OFN_FILEMUSTEXIST;
    if (!GetOpenFileNameA(&dialog))
      return std::string();
  } else {
    dialog.Flags |= OFN_OVERWRITEPROMPT;
    if (!GetSaveFileNameA(&dialog))
      return std::string();
  }
  return std::string(file);
}

constexpr const char* kDiscFilter =
    "Disc Images (*.cue;*.mds;*.bin;*.img;*.iso;*.mdf)\0"
    "*.cue;*.mds;*.bin;*.img;*.iso;*.mdf\0"
    "All files (*.*)\0*.*\0";
constexpr const char* kCardFilter =
    "Memory Card (*.mcr;*.mcd)\0*.mcr;*.mcd\0"
    "All files (*.*)\0*.*\0";
constexpr const char* kExeFilter =
    "PSX Executables (*.exe;*.psx;*.psexe)\0*.exe;*.psx;*.psexe\0"
    "All files (*.*)\0*.*\0";

// Titles the window after whatever is loaded - a disc image, a bare PS-EXE, or
// nothing (the BIOS shell with an empty drive).
void SetWindowTitleForPath(HWND window, const std::string& path) {
  if (path.empty()) {
    SetWindowTextW(window, kWindowTitle);
    return;
  }
  const size_t slash = path.find_last_of("/\\");
  const std::string name =
      (slash == std::string::npos) ? path : path.substr(slash + 1);
  const std::wstring title =
      std::wstring(kWindowTitle) + L" - " + std::wstring(name.begin(), name.end());
  SetWindowTextW(window, title.c_str());
}

// Keyboard to digital pad. Arbitrary but conventional; a real settings file
// belongs here once the core has one.
uint16_t ReadKeyboardPad() {
  struct Binding { int key; uint16_t button; };
  static constexpr Binding kBindings[] = {
    { VK_UP,     Sio::kUp },       { VK_DOWN,  Sio::kDown },
    { VK_LEFT,   Sio::kLeft },     { VK_RIGHT, Sio::kRight },
    { 'X',       Sio::kCross },    { 'Z',      Sio::kSquare },
    { 'S',       Sio::kCircle },   { 'A',      Sio::kTriangle },
    { 'Q',       Sio::kL1 },       { 'W',      Sio::kR1 },
    { '1',       Sio::kL2 },       { '2',      Sio::kR2 },
    { VK_RETURN, Sio::kStart },    { VK_SHIFT, Sio::kSelect },
  };
  uint16_t buttons = 0;
  for (const Binding& binding : kBindings) {
    if (GetAsyncKeyState(binding.key) & 0x8000)
      buttons |= binding.button;
  }
  return buttons;
}

// Creates one directory level, treating "it is already there" as success
// rather than an error - the common case on every run after the first.
bool EnsureDirectory(const std::string& path) {
  if (CreateDirectoryA(path.c_str(), nullptr))
    return true;
  return GetLastError() == ERROR_ALREADY_EXISTS;
}

// Documents\My Games\PSXEmu, following the convention GBAEmu already uses, so
// a person who has one emulator's save data knows where to find the other's.
// CreateDirectoryA only creates one level at a time, so "My Games" is made
// before "PSXEmu" under it.
//
// Empty on failure - which is Documents itself not resolving, not a
// permissions problem on a folder this process just created - and every
// caller treats that as "there is nowhere to keep this" rather than a reason
// to refuse to boot.
std::string ResolveDataRoot() {
  char documents[MAX_PATH] = { 0 };
  if (!SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_MYDOCUMENTS, nullptr, 0,
                                  documents))) {
    return std::string();
  }
  const std::string my_games = std::string(documents) + "\\My Games";
  if (!EnsureDirectory(my_games))
    return std::string();
  const std::string root = my_games + "\\PSXEmu";
  if (!EnsureDirectory(root))
    return std::string();
  return root;
}

// ---------------------------------------------------------------------------
// Machine control
// ---------------------------------------------------------------------------

// Cold boot: the machine comes back in the state it has at power-on. Three
// menu commands need this and each used to carry its own copy.
bool ResetMachine(Application& app, HWND window) {
  app.system->Deinitialize();
  if (app.system->Initialize(app.bios_path.c_str()) != 0) {
    MessageBoxW(window, L"Failed to initialise the system (BIOS missing?).",
                kWindowTitle, MB_OK | MB_ICONERROR);
    return false;
  }
  app.system->set_auto_boot(false);
  return true;
}

// The per-disc identifier used to name its memory card folder: the image's
// own filename, directory and extension stripped. Two copies of the same game
// under different filenames get different cards, which is the same trade-off
// GBAEmu's save files already make for ROMs, and it needs no ISO9660 parsing
// to work on every disc, including ones with no SYSTEM.CNF at all.
std::string DiscIdentifier(const std::string& disc_path) {
  std::string name = disc_path;
  const size_t slash = name.find_last_of("/\\");
  if (slash != std::string::npos)
    name = name.substr(slash + 1);
  const size_t dot = name.find_last_of('.');
  if (dot != std::string::npos)
    name = name.substr(0, dot);
  return name;
}

// Where a save-state slot lives: <savestates_root>\<identifier>.st<slot>,
// the same <identifier> memory cards use (DiscIdentifier), so a state and a
// save are found under the same name per Docs/Save-States-Plan.md. A BIOS-
// only session (no disc mounted) has no disc path to derive that from, so it
// gets a fixed identifier of its own rather than colliding with every other
// BIOS-only session under an empty name.
std::string SaveStateSlotPath(Application& app, int slot) {
  const std::string disc_path = (app.system != nullptr)
                                     ? app.system->cdrom().disc().path()
                                     : std::string();
  const std::string identifier =
      disc_path.empty() ? "bios" : DiscIdentifier(disc_path);
  return app.savestates_root + "\\" + identifier + ".st" +
         std::to_string(slot);
}

// Gives the disc just mounted its own pair of memory cards, in
// memcards_root\<disc>\card1.mcr and card2.mcr - created the first time a
// disc is played and loaded on every boot after that.
//
// Called only from a cold boot. Swapping a disc mid-session leaves the cards
// alone, which is what real hardware does: the memory card slots have nothing
// to do with the disc drive, and disconnecting one under a running game
// mid-swap would be a save silently vanishing from under a game that thinks
// its card is still there.
void LoadOrCreateMemoryCardsForDisc(Application& app, HWND window,
                                    const std::string& disc_path) {
  if (app.memcards_root.empty() || app.system == nullptr)
    return;

  const std::string dir = app.memcards_root + "\\" + DiscIdentifier(disc_path);
  EnsureDirectory(dir);

  for (int slot = 0; slot < 2; ++slot) {
    const std::string path =
        dir + "\\card" + std::to_string(slot + 1) + ".mcr";
    if (app.system->mc(slot).LoadFile(path.c_str()) == S_OK)
      continue;

    // LoadFile fails for two different reasons and only one is worth saying
    // anything about: no card there yet, which is the ordinary case for a
    // game played for the first time, or a file that exists but is not a
    // valid 128 KB card, which CreateFile is about to overwrite.
    FILE* existing = fopen(path.c_str(), "rb");
    const bool had_file = existing != nullptr;
    if (existing != nullptr)
      fclose(existing);

    if (app.system->mc(slot).CreateFile(path.c_str()) != S_OK) {
      const std::wstring message =
          L"Could not create a memory card for slot " +
          std::to_wstring(slot + 1) + L".";
      MessageBoxW(window, message.c_str(), kWindowTitle,
                  MB_OK | MB_ICONWARNING);
    } else if (had_file) {
      const std::wstring message =
          L"The memory card file for slot " + std::to_wstring(slot + 1) +
          L" was not a valid 128 KB card and has been reset:\n\n" +
          std::wstring(path.begin(), path.end());
      MessageBoxW(window, message.c_str(), kWindowTitle,
                  MB_OK | MB_ICONWARNING);
    }
  }
}

// Puts a disc in the drive and starts the machine from cold, which is what
// switching a console on with a game in it does: the BIOS runs its intro,
// checks the disc, reads SYSTEM.CNF, loads the executable it names and jumps
// to it. Nothing here understands the disc - the BIOS does all of it.
bool BootDiscFromFile(Application& app, HWND window, const std::string& path) {
  if (!ResetMachine(app, window))
    return false;
  // The disc has to be in the drive before the BIOS looks, or it finds an open
  // shell and stops at the menu.
  app.system->EjectDisc();
  if (!app.system->LoadDisc(path.c_str())) {
    MessageBoxW(window,
                L"Could not read that disc image.\n\n"
                L"Supported: .cue (with its .bin or .img), .mds (with its "
                L".mdf), .bin, .img, .iso.",
                kWindowTitle, MB_OK | MB_ICONWARNING);
    return false;
  }
  // Each disc gets its own pair of memory cards - a real console has none of
  // this, of course, but "which card was in when I saved" is otherwise a
  // question the player has to answer by hand.
  LoadOrCreateMemoryCardsForDisc(app, window, path);

  SetWindowTitleForPath(window, path);
  app.paused = false;
  return true;
}

// Starts with an empty drive, which lands in the BIOS shell.
void BootBios(Application& app, HWND window) {
  if (!ResetMachine(app, window))
    return;
  app.system->EjectDisc();
  SetWindowTitleForPath(window, std::string());
  app.paused = false;
}

// A quick, read-only sanity check - just the 8-byte magic every PS-EXE
// starts with - so picking the wrong kind of file is caught immediately
// rather than several seconds into a BIOS boot. The authoritative check is
// still System::LoadPsExe, which runs later; this only exists because that
// one cannot run yet without undoing the whole point of booting through the
// BIOS first.
bool LooksLikePsExe(const std::string& path) {
  FILE* fp = fopen(path.c_str(), "rb");
  if (fp == nullptr)
    return false;
  char id[8] = {};
  const size_t read = fread(id, 1, sizeof(id), fp);
  fclose(fp);
  return read == sizeof(id) && memcmp(id, "PS-X EXE", sizeof(id)) == 0;
}

// Boots through the BIOS for real, the same as switching the console on with
// an empty drive, and only once it reaches the address it would hand a game
// control at does the executable get side-loaded on top. Letting the BIOS
// run first is what a raw side-load skips: clearing BEV and Isolate Cache,
// and setting up the default video mode, both of which a standalone test
// program can depend on having happened, the same way it could on real
// hardware.
bool BootPsExeFromFile(Application& app, HWND window, const std::string& path) {
  if (!LooksLikePsExe(path)) {
    MessageBoxW(window,
                L"Could not load that file as a PS-X EXE.\n\n"
                L"It must be the executable itself - the header starts with "
                L"the 8 bytes \"PS-X EXE\" - not a disc image or a Windows "
                L"executable.",
                kWindowTitle, MB_OK | MB_ICONWARNING);
    return false;
  }
  if (!ResetMachine(app, window))
    return false;
  // Nothing about a leftover disc should affect a test program that never
  // asks the CD-ROM for anything.
  app.system->EjectDisc();
  app.system->set_auto_boot_exe(true, path);
  SetWindowTitleForPath(window, path);
  app.paused = false;
  return true;
}

// ---------------------------------------------------------------------------
// Menu
// ---------------------------------------------------------------------------

HMENU CreateMainMenu() {
  HMENU file = CreatePopupMenu();
  AppendMenuW(file, MF_STRING, kCommandBootDisc, L"&Boot disc...");
  AppendMenuW(file, MF_STRING, kCommandSwapDisc, L"S&wap disc...");
  AppendMenuW(file, MF_STRING, kCommandEjectDisc, L"&Eject disc");
  AppendMenuW(file, MF_STRING, kCommandBootBios, L"Boot &BIOS");
  AppendMenuW(file, MF_STRING, kCommandBootExe, L"Boot PSX-&EXE...");
  AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(file, MF_STRING, kCommandOpenMemoryCardSlot1,
              L"Open Memory Card (Slot 1)...");
  AppendMenuW(file, MF_STRING, kCommandOpenMemoryCardSlot2,
              L"Open Memory Card (Slot 2)...");
  AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(file, MF_STRING, kCommandCreateMemoryCardSlot1,
              L"Create Memory Card (Slot 1)...");
  AppendMenuW(file, MF_STRING, kCommandCreateMemoryCardSlot2,
              L"Create Memory Card (Slot 2)...");
  AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(file, MF_STRING, kCommandExit, L"E&xit\tAlt+F4");

  HMENU emulation = CreatePopupMenu();
  AppendMenuW(emulation, MF_STRING, kCommandReset, L"&Reset");
  AppendMenuW(emulation, MF_STRING, kCommandPause, L"&Pause\tSpace");
  AppendMenuW(emulation, MF_SEPARATOR, 0, nullptr);
  // Act on the slot F1-F8 last selected (slot 1 until one of them is
  // pressed), so the keyboard and the menu stay in step with each other.
  AppendMenuW(emulation, MF_STRING, kCommandSaveState,
              L"&Save State\tShift+F1..F8");
  AppendMenuW(emulation, MF_STRING, kCommandLoadState,
              L"&Load State\tF1..F8");
  AppendMenuW(emulation, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(emulation, MF_STRING,
              static_cast<UINT_PTR>(kCommandCdMechanicalTiming),
              L"CD-ROM &Mechanical Timing");

  // Volume. The labels carry a literal percent sign, so they are built with
  // the doubled form the table stores rather than passed through a formatter.
  HMENU volume = CreatePopupMenu();
  for (size_t i = 0; i < std::size(kVolumeSteps); ++i) {
    std::wstring label = kVolumeSteps[i].label;
    size_t percent = label.find(L"%%");
    while (percent != std::wstring::npos) {
      label.erase(percent, 1);
      percent = label.find(L"%%", percent + 1);
    }
    AppendMenuW(volume, MF_STRING,
                static_cast<UINT_PTR>(kCommandVolumeFirst + i), label.c_str());
  }

  HMENU renderer = CreatePopupMenu();
  for (size_t i = 0; i < std::size(kBackendChoices); ++i) {
    AppendMenuW(renderer, MF_STRING,
                static_cast<UINT_PTR>(kCommandRendererFirst + i),
                kBackendChoices[i].label);
  }

  HMENU filter = CreatePopupMenu();
  for (size_t i = 0; i < std::size(kFilterChoices); ++i) {
    AppendMenuW(filter, MF_STRING,
                static_cast<UINT_PTR>(kCommandFilterFirst + i),
                kFilterChoices[i].label);
  }

  HMENU video = CreatePopupMenu();
  AppendMenuW(video, MF_POPUP, reinterpret_cast<UINT_PTR>(renderer),
              L"&Renderer");
  AppendMenuW(video, MF_POPUP, reinterpret_cast<UINT_PTR>(filter), L"&Filter");
  AppendMenuW(video, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(video, MF_STRING, static_cast<UINT_PTR>(kCommandViewVram),
              L"View &VRAM");

  // Two ports, each with its own controller-type choice and its own input
  // source - four small popups rather than one flat list, so ticking one
  // port's choice never has to be told apart from the other's.
  HMENU controller_port[2];
  HMENU source_port[2];
  for (int port = 0; port < 2; ++port) {
    controller_port[port] = CreatePopupMenu();
    for (size_t i = 0; i < std::size(kControllerTypeChoices); ++i) {
      AppendMenuW(controller_port[port], MF_STRING,
                  static_cast<UINT_PTR>(kCommandControllerTypeFirst +
                                        port * 3 + static_cast<int>(i)),
                  kControllerTypeChoices[i].label);
    }
    source_port[port] = CreatePopupMenu();
    for (size_t i = 0; i < std::size(kInputSourceChoices); ++i) {
      AppendMenuW(source_port[port], MF_STRING,
                  static_cast<UINT_PTR>(kCommandInputSourceFirst +
                                        port * 3 + static_cast<int>(i)),
                  kInputSourceChoices[i].label);
    }
  }

  HMENU input = CreatePopupMenu();
  AppendMenuW(input, MF_POPUP, reinterpret_cast<UINT_PTR>(controller_port[0]),
              L"Controller Port &1");
  AppendMenuW(input, MF_POPUP, reinterpret_cast<UINT_PTR>(controller_port[1]),
              L"Controller Port &2");
  AppendMenuW(input, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(input, MF_POPUP, reinterpret_cast<UINT_PTR>(source_port[0]),
              L"Port 1 &Source");
  AppendMenuW(input, MF_POPUP, reinterpret_cast<UINT_PTR>(source_port[1]),
              L"Port 2 S&ource");

  HMENU bar = CreateMenu();
  AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"&File");
  AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(emulation),
              L"&Emulation");
  AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(input), L"&Input");
  AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(volume), L"&Audio");
  AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(video), L"&Video");
  return bar;
}

void OnCommand(Application& app, HWND window, int command) {
  switch (command) {
    case kCommandBootDisc: {
      // Switching the console on with a game in the drive. Pick an image,
      // then the machine starts from cold and the BIOS boots it.
      const std::string path =
          ChooseFile(window, FileDialog::kOpen, kDiscFilter, nullptr);
      if (!path.empty())
        BootDiscFromFile(app, window, path);
      break;
    }

    case kCommandSwapDisc: {
      // Changing the disc in a running machine, for a game that asks for its
      // second one. No reset - that is what Boot disc is for.
      const std::string path =
          ChooseFile(window, FileDialog::kOpen, kDiscFilter, nullptr);
      if (path.empty())
        break;
      if (!app.system->LoadDisc(path.c_str())) {
        MessageBoxW(window, L"Could not read that disc image.", kWindowTitle,
                    MB_OK | MB_ICONWARNING);
        break;
      }
      SetWindowTitleForPath(window, path);
      break;
    }

    case kCommandEjectDisc:
      app.system->EjectDisc();
      SetWindowTitleForPath(window, std::string());
      break;

    case kCommandBootBios:
      BootBios(app, window);
      break;

    case kCommandBootExe: {
      // A standalone test program or homebrew binary - no disc. The BIOS
      // boots normally first; see BootPsExeFromFile for why.
      const std::string path =
          ChooseFile(window, FileDialog::kOpen, kExeFilter, nullptr);
      if (!path.empty())
        BootPsExeFromFile(app, window, path);
      break;
    }

    case kCommandOpenMemoryCardSlot1:
    case kCommandOpenMemoryCardSlot2: {
      const int slot = (command == kCommandOpenMemoryCardSlot1) ? 0 : 1;
      const std::string path =
          ChooseFile(window, FileDialog::kOpen, kCardFilter, "mcr");
      if (path.empty())
        break;
      if (app.system->mc(slot).LoadFile(path.c_str()) != S_OK) {
        MessageBoxW(window,
                    L"Could not open that memory card. It must be exactly "
                    L"128 KB.",
                    kWindowTitle, MB_OK | MB_ICONWARNING);
      }
      break;
    }

    case kCommandCreateMemoryCardSlot1:
    case kCommandCreateMemoryCardSlot2: {
      const int slot = (command == kCommandCreateMemoryCardSlot1) ? 0 : 1;
      const std::string path =
          ChooseFile(window, FileDialog::kSave, kCardFilter, "mcr");
      if (path.empty())
        break;
      if (app.system->mc(slot).CreateFile(path.c_str()) != S_OK) {
        MessageBoxW(window, L"Could not create that memory card file.",
                    kWindowTitle, MB_OK | MB_ICONWARNING);
      }
      break;
    }

    case kCommandReset:
      ResetMachine(app, window);
      break;

    case kCommandPause:
      app.paused = !app.paused;
      break;

    case kCommandSaveState:
      app.pending_save_slot = app.last_slot;
      break;

    case kCommandLoadState:
      app.pending_load_slot = app.last_slot;
      break;

    case kCommandViewVram: {
      app.view_vram = !app.view_vram;
      HMENU bar = GetMenu(window);
      if (bar != nullptr) {
        CheckMenuItem(bar, static_cast<UINT>(kCommandViewVram),
                      MF_BYCOMMAND |
                          (app.view_vram ? MF_CHECKED : MF_UNCHECKED));
      }
      break;
    }

    case kCommandCdMechanicalTiming:
      if (app.system != nullptr) {
        SetCdMechanicalTiming(app, window,
                              !app.system->config().cdrom_mechanical_timing);
      }
      break;

    case kCommandExit:
      PostMessageW(window, WM_CLOSE, 0, 0);
      break;

    default:
      // The volume steps are one contiguous run of command ids.
      if (command >= kCommandVolumeFirst &&
          command < kCommandVolumeFirst +
                        static_cast<int>(std::size(kVolumeSteps))) {
        SetVolume(app, window,
                  kVolumeSteps[command - kCommandVolumeFirst].value);
      } else if (command >= kCommandRendererFirst &&
                command < kCommandRendererFirst +
                              static_cast<int>(std::size(kBackendChoices))) {
        SetRenderer(
            app, window,
            kBackendChoices[command - kCommandRendererFirst].key);
      } else if (command >= kCommandFilterFirst &&
                command < kCommandFilterFirst +
                              static_cast<int>(std::size(kFilterChoices))) {
        SetFilter(app, window,
                 kFilterChoices[command - kCommandFilterFirst].key);
      } else if (command >= kCommandControllerTypeFirst &&
                command <= kCommandControllerTypeLast) {
        const int offset = command - kCommandControllerTypeFirst;
        SetControllerType(app, window, offset / 3,
                          kControllerTypeChoices[offset % 3].key);
      } else if (command >= kCommandInputSourceFirst &&
                command <= kCommandInputSourceLast) {
        const int offset = command - kCommandInputSourceFirst;
        SetInputSource(app, window, offset / 3,
                       kInputSourceChoices[offset % 3].key);
      }
      break;
  }
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam,
                            LPARAM lparam) {
  // The application pointer arrives with the window and lives in its user
  // data, which is what a global used to do less safely.
  if (message == WM_NCCREATE) {
    const CREATESTRUCTW* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
    SetWindowLongPtrW(window, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    return DefWindowProcW(window, message, wparam, lparam);
  }

  Application* app = AppFrom(window);

  switch (message) {
    case WM_SIZE:
      if (app != nullptr && app->graphics != nullptr &&
          wparam != SIZE_MINIMIZED)
        app->graphics->Resize(LOWORD(lparam), HIWORD(lparam));
      return 0;

    case WM_COMMAND:
      // Every command needs the machine, and it does not exist until after the
      // window does.
      if (app != nullptr && app->system != nullptr)
        OnCommand(*app, window, LOWORD(wparam));
      return 0;

    case WM_KEYDOWN:
      if (wparam == VK_SPACE && app != nullptr)
        app->paused = !app->paused;
      if (wparam == VK_ESCAPE)
        PostMessageW(window, WM_CLOSE, 0, 0);
      // F1-F8: plain loads that slot, Shift+ saves it. Both also become the
      // slot the Save State/Load State menu items act on, so pressing F3 and
      // then using the menu (or another F-key) do not disagree about which
      // slot is "current".
      if (app != nullptr && wparam >= VK_F1 && wparam <= VK_F8) {
        const int slot = static_cast<int>(wparam - VK_F1) + 1;
        app->last_slot = slot;
        if (GetKeyState(VK_SHIFT) & 0x8000)
          app->pending_save_slot = slot;
        else
          app->pending_load_slot = slot;
      }
      return 0;

    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;

    default:
      break;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

// ---------------------------------------------------------------------------
// Startup
// ---------------------------------------------------------------------------

// Resolves and creates the per-user directories, once, at startup. A failure
// here is silent - the settings file still lives beside the executable and
// keeps working - because refusing to run the emulator over a save-data
// folder is a worse failure than the one it would be protecting against.
void SetUpDataDirectories(Application& app) {
  app.data_root = ResolveDataRoot();
  if (app.data_root.empty())
    return;
  app.memcards_root = app.data_root + "\\memcards";
  app.savestates_root = app.data_root + "\\savestates";
  EnsureDirectory(app.memcards_root);
  EnsureDirectory(app.savestates_root);
}

// Works out where the BIOS is. A command line wins; otherwise look beside the
// executable and in a bios folder under it, which is where the repository
// keeps it.
std::string FindBios(const std::string& from_command_line) {
  if (!from_command_line.empty()) {
    char full_path[MAX_PATH] = { 0 };
    GetFullPathNameA(from_command_line.c_str(), MAX_PATH, full_path, nullptr);
    return full_path;
  }

  char module[MAX_PATH] = { 0 };
  GetModuleFileNameA(nullptr, module, MAX_PATH);
  std::string directory = module;
  const size_t slash = directory.find_last_of("/\\");
  directory = (slash == std::string::npos) ? std::string()
                                           : directory.substr(0, slash + 1);

  static constexpr const char* kCandidates[] = {
    "bios\\SCPH1001.BIN",
    "SCPH1001.BIN",
    "..\\..\\..\\bios\\SCPH1001.BIN",
  };
  for (const char* candidate : kCandidates) {
    const std::string path = directory + candidate;
    FILE* fp = fopen(path.c_str(), "rb");
    if (fp != nullptr) {
      fclose(fp);
      return path;
    }
  }
  return std::string();
}

// Tries the modern output first and falls back. Audio is optional: a machine
// with no working output device should still run, silently, rather than
// refusing to start.
std::unique_ptr<IAudioEngine> CreateAudioEngine() {
  auto wasapi = std::make_unique<WASAPIAudioEngine>();
  if (wasapi->Initialize(Spu::kSampleRate, 2))
    return wasapi;

  auto dsound = std::make_unique<DirectSoundAudioEngine>();
  if (dsound->Initialize(Spu::kSampleRate, 2))
    return dsound;

  return nullptr;
}

struct CommandLine {
  std::string bios;
  std::string disc;
};

CommandLine ParseCommandLine() {
  CommandLine result;
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (argv == nullptr)
    return result;
  if (argc > 1)
    result.bios = Narrow(argv[1]);
  if (argc > 2)
    result.disc = Narrow(argv[2]);
  LocalFree(argv);
  return result;
}

// ---------------------------------------------------------------------------
// The frame
// ---------------------------------------------------------------------------

// Runs the machine until the GPU says a frame is finished. That keeps the pace
// tied to the emulated display rather than to a timer here, and it is the same
// loop the headless harness runs. The guard stops a machine that has stopped
// producing frames from hanging the window.
void RunOneFrame(Application& app) {
  constexpr uint64_t kMaxInstructionsPerFrame = 8000000;
  const uint64_t target_frame = app.system->gpu().frame_count() + 1;
  uint64_t guard = 0;
  while (app.system->gpu().frame_count() < target_frame &&
         guard++ < kMaxInstructionsPerFrame) {
    app.system->StepInstruction();
  }
}

// Drains whatever the SPU generated during that frame and hands it to the
// audio device. Pulling here rather than pushing from inside the core is what
// keeps the core free of any audio API: it just fills a buffer.
void PumpAudio(Application& app) {
  if (app.audio == nullptr)
    return;
  const int frames = app.system->spu().ReadSamples(
      app.audio_scratch.data(),
      static_cast<int>(app.audio_scratch.size() / 2));
  if (frames > 0)
    app.audio->QueueAudio(app.audio_scratch.data(), frames * 2);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int show) {
  Application app;

  const CommandLine command_line = ParseCommandLine();
  app.bios_path = FindBios(command_line.bios);
  if (app.bios_path.empty()) {
    MessageBoxW(nullptr,
                L"No BIOS image found.\n\n"
                L"A PlayStation BIOS dump is required. Put SCPH1001.BIN in a "
                L"'bios' folder beside the executable, or pass its path as the "
                L"first argument.",
                kWindowTitle, MB_OK | MB_ICONERROR);
    return 1;
  }

  // Loaded early, before the graphics engine exists to hold the choice - the
  // rest of the settings (audio_volume and friends, which live on
  // System::config()) are read back later via LoadConfig, once the machine
  // exists to hold them; graphics_backend is read directly here too, purely
  // to decide which engine to construct, and it is read again through the
  // normal LoadConfig path below to end up in the same place either way.
  app.settings_path = Narrow(SettingsPathBesideExecutable());
  app.settings.Load(app.settings_path);

  WNDCLASSEXW window_class = {};
  window_class.cbSize = sizeof(window_class);
  window_class.style = CS_HREDRAW | CS_VREDRAW;
  window_class.lpfnWndProc = WindowProc;
  window_class.hInstance = instance;
  window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  window_class.hbrBackground =
      reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
  window_class.lpszClassName = kWindowClass;
  if (RegisterClassExW(&window_class) == 0)
    return 1;

  RECT bounds = { 0, 0, 640, 480 };
  AdjustWindowRect(&bounds, WS_OVERLAPPEDWINDOW, TRUE);
  HWND window = CreateWindowExW(
      0, kWindowClass, kWindowTitle, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
      CW_USEDEFAULT, bounds.right - bounds.left, bounds.bottom - bounds.top,
      nullptr, CreateMainMenu(), instance, &app);
  if (window == nullptr)
    return 1;

  {
    RECT client;
    GetClientRect(window, &client);
    const int client_width = client.right - client.left;
    const int client_height = client.bottom - client.top;
    const std::string requested_backend =
        app.settings.GetString("graphics_backend", "d3d11");
    const GraphicsBackend preferred = (requested_backend == "d3d12")
                                          ? GraphicsBackend::kD3D12
                                          : GraphicsBackend::kD3D11;
    app.graphics = CreateGraphicsEngine(preferred, window, client_width,
                                        client_height, window,
                                        &app.current_backend);
  }
  if (app.graphics == nullptr) {
    MessageBoxW(window, L"Could not create a Direct3D device.", kWindowTitle,
                MB_OK | MB_ICONERROR);
    return 1;
  }

  app.audio = CreateAudioEngine();
  if (app.audio != nullptr)
    app.audio->Play();

  app.system = std::make_unique<System>();
  if (app.system->Initialize(app.bios_path.c_str()) != 0) {
    MessageBoxW(window,
                L"The BIOS image could not be loaded. It must be exactly "
                L"512 KB.",
                kWindowTitle, MB_OK | MB_ICONERROR);
    return 1;
  }

  // The rest of the settings, now that the machine exists to hold them - the
  // file itself was already loaded above, before the graphics engine, to
  // decide which one to construct. A missing file is normal on a first run
  // and leaves the defaults in place.
  emulation::psx::LoadConfig(app.settings, app.system->config());
  // The engine actually running can differ from the file's own preference
  // if that one failed and CreateGraphicsEngine fell back - reflect reality
  // rather than silently trusting what LoadConfig just read.
  app.system->config().graphics_backend = app.current_backend;
  UpdateVolumeMenu(window, app);
  UpdateRendererMenu(window, app);
  app.system->sio().set_controller_type(
      0, ParseControllerType(app.system->config().controller_type[0]));
  app.system->sio().set_controller_type(
      1, ParseControllerType(app.system->config().controller_type[1]));
  UpdateControllerTypeMenu(window, app);
  UpdateInputSourceMenu(window, app);
  UpdateCdTimingMenu(window, app);
  if (app.current_backend == "d3d12") {
    LoadAllFilters(*app.graphics);
    SetFilter(app, window, app.system->config().video_filter);
  } else {
    UpdateFilterMenu(window, app);
  }

  // Per-disc data, under Documents\My Games\PSXEmu.
  SetUpDataDirectories(app);

  if (!command_line.disc.empty() &&
      app.system->LoadDisc(command_line.disc.c_str())) {
    SetWindowTitleForPath(window, command_line.disc);
    LoadOrCreateMemoryCardsForDisc(app, window, command_line.disc);
  }

  ShowWindow(window, show);
  UpdateWindow(window);

  app.running = true;
  MSG message = {};
  while (app.running) {
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      if (message.message == WM_QUIT) {
        app.running = false;
        break;
      }
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    if (!app.running)
      break;

    if (app.paused) {
      Sleep(16);
      continue;
    }

    // Actioned here, between frames, never mid-frame - the top of the loop
    // is the one point nothing about the current frame is half-done yet.
    if (app.pending_save_slot >= 0) {
      const std::string path = SaveStateSlotPath(app, app.pending_save_slot);
      const std::string error = app.system->SaveState(path);
      if (!error.empty()) {
        const std::wstring message(error.begin(), error.end());
        MessageBoxW(window, message.c_str(), kWindowTitle,
                    MB_OK | MB_ICONERROR);
      }
      app.pending_save_slot = -1;
    }
    if (app.pending_load_slot >= 0) {
      const std::string path = SaveStateSlotPath(app, app.pending_load_slot);
      const std::string error = app.system->LoadState(path);
      if (!error.empty()) {
        const std::wstring message(error.begin(), error.end());
        MessageBoxW(window, message.c_str(), kWindowTitle,
                    MB_OK | MB_ICONERROR);
      }
      app.pending_load_slot = -1;
    }

    // Input is sampled once per frame, on this thread, and handed to the core.
    // The pads are polled unconditionally, focused or not, so a controller
    // being unplugged mid-game is noticed straight away rather than only
    // after the window is clicked back into; only the *buttons and axes* are
    // withheld while unfocused, matching what the keyboard already does.
    const psxemu::Gamepad::State gamepad_state[2] = {
        app.gamepads[0].Poll(), app.gamepads[1].Poll() };
    const uint16_t keyboard_buttons = ReadKeyboardPad();
    const bool focused = (GetForegroundWindow() == window);

    // Re-applied every frame rather than only when the menu changes it -
    // exactly how set_connected below already has to be, since a Reset or a
    // fresh disc boot reinitialises Sio to its power-on defaults, and this is
    // what makes either pick the configured controller back up without
    // either call site needing to know that happened. set_controller_type is
    // a no-op once converged, so this costs nothing in the steady state.
    app.system->sio().set_controller_type(
        0, ParseControllerType(app.system->config().controller_type[0]));
    app.system->sio().set_controller_type(
        1, ParseControllerType(app.system->config().controller_type[1]));

    for (int port = 0; port < 2; ++port) {
      const InputSource source =
          ParseInputSource(app.system->config().input_source[port]);
      bool connected = true;   // the keyboard is always "there"
      uint16_t buttons = 0;
      uint8_t left_x = 0x80, left_y = 0x80, right_x = 0x80, right_y = 0x80;
      int rumble_target = -1;  // which gamepads[] slot feels this port's motors

      switch (source) {
        case InputSource::kKeyboard:
          buttons = focused ? keyboard_buttons : 0;
          break;
        case InputSource::kGamepad1:
        case InputSource::kGamepad2: {
          const int g = (source == InputSource::kGamepad1) ? 0 : 1;
          connected = app.gamepads[g].connected();
          buttons = focused ? gamepad_state[g].buttons : 0;
          left_x = gamepad_state[g].left_x;
          left_y = gamepad_state[g].left_y;
          right_x = gamepad_state[g].right_x;
          right_y = gamepad_state[g].right_y;
          rumble_target = g;
          break;
        }
      }

      app.system->sio().set_connected(port, connected);
      app.system->sio().set_buttons(port, buttons);
      app.system->sio().set_axes(port, left_x, left_y, right_x, right_y);

      // Rumble is an output, not an input, so it is not gated on focus - the
      // emulated machine keeps running in the background (only Pause
      // actually stops it), and a real console would not silence a
      // controller's motor just because another window has focus. If both
      // ports are ever mapped to the same physical pad, the second port's
      // SetRumble call below simply wins for that frame - a real edge case
      // (mirroring one pad to both ports), not a bug.
      if (rumble_target >= 0) {
        uint8_t motor_small = 0, motor_large = 0;
        app.system->sio().motor_state(port, &motor_small, &motor_large);
        app.gamepads[rumble_target].SetRumble(motor_small, motor_large);
      }
    }

    RunOneFrame(app);
    PumpAudio(app);

    //PumpAudio(app);

    int width = 0;
    int height = 0;
    const uint32_t* pixels = app.system->gpu().framebuffer(width, height);

    // Video > View VRAM substitutes the whole 1 MB VRAM, converted the same
    // way the display area already is, for the display framebuffer - same
    // presentation path, same letterbox helper, just a different (and much
    // bigger, non-4:3) source rectangle. Rebuilt every frame since VRAM is
    // never still while the machine runs.
    if (app.view_vram) {
      const int vram_width = emulation::psx::GpuCore::kVramWidth;
      const int vram_height = emulation::psx::GpuCore::kVramHeight;
      app.vram_view_scratch.resize(
          static_cast<size_t>(vram_width) * vram_height);
      const uint16_t* vram = app.system->gpu().vram();
      for (int i = 0; i < vram_width * vram_height; ++i) {
        const uint16_t p = vram[i];
        const uint32_t r = ((p & 0x1F) << 3) | ((p & 0x1F) >> 2);
        const uint32_t g = (((p >> 5) & 0x1F) << 3) | (((p >> 5) & 0x1F) >> 2);
        const uint32_t b = (((p >> 10) & 0x1F) << 3) | (((p >> 10) & 0x1F) >> 2);
        app.vram_view_scratch[i] = 0xFF000000u | (r << 16) | (g << 8) | b;
      }
      pixels = app.vram_view_scratch.data();
      width = vram_width;
      height = vram_height;
    }

    if (app.graphics != nullptr) {
      app.graphics->BeginFrame();
      app.graphics->RenderFramebuffer(pixels, width, height);
      app.graphics->EndFrame();
    }
  }

  // Written on every change already; this catches anything the last edit
  // missed and costs nothing when there is nothing to write.
  SaveSettingsIfChanged(app);

  // Everything Application owns is released by its destructor, in the order it
  // was declared in.
  return static_cast<int>(message.wParam);
}
