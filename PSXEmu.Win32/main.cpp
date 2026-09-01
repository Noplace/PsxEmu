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
#include <cstdio>
#include <string>

#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")

using emulation::psx::Spu;

namespace {

const wchar_t kWindowClass[] = L"PSXEmuWindow";
const wchar_t kWindowTitle[] = L"PSXEmu";

// Menu command ids.
enum {
  kCommandOpenDisc = 1000,
  kCommandEjectDisc,
  kCommandBootDisc,
  kCommandBootBios,
  kCommandOpenMemoryCardSlot1,
  kCommandOpenMemoryCardSlot2,
  kCommandCreateMemoryCardSlot1,
  kCommandCreateMemoryCardSlot2,
  kCommandReset,
  kCommandPause,
  kCommandExit,
};

struct Application {
  std::unique_ptr<emulation::psx::System> system;
  psxemu::D3D11Presenter presenter;
  IAudioEngine* audio;
  std::string bios_path;
  bool running;
  bool paused;

  Application()
      : system(nullptr), audio(nullptr), running(false), paused(true) {}
};

Application* g_app = nullptr;

// Keyboard to digital pad. Arbitrary but conventional; a real settings file
// belongs here once the core has one.
uint16_t ReadKeyboardPad() {
  using emulation::psx::Sio;
  uint16_t buttons = 0;
  struct Binding { int key; uint16_t button; };
  static const Binding kBindings[] = {
    { VK_UP,     Sio::kUp },       { VK_DOWN,  Sio::kDown },
    { VK_LEFT,   Sio::kLeft },     { VK_RIGHT, Sio::kRight },
    { 'X',       Sio::kCross },    { 'Z',      Sio::kSquare },
    { 'S',       Sio::kCircle },   { 'A',      Sio::kTriangle },
    { 'Q',       Sio::kL1 },       { 'W',      Sio::kR1 },
    { '1',       Sio::kL2 },       { '2',      Sio::kR2 },
    { VK_RETURN, Sio::kStart },    { VK_SHIFT, Sio::kSelect },
  };
  for (size_t i = 0; i < ARRAYSIZE(kBindings); ++i) {
    if (GetAsyncKeyState(kBindings[i].key) & 0x8000)
      buttons |= kBindings[i].button;
  }
  return buttons;
}

std::string OpenDiscDialog(HWND window) {
  char file[MAX_PATH] = { 0 };
  OPENFILENAMEA dialog;
  memset(&dialog, 0, sizeof(dialog));
  dialog.lStructSize = sizeof(dialog);
  dialog.hwndOwner = window;
  dialog.lpstrFilter =
      "Disc Images (*.cue;*.bin;*.img;*.iso)\0*.cue;*.bin;*.img;*.iso\0"
      "All files (*.*)\0*.*\0";
  dialog.lpstrFile = file;
  dialog.nMaxFile = sizeof(file);
  dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
  if (!GetOpenFileNameA(&dialog))
    return std::string();
  return std::string(file);
}

void SetWindowTitleForDisc(HWND window, const std::string& path) {
  if (path.empty()) {
    SetWindowTextW(window, kWindowTitle);
    return;
  }
  const size_t slash = path.find_last_of("\\/");
  const std::string name = (slash == std::string::npos) ? path : path.substr(slash + 1);
  const std::wstring wide(name.begin(), name.end());
  const std::wstring title = std::wstring(kWindowTitle) + L" - " + wide;
  SetWindowTextW(window, title.c_str());
}

HMENU CreateMainMenu() {
  HMENU file = CreatePopupMenu();
  AppendMenuW(file, MF_STRING, kCommandOpenDisc, L"&Open disc...\tCtrl+O");
  AppendMenuW(file, MF_STRING, kCommandEjectDisc, L"&Eject disc");
  AppendMenuW(file, MF_STRING, kCommandBootDisc, L"&Boot disc");
  AppendMenuW(file, MF_STRING, kCommandBootBios, L"Boot &BIOS");
  AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(file, MF_STRING, kCommandOpenMemoryCardSlot1, L"Open Memory Card (Slot 1)...");
  AppendMenuW(file, MF_STRING, kCommandOpenMemoryCardSlot2, L"Open Memory Card (Slot 2)...");
  AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(file, MF_STRING, kCommandCreateMemoryCardSlot1, L"Create Memory Card (Slot 1)...");
  AppendMenuW(file, MF_STRING, kCommandCreateMemoryCardSlot2, L"Create Memory Card (Slot 2)...");
  AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(file, MF_STRING, kCommandExit, L"E&xit\tAlt+F4");

  HMENU emulation = CreatePopupMenu();
  AppendMenuW(emulation, MF_STRING, kCommandReset, L"&Reset\tCtrl+R");
  AppendMenuW(emulation, MF_STRING, kCommandPause, L"&Pause\tPause");

  HMENU bar = CreateMenu();
  AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"&File");
  AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(emulation),
              L"&Emulation");
  return bar;
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam,
                            LPARAM lparam) {
  switch (message) {
    case WM_SIZE:
      if (g_app != nullptr && wparam != SIZE_MINIMIZED)
        g_app->presenter.Resize(LOWORD(lparam), HIWORD(lparam));
      return 0;

    case WM_COMMAND: {
      if (g_app == nullptr)
        break;
      switch (LOWORD(wparam)) {
        case kCommandOpenDisc: {
          const std::string path = OpenDiscDialog(window);
          if (path.empty())
            break;
          g_app->system->Deinitialize();
          if (g_app->system->Initialize(g_app->bios_path.c_str()) != 0) {
            MessageBoxA(window, "Failed to initialize the system (BIOS missing?).",
                        "PSXEmu", MB_OK | MB_ICONERROR);
            break;
          }
          g_app->system->EjectDisc(); // Start with tray open
          g_app->system->LoadDisc(path.c_str());
          SetWindowTitleForDisc(window, path);
         // g_app->system->set_auto_boot(true, path);
          g_app->system->set_auto_boot(false);
          g_app->paused = false;
          break;
        }
        case kCommandBootBios: {
          g_app->system->Deinitialize();
          if (g_app->system->Initialize(g_app->bios_path.c_str()) != 0) {
            MessageBoxA(window, "Failed to initialize the system (BIOS missing?).",
                        "PSXEmu", MB_OK | MB_ICONERROR);
            break;
          }
          g_app->system->EjectDisc(); // Keep tray open to force shell
          SetWindowTitleForDisc(window, "");
          g_app->system->set_auto_boot(false);
          g_app->paused = false;
          break;
        }
        case kCommandBootDisc: {
          // Reads SYSTEM.CNF and starts the executable it names, skipping the
          // BIOS shell. A failure says which step failed rather than just
          // leaving a black screen.
          emulation::psx::System::DiscBootInfo info;
          if (!g_app->system->BootDisc(&info)) {
            MessageBoxA(window,
                        info.error ? info.error : "The disc could not be booted.",
                        "PSXEmu", MB_OK | MB_ICONWARNING);
            break;
          }
          break;
        }
        case kCommandEjectDisc:
          g_app->system->EjectDisc();
          SetWindowTitleForDisc(window, std::string());
          break;
        case kCommandOpenMemoryCardSlot1:
        case kCommandOpenMemoryCardSlot2: {
          const int slot = (LOWORD(wparam) == kCommandOpenMemoryCardSlot1) ? 0 : 1;
          char file[MAX_PATH] = { 0 };
          OPENFILENAMEA dialog;
          memset(&dialog, 0, sizeof(dialog));
          dialog.lStructSize = sizeof(dialog);
          dialog.hwndOwner = window;
          dialog.lpstrFilter =
              "Memory Card (*.mcr;*.mcd)\0*.mcr;*.mcd\0"
              "All files (*.*)\0*.*\0";
          dialog.lpstrFile = file;
          dialog.nMaxFile = sizeof(file);
          dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
          if (GetOpenFileNameA(&dialog)) {
            if (g_app->system->mc(slot).LoadFile(file) != S_OK) {
              MessageBoxA(window, "Could not open memory card (must be 128KB).", "PSXEmu",
                          MB_OK | MB_ICONWARNING);
            }
          }
          break;
        }
        case kCommandCreateMemoryCardSlot1:
        case kCommandCreateMemoryCardSlot2: {
          const int slot = (LOWORD(wparam) == kCommandCreateMemoryCardSlot1) ? 0 : 1;
          char file[MAX_PATH] = { 0 };
          OPENFILENAMEA dialog;
          memset(&dialog, 0, sizeof(dialog));
          dialog.lStructSize = sizeof(dialog);
          dialog.hwndOwner = window;
          dialog.lpstrFilter =
              "Memory Card (*.mcr;*.mcd)\0*.mcr;*.mcd\0"
              "All files (*.*)\0*.*\0";
          dialog.lpstrFile = file;
          dialog.nMaxFile = sizeof(file);
          dialog.Flags = OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
          dialog.lpstrDefExt = "mcr";
          if (GetSaveFileNameA(&dialog)) {
            if (g_app->system->mc(slot).CreateFile(file) != S_OK) {
              MessageBoxA(window, "Could not create memory card file.", "PSXEmu",
                          MB_OK | MB_ICONWARNING);
            }
          }
          break;
        }
        case kCommandReset:
          g_app->system->Deinitialize();
          g_app->system->Initialize(g_app->bios_path.c_str());
          break;
        case kCommandPause:
          g_app->paused = !g_app->paused;
          break;
        case kCommandExit:
          PostMessage(window, WM_CLOSE, 0, 0);
          break;
        default:
          break;
      }
      return 0;
    }

    case WM_KEYDOWN:
      if (wparam == VK_F12 && g_app && g_app->system) {
       // g_app->system->cpu().DumpTrace("trace.txt");
        //MessageBoxA(window, "Execution trace dumped to trace.txt!", "PSXEmu", MB_OK | MB_ICONINFORMATION);
      }
      if (wparam == VK_SPACE && g_app != nullptr)
        g_app->paused = !g_app->paused;
      if (wparam == VK_ESCAPE)
        PostMessage(window, WM_CLOSE, 0, 0);
      return 0;

    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;

    default:
      break;
  }
  return DefWindowProcW(window, message, wparam, lparam);
}

// Works out where the BIOS is. A command line wins; otherwise look beside the
// executable and in a bios folder under it, which is where the repository
// keeps it.
std::string FindBios(const char* from_command_line) {
  if (from_command_line != nullptr && from_command_line[0] != '\0') {
    char full_path[MAX_PATH];
    GetFullPathNameA(from_command_line, MAX_PATH, full_path, nullptr);
    return full_path;
  }

  char module[MAX_PATH] = { 0 };
  GetModuleFileNameA(nullptr, module, MAX_PATH);
  std::string directory = module;
  const size_t slash = directory.find_last_of("/\\");
  directory = (slash == std::string::npos) ? std::string()
                                           : directory.substr(0, slash + 1);

  const char* kCandidates[] = {
    "bios\\SCPH1001.BIN",
    "SCPH1001.BIN",
    "..\\..\\..\\bios\\SCPH1001.BIN",
  };
  for (size_t i = 0; i < ARRAYSIZE(kCandidates); ++i) {
    const std::string candidate = directory + kCandidates[i];
    FILE* fp = fopen(candidate.c_str(), "rb");
    if (fp != nullptr) {
      fclose(fp);
      return candidate;
    }
  }
  return std::string();
}

}  // namespace



int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int show) {
  Application app;
  g_app = &app;

  // Command line: [bios] [disc]
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  std::string bios_argument;
  std::string disc_argument;
  if (argv != nullptr) {
    if (argc > 1) {
      const std::wstring wide = argv[1];
      int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
      bios_argument.resize(size - 1);
      WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, &bios_argument[0], size, nullptr, nullptr);
    }
    if (argc > 2) {
      const std::wstring wide = argv[2];
      int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
      disc_argument.resize(size - 1);
      WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, &disc_argument[0], size, nullptr, nullptr);
    }
    LocalFree(argv);
  }

  app.bios_path = FindBios(bios_argument.c_str());
  if (app.bios_path.empty()) {
    MessageBoxW(nullptr,
                L"No BIOS image found.\n\n"
                L"A PlayStation BIOS dump is required. Put SCPH1001.BIN in a "
                L"'bios' folder beside the executable, or pass its path as the "
                L"first argument.",
                kWindowTitle, MB_OK | MB_ICONERROR);
    return 1;
  }

  WNDCLASSEXW window_class;
  memset(&window_class, 0, sizeof(window_class));
  window_class.cbSize = sizeof(window_class);
  window_class.style = CS_HREDRAW | CS_VREDRAW;
  window_class.lpfnWndProc = WindowProc;
  window_class.hInstance = instance;
  window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
  window_class.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
  window_class.lpszClassName = kWindowClass;
  RegisterClassExW(&window_class);

  RECT bounds = { 0, 0, 640, 480 };
  AdjustWindowRect(&bounds, WS_OVERLAPPEDWINDOW, TRUE);
  HWND window = CreateWindowExW(
      0, kWindowClass, kWindowTitle, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
      CW_USEDEFAULT, bounds.right - bounds.left, bounds.bottom - bounds.top,
      nullptr, CreateMainMenu(), instance, nullptr);
  if (window == nullptr)
    return 1;

  if (!app.presenter.Initialize(window)) {
    MessageBoxW(window, L"Could not create a Direct3D 11 device.",
                kWindowTitle, MB_OK | MB_ICONERROR);
    return 1;
  }

  // Audio is optional: a machine with no working output device should still
  // run, silently, rather than refusing to start.
  {
    WASAPIAudioEngine* wasapi = new WASAPIAudioEngine();
    if (wasapi->Initialize(Spu::kSampleRate, 2)) {
      app.audio = wasapi;
    } else {
      delete wasapi;
      DirectSoundAudioEngine* dsound = new DirectSoundAudioEngine();
      if (dsound->Initialize(Spu::kSampleRate, 2))
        app.audio = dsound;
      else
        delete dsound;
    }
    if (app.audio != nullptr)
      app.audio->Play();
  }

  app.system = std::make_unique<emulation::psx::System>();
  if (app.system->Initialize(app.bios_path.c_str()) != 0) {
    MessageBoxW(window, L"The BIOS image could not be loaded. It must be "
                        L"exactly 512 KB.",
                kWindowTitle, MB_OK | MB_ICONERROR);
    return 1;
  }

  if (!disc_argument.empty() && app.system->LoadDisc(disc_argument.c_str()))
    SetWindowTitleForDisc(window, disc_argument);

  ShowWindow(window, show);
  UpdateWindow(window);

  app.running = true;
  MSG message;
  memset(&message, 0, sizeof(message));
  
  const bool kUseSeparateThread = false; // Set to true to use System::Run() in a separate thread

  if (kUseSeparateThread) {
    app.system->Run();
  }

  while (app.running) {
    while (PeekMessage(&message, nullptr, 0, 0, PM_REMOVE)) {
      if (message.message == WM_QUIT) {
        app.running = false;
        break;
      }
      TranslateMessage(&message);
      DispatchMessage(&message);
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

    if (!kUseSeparateThread) {
      // Run the machine until the GPU says a frame is finished. That keeps the
      // pace tied to the emulated display rather than to a timer here, and it is
      // the same loop the headless harness runs.
      const uint64_t target_frame = app.system->gpu().frame_count() + 1;
      uint64_t guard = 0;
      const uint64_t kMaxInstructionsPerFrame = 8000000;
      while (app.system->gpu().frame_count() < target_frame &&
             guard++ < kMaxInstructionsPerFrame) {
        app.system->StepInstruction();
      }
    } else {
      // If we are using a separate thread, the machine is running freely.
      // We still need to throttle this UI loop, typically by v-sync or a short sleep
      // to avoid spinning at 100% CPU. Since the thread handles the system execution,
      // we only wait until a new frame is ready to present.
      static uint64_t last_presented_frame = 0;
      while (app.system->gpu().frame_count() == last_presented_frame && app.running) {
        Sleep(1);
      }
      last_presented_frame = app.system->gpu().frame_count();
    }

    // Drain whatever the SPU generated during that frame and hand it to the
    // audio device. Pulling here rather than pushing from inside the core is
    // what keeps the core free of any audio API: it just fills a buffer.
    if (app.audio != nullptr) {
      static int16_t samples[Spu::kSampleRate / 30 * 2];
      const int frames = app.system->spu().ReadSamples(
          samples, static_cast<int>(ARRAYSIZE(samples) / 2));
      if (frames > 0)
        app.audio->QueueAudio(samples, frames * 2);
    }

    int width = 0;
    int height = 0;
    const uint32_t* pixels = app.system->gpu().framebuffer(width, height);
    app.presenter.Present(pixels, width, height);
  }

  if (kUseSeparateThread) {
    app.system->Stop();
  }

  if (app.audio != nullptr) {
    app.audio->Shutdown();
    delete app.audio;
    app.audio = nullptr;
  }

  app.system->Deinitialize();
  
  app.system = nullptr;
  app.presenter.Deinitialize();
  g_app = nullptr;
  return static_cast<int>(message.wParam);
}
