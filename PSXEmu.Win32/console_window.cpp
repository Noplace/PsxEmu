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
#include "console_window.h"
#include "app_icon.h"

namespace psxemu {

    namespace {

        const wchar_t kConsoleClass[] = L"PSXEmuBiosConsole";
        const UINT kCommandClear = 1;

        // Past this many characters the oldest quarter goes, at a line boundary. A game that
        // prints every frame would otherwise grow the control until it crawls.
        const int kMaxCharacters = 1000000;

        // Shift-JIS lead bytes: the first of a two-byte character.
        bool IsShiftJisLead(unsigned char c) {
            return (c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC);
        }

    }   // namespace

    ConsoleWindow::~ConsoleWindow() {
        if (window_ != nullptr && IsWindow(window_))
            DestroyWindow(window_);
        if (font_ != nullptr)
            DeleteObject(font_);
    }

    bool ConsoleWindow::Create(HINSTANCE instance, HWND owner, std::function<void()> on_closed) {
        on_closed_ = std::move(on_closed);

        WNDCLASSEXW window_class = {};
        window_class.cbSize = sizeof(window_class);
        if (!GetClassInfoExW(instance, kConsoleClass, &window_class)) {
            window_class = {};
            window_class.cbSize = sizeof(window_class);
            window_class.lpfnWndProc = WindowProc;
            window_class.hInstance = instance;
            window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            window_class.hIcon = AppIcon(instance);
            window_class.hIconSm = AppIconSmall(instance);
            window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
            window_class.lpszClassName = kConsoleClass;
            if (RegisterClassExW(&window_class) == 0)
                return false;
        }

        HMENU menu = CreateMenu();
        AppendMenuW(menu, MF_STRING, kCommandClear, L"&Clear");

        window_ = CreateWindowExW(0, kConsoleClass, L"PSXEmu - BIOS Console", WS_OVERLAPPEDWINDOW,
                                  CW_USEDEFAULT, CW_USEDEFAULT, 760, 420, owner, menu, instance,
                                  this);
        if (window_ == nullptr)
            return false;

        edit_ = CreateWindowExW(0, L"EDIT", L"",
                                WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE |
                                    ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_NOHIDESEL,
                                0, 0, 0, 0, window_, nullptr, instance, nullptr);
        if (edit_ == nullptr)
            return false;
        SendMessageW(edit_, EM_SETLIMITTEXT, 0, 0);   // the ceiling, not the default 32K

        const UINT dpi = GetDpiForWindow(window_);
        font_ = CreateFontW(-MulDiv(10, dpi == 0 ? 96 : static_cast<int>(dpi), 72), 0, 0, 0,
                            FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN,
                            L"Consolas");
        if (font_ != nullptr)
            SendMessageW(edit_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), FALSE);

        RECT client = {};
        GetClientRect(window_, &client);
        MoveWindow(edit_, 0, 0, client.right, client.bottom, FALSE);
        return true;
    }

    void ConsoleWindow::Show(bool on) {
        if (window_ == nullptr)
            return;
        ShowWindow(window_, on ? SW_SHOWNOACTIVATE : SW_HIDE);
        if (on)
            SendMessageW(edit_, EM_SCROLLCARET, 0, 0);
    }

    void ConsoleWindow::Append(const std::string& text) {
        if (edit_ == nullptr || text.empty())
            return;

        // One pass that both finds the character boundaries - so a trail byte, which can be any
        // value from 40h up, is never mistaken for anything else - and turns the line ends into
        // the CR LF an edit control needs. Other control characters are dropped: they have no
        // glyph, and a stray one draws as a box.
        std::string bytes = held_ + text;
        held_.clear();
        std::string clean;
        clean.reserve(bytes.size() + bytes.size() / 16);
        for (size_t i = 0; i < bytes.size(); ++i) {
            const unsigned char c = static_cast<unsigned char>(bytes[i]);
            if (IsShiftJisLead(c)) {
                if (i + 1 == bytes.size()) {
                    held_.assign(1, static_cast<char>(c));
                    break;
                }
                clean.push_back(static_cast<char>(c));
                clean.push_back(bytes[++i]);
            } else if (c == '\n') {
                clean += "\r\n";
            } else if (c == '\t' || c >= 0x20) {
                clean.push_back(static_cast<char>(c));
            }
        }
        if (clean.empty())
            return;

        const int length = MultiByteToWideChar(932, 0, clean.data(),
                                               static_cast<int>(clean.size()), nullptr, 0);
        if (length <= 0)
            return;
        std::wstring wide(static_cast<size_t>(length), L'\0');
        MultiByteToWideChar(932, 0, clean.data(), static_cast<int>(clean.size()), &wide[0],
                            length);
        at_line_start_ = (clean.back() == '\n');
        AppendWide(wide);
    }

    void ConsoleWindow::AppendMarker(const wchar_t* text) {
        if (edit_ == nullptr)
            return;
        held_.clear();
        // Nothing to separate from: a first boot, or right after Clear.
        if (GetWindowTextLengthW(edit_) == 0)
            return;
        std::wstring line = at_line_start_ ? L"" : L"\r\n";
        line += L"----- ";
        line += text;
        line += L" -----\r\n";
        at_line_start_ = true;
        AppendWide(line);
    }

    void ConsoleWindow::Clear() {
        if (edit_ == nullptr)
            return;
        SetWindowTextW(edit_, L"");
        held_.clear();
        at_line_start_ = true;
    }

    void ConsoleWindow::TrimIfLong() {
        const int length = GetWindowTextLengthW(edit_);
        if (length <= kMaxCharacters)
            return;
        const LRESULT line = SendMessageW(edit_, EM_LINEFROMCHAR, length / 4, 0);
        const LRESULT cut = SendMessageW(edit_, EM_LINEINDEX, line + 1, 0);
        if (cut <= 0)
            return;
        SendMessageW(edit_, EM_SETSEL, 0, cut);
        SendMessageW(edit_, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(L""));
    }

    void ConsoleWindow::AppendWide(const std::wstring& text) {
        SendMessageW(edit_, WM_SETREDRAW, FALSE, 0);
        TrimIfLong();

        // Follow the end only if the view is already there. Someone scrolled up to read something
        // should not be dragged back down by every frame's output.
        SCROLLINFO scroll = {};
        scroll.cbSize = sizeof(scroll);
        scroll.fMask = SIF_ALL;
        const bool has_bar = GetScrollInfo(edit_, SB_VERT, &scroll) != FALSE;
        const bool follow = !has_bar || scroll.nMax == 0 ||
                            scroll.nPos + static_cast<int>(scroll.nPage) > scroll.nMax;

        DWORD selection_start = 0, selection_end = 0;
        SendMessageW(edit_, EM_GETSEL, reinterpret_cast<WPARAM>(&selection_start),
                     reinterpret_cast<LPARAM>(&selection_end));
        const LRESULT first_visible = SendMessageW(edit_, EM_GETFIRSTVISIBLELINE, 0, 0);

        const int end = GetWindowTextLengthW(edit_);
        SendMessageW(edit_, EM_SETSEL, end, end);
        SendMessageW(edit_, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(text.c_str()));

        if (follow) {
            SendMessageW(edit_, EM_SCROLLCARET, 0, 0);
        } else {
            SendMessageW(edit_, EM_SETSEL, selection_start, selection_end);
            const LRESULT now_visible = SendMessageW(edit_, EM_GETFIRSTVISIBLELINE, 0, 0);
            SendMessageW(edit_, EM_LINESCROLL, 0, first_visible - now_visible);
        }

        SendMessageW(edit_, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(edit_, nullptr, TRUE);
    }

    LRESULT CALLBACK ConsoleWindow::WindowProc(HWND window, UINT message, WPARAM wparam,
                                               LPARAM lparam) {
        if (message == WM_NCCREATE) {
            const CREATESTRUCTW* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            SetWindowLongPtrW(window, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        }
        ConsoleWindow* self =
            reinterpret_cast<ConsoleWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (self == nullptr)
            return DefWindowProcW(window, message, wparam, lparam);

        switch (message) {
            case WM_SIZE:
                if (self->edit_ != nullptr)
                    MoveWindow(self->edit_, 0, 0, LOWORD(lparam), HIWORD(lparam), TRUE);
                return 0;

            case WM_SETFOCUS:
                if (self->edit_ != nullptr)
                    SetFocus(self->edit_);
                return 0;

            // A read-only edit control asks for the dialog-grey background; the console reads
            // better as an ordinary text area.
            case WM_CTLCOLORSTATIC: {
                HDC dc = reinterpret_cast<HDC>(wparam);
                SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
                SetBkColor(dc, GetSysColor(COLOR_WINDOW));
                return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
            }

            case WM_COMMAND:
                if (LOWORD(wparam) == kCommandClear && lparam == 0) {
                    self->Clear();
                    return 0;
                }
                break;

            // Closing hides it: the text is kept, and keeps arriving, for when it is opened again.
            case WM_CLOSE:
                ShowWindow(window, SW_HIDE);
                if (self->on_closed_)
                    self->on_closed_();
                return 0;

            case WM_NCDESTROY:
                SetWindowLongPtrW(window, GWLP_USERDATA, 0);
                self->window_ = nullptr;
                self->edit_ = nullptr;
                break;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

}   // namespace psxemu
