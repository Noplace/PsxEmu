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

// The Win32 front end: a window, menus, settings - and four threads doing everything else.
//
// This class is the UI thread. It owns the window and the menus, keeps the settings, and answers
// messages; it does not emulate, draw or make a sound, and it never waits on a thread that does.
// Every menu command that changes the machine is posted to the machine's thread and applied there
// between frames; everything the threads have to say comes back as a posted message (PostToUi),
// never a sent one - see Docs/Threading-Plan.md's rules, and rule 2 in particular: a SendMessage
// from another thread is a wait on this one, and at shutdown that is a deadlock.
//
// Who owns what:
//
//   UI thread (this)        the window, menus, dialogs, psxemu.ini and `config_`
//   host::Machine           `system_` - all emulation - the frame limiter, save states
//   host::VideoOutput       the Direct3D engine, the swap chain, the filters
//   host::AudioOutput       the sound device
//   InputThread             the pads, the keyboard, the mouse
//
// The machine is single-threaded exactly as it was: only host::Machine's thread ever calls into
// System, which is why every baseline in Docs/Test-Suite.md still holds, and why host_test can
// check a threaded BIOS boot against boot_runner instruction for instruction.

#include "framework.h"

#include "const.h"
#include "engine_factory.h"
#include "host/audio_output.h"
#include "host/machine.h"
#include "host/video_output.h"
#include "input_thread.h"
#include "video_presenter.h"

#include <functional>

namespace psxemu {

    class App {
     public:
        App() = default;
        ~App();

        // Its address lives in the window's user data for as long as the window does, and in the
        // threads' hooks, so it cannot be moved or copied out from under either.
        App(const App&) = delete;
        App& operator=(const App&) = delete;

        // Brings the front end up and runs until the window closes. The process exit code; 1 if
        // anything needed to start was missing.
        int Run(HINSTANCE instance, int show_command);

     private:
        // ---------------------------------------------------------------------------------------
        // Startup and shutdown
        // ---------------------------------------------------------------------------------------

        bool Initialize(HINSTANCE instance, int show_command);
        bool CreateAppWindow(HINSTANCE instance);
        bool CreateMachine();

        // Starts input, audio, video and the machine, in that order - outputs before the thing
        // that feeds them.
        void StartThreads();

        // Stops them in the reverse order: the machine first, so nothing more is produced, then
        // video (which releases the swap chain before the window goes), then audio, then input.
        // Called from WM_CLOSE, before the window is destroyed.
        void StopThreads();

        // The settings the machine and the menus start from.
        void ApplySettings();

        void SetUpDataDirectories();
        std::string ResolveBiosPath(const std::string& from_command_line);

        // GetMessage and nothing else. The machine has its own thread; this one is free to answer
        // the window even while a frame is being run or presented.
        int MainLoop();

        // ---------------------------------------------------------------------------------------
        // Between threads
        // ---------------------------------------------------------------------------------------

        // Any thread: runs `work` on the UI thread. A posted message, never a sent one.
        void PostToUi(std::function<void()> work);

        // Hands the machine a request. Sugar for machine_.Post.
        void PostToMachine(std::function<void(emulation::host::Machine&)> request);

        // Copies the front end's settings onto the machine, where they take effect between frames.
        void SendConfigToMachine();

        // Called on the machine's thread, once a second: posted on to the UI, which shows it.
        void OnMachineReport(const emulation::host::MachineReport& report);

        // Called on the machine's thread before each frame: maps what the input thread last read
        // onto the ports the settings name, and hands the pads' motors back the other way. All of
        // it touches nothing but `system` and the machine-thread-only members below.
        void ApplyInput(emulation::psx::System& system,
                        const emulation::host::HostInput& input);

        // The window title: the disc's name, the speed, and - with Emulation > Show Timings - where
        // each frame's time went.
        void UpdateTitle();

        // ---------------------------------------------------------------------------------------
        // Menus and settings, all on the UI thread
        // ---------------------------------------------------------------------------------------

        // Writes only when something actually changed, which is what makes it safe to call on every
        // edit - and calling it on every edit is what stops a crash or a kill losing them.
        void SaveSettingsIfChanged();

        void SetVolume(float value);
        void SetFilter(const std::string& key);
        void SetRenderer(const std::string& key);
        void SetControllerType(int port, const std::string& key);
        void SetInputSource(int port, const std::string& key);
        void SetMultitapSource(int port, int player, const std::string& key);
        void SetFrameLimiter(bool on);
        void SetSpeed(float speed);
        void SetCdMechanicalTiming(bool on);
        void SetSkipBiosIntro(bool on);
        void SetRecompiler(bool on);
        void SetAudioBackend(const std::string& key);
        void SetPauseInMenus(bool on);
        void SetShowTimings(bool on);

        void RefreshBiosMenu();
        void SelectBios(int index);

        void UpdateVolumeMenu();
        void UpdateRendererMenu();
        void UpdateFilterMenu();
        void UpdateControllerTypeMenu();
        void UpdateInputSourceMenu();
        void UpdateMultitapSourceMenu();
        void UpdateFrameLimiterMenu();
        void UpdateSpeedMenu();
        void UpdateCdTimingMenu();
        void UpdateSkipBiosIntroMenu();
        void UpdateRecompilerMenu();
        void UpdateAudioBackendMenu();
        void UpdatePauseInMenusMenu();
        void UpdateShowTimingsMenu();

        // ---------------------------------------------------------------------------------------
        // The machine, asked for from here and done there
        // ---------------------------------------------------------------------------------------

        void BootDiscFromFile(const std::string& path);
        void BootBios();
        void BootPsExeFromFile(const std::string& path);
        void ResetMachine();
        void SetUserPaused(bool paused);

        // Gives the disc just mounted its own pair of memory cards, in
        // memcards_root\<disc>\card1.mcr and card2.mcr. Runs on the machine's thread, from the
        // boot paths - and on this one at startup, before the threads exist.
        void LoadOrCreateMemoryCardsForDisc(emulation::psx::System& system,
                                            const std::string& disc_path);

        // While a menu - or a dialog opened from one - is up. Pauses the machine only if
        // EmuConfig::pause_in_menus asks for it; counted, since a dialog can open over a menu.
        void EnterMenuPause();
        void LeaveMenuPause();

        // Scoped EnterMenuPause, for the modal dialogs this thread puts up itself.
        struct MenuPause {
            explicit MenuPause(App* app) : app_(app) { app_->EnterMenuPause(); }
            ~MenuPause() { app_->LeaveMenuPause(); }
            MenuPause(const MenuPause&) = delete;
            MenuPause& operator=(const MenuPause&) = delete;
            App* app_;
        };

        std::string SaveStateSlotPath(emulation::psx::System& system, int slot) const;
        void SetWindowTitleForPath(const std::string& path);

        // ---------------------------------------------------------------------------------------
        // Messages
        // ---------------------------------------------------------------------------------------

        static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
        static App* From(HWND window);

        void OnCommand(int command);
        void OnKeyDown(WPARAM key);

        // ---------------------------------------------------------------------------------------
        // What it owns
        // ---------------------------------------------------------------------------------------

        // Not owned - the window owns itself once created, and destroys itself on WM_DESTROY.
        HWND window_ = nullptr;

        // The emulated machine. Created here, then driven only by the machine's thread until
        // StopThreads has returned.
        std::unique_ptr<emulation::psx::System> system_;

        // The threads, and the channels between them. Declared after `system_`, so they are torn
        // down before it; each one stops its thread in its own destructor as a backstop, though
        // StopThreads has normally done it already.
        std::unique_ptr<emulation::host::AudioOutput> audio_;
        std::unique_ptr<emulation::host::VideoOutput> video_;
        std::unique_ptr<emulation::host::Machine> machine_;
        std::unique_ptr<InputThread> input_;

        // The front end's own copy of the settings: what the menus tick against and what
        // psxemu.ini is written from. The machine has a copy of its own, which this one is sent
        // to; reading the machine's would be reading what another thread writes.
        emulation::psx::EmuConfig config_;
        emulation::psx::SettingsFile settings_;

        // Whichever backends are actually open, as the threads that opened them reported. Not the
        // requested ones: opening can fall back.
        std::string current_backend_ = "d3d11";
        std::string current_audio_backend_;
        std::string current_filter_;

        // Video > View VRAM: the machine ships all of VRAM instead of the display area. Not
        // persisted - always starts off.
        bool view_vram_ = false;

        // The BIOS in use, as a full path, and the images the last scan found. bios_path_ is what
        // the *next* cold boot will use, which is not necessarily what the running machine was
        // built with.
        std::string bios_path_;
        std::vector<std::string> bios_files_;

        // Per-user data, under Documents\My Games\PSXEmu.
        std::string data_root_;
        std::string memcards_root_;
        std::string savestates_root_;
        std::string bios_root_;
        std::string settings_path_;

        // What the UI believes the machine's pause state is: its own, and how deep the menus are.
        // The machine has the last word on both, but the menu has to know what it is asking for.
        bool paused_by_user_ = true;
        int menu_depth_ = 0;

        // Set once WM_CLOSE has started shutting things down: menu commands stop being taken, so
        // nothing is posted to a thread that is being joined.
        bool stopping_ = false;

        // The slot F1-F8 last used, which the Save State/Load State menu items act on too.
        int last_slot_ = 1;

        // The title, and the last thing the machine said about itself.
        std::wstring title_base_ = kWindowTitle;
        emulation::host::MachineReport report_;
        bool have_report_ = false;
        double present_ms_ = 0.0;      // mean, from the video thread's own timing
        double presents_per_second_ = 0.0;

        // ---------------------------------------------------------------------------------------
        // The machine thread's own, touched in ApplyInput and in posted requests - never here
        // ---------------------------------------------------------------------------------------

        // Frames each port has left to sit empty before the controller just chosen for it is
        // plugged in - see SetControllerType. Zero is the steady state.
        std::array<int, 2> replug_frames_ = { 0, 0 };
        std::array<std::string, 2> plugged_type_ = { "", "" };
    };

}   // namespace psxemu
