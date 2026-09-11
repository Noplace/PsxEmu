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

// Every constant the front end has, in one place: window names, menu command ids, the tables the
// menus are built and ticked from, the file dialog filters, where the BIOS is looked for, and the
// keyboard map.
//
// None of it is state. `inline constexpr` rather than plain `const` so a table read by three
// translation units is still one object rather than a private copy in each, and so all of it is
// usable in a constant expression.
//
// The order a table lists its entries in is load-bearing wherever the menu ids are contiguous runs
// (see MenuCommand): the nth entry of a table is the nth id of its run, in both directions - the
// menu is built by walking the table, and a click is turned back into a table entry by subtracting
// the run's first id. Reordering a table without reordering EmuConfig's matching kValid* list
// silently remaps the settings file.

#include "framework.h"

namespace psxemu {

    // ---------------------------------------------------------------------------------------------
    // Window
    // ---------------------------------------------------------------------------------------------

    inline constexpr wchar_t kWindowClass[] = L"PSXEmuWindow";
    inline constexpr wchar_t kWindowTitle[] = L"PSXEmu";

    // ---------------------------------------------------------------------------------------------
    // Menu command ids
    // ---------------------------------------------------------------------------------------------

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
        kCommandFilterLast = kCommandFilterFirst + 8,   // None + 8 filters
        kCommandViewVram,
        kCommandFrameLimiter,
        kCommandCdMechanicalTiming,
        kCommandControllerTypeFirst,
        kCommandControllerTypeLast = kCommandControllerTypeFirst + 5,   // 2 ports x 3 types
        kCommandInputSourceFirst,
        kCommandInputSourceLast = kCommandInputSourceFirst + 5,   // 2 ports x 3 sources
        kCommandExit,
    };

    // ---------------------------------------------------------------------------------------------
    // Menu choices
    // ---------------------------------------------------------------------------------------------

    // clang-format off
    //
    // Every table below is laid out one entry per line, in the order its menu ids run in. That
    // correspondence is the thing a reader has to be able to check at a glance, so the layout is
    // kept by hand rather than packed onto shared lines.

    // The volume steps the menu offers, as multiples of the hardware's own level. A PlayStation
    // mixes quietly - the discs tested here peak at about a fifth of full scale - so the default
    // lifts it rather than being faithful and inaudible.
    //
    // The labels carry a literal percent sign, stored doubled, because the menu builder collapses
    // the pair rather than passing them through a formatter.
    struct VolumeStep { float value; const wchar_t* label; };

    inline constexpr VolumeStep kVolumeSteps[] = {
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

    inline constexpr BackendChoice kBackendChoices[] = {
        { "d3d11", L"Direct3D &11" },
        { "d3d12", L"Direct3D &12" },
    };

    // The filter choices - None plus the eight ported from GBAEmu (see shaders/), in the order the
    // Video > Filter menu and EmuConfig::kValidVideoFilters both list them. Only D3D12 supports
    // these; see D3D11Presenter's class comment for why.
    struct FilterChoice { const char* key; const wchar_t* label; };

    inline constexpr FilterChoice kFilterChoices[] = {
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

    // The three real PS1 controllers a port can hold, in the order EmuConfig::kValidControllerTypes
    // and Sio::ControllerType both list them.
    struct ControllerTypeChoice { const char* key; const wchar_t* label; };

    inline constexpr ControllerTypeChoice kControllerTypeChoices[] = {
        { "digital",     L"&Original (Digital)" },
        { "dual_analog", L"&Dual Analog (no rumble)" },
        { "dualshock",   L"Dual&Shock" },
    };

    // The three sources a PSX port can be mapped to, in the order EmuConfig::kValidInputSources
    // lists them. Front-end-only - Sio has no notion of where a port's buttons come from, only what
    // they are.
    struct InputSourceChoice { const char* key; const wchar_t* label; };

    inline constexpr InputSourceChoice kInputSourceChoices[] = {
        { "keyboard", L"&Keyboard" },
        { "gamepad1", L"&Gamepad 1" },
        { "gamepad2", L"Gamepad &2" },
    };

    // clang-format on

    // ---------------------------------------------------------------------------------------------
    // File dialogs
    // ---------------------------------------------------------------------------------------------

    // Double-null-terminated pairs of description and pattern, which is the shape OPENFILENAMEA
    // wants rather than anything this project chose.
    inline constexpr const char* kDiscFilter =
        "Disc Images (*.cue;*.mds;*.bin;*.img;*.iso;*.mdf)\0"
        "*.cue;*.mds;*.bin;*.img;*.iso;*.mdf\0"
        "All files (*.*)\0*.*\0";
    inline constexpr const char* kCardFilter =
        "Memory Card (*.mcr;*.mcd)\0*.mcr;*.mcd\0"
        "All files (*.*)\0*.*\0";
    inline constexpr const char* kExeFilter =
        "PSX Executables (*.exe;*.psx;*.psexe)\0*.exe;*.psx;*.psexe\0"
        "All files (*.*)\0*.*\0";

    // ---------------------------------------------------------------------------------------------
    // BIOS
    // ---------------------------------------------------------------------------------------------

    // Where a BIOS image is looked for when the command line did not name one, relative to the
    // executable, in the order they are tried. The third is what makes the emulator work when run
    // straight out of the build directory, where the repository's own bios folder is three levels up.
    // clang-format off
    inline constexpr const char* kBiosCandidates[] = {
        "bios\\SCPH1001.BIN",
        "SCPH1001.BIN",
        "..\\..\\..\\bios\\SCPH1001.BIN",
    };
    // clang-format on

    // ---------------------------------------------------------------------------------------------
    // Input
    // ---------------------------------------------------------------------------------------------

    // Keyboard to digital pad. Arbitrary but conventional; a real settings file belongs here once
    // the core has one.
    struct KeyBinding {
        int key;
        uint16_t button;
    };

    // clang-format off
    inline constexpr KeyBinding kKeyBindings[] = {
        { VK_UP,     emulation::psx::Sio::kUp },
        { VK_DOWN,   emulation::psx::Sio::kDown },
        { VK_LEFT,   emulation::psx::Sio::kLeft },
        { VK_RIGHT,  emulation::psx::Sio::kRight },
        { 'X',       emulation::psx::Sio::kCross },
        { 'Z',       emulation::psx::Sio::kSquare },
        { 'S',       emulation::psx::Sio::kCircle },
        { 'A',       emulation::psx::Sio::kTriangle },
        { 'Q',       emulation::psx::Sio::kL1 },
        { 'W',       emulation::psx::Sio::kR1 },
        { '1',       emulation::psx::Sio::kL2 },
        { '2',       emulation::psx::Sio::kR2 },
        { VK_RETURN, emulation::psx::Sio::kStart },
        { VK_SHIFT,  emulation::psx::Sio::kSelect },
    };
    // clang-format on

    // ---------------------------------------------------------------------------------------------
    // The frame
    // ---------------------------------------------------------------------------------------------

    // How long one frame is allowed to run for before the loop gives up on it. Not a timing
    // constant - a machine that has stopped producing frames at all would otherwise hang the
    // window, and this is what makes that show up as a frozen picture rather than a hung process.
    inline constexpr uint64_t kMaxInstructionsPerFrame = 8000000;

}   // namespace psxemu
