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
#include "debugger_window.h"

#include "psx/disasm.h"

#include <cwchar>
#include <string>

#pragma comment(lib, "comctl32.lib")

namespace psxemu {

    namespace {

        const wchar_t kDebuggerClass[] = L"PSXEmuDebugger";

        const int kIdControlBase = 100;   // + DebuggerWindow::Control
        const int kIdAddress = 200;
        const int kIdCode = 201;
        const int kIdRegisters = 202;
        const int kIdBreakpoints = 203;
        const int kIdMemory = 204;
        const int kIdMemoryAddress = 205;
        const int kIdMemoryBytes = 206;
        const int kIdRegisterValue = 207;

        const UINT_PTR kGreyTimer = 1;
        const UINT kGreyMs = 150;
        const UINT_PTR kLiveTimer = 2;   // the memory pane, while the machine runs
        const UINT kLiveMs = 500;

        // The context menus' commands - returned by TrackPopupMenu, never posted.
        enum MenuCommand {
            kMenuToggle = 1, kMenuRunTo, kMenuFollow, kMenuGoToPc,
            kMenuShowMemory, kMenuShowCode, kMenuEdit,
            kMenuFollowWord = 20,   // + word index 0-3, into memory; + 4-7, into the listing
        };

        const int kExtraRegisters = 7;    // hi, lo, pc, sr, cause, epc, badvaddr
        const int kRowHi = 32, kRowLo = 33, kRowPc = 34, kRowSr = 35, kRowCause = 36,
                  kRowEpc = 37, kRowBadVaddr = 38;

        const wchar_t* const kControlLabels[] = {
            L"Continue (F5)", L"Break", L"Step Into (F11)", L"Step Over (F10)",
            L"Step Out (Shift+F11)", L"Run to Cursor", L"Go To", L"PC", L"Toggle Breakpoint",
            L"Remove", L"Remove All", L"View", L"< Prev", L"Next >", L"Write", L"Set",
        };

        bool ParseHex(const wchar_t* text, uint32_t* value) {
            while (*text == L' ')
                ++text;
            if (text[0] == L'0' && (text[1] == L'x' || text[1] == L'X'))
                text += 2;
            wchar_t* end = nullptr;
            const unsigned long parsed = wcstoul(text, &end, 16);
            while (end != nullptr && *end == L' ')
                ++end;
            if (text[0] == 0 || end == nullptr || *end != 0)
                return false;
            *value = static_cast<uint32_t>(parsed);
            return true;
        }

        // "DE AD BE EF" or "DEADBEEF": bytes in memory order. False on anything else.
        bool ParseBytes(const wchar_t* text, std::vector<uint8_t>* bytes) {
            bytes->clear();
            int high = -1;
            for (const wchar_t* at = text; *at != 0; ++at) {
                int digit = -1;
                if (*at >= L'0' && *at <= L'9') digit = *at - L'0';
                else if (*at >= L'A' && *at <= L'F') digit = *at - L'A' + 10;
                else if (*at >= L'a' && *at <= L'f') digit = *at - L'a' + 10;
                else if (*at == L' ' || *at == L',') {
                    if (high >= 0)
                        return false;   // an odd digit out: "1 2" is not two bytes
                    continue;
                } else {
                    return false;
                }
                if (high < 0) {
                    high = digit;
                } else {
                    bytes->push_back(static_cast<uint8_t>(high * 16 + digit));
                    high = -1;
                }
            }
            return high < 0 && !bytes->empty();
        }

        uint32_t Physical(uint32_t address) { return address & 0x1FFFFFFF; }

        std::wstring Hex(uint32_t value) {
            wchar_t text[16];
            swprintf_s(text, L"%08X", value);
            return text;
        }

        std::wstring Widen(const std::string& text) { return std::wstring(text.begin(), text.end()); }

        const wchar_t* ReasonText(DebuggerWindow::Debugger::HaltReason reason) {
            typedef DebuggerWindow::Debugger::HaltReason Reason;
            switch (reason) {
                case Reason::kBreakpoint: return L"breakpoint";
                case Reason::kStep:       return L"step";
                case Reason::kRunTo:      return L"run to cursor";
                case Reason::kRequested:  return L"break";
                default:                  return L"";
            }
        }

        const wchar_t* ExceptionName(uint32_t code) {
            switch (code) {
                case 0:  return L"Int";
                case 4:  return L"AdEL";
                case 5:  return L"AdES";
                case 6:  return L"IBE";
                case 7:  return L"DBE";
                case 8:  return L"Syscall";
                case 9:  return L"Bp";
                case 10: return L"RI";
                case 11: return L"CpU";
                case 12: return L"Ov";
                default: return L"?";
            }
        }

        void SetText(HWND list, int row, int column, const std::wstring& text) {
            ListView_SetItemText(list, row, column, const_cast<wchar_t*>(text.c_str()));
        }

        void AddColumn(HWND list, int index, const wchar_t* title, int width) {
            LVCOLUMNW column = {};
            column.mask = LVCF_TEXT | LVCF_WIDTH;
            column.pszText = const_cast<wchar_t*>(title);
            column.cx = width;
            ListView_InsertColumn(list, index, &column);
        }

        void InsertRow(HWND list, int row, const std::wstring& first, LPARAM param) {
            LVITEMW item = {};
            item.mask = LVIF_TEXT | LVIF_PARAM;
            item.iItem = row;
            item.pszText = const_cast<wchar_t*>(first.c_str());
            item.lParam = param;
            ListView_InsertItem(list, &item);
        }

        // Puts `row` at the top of the list's view.
        void ScrollToTop(HWND list, int row) {
            RECT rect = {};
            if (!ListView_GetItemRect(list, 0, &rect, LVIR_BOUNDS))
                return;
            const int height = rect.bottom - rect.top;
            ListView_Scroll(list, 0, (row - ListView_GetTopIndex(list)) * height);
        }

    }   // namespace

    DebuggerWindow::~DebuggerWindow() {
        if (window_ != nullptr && IsWindow(window_))
            DestroyWindow(window_);
        if (font_ != nullptr)
            DeleteObject(font_);
        if (mono_ != nullptr)
            DeleteObject(mono_);
    }

    bool DebuggerWindow::Create(HINSTANCE instance, HWND owner, Host host) {
        host_ = std::move(host);

        INITCOMMONCONTROLSEX common = { sizeof(common), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES };
        InitCommonControlsEx(&common);

        WNDCLASSEXW window_class = {};
        window_class.cbSize = sizeof(window_class);
        if (!GetClassInfoExW(instance, kDebuggerClass, &window_class)) {
            window_class = {};
            window_class.cbSize = sizeof(window_class);
            window_class.lpfnWndProc = WindowProc;
            window_class.hInstance = instance;
            window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
            window_class.lpszClassName = kDebuggerClass;
            if (RegisterClassExW(&window_class) == 0)
                return false;
        }

        window_ = CreateWindowExW(0, kDebuggerClass, L"PSXEmu - Debugger", WS_OVERLAPPEDWINDOW,
                                  CW_USEDEFAULT, CW_USEDEFAULT, 1100, 860, owner, nullptr,
                                  instance, this);
        if (window_ == nullptr)
            return false;

        NONCLIENTMETRICSW metrics = { sizeof(metrics) };
        if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0))
            font_ = CreateFontIndirectW(&metrics.lfMessageFont);
        mono_ = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                            FIXED_PITCH | FF_MODERN, L"Consolas");

        auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id,
                        DWORD ex_style = 0) {
            HWND control = CreateWindowExW(ex_style, cls, text, WS_CHILD | WS_VISIBLE | style,
                                           0, 0, 0, 0, window_,
                                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                           instance, nullptr);
            if (control != nullptr && font_ != nullptr)
                SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font_), FALSE);
            return control;
        };
        auto make_list = [&](int id, DWORD ex) {
            HWND list = make(WC_LISTVIEWW, L"",
                             LVS_REPORT | LVS_SHOWSELALWAYS | LVS_SINGLESEL | WS_BORDER | WS_TABSTOP,
                             id);
            ListView_SetExtendedListViewStyle(list,
                                              LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | ex);
            if (mono_ != nullptr)
                SendMessageW(list, WM_SETFONT, reinterpret_cast<WPARAM>(mono_), FALSE);
            return list;
        };

        for (int c = 0; c < kControlCount; ++c) {
            controls_[c] = make(L"BUTTON", kControlLabels[c], BS_PUSHBUTTON | WS_TABSTOP,
                                kIdControlBase + c);
        }
        address_ = make(L"EDIT", L"", ES_AUTOHSCROLL | ES_UPPERCASE | WS_TABSTOP, kIdAddress,
                        WS_EX_CLIENTEDGE);
        SendMessageW(address_, EM_SETLIMITTEXT, 8, 0);
        SendMessageW(address_, EM_SETCUEBANNER, TRUE,
                     reinterpret_cast<LPARAM>(L"address (hex)"));
        status_ = make(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, -1);
        code_label_ = make(L"STATIC", L"Disassembly", SS_LEFT, -1);
        registers_label_ = make(L"STATIC", L"Registers", SS_LEFT, -1);
        breakpoints_label_ = make(L"STATIC", L"Breakpoints", SS_LEFT, -1);

        code_ = make_list(kIdCode, 0);
        AddColumn(code_, 0, L"", 44);
        AddColumn(code_, 1, L"Address", 84);
        AddColumn(code_, 2, L"Word", 84);
        AddColumn(code_, 3, L"Instruction", 320);
        AddColumn(code_, 4, L"", 90);

        registers_ = make_list(kIdRegisters, 0);
        AddColumn(registers_, 0, L"Register", 74);
        AddColumn(registers_, 1, L"Value", 84);
        AddColumn(registers_, 2, L"", 180);
        {
            const wchar_t* extra[kExtraRegisters] = {
                L"hi", L"lo", L"pc", L"sr", L"cause", L"epc", L"badvaddr",
            };
            for (int r = 0; r < 32 + kExtraRegisters; ++r) {
                const std::wstring name =
                    r < 32 ? Widen(emulation::psx::RegisterName(static_cast<uint32_t>(r)))
                           : std::wstring(extra[r - 32]);
                InsertRow(registers_, r, name, r);
            }
        }

        breakpoint_list_ = make_list(kIdBreakpoints, LVS_EX_CHECKBOXES);
        AddColumn(breakpoint_list_, 0, L"Address", 110);
        AddColumn(breakpoint_list_, 1, L"Hits", 110);

        memory_label_ = make(L"STATIC",
                             L"Memory   (?? = not read: a register whose read would change "
                             L"something, or nothing there)",
                             SS_LEFT | SS_ENDELLIPSIS, -1);
        memory_ = make_list(kIdMemory, 0);
        AddColumn(memory_, 0, L"Address", 84);
        AddColumn(memory_, 1, L"00 01 02 03 04 05 06 07  08 09 0A 0B 0C 0D 0E 0F", 400);
        AddColumn(memory_, 2, L"ASCII", 150);
        for (int r = 0; r < kMemoryBytes / kMemoryRow; ++r)
            InsertRow(memory_, r, L"", r);
        memory_address_ = make(L"EDIT", L"80000000", ES_AUTOHSCROLL | ES_UPPERCASE | WS_TABSTOP,
                               kIdMemoryAddress, WS_EX_CLIENTEDGE);
        SendMessageW(memory_address_, EM_SETLIMITTEXT, 8, 0);
        memory_bytes_ = make(L"EDIT", L"", ES_AUTOHSCROLL | ES_UPPERCASE | WS_TABSTOP,
                             kIdMemoryBytes, WS_EX_CLIENTEDGE);
        SendMessageW(memory_bytes_, EM_SETCUEBANNER, TRUE,
                     reinterpret_cast<LPARAM>(L"bytes to write at the address, in memory order"));
        register_value_ = make(L"EDIT", L"", ES_AUTOHSCROLL | ES_UPPERCASE | WS_TABSTOP,
                               kIdRegisterValue, WS_EX_CLIENTEDGE);
        SendMessageW(register_value_, EM_SETLIMITTEXT, 10, 0);
        SendMessageW(register_value_, EM_SETCUEBANNER, TRUE,
                     reinterpret_cast<LPARAM>(L"new value (hex)"));
        for (HWND edit : { memory_address_, memory_bytes_, register_value_ }) {
            if (mono_ != nullptr)
                SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(mono_), FALSE);
        }

        RECT client = {};
        GetClientRect(window_, &client);
        Layout(client.right, client.bottom);
        UpdateControls();
        UpdateStatus();
        return true;
    }

    void DebuggerWindow::Show(bool on) {
        if (window_ == nullptr)
            return;
        if (on) {
            const bool was_visible = visible();
            ShowWindow(window_, SW_SHOW);
            SetForegroundWindow(window_);
            // Opened: the listing around wherever it was, or the pc the first time, and the memory
            // pane where it was.
            if (!was_visible) {
                const uint32_t view = memory_view_;
                Request([view](Debugger& debugger) { debugger.SetMemoryView(view, kMemoryBytes); },
                        ListingCenter());
            }
            SetTimer(window_, kLiveTimer, kLiveMs, nullptr);
        } else {
            KillTimer(window_, kLiveTimer);
            ShowWindow(window_, SW_HIDE);
        }
    }

    bool DebuggerWindow::visible() const {
        return window_ != nullptr && IsWindowVisible(window_);
    }

    // ---------------------------------------------------------------------------------------------
    // Snapshots in
    // ---------------------------------------------------------------------------------------------

    void DebuggerWindow::SetSnapshot(const Debugger::Snapshot& snapshot, bool from_halt) {
        if (window_ == nullptr || snapshot.lines.empty())
            return;
        // What the listing and the memory pane showed, to tell what this snapshot changes.
        bool same_listing = have_snapshot_ && snapshot.pc == snapshot_.pc &&
                            snapshot.halted == snapshot_.halted &&
                            snapshot.lines.size() == snapshot_.lines.size() &&
                            snapshot.breakpoints.size() == snapshot_.breakpoints.size();
        for (size_t i = 0; same_listing && i < snapshot.lines.size(); ++i) {
            same_listing = snapshot.lines[i].address == snapshot_.lines[i].address &&
                           snapshot.lines[i].word == snapshot_.lines[i].word &&
                           snapshot.lines[i].readable == snapshot_.lines[i].readable;
        }
        for (size_t i = 0; same_listing && i < snapshot.breakpoints.size(); ++i) {
            same_listing = snapshot.breakpoints[i].address == snapshot_.breakpoints[i].address &&
                           snapshot.breakpoints[i].enabled == snapshot_.breakpoints[i].enabled;
        }
        const uint32_t old_memory_address = snapshot_.memory_address;
        const std::vector<uint8_t> old_memory = snapshot_.memory;

        snapshot_ = snapshot;
        have_snapshot_ = true;

        if (snapshot.halted) {
            // Which registers this halt changed, against the last one - the red in the list. Only
            // for a new halt: a snapshot asked for while still halted (a breakpoint toggled) is
            // the same machine state, and would clear them all.
            if (from_halt && !have_halt_) {
                changed_ = 0;
                changed_extra_ = 0;
            }
            if (from_halt && have_halt_) {
                changed_ = 0;
                changed_extra_ = 0;
                for (int r = 0; r < 32; ++r) {
                    if (snapshot.gpr[r] != halt_.gpr[r])
                        changed_ |= 1u << r;
                }
                const uint32_t now[kExtraRegisters] = { snapshot.hi, snapshot.lo, snapshot.pc,
                                                        snapshot.sr, snapshot.cause, snapshot.epc,
                                                        snapshot.badvaddr };
                const uint32_t then[kExtraRegisters] = { halt_.hi, halt_.lo, halt_.pc, halt_.sr,
                                                         halt_.cause, halt_.epc, halt_.badvaddr };
                for (int r = 0; r < kExtraRegisters; ++r) {
                    if (now[r] != then[r])
                        changed_extra_ |= 1u << r;
                }
            }
            halt_ = snapshot;
            have_halt_ = true;
            running_ = false;
            if (from_halt) {
                waiting_ = false;
                break_requested_ = false;
                KillTimer(window_, kGreyTimer);
                if (select_ == kNoSnapshot)
                    select_ = snapshot.pc;
            }
        } else if (!waiting_) {
            running_ = true;
        }

        // The live refresh asks twice a second; a listing that has not changed is left alone, so
        // the selection and the scroll position are not disturbed under the person using them.
        if (!same_listing || select_ != kNoSnapshot)
            FillCode();
        FillRegisters();
        FillBreakpoints();
        FillMemoryPane(old_memory_address, old_memory);
        UpdateControls();
        UpdateStatus();

        if (from_halt) {
            if (!visible()) {
                ShowWindow(window_, SW_SHOW);
                SetTimer(window_, kLiveTimer, kLiveMs, nullptr);
            }
            SetForegroundWindow(window_);
            SetFocus(code_);
        }
    }

    void DebuggerWindow::FillCode() {
        const std::vector<Debugger::Line>& lines = snapshot_.lines;
        const int count = static_cast<int>(lines.size());

        // Keep the view where it was across a refresh, by the address at its top.
        const int old_top = ListView_GetTopIndex(code_);
        uint32_t top_address = kNoSnapshot;
        {
            wchar_t text[16] = {};
            ListView_GetItemText(code_, old_top, 1, text, 16);
            if (text[0] != 0)
                top_address = static_cast<uint32_t>(wcstoul(text, nullptr, 16));
        }
        uint32_t selected = kNoSnapshot;
        SelectedLine(&selected);

        filling_ = true;
        SendMessageW(code_, WM_SETREDRAW, FALSE, 0);
        ListView_DeleteAllItems(code_);
        for (int i = 0; i < count; ++i) {
            const Debugger::Line& line = lines[static_cast<size_t>(i)];
            bool enabled = false;
            const bool breakpoint = HasBreakpoint(line.address, &enabled);
            const bool at_pc = line.address == snapshot_.pc;
            std::wstring marker;
            marker += breakpoint ? (enabled ? L"\x25CF" : L"\x25CB") : L" ";
            marker += at_pc ? L" \x25BA" : L"";
            InsertRow(code_, i, marker, i);
            SetText(code_, i, 1, Hex(line.address));
            if (line.readable) {
                SetText(code_, i, 2, Hex(line.word));
                SetText(code_, i, 3, Widen(line.text));
                if (line.delay_slot)
                    SetText(code_, i, 4, L"delay slot");
            } else {
                SetText(code_, i, 3, L"(not memory - not read)");
            }
        }

        // What to select: an address asked for, else what was selected before.
        const uint32_t want = select_ != kNoSnapshot ? select_ : selected;
        int want_row = -1, top_row = -1;
        for (int i = 0; i < count; ++i) {
            if (lines[static_cast<size_t>(i)].address == want)
                want_row = i;
            if (lines[static_cast<size_t>(i)].address == top_address)
                top_row = i;
        }
        if (want_row >= 0) {
            ListView_SetItemState(code_, want_row, LVIS_SELECTED | LVIS_FOCUSED,
                                  LVIS_SELECTED | LVIS_FOCUSED);
        }
        SendMessageW(code_, WM_SETREDRAW, TRUE, 0);
        if (select_ != kNoSnapshot && want_row >= 0) {
            // A new place: put it a little way down from the top, so what led to it shows too.
            ScrollToTop(code_, want_row > 8 ? want_row - 8 : 0);
        } else if (top_row >= 0) {
            ScrollToTop(code_, top_row);
        }
        select_ = kNoSnapshot;
        filling_ = false;
        InvalidateRect(code_, nullptr, TRUE);
    }

    void DebuggerWindow::FillRegisters() {
        if (!have_halt_) {
            for (int r = 0; r < 32 + kExtraRegisters; ++r) {
                SetText(registers_, r, 1, L"");
                SetText(registers_, r, 2, L"");
            }
            return;
        }
        const Debugger::Snapshot& s = halt_;
        for (int r = 0; r < 32; ++r) {
            SetText(registers_, r, 1, Hex(s.gpr[r]));
            // The load delay, which is what makes a register view of this CPU confusing: the
            // value a load fetched is not in its register yet.
            std::wstring note;
            if (s.landing && s.landing_reg == static_cast<uint32_t>(r) && r != 0)
                note = L"loading " + Hex(s.landing_value) + L" - next instr sees it";
            if (s.in_flight && s.in_flight_reg == static_cast<uint32_t>(r) && r != 0) {
                if (!note.empty())
                    note += L"; ";
                note += L"loading " + Hex(s.in_flight_value) + L" - after next instr";
            }
            SetText(registers_, r, 2, note);
        }
        SetText(registers_, kRowHi, 1, Hex(s.hi));
        SetText(registers_, kRowLo, 1, Hex(s.lo));
        SetText(registers_, kRowPc, 1, Hex(s.pc));
        SetText(registers_, kRowPc, 2, ReasonText(s.reason));
        SetText(registers_, kRowSr, 1, Hex(s.sr));
        {
            wchar_t note[96];
            swprintf_s(note, L"IEc=%u IM=%02X%s%s", s.sr & 1, (s.sr >> 8) & 0xFF,
                       (s.sr & (1u << 16)) ? L" IsC" : L"", (s.sr & (1u << 22)) ? L" BEV" : L"");
            SetText(registers_, kRowSr, 2, note);
        }
        SetText(registers_, kRowCause, 1, Hex(s.cause));
        {
            wchar_t note[96];
            const uint32_t code = (s.cause >> 2) & 31;
            swprintf_s(note, L"ExcCode %u (%s) IP=%02X%s", code, ExceptionName(code),
                       (s.cause >> 8) & 0xFF, (s.cause & 0x80000000u) ? L" BD" : L"");
            SetText(registers_, kRowCause, 2, note);
        }
        SetText(registers_, kRowEpc, 1, Hex(s.epc));
        SetText(registers_, kRowBadVaddr, 1, Hex(s.badvaddr));
        InvalidateRect(registers_, nullptr, TRUE);
    }

    void DebuggerWindow::FillBreakpoints() {
        filling_ = true;
        const std::vector<Debugger::Breakpoint>& same = snapshot_.breakpoints;
        if (ListView_GetItemCount(breakpoint_list_) == static_cast<int>(same.size())) {
            // The same breakpoints, most likely with new hit counts: updated in place.
            for (size_t i = 0; i < same.size(); ++i) {
                const int row = static_cast<int>(i);
                SetText(breakpoint_list_, row, 0, Hex(same[i].address));
                SetText(breakpoint_list_, row, 1, std::to_wstring(same[i].hits));
                const bool checked = ListView_GetCheckState(breakpoint_list_, row) != FALSE;
                if (checked != same[i].enabled)
                    ListView_SetCheckState(breakpoint_list_, row, same[i].enabled ? TRUE : FALSE);
            }
            filling_ = false;
            return;
        }
        const int selected = ListView_GetNextItem(breakpoint_list_, -1, LVNI_SELECTED);
        SendMessageW(breakpoint_list_, WM_SETREDRAW, FALSE, 0);
        ListView_DeleteAllItems(breakpoint_list_);
        const std::vector<Debugger::Breakpoint>& list = snapshot_.breakpoints;
        for (size_t i = 0; i < list.size(); ++i) {
            const int row = static_cast<int>(i);
            InsertRow(breakpoint_list_, row, Hex(list[i].address), row);
            SetText(breakpoint_list_, row, 1, std::to_wstring(list[i].hits));
            ListView_SetCheckState(breakpoint_list_, row, list[i].enabled ? TRUE : FALSE);
        }
        if (selected >= 0 && selected < static_cast<int>(list.size()))
            ListView_SetItemState(breakpoint_list_, selected, LVIS_SELECTED, LVIS_SELECTED);
        SendMessageW(breakpoint_list_, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(breakpoint_list_, nullptr, TRUE);
        filling_ = false;
    }

    void DebuggerWindow::UpdateControls() {
        const bool halted = have_snapshot_ && snapshot_.halted && !running_ && !waiting_;
        EnableWindow(controls_[kContinue], halted);
        EnableWindow(controls_[kStepInto], halted);
        EnableWindow(controls_[kStepOver], halted);
        EnableWindow(controls_[kStepOut], halted);
        EnableWindow(controls_[kRunToCursor], halted);
        EnableWindow(controls_[kBreak], !halted);
        // Registers are edited only while halted: between two frames of a running game a new
        // value means nothing anyone could predict. Memory can be written either way - poking a
        // value into a running game is half of what a memory editor is for.
        EnableWindow(controls_[kSetRegister], halted);
        EnableWindow(register_value_, halted);
        const bool any = !snapshot_.breakpoints.empty();
        EnableWindow(controls_[kRemoveBreakpoint],
                     ListView_GetNextItem(breakpoint_list_, -1, LVNI_SELECTED) >= 0);
        EnableWindow(controls_[kRemoveAll], any);
    }

    void DebuggerWindow::UpdateStatus() {
        std::wstring text;
        if (have_snapshot_ && snapshot_.halted && !running_) {
            wchar_t line[160];
            swprintf_s(line, L"Halted at %08X - %s.   Cycle %llu.", snapshot_.pc,
                       ReasonText(snapshot_.reason),
                       static_cast<unsigned long long>(snapshot_.cycles));
            text = line;
        } else if (break_requested_) {
            text = L"Break requested - the machine halts at its next instruction once it runs.";
        } else {
            text = L"Running.";
            if (have_halt_)
                text += L" The registers are the last halt's.";
        }
        SetWindowTextW(status_, text.c_str());
        InvalidateRect(registers_, nullptr, TRUE);
        InvalidateRect(code_, nullptr, TRUE);
    }

    // ---------------------------------------------------------------------------------------------
    // Requests out
    // ---------------------------------------------------------------------------------------------

    void DebuggerWindow::Request(Change change, uint32_t center) {
        if (host_.request)
            host_.request(std::move(change), center);
    }

    void DebuggerWindow::RunOn(Change change) {
        // Taken as running at once, so the step keys wait for the halt; greyed only if the halt
        // does not come back within a moment.
        waiting_ = true;
        UpdateControls();
        SetTimer(window_, kGreyTimer, kGreyMs, nullptr);
        Request(std::move(change), kNoSnapshot);
    }

    void DebuggerWindow::GoTo(uint32_t address) {
        address &= ~3u;
        select_ = address;
        Request(nullptr, address);
    }

    void DebuggerWindow::ToggleBreakpoint(uint32_t address) {
        bool enabled = false;
        const bool exists = HasBreakpoint(address, &enabled);
        const uint32_t center = ListingCenter();
        select_ = address;
        if (exists)
            Request([address](Debugger& debugger) { debugger.RemoveBreakpoint(address); }, center);
        else
            Request([address](Debugger& debugger) { debugger.AddBreakpoint(address); }, center);
    }

    bool DebuggerWindow::HasBreakpoint(uint32_t address, bool* enabled) const {
        for (const Debugger::Breakpoint& breakpoint : snapshot_.breakpoints) {
            if (Physical(breakpoint.address) == Physical(address)) {
                *enabled = breakpoint.enabled;
                return true;
            }
        }
        return false;
    }

    bool DebuggerWindow::SelectedLine(uint32_t* address) const {
        const int row = ListView_GetNextItem(code_, -1, LVNI_SELECTED);
        if (row < 0 || row >= static_cast<int>(snapshot_.lines.size()))
            return false;
        *address = snapshot_.lines[static_cast<size_t>(row)].address;
        return true;
    }

    bool DebuggerWindow::ReadAddress(uint32_t* address) const {
        wchar_t text[16] = {};
        GetWindowTextW(address_, text, 16);
        return ParseHex(text, address);
    }

    uint32_t DebuggerWindow::ListingCenter() const {
        return have_snapshot_ && !snapshot_.lines.empty()
                   ? snapshot_.lines[snapshot_.lines.size() / 2].address
                   : kAtPc;
    }

    // ---------------------------------------------------------------------------------------------
    // Memory, and editing
    // ---------------------------------------------------------------------------------------------

    void DebuggerWindow::ViewMemory(uint32_t address) {
        memory_view_ = address & ~static_cast<uint32_t>(kMemoryRow - 1);
        SetWindowTextW(memory_address_, Hex(memory_view_).c_str());
        const uint32_t view = memory_view_;
        Request([view](Debugger& debugger) { debugger.SetMemoryView(view, kMemoryBytes); },
                ListingCenter());
    }

    void DebuggerWindow::FillMemoryPane(uint32_t old_address, const std::vector<uint8_t>& old_bytes) {
        const std::vector<uint8_t>& bytes = snapshot_.memory;
        const std::vector<uint8_t>& ok = snapshot_.memory_readable;
        if (bytes.size() != static_cast<size_t>(kMemoryBytes) || ok.size() != bytes.size())
            return;
        const bool comparable = old_address == snapshot_.memory_address &&
                                old_bytes.size() == bytes.size();
        memory_changed_rows_ = 0;
        const int rows = kMemoryBytes / kMemoryRow;
        for (int r = 0; r < rows; ++r) {
            std::wstring hex, ascii;
            for (int c = 0; c < kMemoryRow; ++c) {
                const size_t i = static_cast<size_t>(r * kMemoryRow + c);
                if (c == 8)
                    hex += L' ';
                if (ok[i]) {
                    wchar_t pair[4];
                    swprintf_s(pair, L"%02X ", bytes[i]);
                    hex += pair;
                    ascii += (bytes[i] >= 0x20 && bytes[i] < 0x7F) ? static_cast<wchar_t>(bytes[i])
                                                                   : L'.';
                } else {
                    hex += L"?? ";
                    ascii += L' ';
                }
                if (comparable && old_bytes[i] != bytes[i])
                    memory_changed_rows_ |= 1u << r;
            }
            SetText(memory_, r, 0,
                    Hex(snapshot_.memory_address + static_cast<uint32_t>(r * kMemoryRow)));
            SetText(memory_, r, 1, hex);
            SetText(memory_, r, 2, ascii);
        }
        InvalidateRect(memory_, nullptr, FALSE);
    }

    int DebuggerWindow::EditableRegister(int row) const {
        if (row >= 1 && row < 32)
            return row;
        if (row == kRowHi) return Debugger::kRegisterHi;
        if (row == kRowLo) return Debugger::kRegisterLo;
        if (row == kRowPc) return Debugger::kRegisterPc;
        return -1;
    }

    // Puts the selected register's value in the edit box, ready to change.
    void DebuggerWindow::EditSelectedRegister() {
        const int row = ListView_GetNextItem(registers_, -1, LVNI_SELECTED);
        if (row < 0 || EditableRegister(row) < 0 || !have_halt_)
            return;
        wchar_t text[16] = {};
        ListView_GetItemText(registers_, row, 1, text, 16);
        SetWindowTextW(register_value_, text);
        SetFocus(register_value_);
        SendMessageW(register_value_, EM_SETSEL, 0, -1);
    }

    // Puts a memory row's address and bytes in the edit boxes, ready to change.
    void DebuggerWindow::EditMemoryRow(int row) {
        if (row < 0 || row >= kMemoryBytes / kMemoryRow || snapshot_.memory.empty())
            return;
        std::wstring hex;
        for (int c = 0; c < kMemoryRow; ++c) {
            const size_t i = static_cast<size_t>(row * kMemoryRow + c);
            if (!snapshot_.memory_readable[i])
                break;
            wchar_t pair[4];
            swprintf_s(pair, c == 0 ? L"%02X" : L" %02X", snapshot_.memory[i]);
            hex += pair;
        }
        SetWindowTextW(memory_address_,
                       Hex(snapshot_.memory_address + static_cast<uint32_t>(row * kMemoryRow))
                           .c_str());
        SetWindowTextW(memory_bytes_, hex.c_str());
        SetFocus(memory_bytes_);
        SendMessageW(memory_bytes_, EM_SETSEL, 0, -1);
    }

    void DebuggerWindow::OnControl(Control control) {
        uint32_t address = 0;
        const bool halted = have_snapshot_ && snapshot_.halted && !running_ && !waiting_;
        switch (control) {
            case kContinue:
                if (halted)
                    RunOn([](Debugger& debugger) { debugger.Resume(); });
                break;
            case kBreak:
                if (!halted) {
                    break_requested_ = true;
                    UpdateStatus();
                    Request([](Debugger& debugger) { debugger.RequestBreak(); }, kNoSnapshot);
                }
                break;
            case kStepInto:
                if (halted)
                    RunOn([](Debugger& debugger) { debugger.StepInto(); });
                break;
            case kStepOver:
                if (halted)
                    RunOn([](Debugger& debugger) { debugger.StepOver(); });
                break;
            case kStepOut:
                if (halted)
                    RunOn([](Debugger& debugger) { debugger.StepOut(); });
                break;
            case kRunToCursor:
                if (halted && SelectedLine(&address))
                    RunOn([address](Debugger& debugger) { debugger.RunTo(address); });
                break;
            case kGoTo:
                if (ReadAddress(&address))
                    GoTo(address);
                else
                    MessageBeep(MB_ICONWARNING);
                break;
            case kGoToPc:
                GoTo(have_snapshot_ ? snapshot_.pc : 0);
                if (!have_snapshot_)
                    Request(nullptr, kAtPc);
                break;
            case kAddBreakpoint:
                // The address box if it has one, else the selected line.
                if (ReadAddress(&address) || SelectedLine(&address))
                    ToggleBreakpoint(address);
                else
                    MessageBeep(MB_ICONWARNING);
                break;
            case kRemoveBreakpoint: {
                const int row = ListView_GetNextItem(breakpoint_list_, -1, LVNI_SELECTED);
                if (row >= 0 && row < static_cast<int>(snapshot_.breakpoints.size())) {
                    address = snapshot_.breakpoints[static_cast<size_t>(row)].address;
                    Request([address](Debugger& debugger) { debugger.RemoveBreakpoint(address); },
                            ListingCenter());
                }
                break;
            }
            case kRemoveAll:
                if (have_snapshot_) {
                    Request([](Debugger& debugger) { debugger.ClearBreakpoints(); },
                            ListingCenter());
                }
                break;
            case kMemoryView: {
                wchar_t text[16] = {};
                GetWindowTextW(memory_address_, text, 16);
                if (ParseHex(text, &address))
                    ViewMemory(address);
                else
                    MessageBeep(MB_ICONWARNING);
                break;
            }
            case kMemoryPrevious:
                ViewMemory(memory_view_ - kMemoryBytes / 2);
                break;
            case kMemoryNext:
                ViewMemory(memory_view_ + kMemoryBytes / 2);
                break;
            case kMemoryWrite: {
                wchar_t text[16] = {};
                GetWindowTextW(memory_address_, text, 16);
                const int length = GetWindowTextLengthW(memory_bytes_);
                std::wstring typed(static_cast<size_t>(length) + 1, L'\0');
                GetWindowTextW(memory_bytes_, &typed[0], length + 1);
                std::vector<uint8_t> bytes;
                if (!ParseHex(text, &address) || !ParseBytes(typed.c_str(), &bytes) ||
                    bytes.size() > static_cast<size_t>(kMemoryBytes)) {
                    MessageBeep(MB_ICONWARNING);
                    break;
                }
                if (host_.write_memory)
                    host_.write_memory(address, std::move(bytes), ListingCenter());
                break;
            }
            case kSetRegister: {
                const int row = ListView_GetNextItem(registers_, -1, LVNI_SELECTED);
                const int index = EditableRegister(row);
                wchar_t text[16] = {};
                GetWindowTextW(register_value_, text, 16);
                uint32_t value = 0;
                if (!halted || index < 0 || !ParseHex(text, &value) ||
                    (index == Debugger::kRegisterPc && (value & 3) != 0)) {
                    MessageBeep(MB_ICONWARNING);
                    break;
                }
                // A new pc moves the listing with it.
                if (index == Debugger::kRegisterPc)
                    select_ = value;
                Request([index, value](Debugger& debugger) { debugger.SetRegister(index, value); },
                        index == Debugger::kRegisterPc ? value : ListingCenter());
                break;
            }
            default:
                break;
        }
    }

    void DebuggerWindow::ShowContextMenu(int x, int y) {
        uint32_t address = 0;
        if (!SelectedLine(&address))
            return;
        const int row = ListView_GetNextItem(code_, -1, LVNI_SELECTED);
        const Debugger::Line& line = snapshot_.lines[static_cast<size_t>(row)];
        const bool halted = have_snapshot_ && snapshot_.halted && !running_;

        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, kMenuToggle, L"Toggle &Breakpoint\tF9");
        AppendMenuW(menu, MF_STRING | (halted ? 0 : MF_GRAYED), kMenuRunTo,
                    L"&Run to Cursor\tCtrl+F10");
        AppendMenuW(menu, MF_STRING | (line.has_target ? 0 : MF_GRAYED), kMenuFollow,
                    L"&Follow Branch\tEnter");
        AppendMenuW(menu, MF_STRING, kMenuGoToPc, L"Go to &PC");
        const int chosen = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, x, y, 0,
                                          window_, nullptr);
        DestroyMenu(menu);
        switch (chosen) {
            case kMenuToggle: ToggleBreakpoint(address); break;
            case kMenuRunTo:  OnControl(kRunToCursor); break;
            case kMenuFollow: GoTo(line.target); break;
            case kMenuGoToPc: OnControl(kGoToPc); break;
            default: break;
        }
    }

    // A register's value as an address: shown in the memory pane or the listing.
    void DebuggerWindow::ShowRegisterMenu(int x, int y) {
        const int row = ListView_GetNextItem(registers_, -1, LVNI_SELECTED);
        if (row < 0 || !have_halt_)
            return;
        wchar_t text[16] = {};
        ListView_GetItemText(registers_, row, 1, text, 16);
        uint32_t value = 0;
        if (!ParseHex(text, &value))
            return;
        const bool halted = have_snapshot_ && snapshot_.halted && !running_ && !waiting_;
        HMENU menu = CreatePopupMenu();
        const std::wstring memory = L"Show " + Hex(value) + L" in &Memory";
        const std::wstring code = L"Show " + Hex(value) + L" in the &Disassembly";
        AppendMenuW(menu, MF_STRING, kMenuShowMemory, memory.c_str());
        AppendMenuW(menu, MF_STRING, kMenuShowCode, code.c_str());
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING | (halted && EditableRegister(row) >= 0 ? 0 : MF_GRAYED),
                    kMenuEdit, L"&Edit Value...\tDouble-click");
        const int chosen = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, x, y, 0,
                                          window_, nullptr);
        DestroyMenu(menu);
        switch (chosen) {
            case kMenuShowMemory: ViewMemory(value); break;
            case kMenuShowCode:   GoTo(value); break;
            case kMenuEdit:       EditSelectedRegister(); break;
            default: break;
        }
    }

    // Following a pointer: any of the row's four words, into the memory pane or the listing.
    void DebuggerWindow::ShowMemoryMenu(int x, int y) {
        const int row = ListView_GetNextItem(memory_, -1, LVNI_SELECTED);
        if (row < 0 || snapshot_.memory.size() != static_cast<size_t>(kMemoryBytes))
            return;
        HMENU menu = CreatePopupMenu();
        HMENU code = CreatePopupMenu();
        uint32_t words[4] = {};
        bool readable[4] = {};
        for (int w = 0; w < 4; ++w) {
            const size_t at = static_cast<size_t>(row * kMemoryRow + w * 4);
            readable[w] = snapshot_.memory_readable[at] && snapshot_.memory_readable[at + 3];
            for (int b = 0; b < 4; ++b)
                words[w] |= static_cast<uint32_t>(snapshot_.memory[at + b]) << (b * 8);
            wchar_t label[64];
            swprintf_s(label, L"Follow +%X: %08X", w * 4, words[w]);
            AppendMenuW(menu, MF_STRING | (readable[w] ? 0 : MF_GRAYED),
                        kMenuFollowWord + w, label);
            swprintf_s(label, L"+%X: %08X", w * 4, words[w]);
            AppendMenuW(code, MF_STRING | (readable[w] ? 0 : MF_GRAYED),
                        kMenuFollowWord + 4 + w, label);
        }
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(code), L"Show in the &Disassembly");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kMenuEdit, L"&Edit Row...\tDouble-click");
        const int chosen = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, x, y, 0,
                                          window_, nullptr);
        DestroyMenu(menu);   // and the submenu with it
        if (chosen >= kMenuFollowWord && chosen < kMenuFollowWord + 4)
            ViewMemory(words[chosen - kMenuFollowWord]);
        else if (chosen >= kMenuFollowWord + 4 && chosen < kMenuFollowWord + 8)
            GoTo(words[chosen - kMenuFollowWord - 4]);
        else if (chosen == kMenuEdit)
            EditMemoryRow(row);
    }

    // ---------------------------------------------------------------------------------------------
    // Keys
    // ---------------------------------------------------------------------------------------------

    bool DebuggerWindow::PreTranslate(const MSG& message) {
        if (window_ == nullptr || (message.message != WM_KEYDOWN && message.message != WM_SYSKEYDOWN))
            return false;
        if (message.hwnd != window_ && !IsChild(window_, message.hwnd))
            return false;
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        uint32_t address = 0;
        switch (message.wParam) {
            case VK_F5:
                OnControl(kContinue);
                return true;
            case VK_CANCEL:   // Ctrl+Break
            case VK_PAUSE:
                OnControl(kBreak);
                return true;
            case VK_F9:
                if (SelectedLine(&address))
                    ToggleBreakpoint(address);
                return true;
            case VK_F10:      // a system key: F10 would otherwise go looking for a menu bar
                OnControl(ctrl ? kRunToCursor : kStepOver);
                return true;
            case VK_F11:
                OnControl(shift ? kStepOut : kStepInto);
                return true;
            case 'G':
                if (!ctrl)
                    return false;
                SetFocus(address_);
                SendMessageW(address_, EM_SETSEL, 0, -1);
                return true;
            case VK_RETURN:
                if (message.hwnd == address_) {
                    OnControl(kGoTo);
                    return true;
                }
                if (message.hwnd == memory_address_) {
                    OnControl(kMemoryView);
                    return true;
                }
                if (message.hwnd == memory_bytes_) {
                    OnControl(kMemoryWrite);
                    return true;
                }
                if (message.hwnd == register_value_) {
                    OnControl(kSetRegister);
                    return true;
                }
                if (message.hwnd == code_) {
                    const int row = ListView_GetNextItem(code_, -1, LVNI_SELECTED);
                    if (row >= 0 && snapshot_.lines[static_cast<size_t>(row)].has_target)
                        GoTo(snapshot_.lines[static_cast<size_t>(row)].target);
                    return true;
                }
                return false;
            default:
                return false;
        }
    }

    // ---------------------------------------------------------------------------------------------
    // Drawing and notifications
    // ---------------------------------------------------------------------------------------------

    LRESULT DebuggerWindow::CustomDrawCode(NMLVCUSTOMDRAW* draw) {
        switch (draw->nmcd.dwDrawStage) {
            case CDDS_PREPAINT:
                return CDRF_NOTIFYITEMDRAW;
            case CDDS_ITEMPREPAINT: {
                const size_t row = static_cast<size_t>(draw->nmcd.dwItemSpec);
                if (row >= snapshot_.lines.size())
                    return CDRF_DODEFAULT;
                const Debugger::Line& line = snapshot_.lines[row];
                bool enabled = false;
                const bool halted = snapshot_.halted && !running_;
                if (line.address == snapshot_.pc && halted)
                    draw->clrTextBk = RGB(255, 236, 140);
                else if (HasBreakpoint(line.address, &enabled) && enabled)
                    draw->clrTextBk = RGB(255, 205, 205);
                if (!line.readable || line.delay_slot)
                    draw->clrText = RGB(110, 110, 110);
                return CDRF_NEWFONT;
            }
            default:
                return CDRF_DODEFAULT;
        }
    }

    // Rows whose bytes the last snapshot changed are red - a value counting down, or a step's
    // store, shows where it is.
    LRESULT DebuggerWindow::CustomDrawMemory(NMLVCUSTOMDRAW* draw) {
        switch (draw->nmcd.dwDrawStage) {
            case CDDS_PREPAINT:
                return CDRF_NOTIFYITEMDRAW;
            case CDDS_ITEMPREPAINT: {
                const int row = static_cast<int>(draw->nmcd.dwItemSpec);
                if (row >= 0 && row < 32 && (memory_changed_rows_ & (1u << row)) != 0)
                    draw->clrText = RGB(200, 0, 0);
                return CDRF_NEWFONT;
            }
            default:
                return CDRF_DODEFAULT;
        }
    }

    LRESULT DebuggerWindow::CustomDrawRegisters(NMLVCUSTOMDRAW* draw) {
        switch (draw->nmcd.dwDrawStage) {
            case CDDS_PREPAINT:
                return CDRF_NOTIFYITEMDRAW;
            case CDDS_ITEMPREPAINT: {
                const int row = static_cast<int>(draw->nmcd.dwItemSpec);
                const bool changed = row < 32 ? (changed_ & (1u << row)) != 0
                                              : (changed_extra_ & (1u << (row - 32))) != 0;
                if (running_ || !have_halt_)
                    draw->clrText = GetSysColor(COLOR_GRAYTEXT);
                else if (changed)
                    draw->clrText = RGB(200, 0, 0);
                return CDRF_NEWFONT;
            }
            default:
                return CDRF_DODEFAULT;
        }
    }

    LRESULT DebuggerWindow::OnNotify(NMHDR* header) {
        if (header->hwndFrom == code_) {
            switch (header->code) {
                case NM_CUSTOMDRAW:
                    return CustomDrawCode(reinterpret_cast<NMLVCUSTOMDRAW*>(header));
                case NM_DBLCLK: {
                    const NMITEMACTIVATE* item = reinterpret_cast<NMITEMACTIVATE*>(header);
                    if (item->iItem >= 0 && item->iItem < static_cast<int>(snapshot_.lines.size()))
                        ToggleBreakpoint(snapshot_.lines[static_cast<size_t>(item->iItem)].address);
                    return 0;
                }
                case NM_RCLICK: {
                    POINT point = {};
                    GetCursorPos(&point);
                    ShowContextMenu(point.x, point.y);
                    return 0;
                }
                case LVN_KEYDOWN: {
                    // Off either end of the listing: ask for the next stretch, keeping the line.
                    const NMLVKEYDOWN* key = reinterpret_cast<NMLVKEYDOWN*>(header);
                    const int row = ListView_GetNextItem(code_, -1, LVNI_SELECTED);
                    const int last = static_cast<int>(snapshot_.lines.size()) - 1;
                    if (row == 0 && (key->wVKey == VK_UP || key->wVKey == VK_PRIOR))
                        GoTo(snapshot_.lines[0].address - 4);
                    else if (row == last && last >= 0 &&
                             (key->wVKey == VK_DOWN || key->wVKey == VK_NEXT))
                        GoTo(snapshot_.lines[static_cast<size_t>(last)].address + 4);
                    return 0;
                }
                default:
                    return 0;
            }
        }
        if (header->hwndFrom == registers_) {
            switch (header->code) {
                case NM_CUSTOMDRAW:
                    return CustomDrawRegisters(reinterpret_cast<NMLVCUSTOMDRAW*>(header));
                case NM_DBLCLK:
                    EditSelectedRegister();
                    return 0;
                case NM_RCLICK: {
                    POINT point = {};
                    GetCursorPos(&point);
                    ShowRegisterMenu(point.x, point.y);
                    return 0;
                }
                default:
                    return 0;
            }
        }
        if (header->hwndFrom == memory_) {
            switch (header->code) {
                case NM_CUSTOMDRAW:
                    return CustomDrawMemory(reinterpret_cast<NMLVCUSTOMDRAW*>(header));
                case NM_DBLCLK:
                    EditMemoryRow(reinterpret_cast<NMITEMACTIVATE*>(header)->iItem);
                    return 0;
                case NM_RCLICK: {
                    POINT point = {};
                    GetCursorPos(&point);
                    ShowMemoryMenu(point.x, point.y);
                    return 0;
                }
                case LVN_KEYDOWN: {
                    // Off either end: the pane moves half a page, keeping the rows in view.
                    const NMLVKEYDOWN* key = reinterpret_cast<NMLVKEYDOWN*>(header);
                    const int row = ListView_GetNextItem(memory_, -1, LVNI_SELECTED);
                    const int last = kMemoryBytes / kMemoryRow - 1;
                    if (row == 0 && (key->wVKey == VK_UP || key->wVKey == VK_PRIOR))
                        OnControl(kMemoryPrevious);
                    else if (row == last && (key->wVKey == VK_DOWN || key->wVKey == VK_NEXT))
                        OnControl(kMemoryNext);
                    return 0;
                }
                default:
                    return 0;
            }
        }
        if (header->hwndFrom == breakpoint_list_) {
            switch (header->code) {
                case LVN_ITEMCHANGED: {
                    const NMLISTVIEW* change = reinterpret_cast<NMLISTVIEW*>(header);
                    if (!filling_ && (change->uChanged & LVIF_STATE) != 0 &&
                        ((change->uNewState ^ change->uOldState) & LVIS_STATEIMAGEMASK) != 0 &&
                        change->iItem >= 0 &&
                        change->iItem < static_cast<int>(snapshot_.breakpoints.size())) {
                        const uint32_t address =
                            snapshot_.breakpoints[static_cast<size_t>(change->iItem)].address;
                        const bool enabled =
                            ListView_GetCheckState(breakpoint_list_, change->iItem) != FALSE;
                        Request([address, enabled](Debugger& debugger) {
                                    debugger.SetBreakpointEnabled(address, enabled);
                                },
                                ListingCenter());
                    }
                    UpdateControls();
                    return 0;
                }
                case NM_DBLCLK: {
                    const NMITEMACTIVATE* item = reinterpret_cast<NMITEMACTIVATE*>(header);
                    if (item->iItem >= 0 &&
                        item->iItem < static_cast<int>(snapshot_.breakpoints.size()))
                        GoTo(snapshot_.breakpoints[static_cast<size_t>(item->iItem)].address);
                    return 0;
                }
                default:
                    return 0;
            }
        }
        return 0;
    }

    void DebuggerWindow::Layout(int width, int height) {
        const int margin = 8, gap = 6, row = 26;
        int x = margin;
        const int widths[] = { 100, 60, 104, 108, 132, 96 };   // kContinue..kRunToCursor
        for (int c = kContinue; c <= kRunToCursor; ++c) {
            MoveWindow(controls_[c], x, margin, widths[c], row, TRUE);
            x += widths[c] + gap;
        }

        const int y2 = margin + row + gap;
        x = margin;
        MoveWindow(address_, x, y2 + 2, 110, row - 4, TRUE);
        x += 110 + gap;
        MoveWindow(controls_[kGoTo], x, y2, 64, row, TRUE);
        x += 64 + gap;
        MoveWindow(controls_[kGoToPc], x, y2, 40, row, TRUE);
        x += 40 + gap;
        MoveWindow(controls_[kAddBreakpoint], x, y2, 124, row, TRUE);
        x += 124 + gap * 2;
        MoveWindow(status_, x, y2 + 5, width - x - margin, row - 6, TRUE);

        const int top = y2 + row + gap;
        const int label = 18;
        const int right_width = 360;
        const int left_width = width - right_width - margin * 2 - gap;
        const int right_x = margin + left_width + gap;
        const int lists_height = height - top - margin;

        // Left: the listing over the memory pane, the memory controls along the bottom.
        const int code_height = (lists_height * 55) / 100;
        MoveWindow(code_label_, margin, top, left_width, label, TRUE);
        MoveWindow(code_, margin, top + label, left_width, code_height - label, TRUE);

        const int memory_top = top + code_height + gap;
        const int memory_controls_y = height - margin - row;
        MoveWindow(memory_label_, margin, memory_top, left_width, label, TRUE);
        MoveWindow(memory_, margin, memory_top + label, left_width,
                   memory_controls_y - gap - (memory_top + label), TRUE);
        x = margin;
        MoveWindow(memory_address_, x, memory_controls_y + 2, 90, row - 4, TRUE);
        x += 90 + gap;
        MoveWindow(controls_[kMemoryView], x, memory_controls_y, 50, row, TRUE);
        x += 50 + gap;
        MoveWindow(controls_[kMemoryPrevious], x, memory_controls_y, 60, row, TRUE);
        x += 60 + gap;
        MoveWindow(controls_[kMemoryNext], x, memory_controls_y, 60, row, TRUE);
        x += 60 + gap * 3;
        const int write_width = 60;
        MoveWindow(memory_bytes_, x, memory_controls_y + 2,
                   margin + left_width - write_width - gap - x, row - 4, TRUE);
        MoveWindow(controls_[kMemoryWrite], margin + left_width - write_width, memory_controls_y,
                   write_width, row, TRUE);

        // Right: the registers with their edit box, then the breakpoints.
        const int registers_height = (lists_height * 60) / 100;
        MoveWindow(registers_label_, right_x, top, right_width, label, TRUE);
        const int registers_list_height = registers_height - label - row - gap;
        MoveWindow(registers_, right_x, top + label, right_width, registers_list_height, TRUE);
        const int set_y = top + label + registers_list_height + gap;
        MoveWindow(register_value_, right_x, set_y + 2, right_width - 60 - gap, row - 4, TRUE);
        MoveWindow(controls_[kSetRegister], right_x + right_width - 60, set_y, 60, row, TRUE);

        const int bp_top = top + registers_height + gap;
        MoveWindow(breakpoints_label_, right_x, bp_top, right_width, label, TRUE);
        const int bp_list_height = height - margin - row - gap - (bp_top + label);
        MoveWindow(breakpoint_list_, right_x, bp_top + label, right_width, bp_list_height, TRUE);
        const int buttons_y = height - margin - row;
        MoveWindow(controls_[kRemoveBreakpoint], right_x, buttons_y, 90, row, TRUE);
        MoveWindow(controls_[kRemoveAll], right_x + 90 + gap, buttons_y, 100, row, TRUE);
    }

    LRESULT CALLBACK DebuggerWindow::WindowProc(HWND window, UINT message, WPARAM wparam,
                                                LPARAM lparam) {
        if (message == WM_NCCREATE) {
            const CREATESTRUCTW* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            SetWindowLongPtrW(window, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(create->lpCreateParams));
            return DefWindowProcW(window, message, wparam, lparam);
        }
        DebuggerWindow* self =
            reinterpret_cast<DebuggerWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (self == nullptr)
            return DefWindowProcW(window, message, wparam, lparam);

        switch (message) {
            case WM_SIZE:
                self->Layout(LOWORD(lparam), HIWORD(lparam));
                return 0;

            case WM_GETMINMAXINFO: {
                MINMAXINFO* info = reinterpret_cast<MINMAXINFO*>(lparam);
                info->ptMinTrackSize.x = 860;
                info->ptMinTrackSize.y = 600;
                return 0;
            }

            case WM_COMMAND: {
                const int id = LOWORD(wparam);
                if (id >= kIdControlBase && id < kIdControlBase + kControlCount &&
                    HIWORD(wparam) == BN_CLICKED)
                    self->OnControl(static_cast<Control>(id - kIdControlBase));
                return 0;
            }

            case WM_NOTIFY:
                return self->OnNotify(reinterpret_cast<NMHDR*>(lparam));

            case WM_TIMER:
                if (wparam == kGreyTimer) {
                    // No halt came back: the machine is running, and the window says so.
                    KillTimer(window, kGreyTimer);
                    self->running_ = true;
                    self->UpdateControls();
                    self->UpdateStatus();
                } else if (wparam == kLiveTimer) {
                    // Running: fresh memory (and hit counts) twice a second. Halted, nothing moves,
                    // so nothing is asked for.
                    if (self->running_ && self->visible())
                        self->Request(nullptr, self->ListingCenter());
                }
                return 0;

            case WM_CLOSE:
                // Closing lets a halted machine go - a game frozen behind a closed window is no
                // use to anyone. The breakpoints stay; the next one to hit opens the window again.
                if (self->have_snapshot_ && self->snapshot_.halted && !self->running_)
                    self->RunOn([](Debugger& debugger) { debugger.Resume(); });
                self->Show(false);
                if (self->host_.on_closed)
                    self->host_.on_closed();
                return 0;

            default:
                break;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

}   // namespace psxemu
