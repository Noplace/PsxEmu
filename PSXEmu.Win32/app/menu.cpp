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
#include "app/menu.h"

#include "app/const.h"
#include "app/win32_paths.h"   // Widen, for a filename read off disk into a wide menu label

namespace psxemu {

    namespace {

        // Appends a popup and stamps its item with `tag`, so it can be found again later without
        // anyone counting positions. See kBiosMenuTag for why that matters.
        void AppendTaggedPopup(HMENU parent, HMENU popup, const wchar_t* label, ULONG_PTR tag) {
            AppendMenuW(parent, MF_POPUP, reinterpret_cast<UINT_PTR>(popup), label);
            MENUITEMINFOW info = {};
            info.cbSize = sizeof(info);
            info.fMask = MIIM_DATA;
            info.dwItemData = tag;
            SetMenuItemInfoW(parent, GetMenuItemCount(parent) - 1, TRUE, &info);
        }

        // Depth-first through every popup under `menu`, for the one whose item carries `tag`.
        HMENU FindTaggedPopup(HMENU menu, ULONG_PTR tag) {
            const int count = GetMenuItemCount(menu);
            for (int i = 0; i < count; ++i) {
                MENUITEMINFOW info = {};
                info.cbSize = sizeof(info);
                info.fMask = MIIM_DATA | MIIM_SUBMENU;
                if (!GetMenuItemInfoW(menu, i, TRUE, &info) || info.hSubMenu == nullptr)
                    continue;
                if (info.dwItemData == tag)
                    return info.hSubMenu;
                if (HMENU found = FindTaggedPopup(info.hSubMenu, tag))
                    return found;
            }
            return nullptr;
        }

    }   // namespace

    HMENU CreateMainMenu() {
        HMENU file = CreatePopupMenu();
        AppendMenuW(file, MF_STRING, kCommandBootDisc, L"&Boot disc...");
        // Filled by PopulateRecentDiscsMenu from the settings file, once it is read.
        AppendTaggedPopup(file, CreatePopupMenu(), L"&Recent Discs", kRecentDiscsMenuTag);
        AppendMenuW(file, MF_STRING, kCommandSwapDisc, L"S&wap disc...");
        AppendMenuW(file, MF_STRING, kCommandEjectDisc, L"&Eject disc");
        AppendMenuW(file, MF_STRING, kCommandBootBios, L"Boot &BIOS");
        AppendMenuW(file, MF_STRING, kCommandBootExe, L"Boot PSX-&EXE...");
        AppendMenuW(file, MF_SEPARATOR, 0, nullptr);

        // Memory Cards: per slot, insert, create and eject - all of which can happen while a game
        // runs, as they could on the console - and the editor.
        HMENU cards = CreatePopupMenu();
        const int insert_ids[2] = { kCommandOpenMemoryCardSlot1, kCommandOpenMemoryCardSlot2 };
        const int create_ids[2] = { kCommandCreateMemoryCardSlot1, kCommandCreateMemoryCardSlot2 };
        const int eject_ids[2] = { kCommandEjectMemoryCardSlot1, kCommandEjectMemoryCardSlot2 };
        for (int slot = 0; slot < 2; ++slot) {
            HMENU slot_menu = CreatePopupMenu();
            AppendMenuW(slot_menu, MF_STRING, insert_ids[slot], L"&Insert Card...");
            AppendMenuW(slot_menu, MF_STRING, create_ids[slot], L"&New Card...");
            AppendMenuW(slot_menu, MF_STRING, eject_ids[slot], L"&Eject");
            // The three a multitap in this port adds (bug 99). Each disc gets its own of these
            // too, loaded when the port is a multitap; these are for putting in some other card.
            AppendMenuW(slot_menu, MF_SEPARATOR, 0, nullptr);
            for (int card = 0; card < 3; ++card) {
                HMENU card_menu = CreatePopupMenu();
                const int first = kCommandMultitapCardFirst + (slot * 3 + card) * 3;
                AppendMenuW(card_menu, MF_STRING, first + 0, L"&Insert Card...");
                AppendMenuW(card_menu, MF_STRING, first + 1, L"&New Card...");
                AppendMenuW(card_menu, MF_STRING, first + 2, L"&Eject");
                const wchar_t* names[3] = { L"Multitap Card &B", L"Multitap Card &C",
                                            L"Multitap Card &D" };
                AppendMenuW(slot_menu, MF_POPUP, reinterpret_cast<UINT_PTR>(card_menu), names[card]);
            }
            AppendMenuW(cards, MF_POPUP, reinterpret_cast<UINT_PTR>(slot_menu),
                        slot == 0 ? L"Slot &1" : L"Slot &2");
        }
        AppendMenuW(cards, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(cards, MF_STRING, kCommandMemoryCardEditor, L"Memory Card &Editor...");
        AppendMenuW(file, MF_POPUP, reinterpret_cast<UINT_PTR>(cards), L"&Memory Cards");
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

        // Speed sits with the frame limiter because it is the same control: the
        // limiter decides whether the machine is paced at all, this decides what
        // it is paced to. The percent signs are stored doubled, as the volume
        // labels are, and collapsed the same way.
        HMENU speed = CreatePopupMenu();
        for (size_t i = 0; i < std::size(kSpeedChoices); ++i) {
            std::wstring label = kSpeedChoices[i].label;
            size_t percent = label.find(L"%%");
            while (percent != std::wstring::npos) {
                label.erase(percent, 1);
                percent = label.find(L"%%", percent + 1);
            }
            AppendMenuW(speed, MF_STRING, static_cast<UINT_PTR>(kCommandSpeedFirst + i),
                        label.c_str());
        }
        AppendMenuW(emulation, MF_POPUP, reinterpret_cast<UINT_PTR>(speed), L"&Speed");
        // How the machine is emulated - the CPU, the timing models, the GPU, the CD-ROM - is a
        // window of its own (ui/emulation_settings_window), with the Accuracy and Performance
        // presets. Here as well as under Settings, since this is where it used to be.
        AppendMenuW(emulation, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(emulation, MF_STRING, static_cast<UINT_PTR>(kCommandEmulationSettings),
                    L"Emulation Se&ttings...");
        AppendMenuW(emulation, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(emulation, MF_STRING, static_cast<UINT_PTR>(kCommandShowTimings),
                    L"Show &Timings in Title Bar");
        AppendMenuW(emulation, MF_STRING, static_cast<UINT_PTR>(kCommandBiosConsole),
                    L"BIOS &Console");
        AppendMenuW(emulation, MF_STRING, static_cast<UINT_PTR>(kCommandSerialToConsole),
                    L"Serial &Port to Console");
        AppendMenuW(emulation, MF_STRING, static_cast<UINT_PTR>(kCommandDebugger),
                    L"&Debugger...");

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
        AppendMenuW(video, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(video, MF_STRING, static_cast<UINT_PTR>(kCommandFullscreen),
                    L"F&ull Screen\tAlt+Enter");

        // What is drawn over the picture (ui/overlay): the performance panel at the top left,
        // notifications at the lower left, the controllers at the top right.
        HMENU display = CreatePopupMenu();
        AppendMenuW(display, MF_STRING, static_cast<UINT_PTR>(kCommandStatsOff),
                    L"Performance &Off");
        AppendMenuW(display, MF_STRING, static_cast<UINT_PTR>(kCommandStatsCompact),
                    L"Performance &Compact");
        AppendMenuW(display, MF_STRING, static_cast<UINT_PTR>(kCommandStatsFull),
                    L"Performance &Full, with Graphs\tF9");
        AppendMenuW(display, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(display, MF_STRING, static_cast<UINT_PTR>(kCommandOverlayNotifications),
                    L"&Notifications");
        AppendMenuW(display, MF_STRING, static_cast<UINT_PTR>(kCommandOverlayControllersAlways),
                    L"Always Show &Controllers");
        AppendMenuW(display, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(display, MF_STRING, static_cast<UINT_PTR>(kCommandThemeClassic),
                    L"Theme: C&lassic");
        AppendMenuW(display, MF_STRING, static_cast<UINT_PTR>(kCommandThemeGlass),
                    L"Theme: &Glass");
        AppendMenuW(video, MF_POPUP, reinterpret_cast<UINT_PTR>(display), L"On-Screen &Display");

        // What each port holds and what plays it - the controller type, the source, a multitap's
        // four players - is chosen in the Controllers window, beside the bindings, rather than
        // here: one window that shows a port whole beats six popups that each show a slice.
        HMENU input = CreatePopupMenu();
        AppendMenuW(input, MF_STRING, static_cast<UINT_PTR>(kCommandControllerBindings),
                    L"&Controllers...");
        AppendMenuW(input, MF_SEPARATOR, 0, nullptr);

        // What a host mouse's movement is worth, for whichever port is set to Mouse. Two
        // submenus rather than one flat list: the resolution only means anything to the
        // hardware mode, and burying it keeps the choice that matters at the top.
        HMENU mouse_motion = CreatePopupMenu();
        for (size_t i = 0; i < std::size(kMouseMotionChoices); ++i) {
            AppendMenuW(mouse_motion, MF_STRING,
                        static_cast<UINT_PTR>(kCommandMouseMotionFirst + i),
                        kMouseMotionChoices[i].label);
        }
        HMENU mouse_dpi = CreatePopupMenu();
        for (size_t i = 0; i < std::size(kMouseDpiChoices); ++i) {
            AppendMenuW(mouse_dpi, MF_STRING, static_cast<UINT_PTR>(kCommandMouseDpiFirst + i),
                        std::to_wstring(kMouseDpiChoices[i]).c_str());
        }
        HMENU mouse = CreatePopupMenu();
        AppendMenuW(mouse, MF_POPUP, reinterpret_cast<UINT_PTR>(mouse_motion), L"&Motion");
        AppendMenuW(mouse, MF_POPUP, reinterpret_cast<UINT_PTR>(mouse_dpi), L"Mouse &DPI");
        AppendMenuW(input, MF_POPUP, reinterpret_cast<UINT_PTR>(mouse), L"Mo&use");

        // The ANALOG button, for pads the keyboard's ANALOG key does not reach (bug 97).
        HMENU analog = CreatePopupMenu();
        AppendMenuW(analog, MF_STRING, static_cast<UINT_PTR>(kCommandAnalogButtonPort1), L"Port &1");
        AppendMenuW(analog, MF_STRING, static_cast<UINT_PTR>(kCommandAnalogButtonPort2), L"Port &2");
        AppendMenuW(input, MF_POPUP, reinterpret_cast<UINT_PTR>(analog), L"Press &ANALOG Button");

        // Which API the sound goes out through. Beside the volume rather than in place of it, the
        // way Video holds Renderer and Filter side by side.
        HMENU output = CreatePopupMenu();
        for (size_t i = 0; i < std::size(kAudioBackendChoices); ++i) {
            AppendMenuW(output, MF_STRING, static_cast<UINT_PTR>(kCommandAudioBackendFirst + i),
                        kAudioBackendChoices[i].label);
        }

        HMENU audio = CreatePopupMenu();
        AppendMenuW(audio, MF_POPUP, reinterpret_cast<UINT_PTR>(volume), L"&Volume");
        AppendMenuW(audio, MF_POPUP, reinterpret_cast<UINT_PTR>(output), L"&Output");

        // The BIOS list is the one menu whose contents are not a table in const.h - it is whatever
        // is in the data folder. Built empty here and filled by PopulateBiosMenu once that folder
        // has been scanned, so the bar exists before any of it is known. Tagged rather than
        // counted, so PopulateBiosMenu finds it wherever it sits - see kBiosMenuTag.
        HMENU bios = CreatePopupMenu();

        // Everything that is a preference rather than an action lives here, so the bar itself stays
        // short: File for things to open, Emulation for things the running machine does, Settings
        // for how it does them.
        HMENU settings = CreatePopupMenu();
        AppendMenuW(settings, MF_STRING, static_cast<UINT_PTR>(kCommandEmulationSettings),
                    L"&Emulation...");
        AppendMenuW(settings, MF_POPUP, reinterpret_cast<UINT_PTR>(input), L"&Input");
        AppendMenuW(settings, MF_POPUP, reinterpret_cast<UINT_PTR>(audio), L"&Audio");
        AppendMenuW(settings, MF_POPUP, reinterpret_cast<UINT_PTR>(video), L"&Video");
        AppendMenuW(settings, MF_SEPARATOR, 0, nullptr);
        AppendTaggedPopup(settings, bios, L"&BIOS", kBiosMenuTag);

        HMENU bar = CreateMenu();
        AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"&File");
        AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(emulation), L"&Emulation");
        AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(settings), L"&Settings");
        return bar;
    }

    HMENU MenuBar(HWND window) {
        HMENU bar = GetMenu(window);
        if (bar == nullptr)
            bar = static_cast<HMENU>(GetPropW(window, kDetachedMenuProp));
        return bar;
    }

    void PopulateBiosMenu(HWND window, const std::vector<std::string>& files,
                          const std::string& current) {
        HMENU bar = MenuBar(window);
        if (bar == nullptr)
            return;
        HMENU bios = FindTaggedPopup(bar, kBiosMenuTag);
        if (bios == nullptr)
            return;

        // Emptied and refilled rather than updated in place: the folder can have gained or lost
        // files since the last time, and the ids are positional.
        while (DeleteMenu(bios, 0, MF_BYPOSITION) != 0) {
        }

        const int count = std::min(static_cast<int>(files.size()), kMaxBiosEntries);
        for (int i = 0; i < count; ++i) {
            // Shown as the filename alone - the folder is the same for all of them, and it is the
            // one thing the person choosing already knows.
            const std::wstring label = Widen(files[i]);
            AppendMenuW(bios, MF_STRING, static_cast<UINT_PTR>(kCommandBiosFirst + i),
                        label.c_str());
            if (_stricmp(files[i].c_str(), current.c_str()) == 0) {
                CheckMenuItem(bios, static_cast<UINT>(kCommandBiosFirst + i),
                              MF_BYCOMMAND | MF_CHECKED);
            }
        }

        if (files.empty()) {
            // An empty folder is the ordinary first-run state, so it says what to do about it
            // rather than showing a menu with nothing in it.
            AppendMenuW(bios, MF_STRING | MF_GRAYED, 0,
                        L"(no BIOS images found - put dumps in the folder below)");
        } else if (static_cast<int>(files.size()) > count) {
            AppendMenuW(bios, MF_STRING | MF_GRAYED, 0, L"(more found than can be listed)");
        }

        AppendMenuW(bios, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(bios, MF_STRING, static_cast<UINT_PTR>(kCommandRescanBios), L"&Rescan folder");
        AppendMenuW(bios, MF_STRING, static_cast<UINT_PTR>(kCommandOpenBiosFolder),
                    L"&Open folder...");
        DrawMenuBar(window);
    }

    void PopulateRecentDiscsMenu(HWND window, const std::vector<std::string>& discs) {
        HMENU bar = MenuBar(window);
        if (bar == nullptr)
            return;
        HMENU recent = FindTaggedPopup(bar, kRecentDiscsMenuTag);
        if (recent == nullptr)
            return;
        while (DeleteMenu(recent, 0, MF_BYPOSITION) != 0) {
        }

        const int count = std::min(static_cast<int>(discs.size()), kMaxRecentDiscs);
        for (int i = 0; i < count; ++i) {
            // Numbered for the keyboard, and shown as the image's own name: the folders are long
            // and mostly the same, and the name is what tells two games apart.
            const size_t slash = discs[i].find_last_of("/\\");
            const std::string name =
                (slash == std::string::npos) ? discs[i] : discs[i].substr(slash + 1);
            std::wstring label = L"&" + std::to_wstring(i + 1) + L"  " + Widen(name);
            // A literal ampersand in a game's name would otherwise underline the next letter.
            for (size_t at = label.find(L'&', 1); at != std::wstring::npos;
                 at = label.find(L'&', at + 2))
                label.insert(at, 1, L'&');
            AppendMenuW(recent, MF_STRING, static_cast<UINT_PTR>(kCommandRecentDiscFirst + i),
                        label.c_str());
        }
        if (discs.empty()) {
            AppendMenuW(recent, MF_STRING | MF_GRAYED, 0, L"(none yet)");
        } else {
            AppendMenuW(recent, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(recent, MF_STRING, static_cast<UINT_PTR>(kCommandClearRecentDiscs),
                        L"&Clear Recent Discs");
        }
        DrawMenuBar(window);
    }

    void TickVolume(HWND window, float current) {
        HMENU bar = MenuBar(window);
        if (bar == nullptr)
            return;
        for (size_t i = 0; i < std::size(kVolumeSteps); ++i) {
            const bool on = (current == kVolumeSteps[i].value);
            CheckMenuItem(bar, static_cast<UINT>(kCommandVolumeFirst + i),
                          MF_BYCOMMAND | (on ? MF_CHECKED : MF_UNCHECKED));
        }
    }

    void TickSpeed(HWND window, float current) {
        HMENU bar = MenuBar(window);
        if (bar == nullptr)
            return;
        // Never greyed. A speed does need the frame limiter - with it off the
        // machine runs at whatever blocks first - but greying the choices out
        // makes the menu look broken to someone whose settings happen to have
        // the limiter off, which is what happened. Choosing a speed turns the
        // limiter on instead: "run at 150%" is a request to be paced.
        for (size_t i = 0; i < std::size(kSpeedChoices); ++i) {
            const bool on = (current == kSpeedChoices[i].value);
            CheckMenuItem(bar, static_cast<UINT>(kCommandSpeedFirst + i),
                          MF_BYCOMMAND | (on ? MF_CHECKED : MF_UNCHECKED));
        }
    }

    // An empty `backend` - no output could be opened - leaves both unticked, which is the truth.
    void TickAudioBackend(HWND window, const std::string& backend) {
        HMENU bar = MenuBar(window);
        if (bar == nullptr)
            return;
        for (size_t i = 0; i < std::size(kAudioBackendChoices); ++i) {
            const bool on = (backend == kAudioBackendChoices[i].key);
            CheckMenuItem(bar, static_cast<UINT>(kCommandAudioBackendFirst + i),
                          MF_BYCOMMAND | (on ? MF_CHECKED : MF_UNCHECKED));
        }
    }

    void TickRenderer(HWND window, const std::string& backend) {
        HMENU bar = MenuBar(window);
        if (bar == nullptr)
            return;
        for (size_t i = 0; i < std::size(kBackendChoices); ++i) {
            const bool on = (backend == kBackendChoices[i].key);
            CheckMenuItem(bar, static_cast<UINT>(kCommandRendererFirst + i),
                          MF_BYCOMMAND | (on ? MF_CHECKED : MF_UNCHECKED));
        }
    }

    void TickFilter(HWND window, const std::string& backend, const std::string& filter) {
        HMENU bar = MenuBar(window);
        if (bar == nullptr)
            return;
        const bool filters_available = RendererHasFilters(backend);
        for (size_t i = 0; i < std::size(kFilterChoices); ++i) {
            const UINT id = static_cast<UINT>(kCommandFilterFirst + i);
            const bool on = filters_available && (filter == kFilterChoices[i].key);
            CheckMenuItem(bar, id, MF_BYCOMMAND | (on ? MF_CHECKED : MF_UNCHECKED));
            EnableMenuItem(bar, id, MF_BYCOMMAND | (filters_available ? MF_ENABLED : MF_GRAYED));
        }
    }

    void TickMultitapCards(HWND window, const std::array<std::string, 2>& controller_types) {
        HMENU bar = MenuBar(window);
        if (bar == nullptr)
            return;
        for (int port = 0; port < 2; ++port) {
            const bool is_multitap =
                ParseControllerType(controller_types[port]) == emulation::psx::Sio::kMultitap;
            for (int i = 0; i < 9; ++i) {   // cards B-D x Insert, New, Eject
                const UINT id = static_cast<UINT>(kCommandMultitapCardFirst + port * 9 + i);
                EnableMenuItem(bar, id, MF_BYCOMMAND | (is_multitap ? MF_ENABLED : MF_GRAYED));
            }
        }
    }

    void TickFrameLimiter(HWND window, bool on) {
        HMENU bar = MenuBar(window);
        if (bar == nullptr)
            return;
        CheckMenuItem(bar, static_cast<UINT>(kCommandFrameLimiter),
                      MF_BYCOMMAND | (on ? MF_CHECKED : MF_UNCHECKED));
    }

    void TickShowTimings(HWND window, bool on) {
        HMENU bar = MenuBar(window);
        if (bar == nullptr)
            return;
        CheckMenuItem(bar, static_cast<UINT>(kCommandShowTimings),
                      MF_BYCOMMAND | (on ? MF_CHECKED : MF_UNCHECKED));
    }

    void TickBiosConsole(HWND window, bool on) {
        HMENU bar = MenuBar(window);
        if (bar == nullptr)
            return;
        CheckMenuItem(bar, static_cast<UINT>(kCommandBiosConsole),
                      MF_BYCOMMAND | (on ? MF_CHECKED : MF_UNCHECKED));
    }

    void TickMouseMotion(HWND window, const std::string& key) {
        HMENU bar = MenuBar(window);
        if (bar == nullptr)
            return;
        for (size_t i = 0; i < std::size(kMouseMotionChoices); ++i) {
            const bool on = (key == kMouseMotionChoices[i].key);
            CheckMenuItem(bar, static_cast<UINT>(kCommandMouseMotionFirst + i),
                          MF_BYCOMMAND | (on ? MF_CHECKED : MF_UNCHECKED));
        }
    }

    void TickMouseDpi(HWND window, int dpi) {
        HMENU bar = MenuBar(window);
        if (bar == nullptr)
            return;
        for (size_t i = 0; i < std::size(kMouseDpiChoices); ++i) {
            const bool on = (dpi == kMouseDpiChoices[i]);
            CheckMenuItem(bar, static_cast<UINT>(kCommandMouseDpiFirst + i),
                          MF_BYCOMMAND | (on ? MF_CHECKED : MF_UNCHECKED));
        }
    }

    void TickSerialToConsole(HWND window, bool on) {
        HMENU bar = MenuBar(window);
        if (bar == nullptr)
            return;
        CheckMenuItem(bar, static_cast<UINT>(kCommandSerialToConsole),
                      MF_BYCOMMAND | (on ? MF_CHECKED : MF_UNCHECKED));
    }

    void TickFullscreen(HWND window, bool on) {
        HMENU bar = MenuBar(window);
        if (bar == nullptr)
            return;
        CheckMenuItem(bar, static_cast<UINT>(kCommandFullscreen),
                      MF_BYCOMMAND | (on ? MF_CHECKED : MF_UNCHECKED));
    }

    void TickOnScreenDisplay(HWND window, int stats_mode, bool notifications,
                             bool controllers_always, bool glass) {
        HMENU bar = MenuBar(window);
        if (bar == nullptr)
            return;
        CheckMenuRadioItem(bar, kCommandThemeClassic, kCommandThemeGlass,
                           static_cast<UINT>(glass ? kCommandThemeGlass : kCommandThemeClassic),
                           MF_BYCOMMAND);
        CheckMenuRadioItem(bar, kCommandStatsOff, kCommandStatsFull,
                           static_cast<UINT>(kCommandStatsOff + stats_mode), MF_BYCOMMAND);
        CheckMenuItem(bar, static_cast<UINT>(kCommandOverlayNotifications),
                      MF_BYCOMMAND | (notifications ? MF_CHECKED : MF_UNCHECKED));
        CheckMenuItem(bar, static_cast<UINT>(kCommandOverlayControllersAlways),
                      MF_BYCOMMAND | (controllers_always ? MF_CHECKED : MF_UNCHECKED));
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
        if (key == "multitap")
            return Sio::kMultitap;
        if (key == "guncon")
            return Sio::kGunCon;
        return Sio::kDualShock;
    }

    InputSource ParseInputSource(const std::string& key) {
        if (key == "gamepad1")
            return InputSource::kGamepad1;
        if (key == "gamepad2")
            return InputSource::kGamepad2;
        if (key == "gamepad3")
            return InputSource::kGamepad3;
        if (key == "gamepad4")
            return InputSource::kGamepad4;
        return InputSource::kKeyboard;
    }

}   // namespace psxemu
