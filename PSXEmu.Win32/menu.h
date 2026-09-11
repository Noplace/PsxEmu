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

#include "framework.h"

namespace psxemu {

    // The whole menu bar, built from the choice tables in const.h so a table and its menu cannot
    // drift apart. Ownership passes to the caller, which in practice means passing it to
    // CreateWindowExW and letting the window own it from then on.
    HMENU CreateMainMenu();

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

    // The renderer actually running, which is not necessarily the persisted preference - creating
    // the preferred one can fall back to the other.
    void TickRenderer(HWND window, const std::string& backend);

    // The current filter, and every filter item greyed out when the active renderer does not
    // support them - D3D11Presenter's SetPixelShader is a no-op, and a menu that silently does
    // nothing on click is worse than one that looks unavailable.
    void TickFilter(HWND window, const std::string& backend, const std::string& filter);

    // The controller type set on each port, and the source each port's buttons come from. A port
    // whose type does not use a source at all - Sio::kMouse's mapping is fixed to the real mouse,
    // Sio::kNone has no buttons to source - has its source items greyed out rather than removed, the
    // same treatment TickFilter already gives a filter menu the active renderer cannot use.
    void TickControllerTypes(HWND window, const std::array<std::string, 2>& types);
    void TickInputSources(HWND window, const std::array<std::string, 2>& sources,
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

    // ---------------------------------------------------------------------------------------------
    // Settings keys
    // ---------------------------------------------------------------------------------------------

    // The settings-file key ("digital", "dual_analog", "dualshock", "mouse", "none") as the type Sio
    // takes. An unrecognised key gives a DualShock, which is what a pad that was never configured
    // should be.
    emulation::psx::Sio::ControllerType ParseControllerType(const std::string& key);

    // Where one PSX port's buttons come from. Front-end-only - Sio has no notion of this, only of
    // what the buttons are.
    enum class InputSource { kKeyboard, kGamepad1, kGamepad2 };

    // The settings-file key ("keyboard", "gamepad1", "gamepad2") as that enum. An unrecognised key
    // gives the keyboard, which is the source that is always present.
    InputSource ParseInputSource(const std::string& key);

}   // namespace psxemu
