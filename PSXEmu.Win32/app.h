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

// The Win32 front end: a window, a graphics engine, an audio device, a machine, and the loop that
// drives them.
//
// It owns no emulation. The core in PSXEmu.Core rasterises every pixel on the CPU and this only
// uploads the finished frame; the one thing that flows the other way is input, through the core's
// SIO device. Which graphics engine is running is not this class's business either - it holds an
// IGraphicsEngine and the Video menu swaps which one that is under a running machine.
//
// Everything in it is private and there is one public entry point, Run, because the invariants here
// only hold when the members are changed together: the filter menu ticks against the engine that is
// actually running rather than the one the settings file asked for, the settings file is rewritten
// whenever a setting changes rather than at exit, and the member declaration order below is what
// stops the machine being torn down after the device it is drawing to.
//
// It was a plain struct with public members and forty-five free functions taking it by reference,
// which is what public-by-default was actually for. None of those invariants could be stated, let
// alone kept, from the outside.

#include "framework.h"

#include "const.h"
#include "engine_factory.h"
#include "gamepad.h"
#include "mouse.h"

namespace psxemu {

    class App {
     public:
        App() = default;
        ~App();

        // Its address lives in the window's user data for as long as the window does, so it cannot
        // be moved or copied out from under the window procedure.
        App(const App&) = delete;
        App& operator=(const App&) = delete;

        // Brings the front end up and runs until the window closes. The process exit code; 1 if
        // anything needed to start was missing. Everything owned is released on the way out
        // whichever way it returns, which is what makes the failure paths safe - they used to
        // `return 1` from the middle of startup with the presenter and the audio device already up,
        // leaking both.
        int Run(HINSTANCE instance, int show_command);

     private:
        // ---------------------------------------------------------------------------------------
        // Startup
        // ---------------------------------------------------------------------------------------

        bool Initialize(HINSTANCE instance, int show_command);
        bool CreateAppWindow(HINSTANCE instance);
        bool CreateGraphics();
        bool CreateMachine();

        // The settings the machine holds, applied once it exists, plus every menu tick that follows
        // from them.
        void ApplySettings();

        // Resolves and creates the per-user directories, once, at startup. A failure here is silent
        // - the settings file still lives beside the executable and keeps working - because
        // refusing to run the emulator over a save-data folder is a worse failure than the one it
        // would be protecting against.
        void SetUpDataDirectories();

        // ---------------------------------------------------------------------------------------
        // The loop
        // ---------------------------------------------------------------------------------------

        int MainLoop();

        // Drains the queue. False once WM_QUIT has been seen, which is the only thing that ends the
        // loop.
        bool PumpMessages(MSG* message);

        // A save or load asked for during the last frame, actioned here between frames and never
        // mid-frame, per Docs/Save-States-Plan.md - the top of the loop is the one point nothing
        // about the current frame is half-done yet.
        void ApplyPendingStates();

        // Samples both pads and the keyboard once and hands the result to Sio.
        void PollInput();

        // Runs the machine until the GPU says a frame is finished - the same loop the headless
        // harness runs. Decides how much work an iteration does; it does *not* decide when the next
        // one starts. The wall clock belongs to LimitFrameRate; see bug 49 and
        // platform/frame_limiter.h.
        void RunOneFrame();

        // Drains whatever the SPU generated during that frame and hands it to the audio device.
        // Pulling here rather than pushing from inside the core is what keeps the core free of any
        // audio API: it just fills a buffer.
        void PumpAudio();

        // Uploads the finished frame - or the whole of VRAM, if Video > View VRAM is on - and
        // presents it.
        void PresentFrame();

        // What actually holds the machine to 59.29 Hz (49.76 in PAL). Last in the iteration, so the
        // wait absorbs whatever the rest of it did not take.
        void LimitFrameRate();

        // Appends "59.3 fps (100%)" to the window title once a second: emulated frames actually
        // produced per second of wall clock, and that as a percentage of what the emulated display
        // is producing them at.
        //
        // 100% is a console. Anything else is the front end running the machine at the wrong speed,
        // which is invisible without a number - a boot intro at 280% just looks like a short intro.
        void UpdateSpeedReadout();

        // ---------------------------------------------------------------------------------------
        // Settings
        // ---------------------------------------------------------------------------------------

        // Writes only when something actually changed, which is what makes it safe to call on every
        // edit - and calling it on every edit is what stops a crash or a kill losing them.
        void SaveSettingsIfChanged();

        void SetVolume(float value);
        void SetFilter(const std::string& key);

        // Live switch: tears down the active engine and brings up the other one against the same
        // window, restoring whichever filter was last saved for D3D12 if that is what it switched
        // to. CreateGraphicsEngine's own try-then-fallback already covers "the one just picked will
        // not initialise"; this only has to handle the (very unlikely, since the engine being
        // replaced was working moments ago) case where the fallback fails too.
        void SetRenderer(const std::string& key);

        void SetControllerType(int port, const std::string& key);
        void SetInputSource(int port, const std::string& key);

        // Which source feeds one player (0-3 = A-D) of whichever port's controller_type is
        // "multitap" - meaningless, and never read, otherwise.
        void SetMultitapSource(int port, int player, const std::string& key);

        void SetFrameLimiter(bool on);

        // Takes effect on the next command the drive is given, so there is nothing to reset and no
        // reason to make it a cold-boot-only choice - though a boot already past its logo screen
        // will not replay it.
        void SetCdMechanicalTiming(bool on);

        // This half of the tick functions in menu.h: each reads what is currently set and hands it
        // over. The machine is checked here because these are the call sites that know whether
        // there is one yet.
        void UpdateVolumeMenu();
        void UpdateRendererMenu();
        void UpdateFilterMenu();
        void UpdateControllerTypeMenu();
        void UpdateInputSourceMenu();
        void UpdateMultitapSourceMenu();
        void UpdateFrameLimiterMenu();
        void UpdateCdTimingMenu();

        // ---------------------------------------------------------------------------------------
        // The machine
        // ---------------------------------------------------------------------------------------

        // Cold boot: the machine comes back in the state it has at power-on. Three menu commands
        // need this and each used to carry its own copy.
        bool ResetMachine();

        // Puts a disc in the drive and starts the machine from cold, which is what switching a
        // console on with a game in it does: the BIOS runs its intro, checks the disc, reads
        // SYSTEM.CNF, loads the executable it names and jumps to it. Nothing here understands the
        // disc - the BIOS does all of it.
        bool BootDiscFromFile(const std::string& path);

        // Starts with an empty drive, which lands in the BIOS shell.
        void BootBios();

        // Boots through the BIOS for real, the same as switching the console on with an empty
        // drive, and only once it reaches the address it would hand a game control at does the
        // executable get side-loaded on top. Letting the BIOS run first is what a raw side-load
        // skips: clearing BEV and Isolate Cache, and setting up the default video mode, both of
        // which a standalone test program can depend on having happened, the same way it could on
        // real hardware.
        bool BootPsExeFromFile(const std::string& path);

        // Gives the disc just mounted its own pair of memory cards, in
        // memcards_root\<disc>\card1.mcr and card2.mcr - created the first time a disc is played
        // and loaded on every boot after that.
        //
        // Called only from a cold boot. Swapping a disc mid-session leaves the cards alone, which
        // is what real hardware does: the memory card slots have nothing to do with the disc drive,
        // and disconnecting one under a running game mid-swap would be a save silently vanishing
        // from under a game that thinks its card is still there.
        void LoadOrCreateMemoryCardsForDisc(const std::string& disc_path);

        // Supplies the two things only this knows - where states are kept, and which disc is in the
        // drive - to the path builder in win32_paths.h.
        std::string SaveStateSlotPath(int slot) const;

        // Titles the window after whatever is loaded - a disc image, a bare PS-EXE, or nothing (the
        // BIOS shell with an empty drive).
        //
        // Records the name in title_base_ rather than setting the window text directly, because the
        // speed readout is appended to it once a second and would otherwise be wiped by every disc
        // change.
        void SetWindowTitleForPath(const std::string& path);

        // ---------------------------------------------------------------------------------------
        // Messages
        // ---------------------------------------------------------------------------------------

        // The application pointer arrives with the window and lives in its user data, which is what
        // a global used to do less safely. Messages sent during CreateWindowExW itself can land
        // before that is set, so every use is guarded.
        static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
        static App* From(HWND window);

        void OnCommand(int command);
        void OnKeyDown(WPARAM key);

        // ---------------------------------------------------------------------------------------
        // What it owns
        // ---------------------------------------------------------------------------------------
        //
        // In the order it has to be torn down in: members are destroyed in reverse, so the machine
        // stops before the audio device goes away and both go before the Direct3D device.

        // Not owned - the window owns itself once created, and destroys itself on WM_DESTROY.
        HWND window_ = nullptr;

        // Whichever backend is actually active right now - not necessarily the same as the
        // persisted `graphics_backend` preference, since creating the preferred one can fall back
        // to the other. The Video menu ticks against this, not against the config.
        std::unique_ptr<IGraphicsEngine> graphics_;
        std::string current_backend_ = "d3d12";
        std::string current_filter_;   // ditto, for the filter menu

        // Video > View VRAM: shows the whole 1024x512 VRAM instead of the display area, for chasing
        // texture/CLUT corruption that the normal view only shows the symptom of. Not persisted -
        // always starts off.
        bool view_vram_ = false;
        std::vector<uint32_t> vram_view_scratch_;

        std::unique_ptr<IAudioEngine> audio_;
        std::unique_ptr<emulation::psx::System> system_;

        std::string bios_path_;

        // User settings, and where they are kept. Written as they are changed rather than only at
        // exit, so a crash or a kill does not lose them.
        emulation::psx::SettingsFile settings_;

        // Per-user data, under Documents\My Games\PSXEmu - the same convention GBAEmu uses, so both
        // live in the one place a person would look for either. Empty if Documents could not be
        // resolved, which callers treat as "skip this rather than fail the boot".
        std::string data_root_;
        std::string memcards_root_;   // data_root_\memcards
        std::string savestates_root_;   // data_root_\savestates
        std::string settings_path_;
        bool running_ = false;
        bool paused_ = true;

        // A save or load requested this frame, actioned once at the top of the next frame. -1 means
        // nothing pending. The generic Save State/Load State menu items act on last_slot_, which
        // F1-F8 also update, so the two stay in step.
        int pending_save_slot_ = -1;
        int pending_load_slot_ = -1;
        int last_slot_ = 1;   // matches F1, the first of the eight slots

        // Scratch for one frame of audio, sized for the worst case at 30 fps. A member rather than
        // a function-local static so there is one per application rather than one per process.
        std::array<int16_t, emulation::psx::Spu::kSampleRate / 30 * 2> audio_scratch_ = {};

        // Speed. title_base_ is what the window would be called with no readout on it - kept so the
        // readout can be re-appended without re-deriving the name from the disc path every time it
        // updates.
        //
        // Nothing in this project had ever measured wall-clock speed before this (Docs/Gaps.md said
        // so, and said it mattered more than it sounded), so "the intro plays too fast" had no
        // number attached to it and no way to tell a fix from a placebo.
        std::wstring title_base_ = kWindowTitle;
        uint64_t speed_frames_ = 0;   // emulated frames since the last update
        std::chrono::steady_clock::time_point speed_since_ = std::chrono::steady_clock::now();

        // Without this the loop runs at whatever blocks first - the monitor's refresh rate, or the
        // sound device - see platform/frame_limiter.h.
        utilities::FrameLimiter frame_limiter_;

        // Four fixed XInput slots - "Gamepad 1".."Gamepad 4" in the Input menu, XInput user index
        // 0-3 respectively - the most XInput itself ever supports, which is why there are exactly
        // four and not some other number. Which PSX port (or, for a Multitap, which of its four
        // players) each one feeds, and whether a source uses a gamepad at all rather than the
        // keyboard, is decided by EmuConfig::input_source/multitap_player_source and applied each
        // frame - see PollInput. Two, not four, were ever needed before Multitap existed, since only
        // two ports exist to assign one to each of.
        std::array<Gamepad, 4> gamepads_{ Gamepad(0), Gamepad(1), Gamepad(2), Gamepad(3) };

        // The real mouse. Unlike gamepads_, there is one of these regardless of how many ports use
        // it - a port's controller_type says whether it is Sio::kMouse at all, not which of several
        // physical mice to read, since there is only ever the one. Registered for raw input once, in
        // CreateAppWindow; fed to Sio from PollInput.
        Mouse mouse_;
    };

}   // namespace psxemu
