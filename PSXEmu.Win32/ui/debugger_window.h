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

// Emulation > Debugger: the CPU - disassembly around the pc, the registers, the breakpoints, and
// the controls that step and continue (phase 1 of Docs/Debugger-Plan.md) - and memory, viewed and
// edited, with registers edited too (phase 2).
//
// Phase 4 put tabs under the listing: the memory pane, the BIOS call log (and breaking on a
// particular call), the approximate call stack, and the devices' state; and labels, named in the
// toolbar and loaded from or saved to a text file.
//
// Watchpoints (phase 3) stop the machine after a read or write of a range, by the CPU or by a DMA
// channel; the halt names which, and the listing and the memory pane go to the two places that
// matter - the instruction that made the access, and the address it touched.
//
// The memory pane reads through Debugger::PeekData, so a hardware register whose read has a side
// effect shows as ?? rather than being read. While the machine runs, the memory pane refreshes
// twice a second - watching a value change is what a memory view is for - but the registers do
// not.
//
// The debugger is the machine thread's (psx/debugger.h), so this window never touches it. It is
// shown snapshots - Debugger::Snapshot, taken on the machine thread when it halts and whenever
// this window asks - and every action is a request run there through Host::request. The memory
// card editor works the same way.
//
// While the machine runs, the registers keep showing the last halt, greyed: a register view
// changing sixty times a second would be noise. The disassembly and the breakpoint list are
// refreshed on every request, running or not.

#include "app/framework.h"

#include "psx/debugger.h"

#include <commctrl.h>

#include <functional>
#include <string>
#include <vector>

namespace psxemu {

    class DebuggerWindow {
     public:
        typedef emulation::psx::Debugger Debugger;
        typedef std::function<void(Debugger&)> Change;

        // `center` values for Host::request that are not addresses - an address is always
        // word-aligned, so odd numbers are free.
        static const uint32_t kNoSnapshot = 1;   // do not send one back
        static const uint32_t kAtPc = 3;         // send one back, around the pc

        // Instructions in each snapshot's disassembly.
        static const int kLines = 256;

        // The memory pane: 32 rows of 16 bytes.
        static const int kMemoryRow = 16;
        static const int kMemoryBytes = 512;

        struct Host {
            // On the machine's thread: runs `change` (if any) against the debugger, then - unless
            // `center` is kNoSnapshot - sends a snapshot around `center` back to SetSnapshot.
            std::function<void(Change change, uint32_t center)> request;
            // On the machine's thread: writes `bytes` at `address` (Debugger::WriteMemory), says
            // why not if it refuses, and sends a snapshot around `center` either way.
            std::function<void(uint32_t address, std::vector<uint8_t> bytes, uint32_t center)>
                write_memory;
            // The window was closed.
            std::function<void()> on_closed;
        };

        DebuggerWindow() = default;
        ~DebuggerWindow();

        DebuggerWindow(const DebuggerWindow&) = delete;
        DebuggerWindow& operator=(const DebuggerWindow&) = delete;

        bool Create(HINSTANCE instance, HWND owner, Host host);
        void Show(bool on);
        bool visible() const;
        HWND window() const { return window_; }

        // A snapshot from the machine thread. `from_halt`: the machine has just halted, so the
        // listing follows the pc and the window comes forward.
        void SetSnapshot(const Debugger::Snapshot& snapshot, bool from_halt);

        // For the message loop, before TranslateMessage: the debugger's keys (F5, F9, F10, F11 and
        // the rest) while this window or anything in it has the focus. True if it was one.
        bool PreTranslate(const MSG& message);

     private:
        enum Control {
            kContinue, kBreak, kStepInto, kStepOver, kStepOut, kRunToCursor,
            kGoTo, kGoToPc, kAddBreakpoint, kRemoveBreakpoint, kRemoveAll,
            kMemoryView, kMemoryPrevious, kMemoryNext, kMemoryWrite, kSetRegister,
            kAddWatchpoint, kRemoveWatchpoint,
            kName, kLabels, kClearBiosBreaks,
            kControlCount,
        };

        // The tabs under the listing (phase 4 added all but the first).
        enum Tab { kTabMemory, kTabBios, kTabStack, kTabDevices, kTabCount };

        static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam,
                                           LPARAM lparam);
        LRESULT OnNotify(NMHDR* header);
        LRESULT CustomDrawCode(NMLVCUSTOMDRAW* draw);
        LRESULT CustomDrawRegisters(NMLVCUSTOMDRAW* draw);
        LRESULT CustomDrawMemory(NMLVCUSTOMDRAW* draw);
        void Layout(int width, int height);
        void OnControl(Control control);
        void ShowContextMenu(int x, int y);
        void ShowRegisterMenu(int x, int y);
        void ShowMemoryMenu(int x, int y);

        void FillCode();
        void FillRegisters();
        void FillBreakpoints();
        void FillWatchpoints();
        void FillBiosLog();
        void FillCallStack();
        void FillDevices();
        void ShowTab(int tab);
        void ShowBiosMenu(int x, int y);
        void ShowLabelsMenu();
        void LoadLabels();
        void SaveLabels();
        // What tripped the last watchpoint, in words: "the instruction at 80012340 wrote ...".
        std::wstring DescribeWatchHit() const;
        void FillMemoryPane(uint32_t old_address, const std::vector<uint8_t>& old_bytes);

        // The memory pane at a new address: the window's own record of where it is, and the
        // debugger's (which the snapshot a halt sends reads).
        void ViewMemory(uint32_t address);
        // The listing's centre, for a request that should not move it.
        uint32_t ListingCenter() const;
        // A register's row in the list (0-31, then hi, lo, pc) - or -1 for one that cannot be
        // edited.
        int EditableRegister(int row) const;
        void EditSelectedRegister();
        void EditMemoryRow(int row);
        void UpdateControls();
        void UpdateStatus();

        // Asks the machine for a listing around `address`, and selects that address when it comes.
        void GoTo(uint32_t address);
        void Request(Change change, uint32_t center);
        // Runs the machine on: the step or continue is posted, and the window greys if no halt
        // comes back quickly - so a single step does not flicker.
        void RunOn(Change change);
        void ToggleBreakpoint(uint32_t address);
        bool HasBreakpoint(uint32_t address, bool* enabled) const;
        bool SelectedLine(uint32_t* address) const;
        bool ReadAddress(uint32_t* address) const;

        HWND window_ = nullptr;
        HWND controls_[kControlCount] = {};
        HWND address_ = nullptr;
        HWND status_ = nullptr;
        HWND code_ = nullptr;
        HWND registers_ = nullptr;
        HWND breakpoint_list_ = nullptr;
        HWND code_label_ = nullptr;
        HWND registers_label_ = nullptr;
        HWND breakpoints_label_ = nullptr;
        HWND memory_ = nullptr;
        HWND memory_label_ = nullptr;
        HWND memory_address_ = nullptr;
        HWND memory_bytes_ = nullptr;
        HWND register_value_ = nullptr;
        HWND watch_list_ = nullptr;
        HWND watchpoints_label_ = nullptr;
        HWND watch_address_ = nullptr;
        HWND watch_length_ = nullptr;
        HWND watch_kind_ = nullptr;   // a combo box: writes, reads, either
        HWND label_edit_ = nullptr;
        HWND tab_ = nullptr;
        HWND bios_list_ = nullptr;
        HWND bios_note_ = nullptr;
        HWND stack_track_ = nullptr;  // a check box: track calls
        HWND stack_list_ = nullptr;
        HWND devices_list_ = nullptr;
        int tab_index_ = kTabMemory;
        uint64_t shown_bios_calls_ = ~0ull;   // the call count the BIOS list was last filled at
        HFONT font_ = nullptr;
        HFONT mono_ = nullptr;
        Host host_;

        Debugger::Snapshot snapshot_;      // the latest - disassembly and breakpoints
        Debugger::Snapshot halt_;          // the latest taken while halted - the registers
        bool have_snapshot_ = false;
        bool have_halt_ = false;
        uint32_t changed_ = 0;             // registers that changed between the last two halts
        uint32_t changed_extra_ = 0;       // hi, lo and the rest, by row past the 32
        bool running_ = true;              // greyed: the machine has been let go
        bool waiting_ = false;             // a step or continue is posted; no halt back yet
        bool break_requested_ = false;
        uint32_t select_ = kNoSnapshot;    // the address to select when the next listing arrives
        bool filling_ = false;             // list changes made here, not by the person

        uint32_t memory_view_ = 0x80000000;   // the memory pane's first address
        uint32_t memory_changed_rows_ = 0;    // rows whose bytes the last snapshot changed
    };

}   // namespace psxemu
