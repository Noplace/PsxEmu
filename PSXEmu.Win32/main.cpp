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
// Owns a window, a Direct3D presenter and the message loop. It does not own
// any emulation: the core in PSXEmu.Core rasterises every pixel on the CPU and
// this only uploads the finished frame. The one thing that flows the other way
// is input, through the core's SIO device.
//
//   PSXEmu.Win32.exe [bios.bin] [disc]
//

#include "psx/psx.h"

#include "d3d11_presenter.h"
#include "audio/wasapiaudioengine.h"
#include "audio/dsoundaudioengine.h"

#include <commdlg.h>
#include <shellapi.h>   // CommandLineToArgvW

#include <array>
#include <cstdio>
#include <iterator>
#include <memory>
#include <string>

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
  kCommandOpenMemoryCardSlot1,
  kCommandOpenMemoryCardSlot2,
  kCommandCreateMemoryCardSlot1,
  kCommandCreateMemoryCardSlot2,
  kCommandReset,
  kCommandPause,
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
  psxemu::D3D11Presenter presenter;
  std::unique_ptr<IAudioEngine> audio;
  std::unique_ptr<System> system;

  std::string bios_path;
  bool running = false;
  bool paused = true;

  // Scratch for one frame of audio, sized for the worst case at 30 fps. A
  // member rather than a function-local static so there is one per
  // application rather than one per process.
  std::array<int16_t, Spu::kSampleRate / 30 * 2> audio_scratch = {};

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
    "Disc Images (*.cue;*.bin;*.img;*.iso)\0*.cue;*.bin;*.img;*.iso\0"
    "All files (*.*)\0*.*\0";
constexpr const char* kCardFilter =
    "Memory Card (*.mcr;*.mcd)\0*.mcr;*.mcd\0"
    "All files (*.*)\0*.*\0";

void SetWindowTitleForDisc(HWND window, const std::string& path) {
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
                L"Supported: .cue (with its .bin or .img), .bin, .img, .iso.",
                kWindowTitle, MB_OK | MB_ICONWARNING);
    return false;
  }
  SetWindowTitleForDisc(window, path);
  app.paused = false;
  return true;
}

// Starts with an empty drive, which lands in the BIOS shell.
void BootBios(Application& app, HWND window) {
  if (!ResetMachine(app, window))
    return;
  app.system->EjectDisc();
  SetWindowTitleForDisc(window, std::string());
  app.paused = false;
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

  HMENU bar = CreateMenu();
  AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"&File");
  AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(emulation),
              L"&Emulation");
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
      SetWindowTitleForDisc(window, path);
      break;
    }

    case kCommandEjectDisc:
      app.system->EjectDisc();
      SetWindowTitleForDisc(window, std::string());
      break;

    case kCommandBootBios:
      BootBios(app, window);
      break;

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

    case kCommandExit:
      PostMessageW(window, WM_CLOSE, 0, 0);
      break;

    default:
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
      if (app != nullptr && wparam != SIZE_MINIMIZED)
        app->presenter.Resize(LOWORD(lparam), HIWORD(lparam));
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

  if (!app.presenter.Initialize(window)) {
    MessageBoxW(window, L"Could not create a Direct3D 11 device.",
                kWindowTitle, MB_OK | MB_ICONERROR);
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

  if (!command_line.disc.empty() &&
      app.system->LoadDisc(command_line.disc.c_str())) {
    SetWindowTitleForDisc(window, command_line.disc);
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

    // Input is sampled once per frame, on this thread, and handed to the core.
    const bool focused = (GetForegroundWindow() == window);
    app.system->sio().set_buttons(0, focused ? ReadKeyboardPad() : 0);

    RunOneFrame(app);
    PumpAudio(app);

    int width = 0;
    int height = 0;
    const uint32_t* pixels = app.system->gpu().framebuffer(width, height);
    app.presenter.Present(pixels, width, height);
  }

  // Everything Application owns is released by its destructor, in the order it
  // was declared in.
  return static_cast<int>(message.wParam);
}
