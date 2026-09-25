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
        kCommandAudioBackendFirst,
        kCommandAudioBackendLast = kCommandAudioBackendFirst + 1,   // WASAPI, DirectSound
        kCommandRendererFirst,
        kCommandRendererLast = kCommandRendererFirst + 1,   // Direct3D 11, 12
        kCommandFilterFirst,
        kCommandFilterLast = kCommandFilterFirst + 9,   // None + 9 filters
        kCommandViewVram,
        kCommandFrameLimiter,
        kCommandCdMechanicalTiming,
        kCommandSkipBiosIntro,
        kCommandRecompiler,
        kCommandGpuThread,
        kCommandGpuTransferTiming,
        kCommandICacheTiming,
        kCommandControllerTypeFirst,
        kCommandControllerTypeLast = kCommandControllerTypeFirst + 13,   // 2 ports x 7 types
        kCommandInputSourceFirst,
        kCommandInputSourceLast = kCommandInputSourceFirst + 9,   // 2 ports x 5 sources
        // Player A-D source for whichever port(s) are set to Multitap - greyed out otherwise. A
        // separate command-id range from kCommandInputSource* above, since a port's own source and
        // its four players' sources are ticked/dispatched independently.
        kCommandMultitapSourceFirst,
        kCommandMultitapSourceLast = kCommandMultitapSourceFirst + 39,   // 2 ports x 4 players x 5 sources
        kCommandMultitapTypeFirst,
        kCommandMultitapTypeLast = kCommandMultitapTypeFirst + 31,   // 2 ports x 4 players x 4 types
        // The BIOS images found in the data folder. Unlike every other run here, what these ids
        // mean is not a table in this file but whatever is on disk when the menu was last filled -
        // see App::RefreshBiosMenu, which holds the list the nth id resolves through.
        kCommandBiosFirst,
        kCommandBiosLast = kCommandBiosFirst + 31,   // kMaxBiosEntries
        kCommandRescanBios,
        kCommandOpenBiosFolder,
        kCommandSpeedFirst,
        kCommandSpeedLast = kCommandSpeedFirst + 5,   // 50, 100, 150, 200, 250, 300%
        kCommandPauseInMenus,
        kCommandShowTimings,
        kCommandBiosConsole,
        kCommandSerialToConsole,
        // How a host mouse's movement becomes a PSX mouse's counts, and what the host mouse's
        // own resolution is - see EmuConfig::mouse_motion and kMouseMotionChoices below.
        kCommandMouseMotionFirst,
        kCommandMouseMotionLast = kCommandMouseMotionFirst + 2,   // desktop, windows, hardware
        kCommandMouseDpiFirst,
        kCommandMouseDpiLast = kCommandMouseDpiFirst + 3,         // 400, 800, 1600, 3200
        kCommandEjectMemoryCardSlot1,
        kCommandEjectMemoryCardSlot2,
        kCommandMemoryCardEditor,
        kCommandRecentDiscFirst,
        kCommandRecentDiscLast = kCommandRecentDiscFirst + 7,   // kMaxRecentDiscs
        kCommandClearRecentDiscs,
        kCommandKeyBindings,
        kCommandAnalogButtonPort1,
        kCommandAnalogButtonPort2,
        // Insert, New and Eject for the cards a multitap adds, B-D behind each port (bug 99) -
        // greyed out for a port with no multitap. Card A is the port's own, kCommand*Slot1/2.
        kCommandMultitapCardFirst,
        kCommandMultitapCardLast = kCommandMultitapCardFirst + 17,   // 2 ports x 3 cards x 3 actions
        kCommandDebugger,
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

    // How fast the machine runs against the wall clock, in the order the
    // Emulation > Speed menu and EmuConfig::kValidSpeeds both list them. The
    // labels carry a literal percent sign, stored doubled, the same way the
    // volume steps above do.
    struct SpeedChoice { float value; const wchar_t* label; };

    inline constexpr SpeedChoice kSpeedChoices[] = {
        { 0.5f, L"&50%% (half)" },
        { 1.0f, L"&100%% (console)" },
        { 1.5f, L"1&50%%" },
        { 2.0f, L"&200%% (double)" },
        { 2.5f, L"2&50%%" },
        { 3.0f, L"&300%% (triple)" },
    };

    // How a host mouse's movement is scaled, in the order Input > Mouse > Motion lists them and
    // EmuConfig::kValidMouseMotions holds them. The keys are what psxemu.ini stores.
    struct MouseMotionChoice { const char* key; const wchar_t* label; };

    inline constexpr MouseMotionChoice kMouseMotionChoices[] = {
        { "desktop",  L"Match &Desktop Pointer" },
        { "windows",  L"Windows &Acceleration (approximated)" },
        { "hardware", L"&Hardware (linear, 1994 mouse)" },
    };

    // The host mouse resolutions Input > Mouse > DPI offers, in the order
    // EmuConfig::kValidMouseDpis holds them. Only "hardware" reads this.
    inline constexpr int kMouseDpiChoices[] = { 400, 800, 1600, 3200 };

    // The two renderer choices, in the order the Video > Renderer menu and
    // EmuConfig::kValidGraphicsBackends both list them.
    struct BackendChoice { const char* key; const wchar_t* label; };

    // The sound outputs, in the order Settings > Audio > Output lists them. The keys are what
    // psxemu.ini stores and EmuConfig::kValidAudioBackends accepts.
    inline constexpr BackendChoice kAudioBackendChoices[] = {
        { "wasapi", L"&WASAPI" },
        { "dsound", L"&DirectSound" },
    };

    inline constexpr BackendChoice kBackendChoices[] = {
        { "d3d11", L"Direct3D &11" },
        { "d3d12", L"Direct3D &12" },
    };

    // The filter choices - None plus the ones ported from GBAEmu (see shaders/) and the multi-pass
    // Super-xBR, in the order the Video > Filter menu and EmuConfig::kValidVideoFilters both list them. Only D3D12 supports
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
        { "superxbr",    L"Super-&xBR (3 pass)" },
    };

    // What a port can hold - the three real PS1 controllers, a mouse, a multitap, a GunCon, or
    // nothing at all - in the order EmuConfig::kValidControllerTypes and Sio::ControllerType both list them.
    struct ControllerTypeChoice { const char* key; const wchar_t* label; };

    inline constexpr ControllerTypeChoice kControllerTypeChoices[] = {
        { "digital",     L"&Original (Digital)" },
        { "dual_analog", L"&Dual Analog (no rumble)" },
        { "dualshock",   L"Dual&Shock" },
        { "mouse",       L"&Mouse" },
        { "none",        L"&None (Disconnected)" },
        { "multitap",    L"Multi&tap (4 players)" },
        { "guncon",      L"&GunCon (light gun)" },
    };

    // The sources a PSX port - or, for a Multitap, one of its four players - can be mapped to, in
    // the order EmuConfig::kValidInputSources lists them. Front-end-only - Sio has no notion of
    // where a port's buttons come from, only what they are.
    struct InputSourceChoice { const char* key; const wchar_t* label; };

    // What a multitap's player can be (bug 98), in EmuConfig::kValidMultitapPlayerTypes order.
    inline constexpr ControllerTypeChoice kMultitapPlayerTypeChoices[] = {
        { "digital",     L"&Original (Digital)" },
        { "dual_analog", L"&Dual Analog (no rumble)" },
        { "dualshock",   L"Dual&Shock" },
        { "none",        L"&None (Disconnected)" },
    };

    inline constexpr InputSourceChoice kInputSourceChoices[] = {
        { "keyboard", L"&Keyboard" },
        { "gamepad1", L"&Gamepad 1" },
        { "gamepad2", L"Gamepad &2" },
        { "gamepad3", L"Gamepad &3" },
        { "gamepad4", L"Gamepad &4" },
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
    inline constexpr const char* kSaveFilter =
        "Single Save (*.mcs)\0*.mcs\0"
        "All files (*.*)\0*.*\0";

    // The debugger's labels: text, one "address name" per line.
    inline constexpr const char* kLabelFilter =
        "Labels (*.txt;*.sym)\0*.txt;*.sym\0"
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

    // How big a PlayStation BIOS image is, and what ScanBiosFolder recognises one by. The core
    // requires exactly this and refuses anything else, so a file of this size is the honest
    // definition of "a BIOS the user could pick" - dumps are named every way imaginable.
    inline constexpr uint32_t kBiosImageBytes = 512 * 1024;

    // How many BIOS images the Settings > BIOS menu can list. A command id run has to be fixed at
    // compile time, and anyone with more than this many dumps in one folder has a different
    // problem; the list is truncated rather than overflowing into the next run's ids.
    inline constexpr int kMaxBiosEntries = 32;

    // How the one function that refills a menu at runtime finds it again - the BIOS list, whose
    // contents depend on what is on disk. The popup carries this in its item data and is found by
    // searching the bar for it.
    //
    // It used to be found by position - the bar's sixth item, then the Settings menu's first -
    // with a comment warning that reordering either would silently refill the wrong popup. Moving
    // Input, Audio and Video into Settings is exactly that reordering, so the position went and the
    // tag replaced it: the BIOS list can now move anywhere without anything having to be told.
    inline constexpr ULONG_PTR kBiosMenuTag = 0x42494F53;   // 'BIOS'

    // File > Recent Discs: the other menu filled at runtime, found the same way.
    inline constexpr ULONG_PTR kRecentDiscsMenuTag = 0x52435344;   // 'RCSD'
    inline constexpr int kMaxRecentDiscs = 8;

    // ---------------------------------------------------------------------------------------------
    // Input
    // ---------------------------------------------------------------------------------------------

    // Keyboard to digital pad: the pad's fourteen buttons, the default key for each, the key it
    // is stored under in psxemu.ini, and what Settings > Input > Keyboard Bindings calls it. The
    // keys themselves are the person's to change - see keyboard.h - and this is only where they
    // start.
    // The ANALOG button is not one of the pad's sixteen button bits: pressing it changes the
    // pad's mode rather than being reported to the game. So it rides above them, in bit 16, and
    // App::ApplyInput takes it back out (bug 97).
    inline constexpr uint32_t kAnalogKey = 1u << 16;

    struct KeyBinding {
        int key;
        uint32_t button;   // a Sio::k* bit, or kAnalogKey
        const char* setting;
        const wchar_t* label;
    };

    // clang-format off
    inline constexpr KeyBinding kKeyBindings[] = {
        { VK_UP,     emulation::psx::Sio::kUp,       "key_up",       L"Up" },
        { VK_DOWN,   emulation::psx::Sio::kDown,     "key_down",     L"Down" },
        { VK_LEFT,   emulation::psx::Sio::kLeft,     "key_left",     L"Left" },
        { VK_RIGHT,  emulation::psx::Sio::kRight,    "key_right",    L"Right" },
        { 'X',       emulation::psx::Sio::kCross,    "key_cross",    L"Cross" },
        { 'Z',       emulation::psx::Sio::kSquare,   "key_square",   L"Square" },
        { 'S',       emulation::psx::Sio::kCircle,   "key_circle",   L"Circle" },
        { 'A',       emulation::psx::Sio::kTriangle, "key_triangle", L"Triangle" },
        { 'Q',       emulation::psx::Sio::kL1,       "key_l1",       L"L1" },
        { 'W',       emulation::psx::Sio::kR1,       "key_r1",       L"R1" },
        { '1',       emulation::psx::Sio::kL2,       "key_l2",       L"L2" },
        { '2',       emulation::psx::Sio::kR2,       "key_r2",       L"R2" },
        { VK_RETURN, emulation::psx::Sio::kStart,    "key_start",    L"Start" },
        { VK_SHIFT,  emulation::psx::Sio::kSelect,   "key_select",   L"Select" },
        { 'E',       kAnalogKey,                     "key_analog",   L"ANALOG" },
    };
    // clang-format on
    inline constexpr int kPadButtons = static_cast<int>(std::size(kKeyBindings));

    // How long a port stays empty when the Input menu swaps its controller for a different kind,
    // before the new one is plugged in - see App::SetControllerType. About a second, roughly what
    // swapping a pad by hand takes: Bomberman Party Edition needs somewhere between eleven and
    // thirty frames of an empty port before it stops reading the old device's layout, and there is
    // no reason to think every game is that quick.
    inline constexpr int kControllerReplugFrames = 60;

    // ---------------------------------------------------------------------------------------------
    // The frame
    // ---------------------------------------------------------------------------------------------
    //
    // Nothing about running a frame lives here any more. The machine's own thread owns all of it -
    // how long a frame may run for before it is given up on is host::Machine::kMaxInstructions-
    // PerFrame, and how much sound to keep in hand is host::Machine::kAudioTargetFrames, measured
    // in the ring between the machine and the audio thread rather than in any one device.

}   // namespace psxemu
