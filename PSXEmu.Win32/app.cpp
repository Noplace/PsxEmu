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

namespace psxemu {

    using emulation::psx::System;

    App::~App() {
        if (system_ != nullptr)
            system_->Deinitialize();
        if (audio_ != nullptr)
            audio_->Shutdown();
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
        bios_path_ = FindBios(command_line.bios);
        if (bios_path_.empty()) {
            ShowError(nullptr,
                      L"No BIOS image found.\n\n"
                      L"A PlayStation BIOS dump is required. Put SCPH1001.BIN in a "
                      L"'bios' folder beside the executable, or pass its path as the "
                      L"first argument.");
            return false;
        }

        // Loaded early, before the graphics engine exists to hold the choice - the rest of the
        // settings (audio_volume and friends, which live on System::config()) are read back later
        // via LoadConfig, once the machine exists to hold them; graphics_backend is read directly
        // in CreateGraphics too, purely to decide which engine to construct, and it is read again
        // through the normal LoadConfig path to end up in the same place either way.
        settings_path_ = Narrow(SettingsPathBesideExecutable());
        settings_.Load(settings_path_);

        if (!CreateAppWindow(instance))
            return false;
        if (!CreateGraphics())
            return false;

        audio_ = CreateAudioEngine();
        if (audio_ != nullptr)
            audio_->Play();

        if (!CreateMachine())
            return false;

        ApplySettings();

        // Per-disc data, under Documents\My Games\PSXEmu.
        SetUpDataDirectories();

        if (!command_line.disc.empty() && system_->LoadDisc(command_line.disc.c_str())) {
            SetWindowTitleForPath(command_line.disc);
            LoadOrCreateMemoryCardsForDisc(command_line.disc);
        }

        ShowWindow(window_, show_command);
        UpdateWindow(window_);
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

    bool App::CreateGraphics() {
        RECT client;
        GetClientRect(window_, &client);
        const int client_width = client.right - client.left;
        const int client_height = client.bottom - client.top;
        const std::string requested_backend = settings_.GetString("graphics_backend", "d3d11");
        const GraphicsBackend preferred =
            (requested_backend == "d3d12") ? GraphicsBackend::kD3D12 : GraphicsBackend::kD3D11;
        graphics_ = CreateGraphicsEngine(preferred, window_, client_width, client_height, window_,
                                         &current_backend_);
        if (graphics_ == nullptr) {
            ShowError(window_, L"Could not create a Direct3D device.");
            return false;
        }
        return true;
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

    void App::ApplySettings() {
        // The rest of the settings, now that the machine exists to hold them - the file itself was
        // already loaded in Initialize, before the graphics engine, to decide which one to
        // construct. A missing file is normal on a first run and leaves the defaults in place.
        emulation::psx::LoadConfig(settings_, system_->config());
        // The engine actually running can differ from the file's own preference if that one failed
        // and CreateGraphicsEngine fell back - reflect reality rather than silently trusting what
        // LoadConfig just read.
        system_->config().graphics_backend = current_backend_;
        UpdateVolumeMenu();
        UpdateRendererMenu();
        system_->sio().set_controller_type(
            0, ParseControllerType(system_->config().controller_type[0]));
        system_->sio().set_controller_type(
            1, ParseControllerType(system_->config().controller_type[1]));
        UpdateControllerTypeMenu();
        UpdateInputSourceMenu();
        UpdateFrameLimiterMenu();
        UpdateCdTimingMenu();
        if (current_backend_ == "d3d12") {
            LoadAllFilters(*graphics_);
            SetFilter(system_->config().video_filter);
        } else {
            UpdateFilterMenu();
        }
    }

    void App::SetUpDataDirectories() {
        data_root_ = ResolveDataRoot();
        if (data_root_.empty())
            return;
        memcards_root_ = data_root_ + "\\memcards";
        savestates_root_ = data_root_ + "\\savestates";
        EnsureDirectory(memcards_root_);
        EnsureDirectory(savestates_root_);
    }

    // ---------------------------------------------------------------------------------------------
    // The loop
    // ---------------------------------------------------------------------------------------------

    int App::MainLoop() {
        running_ = true;
        MSG message = {};
        while (running_) {
            if (!PumpMessages(&message))
                break;

            if (paused_) {
                Sleep(16);
                // Nothing to catch up on when the machine starts again. The limiter would work this
                // out for itself on the first frame back, but saying so here is cheaper than
                // relying on that.
                frame_limiter_.Reset();
                continue;
            }

            ApplyPendingStates();
            PollInput();
            RunOneFrame();
            PumpAudio();
            UpdateSpeedReadout();

            //PumpAudio();

            PresentFrame();
            LimitFrameRate();
        }

        // Written on every change already; this catches anything the last edit missed and costs
        // nothing when there is nothing to write.
        SaveSettingsIfChanged();

        // Everything owned is released by the destructor, in the order it was declared in.
        return static_cast<int>(message.wParam);
    }

    bool App::PumpMessages(MSG* message) {
        while (PeekMessageW(message, nullptr, 0, 0, PM_REMOVE)) {
            if (message->message == WM_QUIT) {
                running_ = false;
                break;
            }
            TranslateMessage(message);
            DispatchMessageW(message);
        }
        return running_;
    }

    void App::ApplyPendingStates() {
        if (pending_save_slot_ >= 0) {
            const std::string path = SaveStateSlotPath(pending_save_slot_);
            const std::string error = system_->SaveState(path);
            if (!error.empty()) {
                const std::wstring message(error.begin(), error.end());
                ShowError(window_, message.c_str());
            }
            pending_save_slot_ = -1;
        }
        if (pending_load_slot_ >= 0) {
            const std::string path = SaveStateSlotPath(pending_load_slot_);
            const std::string error = system_->LoadState(path);
            if (!error.empty()) {
                const std::wstring message(error.begin(), error.end());
                ShowError(window_, message.c_str());
            }
            pending_load_slot_ = -1;
        }
    }

    void App::PollInput() {
        // Input is sampled once per frame, on this thread, and handed to the core. The pads are
        // polled unconditionally, focused or not, so a controller being unplugged mid-game is
        // noticed straight away rather than only after the window is clicked back into; only the
        // *buttons and axes* are withheld while unfocused, matching what the keyboard already does.
        const Gamepad::State gamepad_state[2] = { gamepads_[0].Poll(), gamepads_[1].Poll() };
        const uint16_t keyboard_buttons = ReadKeyboardPad();
        const bool focused = (GetForegroundWindow() == window_);

        // Re-applied every frame rather than only when the menu changes it - exactly how
        // set_connected below already has to be, since a Reset or a fresh disc boot reinitialises
        // Sio to its power-on defaults, and this is what makes either pick the configured
        // controller back up without either call site needing to know that happened.
        // set_controller_type is a no-op once converged, so this costs nothing in the steady state.
        system_->sio().set_controller_type(
            0, ParseControllerType(system_->config().controller_type[0]));
        system_->sio().set_controller_type(
            1, ParseControllerType(system_->config().controller_type[1]));

        for (int port = 0; port < 2; ++port) {
            const InputSource source = ParseInputSource(system_->config().input_source[port]);
            bool connected = true;   // the keyboard is always "there"
            uint16_t buttons = 0;
            uint8_t left_x = 0x80, left_y = 0x80, right_x = 0x80, right_y = 0x80;
            int rumble_target = -1;   // which gamepads_[] slot feels this port's motors

            switch (source) {
                case InputSource::kKeyboard:
                    buttons = focused ? keyboard_buttons : 0;
                    break;
                case InputSource::kGamepad1:
                case InputSource::kGamepad2: {
                    const int g = (source == InputSource::kGamepad1) ? 0 : 1;
                    connected = gamepads_[g].connected();
                    buttons = focused ? gamepad_state[g].buttons : 0;
                    left_x = gamepad_state[g].left_x;
                    left_y = gamepad_state[g].left_y;
                    right_x = gamepad_state[g].right_x;
                    right_y = gamepad_state[g].right_y;
                    rumble_target = g;
                    break;
                }
            }

            system_->sio().set_connected(port, connected);
            system_->sio().set_buttons(port, buttons);
            system_->sio().set_axes(port, left_x, left_y, right_x, right_y);

            // Rumble is an output, not an input, so it is not gated on focus - the emulated machine
            // keeps running in the background (only Pause actually stops it), and a real console
            // would not silence a controller's motor just because another window has focus. If both
            // ports are ever mapped to the same physical pad, the second port's SetRumble call
            // below simply wins for that frame - a real edge case (mirroring one pad to both
            // ports), not a bug.
            if (rumble_target >= 0) {
                uint8_t motor_small = 0, motor_large = 0;
                system_->sio().motor_state(port, &motor_small, &motor_large);
                gamepads_[rumble_target].SetRumble(motor_small, motor_large);
            }
        }
    }

    void App::RunOneFrame() {
        const uint64_t target_frame = system_->gpu().frame_count() + 1;
        uint64_t guard = 0;
        while (system_->gpu().frame_count() < target_frame && guard++ < kMaxInstructionsPerFrame) {
            system_->StepInstruction();
        }
    }

    void App::PumpAudio() {
        if (audio_ == nullptr)
            return;
        const int frames = system_->spu().ReadSamples(audio_scratch_.data(),
                                                      static_cast<int>(audio_scratch_.size() / 2));
        if (frames > 0)
            audio_->QueueAudio(audio_scratch_.data(), frames * 2);
    }

    void App::PresentFrame() {
        int width = 0;
        int height = 0;
        const uint32_t* pixels = system_->gpu().framebuffer(width, height);

        // Video > View VRAM substitutes the whole 1 MB VRAM, converted the same way the display
        // area already is, for the display framebuffer - same presentation path, same letterbox
        // helper, just a different (and much bigger, non-4:3) source rectangle. Rebuilt every frame
        // since VRAM is never still while the machine runs.
        if (view_vram_) {
            const int vram_width = emulation::psx::GpuCore::kVramWidth;
            const int vram_height = emulation::psx::GpuCore::kVramHeight;
            vram_view_scratch_.resize(static_cast<size_t>(vram_width) * vram_height);
            const uint16_t* vram = system_->gpu().vram();
            for (int i = 0; i < vram_width * vram_height; ++i) {
                const uint16_t p = vram[i];
                const uint32_t r = ((p & 0x1F) << 3) | ((p & 0x1F) >> 2);
                const uint32_t g = (((p >> 5) & 0x1F) << 3) | (((p >> 5) & 0x1F) >> 2);
                const uint32_t b = (((p >> 10) & 0x1F) << 3) | (((p >> 10) & 0x1F) >> 2);
                vram_view_scratch_[i] = 0xFF000000u | (r << 16) | (g << 8) | b;
            }
            pixels = vram_view_scratch_.data();
            width = vram_width;
            height = vram_height;
        }

        if (graphics_ != nullptr) {
            graphics_->BeginFrame();
            graphics_->RenderFramebuffer(pixels, width, height);
            graphics_->EndFrame();
        }
    }

    void App::LimitFrameRate() {
        // Composes with the two accidental brakes rather than fighting them: if vsync or the audio
        // device already held this frame back past its deadline there is nothing left to wait for
        // and this returns at once, and if neither did, this is what keeps the machine at the speed
        // of a PlayStation instead of the speed of the screen it is drawn on.
        //
        // Emulation > Frame Limiter turns it off, which puts the loop back to being paced by
        // whatever blocks first - useful to get through a load or to read the host's real headroom
        // off the title bar, and wrong for playing. Reset while it is off so re-enabling starts a
        // fresh deadline rather than owing however long it ran unpaced.
        if (system_->config().frame_limiter)
            frame_limiter_.Wait(system_->gpu().refresh_hz());
        else
            frame_limiter_.Reset();
    }

    void App::UpdateSpeedReadout() {
        ++speed_frames_;
        const auto now = std::chrono::steady_clock::now();
        const double elapsed = std::chrono::duration<double>(now - speed_since_).count();
        if (elapsed < 1.0)
            return;

        const double fps = speed_frames_ / elapsed;
        const double target = (system_ != nullptr) ? system_->gpu().refresh_hz() : 0.0;
        speed_frames_ = 0;
        speed_since_ = now;

        wchar_t suffix[64] = {};
        if (target > 0.0) {
            swprintf(suffix, std::size(suffix), L"  -  %.1f fps (%.0f%%)", fps,
                     100.0 * fps / target);
        } else {
            swprintf(suffix, std::size(suffix), L"  -  %.1f fps", fps);
        }
        SetWindowTextW(window_, (title_base_ + suffix).c_str());
    }

    // ---------------------------------------------------------------------------------------------
    // Settings
    // ---------------------------------------------------------------------------------------------

    void App::SaveSettingsIfChanged() {
        if (system_ == nullptr || settings_path_.empty())
            return;
        emulation::psx::SettingsFile updated = settings_;
        emulation::psx::StoreConfig(updated, system_->config());
        if (updated.Serialise() == settings_.Serialise())
            return;
        settings_ = updated;
        settings_.Save(settings_path_);
    }

    void App::UpdateVolumeMenu() {
        if (system_ != nullptr)
            TickVolume(window_, system_->config().audio_volume);
    }

    void App::SetVolume(float value) {
        if (system_ == nullptr)
            return;
        system_->config().audio_volume = value;
        UpdateVolumeMenu();
        SaveSettingsIfChanged();
    }

    // These two tick against what is actually running rather than against the config, since a
    // fallback can leave the two disagreeing - so no machine is needed and none is checked for.
    void App::UpdateRendererMenu() {
        TickRenderer(window_, current_backend_);
    }

    void App::UpdateFilterMenu() {
        TickFilter(window_, current_backend_, current_filter_);
    }

    void App::SetFilter(const std::string& key) {
        if (current_backend_ != "d3d12") {
            // Reachable from the settings file (a saved filter with graphics_backend reverted to
            // d3d11) as well as a stray click on a greyed item - either way, say why rather than
            // doing nothing.
            ShowWarning(window_,
                        L"Filters require the Direct3D 12 renderer. Switch renderer "
                        L"first (Video > Renderer).");
            return;
        }
        if (graphics_ != nullptr)
            graphics_->SetPixelShader(key);
        current_filter_ = key;
        if (system_ != nullptr)
            system_->config().video_filter = key;
        UpdateFilterMenu();
        SaveSettingsIfChanged();
    }

    void App::SetRenderer(const std::string& key) {
        if (key == current_backend_)
            return;

        RECT client;
        GetClientRect(window_, &client);
        const int width = client.right - client.left;
        const int height = client.bottom - client.top;

        if (graphics_ != nullptr)
            graphics_->Shutdown();
        graphics_.reset();

        const GraphicsBackend preferred =
            (key == "d3d12") ? GraphicsBackend::kD3D12 : GraphicsBackend::kD3D11;
        graphics_ =
            CreateGraphicsEngine(preferred, window_, width, height, window_, &current_backend_);
        if (graphics_ == nullptr) {
            ShowError(window_,
                      L"Could not switch renderer, and the previous one could not "
                      L"be restored either. Restart the emulator.");
            current_backend_.clear();
            current_filter_.clear();
            UpdateRendererMenu();
            UpdateFilterMenu();
            return;
        }

        current_filter_.clear();
        if (current_backend_ == "d3d12") {
            LoadAllFilters(*graphics_);
            const std::string preferred_filter =
                (system_ != nullptr) ? system_->config().video_filter : "";
            SetFilter(preferred_filter);   // also saves + updates the menu
        } else {
            UpdateFilterMenu();
        }

        if (system_ != nullptr)
            system_->config().graphics_backend = current_backend_;
        UpdateRendererMenu();
        SaveSettingsIfChanged();
    }

    void App::UpdateControllerTypeMenu() {
        if (system_ != nullptr)
            TickControllerTypes(window_, system_->config().controller_type);
    }

    void App::SetControllerType(int port, const std::string& key) {
        if (system_ == nullptr)
            return;
        system_->config().controller_type[port] = key;
        system_->sio().set_controller_type(port, ParseControllerType(key));
        UpdateControllerTypeMenu();
        SaveSettingsIfChanged();
    }

    void App::UpdateInputSourceMenu() {
        if (system_ != nullptr)
            TickInputSources(window_, system_->config().input_source);
    }

    void App::SetInputSource(int port, const std::string& key) {
        if (system_ == nullptr)
            return;
        system_->config().input_source[port] = key;
        UpdateInputSourceMenu();
        SaveSettingsIfChanged();
    }

    void App::UpdateFrameLimiterMenu() {
        if (system_ != nullptr)
            TickFrameLimiter(window_, system_->config().frame_limiter);
    }

    void App::SetFrameLimiter(bool on) {
        if (system_ == nullptr)
            return;
        system_->config().frame_limiter = on;
        // Whichever way it went, the deadline it was pacing to is stale - it has either just
        // stopped being used or has not been used for a while. Starting clean stops the first frame
        // back from being asked to make up the gap.
        frame_limiter_.Reset();
        UpdateFrameLimiterMenu();
        SaveSettingsIfChanged();
    }

    void App::UpdateCdTimingMenu() {
        if (system_ != nullptr)
            TickCdTiming(window_, system_->config().cdrom_mechanical_timing);
    }

    void App::SetCdMechanicalTiming(bool on) {
        if (system_ == nullptr)
            return;
        system_->config().cdrom_mechanical_timing = on;
        UpdateCdTimingMenu();
        SaveSettingsIfChanged();
    }

    // ---------------------------------------------------------------------------------------------
    // The machine
    // ---------------------------------------------------------------------------------------------

    bool App::ResetMachine() {
        system_->Deinitialize();
        if (system_->Initialize(bios_path_.c_str()) != 0) {
            ShowError(window_, L"Failed to initialise the system (BIOS missing?).");
            return false;
        }
        system_->set_auto_boot(false);
        return true;
    }

    bool App::BootDiscFromFile(const std::string& path) {
        if (!ResetMachine())
            return false;
        // The disc has to be in the drive before the BIOS looks, or it finds an open shell and
        // stops at the menu.
        system_->EjectDisc();
        if (!system_->LoadDisc(path.c_str())) {
            ShowWarning(window_,
                        L"Could not read that disc image.\n\n"
                        L"Supported: .cue (with its .bin or .img), .mds (with its "
                        L".mdf), .bin, .img, .iso.");
            return false;
        }
        // Each disc gets its own pair of memory cards - a real console has none of this, of course,
        // but "which card was in when I saved" is otherwise a question the player has to answer by
        // hand.
        LoadOrCreateMemoryCardsForDisc(path);

        SetWindowTitleForPath(path);
        paused_ = false;
        return true;
    }

    void App::BootBios() {
        if (!ResetMachine())
            return;
        system_->EjectDisc();
        SetWindowTitleForPath(std::string());
        paused_ = false;
    }

    bool App::BootPsExeFromFile(const std::string& path) {
        if (!LooksLikePsExe(path)) {
            ShowWarning(window_,
                        L"Could not load that file as a PS-X EXE.\n\n"
                        L"It must be the executable itself - the header starts with "
                        L"the 8 bytes \"PS-X EXE\" - not a disc image or a Windows "
                        L"executable.");
            return false;
        }
        if (!ResetMachine())
            return false;
        // Nothing about a leftover disc should affect a test program that never asks the CD-ROM for
        // anything.
        system_->EjectDisc();
        system_->set_auto_boot_exe(true, path);
        SetWindowTitleForPath(path);
        paused_ = false;
        return true;
    }

    void App::LoadOrCreateMemoryCardsForDisc(const std::string& disc_path) {
        if (memcards_root_.empty() || system_ == nullptr)
            return;

        const std::string dir = memcards_root_ + "\\" + DiscIdentifier(disc_path);
        EnsureDirectory(dir);

        for (int slot = 0; slot < 2; ++slot) {
            const std::string path = dir + "\\card" + std::to_string(slot + 1) + ".mcr";
            if (system_->mc(slot).LoadFile(path.c_str()) == S_OK)
                continue;

            // LoadFile fails for two different reasons and only one is worth saying anything about:
            // no card there yet, which is the ordinary case for a game played for the first time,
            // or a file that exists but is not a valid 128 KB card, which CreateFile is about to
            // overwrite.
            FILE* existing = fopen(path.c_str(), "rb");
            const bool had_file = existing != nullptr;
            if (existing != nullptr)
                fclose(existing);

            if (system_->mc(slot).CreateFile(path.c_str()) != S_OK) {
                const std::wstring message =
                    L"Could not create a memory card for slot " + std::to_wstring(slot + 1) + L".";
                ShowWarning(window_, message.c_str());
            } else if (had_file) {
                const std::wstring message =
                    L"The memory card file for slot " + std::to_wstring(slot + 1) +
                    L" was not a valid 128 KB card and has been reset:\n\n" +
                    std::wstring(path.begin(), path.end());
                ShowWarning(window_, message.c_str());
            }
        }
    }

    std::string App::SaveStateSlotPath(int slot) const {
        const std::string disc_path =
            (system_ != nullptr) ? system_->cdrom().disc().path() : std::string();
        return psxemu::SaveStateSlotPath(savestates_root_, disc_path, slot);
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
        SetWindowTextW(window_, title_base_.c_str());
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
            case WM_SIZE:
                if (app != nullptr && app->graphics_ != nullptr && wparam != SIZE_MINIMIZED)
                    app->graphics_->Resize(LOWORD(lparam), HIWORD(lparam));
                return 0;

            case WM_COMMAND:
                // Every command needs the machine, and it does not exist until after the window
                // does.
                if (app != nullptr && app->system_ != nullptr)
                    app->OnCommand(LOWORD(wparam));
                return 0;

            case WM_KEYDOWN:
                if (app != nullptr)
                    app->OnKeyDown(wparam);
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
        if (key == VK_SPACE)
            paused_ = !paused_;
        //if (key == VK_ESCAPE)
        //  PostMessageW(window_, WM_CLOSE, 0, 0);
        // F1-F8: plain loads that slot, Ctrl+ saves it. Both also become the slot the Save
        // State/Load State menu items act on, so pressing F3 and then using the menu (or another
        // F-key) do not disagree about which slot is "current".
        if (key >= VK_F1 && key <= VK_F8) {
            const int slot = static_cast<int>(key - VK_F1) + 1;
            last_slot_ = slot;
            if (GetKeyState(VK_CONTROL) & 0x8000)
                pending_save_slot_ = slot;
            else
                pending_load_slot_ = slot;
        }
    }

    void App::OnCommand(int command) {
        switch (command) {
            case kCommandBootDisc: {
                // Switching the console on with a game in the drive. Pick an image, then the
                // machine starts from cold and the BIOS boots it.
                const std::string path =
                    ChooseFile(window_, FileDialog::kOpen, kDiscFilter, nullptr);
                if (!path.empty())
                    BootDiscFromFile(path);
                break;
            }

            case kCommandSwapDisc: {
                // Changing the disc in a running machine, for a game that asks for its second one.
                // No reset - that is what Boot disc is for.
                const std::string path =
                    ChooseFile(window_, FileDialog::kOpen, kDiscFilter, nullptr);
                if (path.empty())
                    break;
                if (!system_->LoadDisc(path.c_str())) {
                    ShowWarning(window_, L"Could not read that disc image.");
                    break;
                }
                SetWindowTitleForPath(path);
                break;
            }

            case kCommandEjectDisc:
                system_->EjectDisc();
                SetWindowTitleForPath(std::string());
                break;

            case kCommandBootBios:
                BootBios();
                break;

            case kCommandBootExe: {
                // A standalone test program or homebrew binary - no disc. The BIOS boots normally
                // first; see BootPsExeFromFile for why.
                const std::string path =
                    ChooseFile(window_, FileDialog::kOpen, kExeFilter, nullptr);
                if (!path.empty())
                    BootPsExeFromFile(path);
                break;
            }

            case kCommandOpenMemoryCardSlot1:
            case kCommandOpenMemoryCardSlot2: {
                const int slot = (command == kCommandOpenMemoryCardSlot1) ? 0 : 1;
                const std::string path = ChooseFile(window_, FileDialog::kOpen, kCardFilter, "mcr");
                if (path.empty())
                    break;
                if (system_->mc(slot).LoadFile(path.c_str()) != S_OK) {
                    ShowWarning(window_,
                                L"Could not open that memory card. It must be exactly "
                                L"128 KB.");
                }
                break;
            }

            case kCommandCreateMemoryCardSlot1:
            case kCommandCreateMemoryCardSlot2: {
                const int slot = (command == kCommandCreateMemoryCardSlot1) ? 0 : 1;
                const std::string path = ChooseFile(window_, FileDialog::kSave, kCardFilter, "mcr");
                if (path.empty())
                    break;
                if (system_->mc(slot).CreateFile(path.c_str()) != S_OK) {
                    ShowWarning(window_, L"Could not create that memory card file.");
                }
                break;
            }

            case kCommandReset:
                ResetMachine();
                break;

            case kCommandPause:
                paused_ = !paused_;
                break;

            case kCommandSaveState:
                pending_save_slot_ = last_slot_;
                break;

            case kCommandLoadState:
                pending_load_slot_ = last_slot_;
                break;

            case kCommandViewVram: {
                view_vram_ = !view_vram_;
                HMENU bar = GetMenu(window_);
                if (bar != nullptr) {
                    CheckMenuItem(bar, static_cast<UINT>(kCommandViewVram),
                                  MF_BYCOMMAND | (view_vram_ ? MF_CHECKED : MF_UNCHECKED));
                }
                break;
            }

            case kCommandFrameLimiter:
                if (system_ != nullptr)
                    SetFrameLimiter(!system_->config().frame_limiter);
                break;

            case kCommandCdMechanicalTiming:
                if (system_ != nullptr)
                    SetCdMechanicalTiming(!system_->config().cdrom_mechanical_timing);
                break;

            case kCommandExit:
                PostMessageW(window_, WM_CLOSE, 0, 0);
                break;

            default:
                // The volume steps are one contiguous run of command ids.
                if (command >= kCommandVolumeFirst &&
                    command < kCommandVolumeFirst + static_cast<int>(std::size(kVolumeSteps))) {
                    SetVolume(kVolumeSteps[command - kCommandVolumeFirst].value);
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
                    const int offset = command - kCommandControllerTypeFirst;
                    SetControllerType(offset / 3, kControllerTypeChoices[offset % 3].key);
                } else if (command >= kCommandInputSourceFirst &&
                           command <= kCommandInputSourceLast) {
                    const int offset = command - kCommandInputSourceFirst;
                    SetInputSource(offset / 3, kInputSourceChoices[offset % 3].key);
                }
                break;
        }
    }

}   // namespace psxemu
