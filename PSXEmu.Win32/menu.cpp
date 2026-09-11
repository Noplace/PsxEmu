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
#include "menu.h"

#include "const.h"

namespace psxemu {

    HMENU CreateMainMenu() {
        HMENU file = CreatePopupMenu();
        AppendMenuW(file, MF_STRING, kCommandBootDisc, L"&Boot disc...");
        AppendMenuW(file, MF_STRING, kCommandSwapDisc, L"S&wap disc...");
        AppendMenuW(file, MF_STRING, kCommandEjectDisc, L"&Eject disc");
        AppendMenuW(file, MF_STRING, kCommandBootBios, L"Boot &BIOS");
        AppendMenuW(file, MF_STRING, kCommandBootExe, L"Boot PSX-&EXE...");
        AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(file, MF_STRING, kCommandOpenMemoryCardSlot1, L"Open Memory Card (Slot 1)...");
        AppendMenuW(file, MF_STRING, kCommandOpenMemoryCardSlot2, L"Open Memory Card (Slot 2)...");
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
        // Act on the slot F1-F8 last selected (slot 1 until one of them is pressed), so the
        // keyboard and the menu stay in step with each other.
        AppendMenuW(emulation, MF_STRING, kCommandSaveState, L"&Save State\tCtrl+F1..F8");
        AppendMenuW(emulation, MF_STRING, kCommandLoadState, L"&Load State\tF1..F8");
        AppendMenuW(emulation, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(emulation, MF_STRING, static_cast<UINT_PTR>(kCommandFrameLimiter),
                    L"&Frame Limiter");
        AppendMenuW(emulation, MF_STRING, static_cast<UINT_PTR>(kCommandCdMechanicalTiming),
                    L"CD-ROM &Mechanical Timing");

        // Volume. The labels carry a literal percent sign, so they are built with the doubled form
        // the table stores rather than passed through a formatter.
        HMENU volume = CreatePopupMenu();
        for (size_t i = 0; i < std::size(kVolumeSteps); ++i) {
            std::wstring label = kVolumeSteps[i].label;
            size_t percent = label.find(L"%%");
            while (percent != std::wstring::npos) {
                label.erase(percent, 1);
                percent = label.find(L"%%", percent + 1);
            }
            AppendMenuW(volume, MF_STRING, static_cast<UINT_PTR>(kCommandVolumeFirst + i),
                        label.c_str());
        }

        HMENU renderer = CreatePopupMenu();
        for (size_t i = 0; i < std::size(kBackendChoices); ++i) {
            AppendMenuW(renderer, MF_STRING, static_cast<UINT_PTR>(kCommandRendererFirst + i),
                        kBackendChoices[i].label);
        }

        HMENU filter = CreatePopupMenu();
        for (size_t i = 0; i < std::size(kFilterChoices); ++i) {
            AppendMenuW(filter, MF_STRING, static_cast<UINT_PTR>(kCommandFilterFirst + i),
                        kFilterChoices[i].label);
        }

        HMENU video = CreatePopupMenu();
        AppendMenuW(video, MF_POPUP, reinterpret_cast<UINT_PTR>(renderer), L"&Renderer");
        AppendMenuW(video, MF_POPUP, reinterpret_cast<UINT_PTR>(filter), L"&Filter");
        AppendMenuW(video, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(video, MF_STRING, static_cast<UINT_PTR>(kCommandViewVram), L"View &VRAM");

        // Two ports, each with its own controller-type choice and its own input source - four small
        // popups rather than one flat list, so ticking one port's choice never has to be told apart
        // from the other's.
        HMENU controller_port[2];
        HMENU source_port[2];
        const int type_count = static_cast<int>(std::size(kControllerTypeChoices));
        for (int port = 0; port < 2; ++port) {
            controller_port[port] = CreatePopupMenu();
            for (size_t i = 0; i < std::size(kControllerTypeChoices); ++i) {
                AppendMenuW(controller_port[port], MF_STRING,
                            static_cast<UINT_PTR>(kCommandControllerTypeFirst + port * type_count +
                                                  static_cast<int>(i)),
                            kControllerTypeChoices[i].label);
            }
            source_port[port] = CreatePopupMenu();
            for (size_t i = 0; i < std::size(kInputSourceChoices); ++i) {
                AppendMenuW(source_port[port], MF_STRING,
                            static_cast<UINT_PTR>(kCommandInputSourceFirst + port * 3 +
                                                  static_cast<int>(i)),
                            kInputSourceChoices[i].label);
            }
        }

        HMENU input = CreatePopupMenu();
        AppendMenuW(input, MF_POPUP, reinterpret_cast<UINT_PTR>(controller_port[0]),
                    L"Controller Port &1");
        AppendMenuW(input, MF_POPUP, reinterpret_cast<UINT_PTR>(controller_port[1]),
                    L"Controller Port &2");
        AppendMenuW(input, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(input, MF_POPUP, reinterpret_cast<UINT_PTR>(source_port[0]), L"Port 1 &Source");
        AppendMenuW(input, MF_POPUP, reinterpret_cast<UINT_PTR>(source_port[1]), L"Port 2 S&ource");

        HMENU bar = CreateMenu();
        AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"&File");
        AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(emulation), L"&Emulation");
        AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(input), L"&Input");
        AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(volume), L"&Audio");
        AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(video), L"&Video");
        return bar;
    }

    void TickVolume(HWND window, float current) {
        HMENU bar = GetMenu(window);
        if (bar == nullptr)
            return;
        for (size_t i = 0; i < std::size(kVolumeSteps); ++i) {
            const bool on = (current == kVolumeSteps[i].value);
            CheckMenuItem(bar, static_cast<UINT>(kCommandVolumeFirst + i),
                          MF_BYCOMMAND | (on ? MF_CHECKED : MF_UNCHECKED));
        }
    }

    void TickRenderer(HWND window, const std::string& backend) {
        HMENU bar = GetMenu(window);
        if (bar == nullptr)
            return;
        for (size_t i = 0; i < std::size(kBackendChoices); ++i) {
            const bool on = (backend == kBackendChoices[i].key);
            CheckMenuItem(bar, static_cast<UINT>(kCommandRendererFirst + i),
                          MF_BYCOMMAND | (on ? MF_CHECKED : MF_UNCHECKED));
        }
    }

    void TickFilter(HWND window, const std::string& backend, const std::string& filter) {
        HMENU bar = GetMenu(window);
        if (bar == nullptr)
            return;
        const bool filters_available = (backend == "d3d12");
        for (size_t i = 0; i < std::size(kFilterChoices); ++i) {
            const UINT id = static_cast<UINT>(kCommandFilterFirst + i);
            const bool on = filters_available && (filter == kFilterChoices[i].key);
            CheckMenuItem(bar, id, MF_BYCOMMAND | (on ? MF_CHECKED : MF_UNCHECKED));
            EnableMenuItem(bar, id, MF_BYCOMMAND | (filters_available ? MF_ENABLED : MF_GRAYED));
        }
    }

    void TickControllerTypes(HWND window, const std::array<std::string, 2>& types) {
        HMENU bar = GetMenu(window);
        if (bar == nullptr)
            return;
        const int type_count = static_cast<int>(std::size(kControllerTypeChoices));
        for (int port = 0; port < 2; ++port) {
            const std::string& current = types[port];
            for (size_t i = 0; i < std::size(kControllerTypeChoices); ++i) {
                const UINT id = static_cast<UINT>(kCommandControllerTypeFirst +
                                                  port * type_count + static_cast<int>(i));
                const bool on = (current == kControllerTypeChoices[i].key);
                CheckMenuItem(bar, id, MF_BYCOMMAND | (on ? MF_CHECKED : MF_UNCHECKED));
            }
        }
    }

    void TickInputSources(HWND window, const std::array<std::string, 2>& sources,
                          const std::array<std::string, 2>& controller_types) {
        HMENU bar = GetMenu(window);
        if (bar == nullptr)
            return;
        for (int port = 0; port < 2; ++port) {
            // A mouse's mapping is fixed and kNone has no buttons at all, so neither port's source
            // choice does anything - greyed out for the same reason TickFilter greys out a filter a
            // renderer cannot use, rather than leaving a clickable item that silently does nothing.
            const emulation::psx::Sio::ControllerType type =
                ParseControllerType(controller_types[port]);
            const bool has_source = (type != emulation::psx::Sio::kMouse &&
                                     type != emulation::psx::Sio::kNone);
            const std::string& current = sources[port];
            for (size_t i = 0; i < std::size(kInputSourceChoices); ++i) {
                const UINT id =
                    static_cast<UINT>(kCommandInputSourceFirst + port * 3 + static_cast<int>(i));
                const bool on = has_source && (current == kInputSourceChoices[i].key);
                CheckMenuItem(bar, id, MF_BYCOMMAND | (on ? MF_CHECKED : MF_UNCHECKED));
                EnableMenuItem(bar, id, MF_BYCOMMAND | (has_source ? MF_ENABLED : MF_GRAYED));
            }
        }
    }

    void TickFrameLimiter(HWND window, bool on) {
        HMENU bar = GetMenu(window);
        if (bar == nullptr)
            return;
        CheckMenuItem(bar, static_cast<UINT>(kCommandFrameLimiter),
                      MF_BYCOMMAND | (on ? MF_CHECKED : MF_UNCHECKED));
    }

    void TickCdTiming(HWND window, bool on) {
        HMENU bar = GetMenu(window);
        if (bar == nullptr)
            return;
        CheckMenuItem(bar, static_cast<UINT>(kCommandCdMechanicalTiming),
                      MF_BYCOMMAND | (on ? MF_CHECKED : MF_UNCHECKED));
    }

    emulation::psx::Sio::ControllerType ParseControllerType(const std::string& key) {
        using emulation::psx::Sio;
        if (key == "digital")
            return Sio::kDigital;
        if (key == "dual_analog")
            return Sio::kDualAnalog;
        if (key == "mouse")
            return Sio::kMouse;
        if (key == "none")
            return Sio::kNone;
        return Sio::kDualShock;
    }

    InputSource ParseInputSource(const std::string& key) {
        if (key == "gamepad1")
            return InputSource::kGamepad1;
        if (key == "gamepad2")
            return InputSource::kGamepad2;
        return InputSource::kKeyboard;
    }

}   // namespace psxemu
