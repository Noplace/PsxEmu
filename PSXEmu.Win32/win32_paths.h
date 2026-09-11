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
#pragma once

// Where the front end's files are and what they are called: the ones the command line named, the
// BIOS, the settings file, the per-user data root, and the names derived from a disc image.
//
// None of this knows the application exists. Every function here takes what it needs and returns a
// path, which is what lets the app hold one line per question ("where do save states go") rather
// than the answer to it.

#include "framework.h"

namespace psxemu {

    // What the two optional arguments named, if anything: PSXEmu.Win32.exe [bios.bin] [disc].
    // Either or both may be empty, which is the ordinary case.
    struct CommandLine {
        std::string bios;
        std::string disc;
    };

    CommandLine ParseCommandLine();

    // Wide to narrow in the codepage the C runtime's fopen expects, which is what the core opens
    // files with. Deliberately not UTF-8: on Windows fopen reads a char path in the active
    // codepage, so UTF-8 bytes would name the wrong file the moment a path stopped being ASCII.
    std::string Narrow(const std::wstring& wide);

    // Creates one directory level, treating "it is already there" as success rather than an error -
    // the common case on every run after the first.
    bool EnsureDirectory(const std::string& path);

    // Documents\My Games\PSXEmu, following the convention GBAEmu already uses, so a person who has
    // one emulator's save data knows where to find the other's. CreateDirectoryA only creates one
    // level at a time, so "My Games" is made before "PSXEmu" under it.
    //
    // Empty on failure - which is Documents itself not resolving, not a permissions problem on a
    // folder this process just created - and every caller treats that as "there is nowhere to keep
    // this" rather than a reason to refuse to boot.
    std::string ResolveDataRoot();

    // psxemu.ini beside the executable. Not under the data root: it has to be readable before the
    // data root has been resolved, since it is what says which graphics backend to bring up.
    std::wstring SettingsPathBesideExecutable();

    // Works out where the BIOS is. A command line wins; otherwise the candidates in const.h are
    // tried in order, relative to the executable. Empty if none of them is there.
    std::string FindBios(const std::string& from_command_line);

    // The per-disc identifier used to name its memory card folder: the image's own filename,
    // directory and extension stripped. Two copies of the same game under different filenames get
    // different cards, which is the same trade-off GBAEmu's save files already make for ROMs, and
    // it needs no ISO9660 parsing to work on every disc, including ones with no SYSTEM.CNF at all.
    std::string DiscIdentifier(const std::string& disc_path);

    // Where a save-state slot lives: <savestates_root>\<identifier>.st<slot>, the same <identifier>
    // memory cards use (DiscIdentifier), so a state and a save are found under the same name per
    // Docs/Save-States-Plan.md. A BIOS-only session has no disc path to derive that from, so an
    // empty `disc_path` gets a fixed identifier of its own rather than colliding with every other
    // BIOS-only session under an empty name.
    std::string SaveStateSlotPath(const std::string& savestates_root, const std::string& disc_path,
                                  int slot);

    // A quick, read-only sanity check - just the 8-byte magic every PS-EXE starts with - so picking
    // the wrong kind of file is caught immediately rather than several seconds into a BIOS boot.
    // The authoritative check is still System::LoadPsExe, which runs later; this only exists
    // because that one cannot run yet without undoing the whole point of booting through the BIOS
    // first.
    bool LooksLikePsExe(const std::string& path);

}   // namespace psxemu
