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
#include "gamepad.h"
#include "audio/wasapiaudioengine.h"
#include "audio/dsoundaudioengine.h"

#include <commdlg.h>
#include <shlobj.h>   // SHGetFolderPathA
#include <shellapi.h>   // CommandLineToArgvW

#include <array>
#include <cstdio>
#include <cstring>
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
  kCommandBootExe,
  kCommandOpenMemoryCardSlot1,
  kCommandOpenMemoryCardSlot2,
  kCommandCreateMemoryCardSlot1,
  kCommandCreateMemoryCardSlot2,
  kCommandReset,
  kCommandPause,
  kCommandVolumeFirst,
  kCommandVolumeLast = kCommandVolumeFirst + 7,
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

  // Scratch for one frame of audio, sized for the worst case at 30 fps. A
  // member rather than a function-local static so there is one per
  // application rather than one per process.
  std::array<int16_t, Spu::kSampleRate / 30 * 2> audio_scratch = {};

  // One XInput pad per PSX controller port. gamepads[0] backs up the
  // keyboard on port 1; gamepads[1] is port 2, which has no keyboard fallback
  // - two controllers need two physical pads, same as the console.
  // gamepad_claimed tracks which XInput user indices are already latched, so
  // the two never end up reading the one physical pad.
  std::array<psxemu::Gamepad, 2> gamepads;
  uint32_t gamepad_claimed = 0;

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
                L"Supported: .cue (with its .bin or .img), .bin, .img, .iso.",
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

  HMENU bar = CreateMenu();
  AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"&File");
  AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(emulation),
              L"&Emulation");
  AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(volume), L"&Audio");
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

  // Settings, once the machine exists to hold them. A missing file is normal
  // on a first run and leaves the defaults in place.
  app.settings_path = Narrow(SettingsPathBesideExecutable());
  app.settings.Load(app.settings_path);
  emulation::psx::LoadConfig(app.settings, app.system->config());
  UpdateVolumeMenu(window, app);

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

    // Input is sampled once per frame, on this thread, and handed to the core.
    // The pads are polled unconditionally, focused or not, so a controller
    // being unplugged mid-game is noticed straight away rather than only
    // after the window is clicked back into; only the *buttons and axes* are
    // withheld while unfocused, matching what the keyboard already does.
    const psxemu::Gamepad::State pad1 = app.gamepads[0].Poll(app.gamepad_claimed);
    const psxemu::Gamepad::State pad2 = app.gamepads[1].Poll(app.gamepad_claimed);
    app.system->sio().set_connected(1, app.gamepads[1].connected());

    const bool focused = (GetForegroundWindow() == window);
    app.system->sio().set_buttons(
        0, focused ? static_cast<uint16_t>(ReadKeyboardPad() | pad1.buttons)
                   : 0);
    app.system->sio().set_buttons(1, focused ? pad2.buttons : 0);
    if (focused) {
      app.system->sio().set_axes(0, pad1.left_x, pad1.left_y, pad1.right_x,
                                 pad1.right_y);
      app.system->sio().set_axes(1, pad2.left_x, pad2.left_y, pad2.right_x,
                                 pad2.right_y);
    } else {
      app.system->sio().set_axes(0, 0x80, 0x80, 0x80, 0x80);
      app.system->sio().set_axes(1, 0x80, 0x80, 0x80, 0x80);
    }

    // Rumble is an output, not an input, so it is not gated on focus - the
    // emulated machine keeps running in the background (only Pause actually
    // stops it), and a real console would not silence a controller's motor
    // just because another window has focus.
    uint8_t small0 = 0, large0 = 0, small1 = 0, large1 = 0;
    app.system->sio().motor_state(0, &small0, &large0);
    app.system->sio().motor_state(1, &small1, &large1);
    app.gamepads[0].SetRumble(small0, large0);
    app.gamepads[1].SetRumble(small1, large1);

    RunOneFrame(app);
    PumpAudio(app);

    PumpAudio(app);

    int width = 0;
    int height = 0;
    const uint32_t* pixels = app.system->gpu().framebuffer(width, height);
    app.presenter.Present(pixels, width, height);
  }

  // Written on every change already; this catches anything the last edit
  // missed and costs nothing when there is nothing to write.
  SaveSettingsIfChanged(app);

  // Everything Application owns is released by its destructor, in the order it
  // was declared in.
  return static_cast<int>(message.wParam);
}
