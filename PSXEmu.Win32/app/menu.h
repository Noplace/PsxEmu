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

// Building the menu bar, and turning the settings-file keys the menus offer back into the types the
// rest of the front end acts on.
//
// What is *not* here: what happens when an item is clicked. That is an answer about the running
// machine rather than about the menu, so it belongs to the application. This file knows what the
// menu contains and how a value is shown in it, both of which are decided by the tables in const.h.

#include "app/framework.h"

namespace psxemu {

    // The whole menu bar, built from the choice tables in const.h so a table and its menu cannot
    // drift apart. Ownership passes to the caller, which in practice means passing it to
    // CreateWindowExW and letting the window own it from then on.
    HMENU CreateMainMenu();

    // The window's menu bar, showing or not. Full screen takes the bar off the window
    // (App::SetFullscreen) and keeps it in a window property under kDetachedMenuProp, and every
    // tick made meanwhile still has to land on it, so the bar is right when it comes back.
    HMENU MenuBar(HWND window);

    // Fills in Settings > BIOS from the images actually found in the data folder, ticking whichever
    // one is in use, and appends the rescan and open-folder items under them. The only menu whose
    // contents are not a table in const.h, so the only one that has to be built after startup - and
    // rebuilt whenever the folder is rescanned.
    //
    // `current` is a filename, not a path: what is ticked is the entry naming the same file, so a
    // BIOS chosen from somewhere else entirely (the command line) ticks nothing, which is right.
    void PopulateBiosMenu(HWND window, const std::vector<std::string>& files,
                          const std::string& current);

    // Fills in File > Recent Discs, most recent first - the first entry is the last disc played.
    void PopulateRecentDiscsMenu(HWND window, const std::vector<std::string>& discs);

    // ---------------------------------------------------------------------------------------------
    // Ticks
    // ---------------------------------------------------------------------------------------------

    // Which item shows as chosen. Each takes the value to tick against rather than reaching for it,
    // so there is one copy of "what is set" and the menu is only ever a view of it.
    //
    // Each resolves the window's own bar and does nothing if it has none, which is what makes them
    // safe to call before a menu has been attached. None of them touches the machine, so a caller
    // holding a value read from a machine that might not exist yet is the one that has to check.

    // The step matching the current volume, so the menu shows what is set.
    void TickVolume(HWND window, float current);

    // Which speed the machine is paced to. Never greyed: a speed needs the frame
    // limiter, and App::SetSpeed turns it on rather than leaving the menu
    // looking broken when it is off.
    void TickSpeed(HWND window, float current);

    // The renderer actually running, which is not necessarily the persisted preference - creating
    // the preferred one can fall back to the other.
    void TickAudioBackend(HWND window, const std::string& backend);
    void TickRenderer(HWND window, const std::string& backend);

    // The current filter, and every filter item greyed out when the active renderer does not
    // support them - D3D11Presenter's SetPixelShader is a no-op, and a menu that silently does
    // nothing on click is worse than one that looks unavailable.
    void TickFilter(HWND window, const std::string& backend, const std::string& filter);

    // The controller type set on each port, and the source each port's buttons come from. A port
    // whose type does not use a source at all - Sio::kMouse's mapping is fixed to the real mouse,
    // Sio::kNone has no buttons to source, Sio::kMultitap sources each of its four players
    // separately (see TickMultitapSources) rather than the port as a whole - has its source items
    // greyed out rather than removed, the same treatment TickFilter already gives a filter menu the
    // active renderer cannot use.
    void TickControllerTypes(HWND window, const std::array<std::string, 2>& types);
    void TickInputSources(HWND window, const std::array<std::string, 2>& sources,
                          const std::array<std::string, 2>& controller_types);

    // The kind of pad each of a Multitap's four players is, and the Memory Cards items for the
    // three cards a multitap adds - both greyed out for a port with no multitap.
    void TickMultitapTypes(
        HWND window, const std::array<std::array<std::string, 4>, 2>& types,
        const std::array<std::string, 2>& controller_types);

    // The source each of a Multitap's four players comes from, for whichever port(s) are actually
    // set to Sio::kMultitap - greyed out for a port that is not, the same way TickInputSources
    // greys out a port's own source for a type that does not use one.
    void TickMultitapSources(
        HWND window, const std::array<std::array<std::string, 4>, 2>& sources,
        const std::array<std::string, 2>& controller_types);

    // Whether the machine is being held to the emulated display's frame rate. Ticked is a console;
    // unticked runs at whatever the monitor's refresh rate or the sound device allows, which the
    // title bar's percentage shows.
    void TickFrameLimiter(HWND window, bool on);

    // Whether the drive is being charged for spin-up, seek distance and rotational latency. Off is
    // the timing the emulator has always had; on makes loading take about as long as a console's,
    // which is most visible on the BIOS's "Licensed by SCEA" logo screen - that screen is up for
    // exactly as long as the drive takes, and nothing else.
    void TickCdTiming(HWND window, bool on);

    // Whether a disc boot arms the BIOS hand-off that skips its logo and disc-check screens - see
    // EmuConfig::skip_bios_intro. Has no separate tick of its own for a PS-EXE boot: that path
    // already always uses the same hand-off, regardless of this setting.
    void TickSkipBiosIntro(HWND window, bool on);
    void TickRecompiler(HWND window, bool on);
    void TickGpuThread(HWND window, bool on);
    void TickGpuTransferTiming(HWND window, bool on);
    void TickICacheTiming(HWND window, bool on);

    // EmuConfig::pause_in_menus and EmuConfig::show_timings.
    void TickPauseInMenus(HWND window, bool on);
    void TickShowTimings(HWND window, bool on);

    // EmuConfig::show_bios_console - whether the BIOS console window is open.
    void TickBiosConsole(HWND window, bool on);

    // EmuConfig::sio1_to_console - whether what the serial port transmits is
    // shown in that same console.
    void TickSerialToConsole(HWND window, bool on);
    void TickFullscreen(HWND window, bool on);

    // EmuConfig::mouse_motion and EmuConfig::mouse_dpi - how a host mouse's movement becomes a
    // PSX mouse's counts, and what the host mouse's own resolution is.
    void TickMouseMotion(HWND window, const std::string& key);
    void TickMouseDpi(HWND window, int dpi);

    // ---------------------------------------------------------------------------------------------
    // Settings keys
    // ---------------------------------------------------------------------------------------------

    // The settings-file key ("digital", "dual_analog", "dualshock", "mouse", "none", "multitap") as
    // the type Sio takes. An unrecognised key gives a DualShock, which is what a pad that was never
    // configured should be.
    emulation::psx::Sio::ControllerType ParseControllerType(const std::string& key);

    // Where one PSX port's buttons come from - or, for a Multitap, one of its four players.
    // Front-end-only - Sio has no notion of this, only of what the buttons are.
    enum class InputSource { kKeyboard, kGamepad1, kGamepad2, kGamepad3, kGamepad4 };

    // The settings-file key ("keyboard", "gamepad1".."gamepad4") as that enum. An unrecognised key
    // gives the keyboard, which is the source that is always present.
    InputSource ParseInputSource(const std::string& key);

}   // namespace psxemu
