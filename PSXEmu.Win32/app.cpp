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
#include "app.h"

#include "keyboard.h"
#include "menu.h"
#include "win32_dialogs.h"
#include "win32_paths.h"

#include <shellapi.h>   // ShellExecuteA, to open the BIOS folder from its menu

namespace psxemu {

    using emulation::psx::EmuConfig;
    using emulation::psx::Sio;
    using emulation::psx::System;
    using emulation::host::HostInput;
    using emulation::host::Machine;
    using emulation::host::MachineReport;
    using emulation::host::Presenter;
    using emulation::host::VideoOutput;

    namespace {

        // Anything a thread asks the UI thread to do arrives as this, carrying a std::function the
        // window procedure runs and deletes. Posted, never sent: see the rules in app.h.
        const UINT kMessageRunOnUi = WM_APP + 1;

    }   // namespace

    App::~App() {
        StopThreads();
        if (system_ != nullptr)
            system_->Deinitialize();
    }

    int App::Run(HINSTANCE instance, int show_command) {
        if (!Initialize(instance, show_command))
            return 1;
        return MainLoop();
    }

    // ---------------------------------------------------------------------------------------------
    // Startup
    // ---------------------------------------------------------------------------------------------

    bool App::Initialize(HINSTANCE instance, int show_command) {
        const CommandLine command_line = ParseCommandLine();

        settings_path_ = Narrow(SettingsPathBesideExecutable());
        settings_.Load(settings_path_);
        emulation::psx::LoadConfig(settings_, config_);

        // Before the machine, because the machine is built around a BIOS: the folder has to exist
        // and be scanned to know whether the one the settings file names is still in it.
        SetUpDataDirectories();
        bios_files_ = ScanBiosFolder(bios_root_);
        bios_path_ = ResolveBiosPath(command_line.bios);
        if (bios_path_.empty()) {
            ShowError(nullptr,
                      L"No BIOS image found.\n\n"
                      L"A PlayStation BIOS dump is required. Put one in\n"
                      L"Documents\\My Games\\PSXEmu\\bios, or in a 'bios' folder beside "
                      L"the executable, or pass its path as the first argument.\n\n"
                      L"A dump is exactly 512 KB.");
            return false;
        }

        if (!CreateAppWindow(instance))
            return false;
        // Created hidden whether or not it is wanted, so it collects from the first frame and
        // opening it later shows everything already written.
        console_.Create(instance, window_, [this] { SetShowBiosConsole(false); });
        {
            MemoryCardEditor::Host host;
            host.refresh = [this] { RefreshMemoryCardEditor(); };
            host.edit = [this](int slot, MemoryCardEditor::Edit edit) {
                EditMemoryCard(slot, std::move(edit));
            };
            card_editor_.Create(instance, window_, std::move(host));
        }
        {
            DebuggerWindow::Host host;
            host.request = [this](DebuggerWindow::Change change, uint32_t center) {
                PostToMachine([this, change = std::move(change), center](Machine& machine) {
                    if (change)
                        change(machine.system().debugger());
                    if (center != DebuggerWindow::kNoSnapshot)
                        SendDebuggerSnapshot(machine, center, false);
                });
            };
            host.write_memory = [this](uint32_t address, std::vector<uint8_t> bytes,
                                       uint32_t center) {
                PostToMachine([this, address, bytes = std::move(bytes), center](Machine& machine) {
                    std::string error;
                    if (!machine.system().debugger().WriteMemory(
                            address, bytes.data(), static_cast<uint32_t>(bytes.size()), &error)) {
                        const std::wstring message(error.begin(), error.end());
                        PostToUi([this, message] {
                            ShowWarning(debugger_.window(), message.c_str());
                        });
                    }
                    SendDebuggerSnapshot(machine, center, false);
                });
            };
            host.on_closed = [this] { debugger_open_ = false; };
            debugger_.Create(instance, window_, std::move(host));
        }
        if (!CreateMachine())
            return false;

        ApplySettings();
        RefreshBiosMenu();
        LoadRecentDiscs();
        LoadKeyBindings();
        key_bindings_.Create(instance, window_, [this](const KeyMap& map) { SetKeyBindings(map); });

        // Before the threads start, so this is still the only thread touching the machine.
        if (!command_line.disc.empty() && system_->LoadDisc(command_line.disc.c_str())) {
            SetWindowTitleForPath(command_line.disc);
            LoadOrCreateMemoryCardsForDisc(*system_, command_line.disc);
            NoteRecentDisc(command_line.disc);
        }

        StartThreads();
        // After the input thread exists, since that is what holds the mouse.
        SendMouseSettingsToInput();

        ShowWindow(window_, show_command);
        UpdateWindow(window_);
        if (config_.show_bios_console)
            console_.Show(true);
        return true;
    }

    bool App::CreateAppWindow(HINSTANCE instance) {
        WNDCLASSEXW window_class = {};
        window_class.cbSize = sizeof(window_class);
        window_class.style = CS_HREDRAW | CS_VREDRAW;
        window_class.lpfnWndProc = WindowProc;
        window_class.hInstance = instance;
        window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        window_class.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        window_class.lpszClassName = kWindowClass;
        if (RegisterClassExW(&window_class) == 0)
            return false;

        RECT bounds = { 0, 0, 640, 480 };
        AdjustWindowRect(&bounds, WS_OVERLAPPEDWINDOW, TRUE);
        window_ =
            CreateWindowExW(0, kWindowClass, kWindowTitle, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                            CW_USEDEFAULT, bounds.right - bounds.left, bounds.bottom - bounds.top,
                            nullptr, CreateMainMenu(), instance, this);
        return window_ != nullptr;
    }

    bool App::CreateMachine() {
        system_ = std::make_unique<System>();
        if (system_->Initialize(bios_path_.c_str()) != 0) {
            ShowError(window_,
                      L"The BIOS image could not be loaded. It must be exactly "
                      L"512 KB.");
            return false;
        }
        return true;
    }

    // The settings file's own choices, onto the machine and the menus, while this is still the
    // only thread there is.
    void App::ApplySettings() {
        system_->config() = config_;
        system_->sio().set_controller_type(0, ParseControllerType(config_.controller_type[0]));
        system_->sio().set_controller_type(1, ParseControllerType(config_.controller_type[1]));
        plugged_type_[0] = config_.controller_type[0];
        plugged_type_[1] = config_.controller_type[1];

        // The renderer and the sound device are ticked when the threads that open them report
        // back what actually opened, which is not always what was asked for.
        UpdateVolumeMenu();
        UpdateControllerTypeMenu();
        UpdateInputSourceMenu();
        UpdateMultitapSourceMenu();
        UpdateFrameLimiterMenu();
        UpdateSpeedMenu();
        UpdateCdTimingMenu();
        UpdateSkipBiosIntroMenu();
        UpdateRecompilerMenu();
        UpdatePauseInMenusMenu();
        UpdateShowTimingsMenu();
        UpdateBiosConsoleMenu();
        UpdateSerialToConsoleMenu();
        UpdateMouseMenu();
        UpdateFilterMenu();
        UpdateRendererMenu();
        UpdateAudioBackendMenu();
    }

    void App::SetUpDataDirectories() {
        data_root_ = ResolveDataRoot();
        if (data_root_.empty())
            return;
        memcards_root_ = data_root_ + "\\memcards";
        savestates_root_ = data_root_ + "\\savestates";
        // Created whether or not anything is in it: an empty folder is where to put a dump, which
        // is a better answer to "where do BIOS images go" than a folder that only appears once one
        // is already there.
        bios_root_ = data_root_ + "\\bios";
        EnsureDirectory(memcards_root_);
        EnsureDirectory(savestates_root_);
        EnsureDirectory(bios_root_);
    }

    // Which image to boot, in the order the answers are allowed to win: the command line, then the
    // one the settings file names if it is still in the folder, then whatever FindBios turns up
    // beside the executable.
    std::string App::ResolveBiosPath(const std::string& from_command_line) {
        if (!from_command_line.empty())
            return FindBios(from_command_line);

        const std::string chosen = settings_.GetString("bios_file", std::string());
        if (!chosen.empty() && !bios_root_.empty()) {
            for (const std::string& file : bios_files_) {
                if (_stricmp(file.c_str(), chosen.c_str()) == 0)
                    return bios_root_ + "\\" + file;
            }
        }

        // Nothing chosen, or what was chosen is gone. A folder with exactly one dump in it is not
        // ambiguous, so use it rather than making the first run a trip through the menu.
        if (bios_files_.size() == 1 && !bios_root_.empty())
            return bios_root_ + "\\" + bios_files_[0];

        return FindBios(std::string());
    }

    void App::RefreshBiosMenu() {
        bios_files_ = ScanBiosFolder(bios_root_);
        PopulateBiosMenu(window_, bios_files_, FileNameOf(bios_path_));
    }

    void App::SelectBios(int index) {
        if (index < 0 || index >= static_cast<int>(bios_files_.size()) || bios_root_.empty())
            return;

        // Chosen, not applied. A BIOS is only read at power-on, so this is what the *next* cold
        // boot will use - Reset, Boot disc, Boot BIOS, or the next time the emulator starts.
        bios_path_ = bios_root_ + "\\" + bios_files_[index];
        config_.bios_file = bios_files_[index];
        SaveSettingsIfChanged();
        SendConfigToMachine();
        RefreshBiosMenu();
    }

    // ---------------------------------------------------------------------------------------------
    // The threads
    // ---------------------------------------------------------------------------------------------

    void App::StartThreads() {
        // Output first, then input, then the machine that feeds and reads them.
        audio_ = std::make_unique<emulation::host::AudioOutput>(
            [](const std::string& backend, std::string* opened) {
                return CreateAudioEngine(
                    (backend == "dsound") ? AudioBackend::kDirectSound : AudioBackend::kWasapi,
                    opened);
            },
            [this](const std::string& opened) {
                // On the audio thread: hand it to the UI, which owns the menus and the settings.
                PostToUi([this, opened] {
                    current_audio_backend_ = opened;
                    if (!opened.empty())
                        config_.audio_backend = opened;
                    UpdateAudioBackendMenu();
                    SaveSettingsIfChanged();
                    if (opened.empty()) {
                        ShowWarning(window_,
                                    L"Neither WASAPI nor DirectSound could be opened, so there is "
                                    L"no sound. The machine keeps running.");
                    }
                });
            });

        // Copied now, not read from `config_` on the video thread: that one belongs to this
        // thread, and a menu command could be changing it while the factory runs.
        const std::string start_renderer = config_.graphics_backend;
        const std::string start_filter = config_.video_filter;
        video_ = std::make_unique<VideoOutput>(
            [this, start_renderer, start_filter]() -> std::unique_ptr<Presenter> {
                // On the video thread: a Direct3D device is created by the thread that will use
                // it, and used by no other.
                auto presenter = std::make_unique<D3DPresenter>(
                    window_, [this](std::function<void()> work) { PostToUi(std::move(work)); });
                if (!presenter->Open(start_renderer, start_filter)) {
                    PostToUi([this] {
                        ShowError(window_, L"Could not create a Direct3D device.");
                        PostMessageW(window_, WM_CLOSE, 0, 0);
                    });
                    return nullptr;
                }
                // What actually opened, which is not always what was asked for.
                const std::string renderer = presenter->renderer();
                const std::string filter = presenter->filter();
                PostToUi([this, renderer, filter] {
                    current_backend_ = renderer;
                    current_filter_ = filter;
                    config_.graphics_backend = renderer;
                    config_.video_filter = filter;
                    UpdateRendererMenu();
                    UpdateFilterMenu();
                    SaveSettingsIfChanged();
                });
                return presenter;
            });

        Machine::Hooks hooks;
        hooks.apply_input = [this](System& system, const HostInput& input) {
            ApplyInput(system, input);
        };
        hooks.report = [this](const MachineReport& report) { OnMachineReport(report); };
        hooks.after_frame = [this](Machine& machine) { CollectConsoleText(machine.system()); };
        hooks.halted = [this](Machine& machine) {
            SendDebuggerSnapshot(machine, DebuggerWindow::kAtPc, true);
        };
        // Still the only thread: the boot already set up is the one the console starts in, and
        // needs no marker above it.
        console_session_ = system_->kernel().session();
        machine_ = std::make_unique<Machine>(system_.get(), &video_->frames(), &audio_->samples(),
                                             hooks);
        input_ = std::make_unique<InputThread>(&machine_->input(), window_);
        input_->SetKeyMap(key_map_);

        input_->Start();
        audio_->Start(config_.audio_backend);
        video_->Start();
        machine_->Start(paused_by_user_ ? emulation::host::kPausedByUser : 0);
    }

    void App::StopThreads() {
        if (stopping_)
            return;
        stopping_ = true;

        // Stopped in order, on a thread of their own, so this one can go on answering messages
        // while it waits - rule 3. Nothing of ours sends a message to this thread, but DXGI is
        // entitled to while a swap chain is being released, and a UI thread sitting in a join
        // would never answer it. Pumping costs nothing and closes that off entirely.
        std::thread stopper([this] {
            // The machine first: nothing more is produced, and nothing more is asked of the
            // outputs. Then video, which destroys the Direct3D engine on its own thread - and
            // has to, before the window it draws into is destroyed.
            if (machine_ != nullptr)
                machine_->Stop();
            if (video_ != nullptr)
                video_->Stop();
            if (audio_ != nullptr)
                audio_->Stop();
            if (input_ != nullptr)
                input_->Stop();
        });

        const HANDLE stopping = static_cast<HANDLE>(stopper.native_handle());
        for (;;) {
            const DWORD woke =
                MsgWaitForMultipleObjects(1, &stopping, FALSE, INFINITE, QS_ALLINPUT);
            if (woke != WAIT_OBJECT_0 + 1)
                break;   // the threads are done, or the wait failed - either way, stop waiting
            MSG message;
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        stopper.join();

        // Anything the threads posted on their way out has nobody left to run it.
        if (window_ != nullptr) {
            MSG message;
            while (PeekMessageW(&message, window_, kMessageRunOnUi, kMessageRunOnUi, PM_REMOVE)) {
                delete reinterpret_cast<std::function<void()>*>(message.lParam);
            }
        }
    }

    int App::MainLoop() {
        MSG message = {};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            // The debugger's keys, while it has the focus - F10 among them, which translated
            // would be a system key.
            if (debugger_.PreTranslate(message))
                continue;
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }

        // Written on every change already; this catches anything the last edit missed.
        SaveSettingsIfChanged();
        return static_cast<int>(message.wParam);
    }

    // ---------------------------------------------------------------------------------------------
    // Between threads
    // ---------------------------------------------------------------------------------------------

    void App::PostToUi(std::function<void()> work) {
        if (window_ == nullptr)
            return;
        std::function<void()>* held = new std::function<void()>(std::move(work));
        if (!PostMessageW(window_, kMessageRunOnUi, 0, reinterpret_cast<LPARAM>(held)))
            delete held;   // the window has gone; nobody will run it
    }

    void App::PostToMachine(std::function<void(Machine&)> request) {
        if (machine_ != nullptr)
            machine_->Post(std::move(request));
    }

    void App::SendConfigToMachine() {
        const EmuConfig config = config_;
        PostToMachine([config](Machine& machine) { machine.ApplyConfig(config); });
    }

    // On the machine's thread. Nothing here may touch the window or the menus, so the whole report
    // goes to the UI thread and is shown there.
    void App::OnMachineReport(const MachineReport& report) {
        PostToUi([this, report] {
            report_ = report;
            have_report_ = true;
            if (video_ != nullptr) {
                uint64_t presents = 0;
                double total_ms = 0.0;
                video_->TakeTiming(&presents, &total_ms);
                present_ms_ = presents > 0 ? total_ms / static_cast<double>(presents) : 0.0;
                presents_per_second_ = static_cast<double>(presents);
            }
            UpdateTitle();
        });
    }

    void App::UpdateTitle() {
        std::wstring title = title_base_;
        if (have_report_) {
            wchar_t suffix[96] = {};
            if (report_.paused) {
                swprintf(suffix, std::size(suffix), L"  -  paused");
            } else if (report_.refresh_hz > 0.0) {
                swprintf(suffix, std::size(suffix), L"  -  %.1f fps (%.0f%%)", report_.fps,
                         100.0 * report_.fps / report_.refresh_hz);
            } else {
                swprintf(suffix, std::size(suffix), L"  -  %.1f fps", report_.fps);
            }
            title += suffix;

            // Where the frame's time went, on each thread that has any - Emulation > Show Timings.
            // "idle" is what the frame limiter slept off: the headroom, and what a faster speed
            // setting has to come out of.
            if (config_.show_timings && !report_.paused) {
                wchar_t timings[192] = {};
                swprintf(timings, std::size(timings),
                         L"  |  emulate %.1f  hand-off %.2f  idle %.1f  present %.2f ms"
                         L"  |  sound %.0f ms",
                         report_.emulate_ms, report_.handoff_ms, report_.idle_ms, present_ms_,
                         report_.audio_queued_frames * 1000.0 / emulation::psx::Spu::kSampleRate);
                title += timings;

                // Only when there is something to say: a frame the video thread never showed, or
                // sound the audio thread had to make up. Zero is the normal state and says
                // nothing.
                if (report_.frames_dropped != 0 || report_.audio_short_frames != 0) {
                    wchar_t edges[96] = {};
                    swprintf(edges, std::size(edges), L"  |  dropped %llu  short %llu",
                             static_cast<unsigned long long>(report_.frames_dropped),
                             static_cast<unsigned long long>(report_.audio_short_frames));
                    title += edges;
                }
            }
        }
        SetWindowTextW(window_, title.c_str());
    }

    // ---------------------------------------------------------------------------------------------
    // Input, on the machine's thread
    // ---------------------------------------------------------------------------------------------

    // What the input thread last read, mapped onto the ports the settings name. This is the old
    // PollInput with the device polling taken out of it: the reading is already in the pad's own
    // vocabulary, and arrives at most a millisecond old.
    void App::ApplyInput(System& system, const HostInput& input) {
        const EmuConfig& config = system.config();

        // A different controller is a different physical device, and on a real console the port
        // sits empty while one is unplugged and the next plugged in. Some games need to see that
        // before they will believe what is there now: Bomberman Party Edition's pad driver, shown
        // a multitap and then a pad with no gap between them, went on decoding the port as a
        // multitap and took no input from anything until the machine was reset (bug 54). Noticed
        // here rather than in the menu, so the countdown belongs to the thread that runs it.
        Sio::ControllerType controller_type[2];
        for (int port = 0; port < 2; ++port) {
            if (config.controller_type[port] != plugged_type_[port]) {
                plugged_type_[port] = config.controller_type[port];
                replug_frames_[port] = kControllerReplugFrames;
            }
            controller_type[port] = replug_frames_[port] > 0
                                        ? Sio::kNone
                                        : ParseControllerType(config.controller_type[port]);
            if (replug_frames_[port] > 0)
                --replug_frames_[port];
            system.sio().set_controller_type(port, controller_type[port]);
        }

        // What one source (the keyboard, or one of the four XInput slots) is doing right now, in
        // Sio's own vocabulary. Shared by the single-pad path below and each of a Multitap's four
        // players.
        struct SourceReading {
            bool connected = true;   // the keyboard is always "there"
            uint16_t buttons = 0;
            uint8_t left_x = 0x80, left_y = 0x80, right_x = 0x80, right_y = 0x80;
            int rumble_target = -1;   // which XInput slot feels this reading's motors
        };
        auto read_source = [&input](InputSource source) {
            SourceReading r;
            int pad = -1;
            switch (source) {
                case InputSource::kKeyboard:
                    r.buttons = input.focused ? input.keyboard : 0;
                    return r;
                case InputSource::kGamepad1: pad = 0; break;
                case InputSource::kGamepad2: pad = 1; break;
                case InputSource::kGamepad3: pad = 2; break;
                case InputSource::kGamepad4: pad = 3; break;
            }
            r.connected = input.pads[pad].connected;
            r.buttons = input.focused ? input.pads[pad].buttons : 0;
            r.left_x = input.pads[pad].left_x;
            r.left_y = input.pads[pad].left_y;
            r.right_x = input.pads[pad].right_x;
            r.right_y = input.pads[pad].right_y;
            r.rumble_target = pad;
            return r;
        };

        // Rumble is an output, not an input, so it is not gated on focus - the machine keeps
        // running in the background, and a real console would not silence a controller's motor
        // because another window has focus. The input thread applies what is left here.
        auto apply_rumble = [this, &system](int port, int player, const SourceReading& r) {
            if (r.rumble_target < 0)
                return;
            uint8_t motor_small = 0;
            uint8_t motor_large = 0;
            system.sio().motor_state(port, &motor_small, &motor_large, player);
            machine_->input().SetRumble(r.rumble_target, motor_small, motor_large);
        };

        for (int port = 0; port < 2; ++port) {
            // A port set to no controller reports nothing at all.
            if (controller_type[port] == Sio::kNone) {
                system.sio().set_connected(port, false);
                continue;
            }

            // A mouse's mapping is not a player choice the way a pad's input_source is - it is
            // always the real Windows mouse, exactly as a real PSX mouse is always whatever is
            // plugged into the port.
            if (controller_type[port] == Sio::kMouse) {
                system.sio().set_connected(port, true);
                system.sio().set_mouse_buttons(port, input.mouse_left, input.mouse_right);
                system.sio().add_mouse_motion(port, input.mouse_dx, input.mouse_dy);
                continue;
            }

            // A Multitap sources each of its four players independently; otherwise this is exactly
            // the single-pad path below, run four times.
            if (controller_type[port] == Sio::kMultitap) {
                for (int player = 0; player < 4; ++player) {
                    const InputSource source =
                        ParseInputSource(config.multitap_player_source[port][player]);
                    const SourceReading r = read_source(source);
                    system.sio().set_connected(port, r.connected, player);
                    system.sio().set_buttons(port, r.buttons, player);
                    system.sio().set_axes(port, r.left_x, r.left_y, r.right_x, r.right_y, player);
                    apply_rumble(port, player, r);
                }
                continue;
            }

            const InputSource source = ParseInputSource(config.input_source[port]);
            const SourceReading r = read_source(source);
            system.sio().set_connected(port, r.connected);
            system.sio().set_buttons(port, r.buttons);
            system.sio().set_axes(port, r.left_x, r.left_y, r.right_x, r.right_y);
            apply_rumble(port, /*player=*/0, r);
        }
    }

    // ---------------------------------------------------------------------------------------------
    // Settings
    // ---------------------------------------------------------------------------------------------

    void App::SaveSettingsIfChanged() {
        if (settings_path_.empty())
            return;
        emulation::psx::SettingsFile updated = settings_;
        emulation::psx::StoreConfig(updated, config_);
        if (updated.Serialise() == settings_.Serialise())
            return;
        settings_ = updated;
        settings_.Save(settings_path_);
    }

    void App::UpdateVolumeMenu() { TickVolume(window_, config_.audio_volume); }

    void App::SetVolume(float value) {
        config_.audio_volume = value;
        UpdateVolumeMenu();
        SaveSettingsIfChanged();
        SendConfigToMachine();
    }

    // These two tick against what is actually running rather than against the config, since a
    // fallback can leave the two disagreeing.
    void App::UpdateRendererMenu() { TickRenderer(window_, current_backend_); }
    void App::UpdateAudioBackendMenu() { TickAudioBackend(window_, current_audio_backend_); }

    // Switching sound output while a game runs: the audio thread closes one device and opens the
    // other, and says which it ended up with.
    void App::SetAudioBackend(const std::string& key) {
        if (key == current_audio_backend_ || audio_ == nullptr)
            return;
        config_.audio_backend = key;
        SaveSettingsIfChanged();
        audio_->SwitchBackend(key);
    }

    void App::UpdateFilterMenu() { TickFilter(window_, current_backend_, current_filter_); }

    void App::SetFilter(const std::string& key) {
        if (current_backend_ != "d3d12") {
            // Reachable from the settings file (a saved filter with graphics_backend reverted to
            // d3d11) as well as a stray click on a greyed item - either way, say why rather than
            // doing nothing.
            ShowWarning(window_,
                        L"Filters require the Direct3D 12 renderer. Switch renderer "
                        L"first (Settings > Video > Renderer).");
            return;
        }
        config_.video_filter = key;
        current_filter_ = key;
        if (video_ != nullptr) {
            video_->Post([key](VideoOutput& video) {
                if (video.presenter() != nullptr)
                    static_cast<D3DPresenter*>(video.presenter())->SetFilter(key);
                video.PresentAgain();
            });
        }
        UpdateFilterMenu();
        SaveSettingsIfChanged();
    }

    // Live renderer switch, done on the video thread - it owns the device. What actually opened
    // comes back from there, the same way it does at startup.
    void App::SetRenderer(const std::string& key) {
        if (key == current_backend_ || video_ == nullptr)
            return;
        video_->Post([this, key](VideoOutput& video) {
            D3DPresenter* presenter = static_cast<D3DPresenter*>(video.presenter());
            if (presenter == nullptr)
                return;
            presenter->SetRenderer(key);
            const std::string renderer = presenter->renderer();
            const std::string filter = presenter->filter();
            video.PresentAgain();
            PostToUi([this, renderer, filter] {
                current_backend_ = renderer;
                current_filter_ = filter;
                if (!renderer.empty())
                    config_.graphics_backend = renderer;
                config_.video_filter = filter;
                UpdateRendererMenu();
                UpdateFilterMenu();
                SaveSettingsIfChanged();
            });
        });
    }

    void App::UpdateControllerTypeMenu() { TickControllerTypes(window_, config_.controller_type); }

    void App::SetControllerType(int port, const std::string& key) {
        config_.controller_type[port] = key;
        UpdateControllerTypeMenu();
        // Switching to or from kMouse/kNone/kMultitap changes whether this port's own source items
        // (or, for kMultitap, its four players' source items) should be greyed out.
        UpdateInputSourceMenu();
        UpdateMultitapSourceMenu();
        SaveSettingsIfChanged();
        SendConfigToMachine();
        // Whether any port is a mouse decides whether the cursor may be captured.
        SendMouseSettingsToInput();
    }

    void App::UpdateInputSourceMenu() {
        TickInputSources(window_, config_.input_source, config_.controller_type);
    }

    void App::SetInputSource(int port, const std::string& key) {
        config_.input_source[port] = key;
        UpdateInputSourceMenu();
        SaveSettingsIfChanged();
        SendConfigToMachine();
    }

    void App::UpdateMultitapSourceMenu() {
        TickMultitapSources(window_, config_.multitap_player_source, config_.controller_type);
    }

    void App::SetMultitapSource(int port, int player, const std::string& key) {
        if (port < 0 || port >= 2 || player < 0 || player >= 4)
            return;
        config_.multitap_player_source[port][player] = key;
        UpdateMultitapSourceMenu();
        SaveSettingsIfChanged();
        SendConfigToMachine();
    }

    void App::UpdateFrameLimiterMenu() { TickFrameLimiter(window_, config_.frame_limiter); }
    void App::UpdateSpeedMenu() { TickSpeed(window_, config_.emulation_speed); }

    void App::SetSpeed(float speed) {
        config_.emulation_speed = speed;
        // A speed is only meaningful if something is pacing the machine, so asking for one asks
        // for the limiter. Without this, choosing a speed with the limiter off does nothing at all
        // and gives no hint why.
        if (!config_.frame_limiter) {
            config_.frame_limiter = true;
            UpdateFrameLimiterMenu();
        }
        UpdateSpeedMenu();
        SaveSettingsIfChanged();
        // The machine restarts its pacing when it sees either of these change - the deadline it
        // was keeping and the resampler's position both belong to the old rate.
        SendConfigToMachine();
    }

    void App::SetFrameLimiter(bool on) {
        config_.frame_limiter = on;
        UpdateFrameLimiterMenu();
        SaveSettingsIfChanged();
        SendConfigToMachine();
    }

    void App::UpdateCdTimingMenu() { TickCdTiming(window_, config_.cdrom_mechanical_timing); }

    void App::SetCdMechanicalTiming(bool on) {
        config_.cdrom_mechanical_timing = on;
        UpdateCdTimingMenu();
        SaveSettingsIfChanged();
        SendConfigToMachine();
    }

    void App::UpdateSkipBiosIntroMenu() { TickSkipBiosIntro(window_, config_.skip_bios_intro); }

    void App::SetSkipBiosIntro(bool on) {
        config_.skip_bios_intro = on;
        UpdateSkipBiosIntroMenu();
        SaveSettingsIfChanged();
        SendConfigToMachine();
    }

    void App::UpdateRecompilerMenu() { TickRecompiler(window_, config_.recompiler); }

    // Changing CPU while a game is running is allowed, and this is all it takes from here: the
    // setting reaches the machine as a request, and System::StepInstruction acts on it between
    // instructions - the only place it is safe, since switching it off frees compiled code.
    void App::SetRecompiler(bool on) {
        config_.recompiler = on;
        UpdateRecompilerMenu();
        SaveSettingsIfChanged();
        SendConfigToMachine();
    }

    void App::UpdatePauseInMenusMenu() { TickPauseInMenus(window_, config_.pause_in_menus); }

    void App::SetPauseInMenus(bool on) {
        config_.pause_in_menus = on;
        UpdatePauseInMenusMenu();
        SaveSettingsIfChanged();
        SendConfigToMachine();
        // Asked for while a menu is open - which is the only way to ask - so it takes effect from
        // the next one rather than pausing under the one being used.
    }

    void App::UpdateShowTimingsMenu() { TickShowTimings(window_, config_.show_timings); }

    void App::SetShowTimings(bool on) {
        config_.show_timings = on;
        UpdateShowTimingsMenu();
        SaveSettingsIfChanged();
        SendConfigToMachine();
        UpdateTitle();
    }

    void App::UpdateBiosConsoleMenu() { TickBiosConsole(window_, config_.show_bios_console); }

    // Nothing to tell the machine: the core records the console whether or not anyone looks.
    void App::SetShowBiosConsole(bool on) {
        config_.show_bios_console = on;
        console_.Show(on);
        UpdateBiosConsoleMenu();
        SaveSettingsIfChanged();
    }

    void App::UpdateSerialToConsoleMenu() { TickSerialToConsole(window_, config_.sio1_to_console); }

    void App::UpdateMouseMenu() {
        TickMouseMotion(window_, config_.mouse_motion);
        TickMouseDpi(window_, config_.mouse_dpi);
    }

    // The input thread owns the scaling, so this goes there rather than to the machine - along
    // with whether Match Desktop Pointer may take the cursor over at all, which is a question
    // about what the front end is doing rather than about the mouse:
    //
    //   - a port has to be set to Mouse, or nothing wants the pointer in the first place;
    //   - the machine has to be running. Nothing is started yet, or it is paused, and the
    //     pointer is the person's own - to reach a menu, to pick a disc, to close the window -
    //     so it stays where they put it and stays visible;
    //   - no menu may be open. The menu bar is driven with that same pointer, and pinning it to
    //     the middle of the window mid-menu would make the menus unusable.
    //
    // Mouse::Poll adds the last condition itself: the window has to have focus.
    void App::SendMouseSettingsToInput() {
        if (input_ == nullptr)
            return;
        utilities::MouseMotion motion = utilities::MouseMotion::kDesktop;
        if (config_.mouse_motion == "windows")
            motion = utilities::MouseMotion::kWindows;
        else if (config_.mouse_motion == "hardware")
            motion = utilities::MouseMotion::kHardware;
        const bool port_is_mouse = (config_.controller_type[0] == "mouse" ||
                                    config_.controller_type[1] == "mouse");
        const bool may_capture = port_is_mouse && !paused_by_user_ && menu_depth_ == 0;
        input_->SetMouseMotion(motion, config_.mouse_dpi, may_capture);

        // Give the arrow back the moment capture ends rather than waiting for the next mouse
        // move to ask through WM_SETCURSOR - a pause the person asked for should show them a
        // pointer straight away.
        if (!may_capture && window_ != nullptr)
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    }

    void App::SetMouseMotion(const std::string& key) {
        config_.mouse_motion = key;
        UpdateMouseMenu();
        SaveSettingsIfChanged();
        SendMouseSettingsToInput();
    }

    void App::SetMouseDpi(int dpi) {
        config_.mouse_dpi = dpi;
        UpdateMouseMenu();
        SaveSettingsIfChanged();
        SendMouseSettingsToInput();
    }

    // This one the machine does need: Sio1 reads it as each byte is transmitted.
    void App::SetSerialToConsole(bool on) {
        config_.sio1_to_console = on;
        UpdateSerialToConsoleMenu();
        SaveSettingsIfChanged();
        SendConfigToMachine();
    }

    namespace {

        // On the machine's thread: copies of both cards, for the editor to read on its own.
        std::array<MemoryCardEditor::Snapshot, 2> SnapshotCards(System& system) {
            std::array<MemoryCardEditor::Snapshot, 2> cards;
            for (int slot = 0; slot < 2; ++slot) {
                const emulation::psx::MC& mc = system.mc(slot);
                cards[slot].inserted = mc.connected();
                if (mc.connected()) {
                    cards[slot].filename = mc.filename();
                    cards[slot].image.assign(mc.data(),
                                             mc.data() + emulation::psx::mcdir::kCardSize);
                }
            }
            return cards;
        }

    }   // namespace

    void App::RefreshMemoryCardEditor() {
        PostToMachine([this](Machine& machine) {
            auto cards = SnapshotCards(machine.system());
            PostToUi([this, cards = std::move(cards)] { card_editor_.SetCards(cards); });
        });
    }

    // The edit runs against the live card, between frames. If it changed anything the card is
    // flagged as swapped - so a game re-reads the directory rather than trusting what it read
    // before - and saved at once, since an editor's change is one the person means to keep.
    void App::EditMemoryCard(int slot, MemoryCardEditor::Edit edit) {
        PostToMachine([this, slot, edit = std::move(edit)](Machine& machine) {
            emulation::psx::MC& mc = machine.system().mc(slot);
            std::string error;
            bool ok = false;
            if (!mc.connected()) {
                error = "There is no card in slot " + std::to_string(slot + 1) + ".";
            } else if (edit(mc.data(), &error)) {
                mc.Modified();
                ok = mc.Flush();
                if (!ok)
                    error = "The change was made, but the card file could not be written.";
            }
            auto cards = SnapshotCards(machine.system());
            PostToUi([this, cards = std::move(cards), ok, error] {
                card_editor_.SetCards(cards);
                if (!ok) {
                    const std::wstring message(error.begin(), error.end());
                    ShowWarning(window_, message.c_str());
                }
            });
        });
    }

    void App::EjectMemoryCard(int slot) {
        PostToMachine([this, slot](Machine& machine) {
            machine.system().mc(slot).Eject();
            PostToUi([this] {
                if (card_editor_.visible())
                    RefreshMemoryCardEditor();
            });
        });
    }

    void App::CollectConsoleText(System& system) {
        emulation::psx::Kernel& kernel = system.kernel();
        const uint32_t session = kernel.session();
        const bool new_boot = (session != console_session_);
        console_session_ = session;

        std::string text;
        kernel.TakeConsoleText(&text);
        if (!new_boot && text.empty())
            return;
        PostToUi([this, new_boot, text = std::move(text)] {
            if (new_boot)
                console_.AppendMarker(L"restart");
            console_.Append(text);
        });
    }

    // On the machine's thread, like everything the debugger window asks for.
    void App::SendDebuggerSnapshot(Machine& machine, uint32_t center, bool from_halt) {
        emulation::psx::Debugger& debugger = machine.system().debugger();
        if (center == DebuggerWindow::kAtPc)
            center = machine.system().cpu().context()->pc;
        auto snapshot = std::make_shared<emulation::psx::Debugger::Snapshot>();
        debugger.Capture(snapshot.get(), center, DebuggerWindow::kLines);
        PostToUi([this, snapshot, from_halt] {
            if (from_halt)
                debugger_open_ = true;
            debugger_.SetSnapshot(*snapshot, from_halt);
        });
    }

    void App::RefreshDebuggerIfOpen(Machine& machine) {
        if (debugger_open_.load())
            SendDebuggerSnapshot(machine, DebuggerWindow::kAtPc, false);
    }

    // ---------------------------------------------------------------------------------------------
    // The machine
    // ---------------------------------------------------------------------------------------------

    // All of these run on the machine's thread. What they have to say comes back through PostToUi.

    void App::ResetMachine() {
        const std::string bios = bios_path_;
        PostToMachine([this, bios](Machine& machine) {
            System& system = machine.system();
            system.Deinitialize();
            if (system.Initialize(bios.c_str()) != 0) {
                PostToUi([this] {
                    ShowError(window_, L"Failed to initialise the system (BIOS missing?).");
                });
                return;
            }
            system.set_auto_boot(false);
            machine.ResetPacing();
            RefreshDebuggerIfOpen(machine);
        });
    }

    void App::BootDiscFromFile(const std::string& path) {
        const std::string bios = bios_path_;
        PostToMachine([this, bios, path](Machine& machine) {
            System& system = machine.system();
            system.Deinitialize();
            if (system.Initialize(bios.c_str()) != 0) {
                PostToUi([this] {
                    ShowError(window_, L"Failed to initialise the system (BIOS missing?).");
                });
                return;
            }
            system.set_auto_boot(false);

            // The disc has to be in the drive before the BIOS looks, or it finds an open shell and
            // stops at the menu.
            system.EjectDisc();
            if (!system.LoadDisc(path.c_str())) {
                PostToUi([this] {
                    ShowWarning(window_,
                                L"Could not read that disc image.\n\n"
                                L"Supported: .cue (with its .bin or .img), .mds (with its "
                                L".mdf), .bin, .img, .iso.");
                });
                return;
            }
            // Already mounted, so there is nothing left for the hand-off to load itself - just arm
            // it before the machine starts running.
            if (system.config().skip_bios_intro)
                system.set_auto_boot(true);

            // Each disc gets its own pair of memory cards - a real console has none of this, but
            // "which card was in when I saved" is otherwise a question the player has to answer by
            // hand.
            LoadOrCreateMemoryCardsForDisc(system, path);

            machine.ResetPacing();
            RefreshDebuggerIfOpen(machine);
            machine.SetPaused(emulation::host::kPausedByUser, false);
            PostToUi([this, path] {
                paused_by_user_ = false;
                SetWindowTitleForPath(path);
                NoteRecentDisc(path);
                SendMouseSettingsToInput();
            });
        });
    }

    void App::BootBios() {
        const std::string bios = bios_path_;
        PostToMachine([this, bios](Machine& machine) {
            System& system = machine.system();
            system.Deinitialize();
            if (system.Initialize(bios.c_str()) != 0) {
                PostToUi([this] {
                    ShowError(window_, L"Failed to initialise the system (BIOS missing?).");
                });
                return;
            }
            system.set_auto_boot(false);
            system.EjectDisc();
            machine.ResetPacing();
            RefreshDebuggerIfOpen(machine);
            machine.SetPaused(emulation::host::kPausedByUser, false);
            PostToUi([this] {
                paused_by_user_ = false;
                SetWindowTitleForPath(std::string());
                SendMouseSettingsToInput();
            });
        });
    }

    void App::BootPsExeFromFile(const std::string& path) {
        if (!LooksLikePsExe(path)) {
            ShowWarning(window_,
                        L"Could not load that file as a PS-X EXE.\n\n"
                        L"It must be the executable itself - the header starts with "
                        L"the 8 bytes \"PS-X EXE\" - not a disc image or a Windows "
                        L"executable.");
            return;
        }
        const std::string bios = bios_path_;
        PostToMachine([this, bios, path](Machine& machine) {
            System& system = machine.system();
            system.Deinitialize();
            if (system.Initialize(bios.c_str()) != 0) {
                PostToUi([this] {
                    ShowError(window_, L"Failed to initialise the system (BIOS missing?).");
                });
                return;
            }
            // Nothing about a leftover disc should affect a test program that never asks the
            // CD-ROM for anything. The BIOS still runs for real first - see the hand-off in
            // System::set_auto_boot_exe.
            system.EjectDisc();
            system.set_auto_boot_exe(true, path);
            machine.ResetPacing();
            RefreshDebuggerIfOpen(machine);
            machine.SetPaused(emulation::host::kPausedByUser, false);
            PostToUi([this, path] {
                paused_by_user_ = false;
                SetWindowTitleForPath(path);
                SendMouseSettingsToInput();
            });
        });
    }

    // On the machine's thread, from the boot paths above and from the command line before the
    // threads start.
    void App::LoadOrCreateMemoryCardsForDisc(System& system, const std::string& disc_path) {
        if (memcards_root_.empty())
            return;

        const std::string dir = memcards_root_ + "\\" + DiscIdentifier(disc_path);
        EnsureDirectory(dir);

        for (int slot = 0; slot < 2; ++slot) {
            const std::string path = dir + "\\card" + std::to_string(slot + 1) + ".mcr";
            if (system.mc(slot).LoadFile(path.c_str()) == S_OK)
                continue;

            // LoadFile fails for two different reasons and only one is worth saying anything
            // about: no card there yet, which is the ordinary case for a game played for the first
            // time, or a file that exists but is not a valid 128 KB card, which CreateFile is
            // about to overwrite.
            FILE* existing = fopen(path.c_str(), "rb");
            const bool had_file = existing != nullptr;
            if (existing != nullptr)
                fclose(existing);

            if (system.mc(slot).CreateFile(path.c_str()) != S_OK) {
                const std::wstring message =
                    L"Could not create a memory card for slot " + std::to_wstring(slot + 1) + L".";
                PostToUi([this, message] { ShowWarning(window_, message.c_str()); });
            } else if (had_file) {
                const std::wstring message =
                    L"The memory card file for slot " + std::to_wstring(slot + 1) +
                    L" was not a valid 128 KB card and has been reset:\n\n" +
                    std::wstring(path.begin(), path.end());
                PostToUi([this, message] { ShowWarning(window_, message.c_str()); });
            }
        }
    }

    void App::SetUserPaused(bool paused) {
        paused_by_user_ = paused;
        PostToMachine([paused](Machine& machine) {
            machine.SetPaused(emulation::host::kPausedByUser, paused);
        });
        // A paused machine gives the pointer back - see SendMouseSettingsToInput.
        SendMouseSettingsToInput();
    }

    void App::EnterMenuPause() {
        ++menu_depth_;
        if (menu_depth_ == 1 && config_.pause_in_menus) {
            PostToMachine(
                [](Machine& machine) { machine.SetPaused(emulation::host::kPausedForMenu, true); });
        }
        // The menus are driven with the pointer, so it is theirs while one is open, whether or
        // not the machine itself pauses for it.
        if (menu_depth_ == 1)
            SendMouseSettingsToInput();
    }

    void App::LeaveMenuPause() {
        if (menu_depth_ > 0)
            --menu_depth_;
        if (menu_depth_ == 0) {
            // Cleared whether or not it was set: turning the setting off while a menu is open
            // would otherwise leave the machine paused with nothing to unpause it.
            PostToMachine([](Machine& machine) {
                machine.SetPaused(emulation::host::kPausedForMenu, false);
            });
            SendMouseSettingsToInput();
        }
    }

    std::string App::SaveStateSlotPath(System& system, int slot) const {
        return psxemu::SaveStateSlotPath(savestates_root_, system.cdrom().disc().path(), slot);
    }

    void App::SetWindowTitleForPath(const std::string& path) {
        if (path.empty()) {
            title_base_ = kWindowTitle;
        } else {
            const size_t slash = path.find_last_of("/\\");
            const std::string name = (slash == std::string::npos) ? path : path.substr(slash + 1);
            title_base_ =
                std::wstring(kWindowTitle) + L" - " + std::wstring(name.begin(), name.end());
        }
        UpdateTitle();
    }

    // ---------------------------------------------------------------------------------------------
    // Messages
    // ---------------------------------------------------------------------------------------------

    App* App::From(HWND window) {
        return reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    }

    LRESULT CALLBACK App::WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        if (message == WM_NCCREATE) {
            const CREATESTRUCTW* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            SetWindowLongPtrW(window, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(create->lpCreateParams));
            return DefWindowProcW(window, message, wparam, lparam);
        }

        App* app = From(window);

        switch (message) {
            case kMessageRunOnUi: {
                // A thread asked for this to happen here. Owned by the message; run it once - or
                // not at all, if the window is on its way out: a warning that opens a dialog while
                // everything is being stopped helps nobody.
                std::function<void()>* work =
                    reinterpret_cast<std::function<void()>*>(lparam);
                if (work != nullptr) {
                    if (app == nullptr || !app->stopping_)
                        (*work)();
                    delete work;
                }
                return 0;
            }

            case WM_SIZE:
                if (app != nullptr && app->video_ != nullptr && wparam != SIZE_MINIMIZED) {
                    const int width = LOWORD(lparam);
                    const int height = HIWORD(lparam);
                    // The swap chain belongs to the video thread; resizing it from here would be
                    // two threads in one device. It shows the current frame again afterwards, so
                    // the window is not left stretched until the next one arrives.
                    app->video_->Post([width, height](VideoOutput& video) {
                        if (video.presenter() != nullptr)
                            video.presenter()->Resize(width, height);
                        video.PresentAgain();
                    });
                }
                return 0;

            // Windows is about to run a modal loop of its own - the menu bar, or a drag or resize
            // of the window. The machine keeps running underneath it now; whether it should is
            // Emulation > Pause While in Menus.
            // The cursor is pinned to the middle of the window while Input > Mouse > Motion is
            // Match Desktop Pointer, so it has to be hidden - an arrow stuck in the centre of the
            // picture is worse than none. Only over the client area: the frame and the menu bar
            // keep theirs.
            case WM_SETCURSOR:
                if (app != nullptr && LOWORD(lparam) == HTCLIENT && app->input_ != nullptr &&
                    app->input_->capturing_mouse()) {
                    SetCursor(nullptr);
                    return TRUE;
                }
                break;

            // Someone has been in the mouse control panel: Windows Acceleration mode reads the
            // pointer speed and the curve from there, so it re-reads them.
            case WM_SETTINGCHANGE:
                if (app != nullptr && app->input_ != nullptr)
                    app->input_->RefreshWindowsPointerSettings();
                break;

            case WM_ENTERMENULOOP:
                if (app != nullptr)
                    app->EnterMenuPause();
                return 0;

            case WM_EXITMENULOOP:
                if (app != nullptr)
                    app->LeaveMenuPause();
                return 0;

            case WM_COMMAND:
                if (app != nullptr)
                    app->OnCommand(LOWORD(wparam));
                return 0;

            case WM_KEYDOWN:
                if (app != nullptr)
                    app->OnKeyDown(wparam);
                return 0;

            case WM_CLOSE:
                // Every thread is stopped before the window goes: the video thread has a swap
                // chain on it, and the machine has a report to post to it.
                if (app != nullptr)
                    app->StopThreads();
                DestroyWindow(window);
                return 0;

            case WM_DESTROY:
                PostQuitMessage(0);
                return 0;

            default:
                break;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    void App::OnKeyDown(WPARAM key) {
        if (stopping_)
            return;
        if (key == VK_SPACE)
            SetUserPaused(!paused_by_user_);
        // F1-F8: plain loads that slot, Ctrl+ saves it. Both also become the slot the Save
        // State/Load State menu items act on, so pressing F3 and then using the menu (or another
        // F-key) do not disagree about which slot is "current".
        if (key >= VK_F1 && key <= VK_F8) {
            const int slot = static_cast<int>(key - VK_F1) + 1;
            last_slot_ = slot;
            const bool save = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            PostToMachine([this, slot, save](Machine& machine) {
                const std::string path = SaveStateSlotPath(machine.system(), slot);
                const std::string error = save ? machine.system().SaveState(path)
                                               : machine.system().LoadState(path);
                if (!save) {
                    machine.ResetPacing();
                    RefreshDebuggerIfOpen(machine);
                }
                if (!error.empty()) {
                    const std::wstring message(error.begin(), error.end());
                    PostToUi([this, message] { ShowError(window_, message.c_str()); });
                }
            });
        }
    }

    void App::OnCommand(int command) {
        if (stopping_)
            return;

        switch (command) {
            case kCommandBootDisc: {
                // Switching the console on with a game in the drive. Pick an image, then the
                // machine starts from cold and the BIOS boots it.
                MenuPause held(this);
                const std::string path =
                    ChooseFile(window_, FileDialog::kOpen, kDiscFilter, nullptr);
                if (!path.empty())
                    BootDiscFromFile(path);
                break;
            }

            case kCommandSwapDisc: {
                // Changing the disc in a running machine, for a game that asks for its second one.
                // No reset - that is what Boot disc is for.
                MenuPause held(this);
                const std::string path =
                    ChooseFile(window_, FileDialog::kOpen, kDiscFilter, nullptr);
                if (path.empty())
                    break;
                PostToMachine([this, path](Machine& machine) {
                    if (!machine.system().LoadDisc(path.c_str())) {
                        PostToUi([this] {
                            ShowWarning(window_, L"Could not read that disc image.");
                        });
                        return;
                    }
                    PostToUi([this, path] {
                        SetWindowTitleForPath(path);
                        NoteRecentDisc(path);
                    });
                });
                break;
            }

            case kCommandEjectDisc:
                PostToMachine([this](Machine& machine) {
                    machine.system().EjectDisc();
                    PostToUi([this] { SetWindowTitleForPath(std::string()); });
                });
                break;

            case kCommandBootBios:
                BootBios();
                break;

            case kCommandBootExe: {
                // A standalone test program or homebrew binary - no disc.
                MenuPause held(this);
                const std::string path =
                    ChooseFile(window_, FileDialog::kOpen, kExeFilter, nullptr);
                if (!path.empty())
                    BootPsExeFromFile(path);
                break;
            }

            case kCommandOpenMemoryCardSlot1:
            case kCommandOpenMemoryCardSlot2: {
                const int slot = (command == kCommandOpenMemoryCardSlot1) ? 0 : 1;
                MenuPause held(this);
                const std::string path = ChooseFile(window_, FileDialog::kOpen, kCardFilter, "mcr");
                if (path.empty())
                    break;
                PostToMachine([this, slot, path](Machine& machine) {
                    // Inserting ejects - and so saves - whatever card was in the slot first.
                    const bool ok = machine.system().mc(slot).LoadFile(path.c_str()) == S_OK;
                    PostToUi([this, ok, path] {
                        if (!ok) {
                            const bool exists =
                                GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES;
                            ShowWarning(window_,
                                        exists ? L"That is not a memory card. A card file is "
                                                 L"exactly 128 KB."
                                               : L"Could not open that memory card file.");
                        }
                        if (card_editor_.visible())
                            RefreshMemoryCardEditor();
                    });
                });
                break;
            }

            case kCommandCreateMemoryCardSlot1:
            case kCommandCreateMemoryCardSlot2: {
                const int slot = (command == kCommandCreateMemoryCardSlot1) ? 0 : 1;
                MenuPause held(this);
                const std::string path = ChooseFile(window_, FileDialog::kSave, kCardFilter, "mcr");
                if (path.empty())
                    break;
                PostToMachine([this, slot, path](Machine& machine) {
                    const bool ok = machine.system().mc(slot).CreateFile(path.c_str()) == S_OK;
                    PostToUi([this, ok] {
                        if (!ok)
                            ShowWarning(window_, L"Could not create that memory card file.");
                        if (card_editor_.visible())
                            RefreshMemoryCardEditor();
                    });
                });
                break;
            }

            case kCommandKeyBindings:
                key_bindings_.Show(key_map_);
                break;

            case kCommandClearRecentDiscs:
                recent_discs_.clear();
                SaveRecentDiscs();
                break;

            case kCommandEjectMemoryCardSlot1:
            case kCommandEjectMemoryCardSlot2:
                EjectMemoryCard(command == kCommandEjectMemoryCardSlot1 ? 0 : 1);
                break;

            case kCommandMemoryCardEditor:
                card_editor_.Show(true);
                break;

            case kCommandReset:
                ResetMachine();
                break;

            case kCommandPause:
                SetUserPaused(!paused_by_user_);
                break;

            case kCommandSaveState:
            case kCommandLoadState: {
                const int slot = last_slot_;
                const bool save = (command == kCommandSaveState);
                PostToMachine([this, slot, save](Machine& machine) {
                    const std::string path = SaveStateSlotPath(machine.system(), slot);
                    const std::string error = save ? machine.system().SaveState(path)
                                                   : machine.system().LoadState(path);
                    if (!save) {
                        machine.ResetPacing();
                        RefreshDebuggerIfOpen(machine);
                    }
                    if (!error.empty()) {
                        const std::wstring message(error.begin(), error.end());
                        PostToUi([this, message] { ShowError(window_, message.c_str()); });
                    }
                });
                break;
            }

            case kCommandViewVram: {
                view_vram_ = !view_vram_;
                HMENU bar = GetMenu(window_);
                if (bar != nullptr) {
                    CheckMenuItem(bar, static_cast<UINT>(kCommandViewVram),
                                  MF_BYCOMMAND | (view_vram_ ? MF_CHECKED : MF_UNCHECKED));
                }
                const bool on = view_vram_;
                PostToMachine([on](Machine& machine) { machine.set_view_vram(on); });
                break;
            }

            case kCommandFrameLimiter:
                SetFrameLimiter(!config_.frame_limiter);
                break;

            case kCommandCdMechanicalTiming:
                SetCdMechanicalTiming(!config_.cdrom_mechanical_timing);
                break;

            case kCommandSkipBiosIntro:
                SetSkipBiosIntro(!config_.skip_bios_intro);
                break;

            case kCommandRecompiler:
                SetRecompiler(!config_.recompiler);
                break;

            case kCommandPauseInMenus:
                SetPauseInMenus(!config_.pause_in_menus);
                break;

            case kCommandShowTimings:
                SetShowTimings(!config_.show_timings);
                break;
            case kCommandBiosConsole:
                SetShowBiosConsole(!config_.show_bios_console);
                break;
            case kCommandSerialToConsole:
                SetSerialToConsole(!config_.sio1_to_console);
                break;

            case kCommandDebugger:
                debugger_open_ = true;
                debugger_.Show(true);
                break;

            case kCommandRescanBios:
                RefreshBiosMenu();
                break;

            case kCommandOpenBiosFolder:
                // Where to put a dump is the question an empty list raises, so answer it by
                // opening the folder rather than naming it in a message box.
                if (!bios_root_.empty())
                    ShellExecuteA(window_, "open", bios_root_.c_str(), nullptr, nullptr, SW_SHOW);
                break;

            case kCommandExit:
                PostMessageW(window_, WM_CLOSE, 0, 0);
                break;

            default:
                // The volume steps are one contiguous run of command ids.
                if (command >= kCommandVolumeFirst &&
                    command < kCommandVolumeFirst + static_cast<int>(std::size(kVolumeSteps))) {
                    SetVolume(kVolumeSteps[command - kCommandVolumeFirst].value);
                } else if (command >= kCommandAudioBackendFirst &&
                           command < kCommandAudioBackendFirst +
                                         static_cast<int>(std::size(kAudioBackendChoices))) {
                    SetAudioBackend(
                        kAudioBackendChoices[command - kCommandAudioBackendFirst].key);
                } else if (command >= kCommandRendererFirst &&
                           command < kCommandRendererFirst +
                                         static_cast<int>(std::size(kBackendChoices))) {
                    SetRenderer(kBackendChoices[command - kCommandRendererFirst].key);
                } else if (command >= kCommandFilterFirst &&
                           command <
                               kCommandFilterFirst + static_cast<int>(std::size(kFilterChoices))) {
                    SetFilter(kFilterChoices[command - kCommandFilterFirst].key);
                } else if (command >= kCommandControllerTypeFirst &&
                           command <= kCommandControllerTypeLast) {
                    const int type_count = static_cast<int>(std::size(kControllerTypeChoices));
                    const int offset = command - kCommandControllerTypeFirst;
                    SetControllerType(offset / type_count,
                                     kControllerTypeChoices[offset % type_count].key);
                } else if (command >= kCommandInputSourceFirst &&
                           command <= kCommandInputSourceLast) {
                    const int source_count = static_cast<int>(std::size(kInputSourceChoices));
                    const int offset = command - kCommandInputSourceFirst;
                    SetInputSource(offset / source_count,
                                   kInputSourceChoices[offset % source_count].key);
                } else if (command >= kCommandMultitapSourceFirst &&
                           command <= kCommandMultitapSourceLast) {
                    // Same offset math as the plain per-port source above, with one more dimension
                    // (player) folded in - see menu.cpp's CreateMainMenu for how it was built.
                    const int source_count = static_cast<int>(std::size(kInputSourceChoices));
                    const int offset = command - kCommandMultitapSourceFirst;
                    const int port = offset / (4 * source_count);
                    const int player = (offset / source_count) % 4;
                    SetMultitapSource(port, player,
                                      kInputSourceChoices[offset % source_count].key);
                } else if (command >= kCommandMouseMotionFirst &&
                           command <= kCommandMouseMotionLast) {
                    SetMouseMotion(kMouseMotionChoices[command - kCommandMouseMotionFirst].key);
                } else if (command >= kCommandMouseDpiFirst &&
                           command <= kCommandMouseDpiLast) {
                    SetMouseDpi(kMouseDpiChoices[command - kCommandMouseDpiFirst]);
                } else if (command >= kCommandSpeedFirst &&
                           command < kCommandSpeedFirst +
                                         static_cast<int>(std::size(kSpeedChoices))) {
                    SetSpeed(kSpeedChoices[command - kCommandSpeedFirst].value);
                } else if (command >= kCommandBiosFirst && command <= kCommandBiosLast) {
                    // The only run here whose entries are not a table in const.h: the nth id is
                    // the nth image the last scan found, which SelectBios bounds-checks against
                    // the list it holds - the folder can have changed since the menu was filled.
                    SelectBios(command - kCommandBiosFirst);
                } else if (command >= kCommandRecentDiscFirst &&
                           command <= kCommandRecentDiscLast) {
                    const size_t index = static_cast<size_t>(command - kCommandRecentDiscFirst);
                    if (index >= recent_discs_.size())
                        break;
                    const std::string path = recent_discs_[index];
                    // An image that has moved or gone - a share that is offline, a renamed
                    // folder - is said so and dropped, rather than booting into an empty drive.
                    if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
                        // Removed before the warning, not after: the message box pumps messages,
                        // so another command - Clear, another pick from this list - can run while
                        // it is up, and an index held across it would point at the wrong entry
                        // or past the end.
                        recent_discs_.erase(recent_discs_.begin() + index);
                        SaveRecentDiscs();
                        const std::wstring message =
                            L"That disc image is no longer there, so it has been removed from "
                            L"Recent Discs:\n\n" + Widen(path);
                        ShowWarning(window_, message.c_str());
                        break;
                    }
                    BootDiscFromFile(path);
                }
                break;
        }
    }

    // A button missing from the file keeps its default; one present but empty stays unbound -
    // clearing a key in the editor has to survive a restart.
    void App::LoadKeyBindings() {
        for (int i = 0; i < kPadButtons; ++i) {
            const std::string name = settings_.GetString(
                kKeyBindings[i].setting, KeyName(kKeyBindings[i].key));
            key_map_[i] = KeyFromName(name);
        }
    }

    void App::SetKeyBindings(const KeyMap& map) {
        key_map_ = map;
        if (input_ != nullptr)
            input_->SetKeyMap(key_map_);
        if (settings_path_.empty())
            return;
        emulation::psx::SettingsFile updated = settings_;
        for (int i = 0; i < kPadButtons; ++i)
            updated.SetString(kKeyBindings[i].setting, KeyName(key_map_[i]));
        if (updated.Serialise() == settings_.Serialise())
            return;
        settings_ = updated;
        settings_.Save(settings_path_);
    }

    void App::LoadRecentDiscs() {
        recent_discs_.clear();
        for (int i = 1; i <= kMaxRecentDiscs; ++i) {
            const std::string key = "recent_disc_" + std::to_string(i);
            const std::string path = settings_.GetString(key.c_str(), std::string());
            if (!path.empty())
                recent_discs_.push_back(path);
        }
        PopulateRecentDiscsMenu(window_, recent_discs_);
    }

    void App::NoteRecentDisc(const std::string& path) {
        for (size_t i = 0; i < recent_discs_.size(); ++i) {
            if (_stricmp(recent_discs_[i].c_str(), path.c_str()) == 0) {
                recent_discs_.erase(recent_discs_.begin() + i);
                break;
            }
        }
        recent_discs_.insert(recent_discs_.begin(), path);
        if (recent_discs_.size() > static_cast<size_t>(kMaxRecentDiscs))
            recent_discs_.resize(kMaxRecentDiscs);
        SaveRecentDiscs();
    }

    // Every slot is written, the unused ones empty, so a shortened list does not leave its old
    // tail behind in the file for the next load to pick up.
    void App::SaveRecentDiscs() {
        PopulateRecentDiscsMenu(window_, recent_discs_);
        if (settings_path_.empty())
            return;
        emulation::psx::SettingsFile updated = settings_;
        for (int i = 1; i <= kMaxRecentDiscs; ++i) {
            const std::string key = "recent_disc_" + std::to_string(i);
            const size_t at = static_cast<size_t>(i - 1);
            updated.SetString(key.c_str(), at < recent_discs_.size() ? recent_discs_[at] : "");
        }
        if (updated.Serialise() == settings_.Serialise())
            return;
        settings_ = updated;
        settings_.Save(settings_path_);
    }

}   // namespace psxemu
