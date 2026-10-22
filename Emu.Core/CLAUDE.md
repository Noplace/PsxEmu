# Emu.Core - instructions for Claude

Emu.Core is a shared static library for every emulator in this family: the machine's thread,
pacing, save states, rewind and run-ahead, settings, disc images, PNGs, host input, an x64 code
emitter, and vendored zlib/LZMA/libchdr. `README.md` beside this file is the developer guide: read
it before using or changing anything here, then read the header of each type you touch (every
header explains itself at the top).

## When you are in another emulator's project and want to use it

1. **Reference it, don't copy it.** Add `$(SolutionDir)Emu.Core` to the include path and a
   `ProjectReference` to `Emu.Core.vcxproj` (README, "Using it from a project"). A static library of
   yours referencing it sets `LinkLibraryDependencies` to false; the executable links it.
2. **Include through `emucore/`**: `#include "emucore/host/machine.h"`. Never put Emu.Core's
   folders themselves on an include path.
3. **To run the console on the shared machine thread, implement `emulation::ICore`**
   (`emucore/core.h`) in your console's library, not in Emu.Core. Model it on PSXEmu's
   `psx::PsxCore` (`PSXEmu.Core/emu/psx_core.h` and `.cpp`). Keep the frame loop inside `RunFrame`,
   with one virtual call a frame, never one a step: a step is called millions of times a second.
4. **Give your console's front end the same three helpers PSXEmu has**: a `CoreOf(machine)` and
   `SystemOf(machine)` cast, and an `ApplyConfig(machine, config)` that gives the console its
   settings and the machine its `MachineConfig`. Then requests stay short:
   `machine.Post([](Machine& m) { SystemOf(m).Reset(); });`.
5. **Save states**: one `Serialise(state::StateIO&)` per component, both directions. Put your file
   format's magic and version in your console's code (PSXEmu: `psx/state_format.h`). You can import
   `StateIO` into your console's namespace with a `using` declaration so components name it short.
6. **Settings**: derive your console's `SettingsFile` from `config::SettingsFile` and give it its
   header line; keep your settings struct and its load/store in your console.
7. **Audio**: the core reports `sample_rate()`. Pass the same rate to the front end's audio device
   (Emu.Frontend.Win32's `CreateAudioEngine(backend, &opened, rate)`).

## When you change Emu.Core itself

- **Nothing of any console goes in.** No `psx/`, no console types, no console constants. If shared
  code needs a console's number, make it a parameter or ask through `ICore`. Check with:
  `git grep -nE '#include "(psx|emu|render|bios|debug|enhance|media|app|ui)/' -- 'Emu.Core/*.h' 'Emu.Core/*.cpp'`
  - it must print nothing (`emucore/` paths are fine).
- **No change in what a machine computes.** A move or refactor here must leave every user's
  results identical. In PSXEmu that means: build every harness, run them, run the disc batch, and
  diff both against the recorded baseline (PSXEmu's `Docs/Shared-Libraries-Plan.md`, phase 0,
  describes the scripts and `Temp\p0\`). Only then commit.
- **Moving more code here**: when a second emulator wants movies, texture packs or breakpoints,
  start from the README's "Candidates" table: it says which half of each comes here and which
  stays the console's.
- **Every user builds.** After changing a header, build PSXEmu.Win32 Release x64 (which builds
  Emu.Core, PSXEmu.Core and Emu.Frontend.Win32) and the harnesses (`PSXEmu.Core\tools\build_tools.bat`,
  or a copy of it with `/MP` added).
- **Keep the PSX-specific leftovers listed in the README** until a second emulator needs them
  changed; then make them parameters, and check PSXEmu's results do not move.
- **Update `README.md`** when you add a module, a type a user is meant to call, or a rule.

## Gotchas that cost time here

- The project folder is the include root, so `"emucore/..."` resolves from any project that has
  `$(SolutionDir)Emu.Core` on its path. A file in Emu.Core that includes `"zlib.h"` or libchdr needs
  the lib folder on its own `AdditionalIncludeDirectories` (see `png_file.cpp` in the vcxproj).
- Files are CRLF in the working copy and LF in git (`core.autocrlf=true`). GNU `sed -i` in Git Bash
  rewrites them LF; use `perl -pi` (keeps `\r`) or the Edit tool.
- The Bash tool halves backslashes in heredocs and inline `sed`/`perl`: write any edit involving
  Windows paths or C++ escapes as a script file with the Write tool, or use the Edit tool.
- Visual Studio, open on the solution, can hold a directory so `git mv` of the whole folder fails
  with "Permission denied": move the files one at a time.
