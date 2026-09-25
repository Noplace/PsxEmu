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
#include "key_bindings_window.h"
#include "app_icon.h"

#include "win32_paths.h"   // Widen

#include <commctrl.h>
#pragma comment(lib, "comctl32.lib")

namespace psxemu {

    namespace {

        const wchar_t kBindingsClass[] = L"PSXEmuKeyBindings";
        const int kIdList = 100;
        const int kIdSet = 101;
        const int kIdClear = 102;
        const int kIdDefaults = 103;
        const UINT kMessageBeginCapture = WM_APP + 1;

        const wchar_t kIdleHelp[] =
            L"Double-click a button, or select it and press Set Key, then press the key.";

        std::wstring KeyLabel(int key) {
            return key == 0 ? std::wstring(L"(none)") : Widen(KeyName(key));
        }

    }   // namespace

    KeyBindingsWindow::~KeyBindingsWindow() {
        if (window_ != nullptr && IsWindow(window_))
            DestroyWindow(window_);
        if (font_ != nullptr)
            DeleteObject(font_);
    }

    bool KeyBindingsWindow::Create(HINSTANCE instance, HWND owner,
                                   std::function<void(const KeyMap&)> on_change) {
        on_change_ = std::move(on_change);

        INITCOMMONCONTROLSEX controls = { sizeof(controls), ICC_LISTVIEW_CLASSES };
        InitCommonControlsEx(&controls);

        WNDCLASSEXW window_class = {};
        window_class.cbSize = sizeof(window_class);
        if (!GetClassInfoExW(instance, kBindingsClass, &window_class)) {
            window_class = {};
            window_class.cbSize = sizeof(window_class);
            window_class.lpfnWndProc = WindowProc;
            window_class.hInstance = instance;
            window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            window_class.hIcon = AppIcon(instance);
            window_class.hIconSm = AppIconSmall(instance);
            window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
            window_class.lpszClassName = kBindingsClass;
            if (RegisterClassExW(&window_class) == 0)
                return false;
        }

        window_ = CreateWindowExW(0, kBindingsClass, L"PSXEmu - Keyboard Bindings",
                                  WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME,
                                  CW_USEDEFAULT, CW_USEDEFAULT, 420, 470, owner, nullptr, instance,
                                  this);
        if (window_ == nullptr)
            return false;

        NONCLIENTMETRICSW metrics = { sizeof(metrics) };
        if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0))
            font_ = CreateFontIndirectW(&metrics.lfMessageFont);

        auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
            HWND control = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0,
                                           window_,
                                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                           instance, nullptr);
            if (control != nullptr && font_ != nullptr)
                SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font_), FALSE);
            return control;
        };

        list_ = make(WC_LISTVIEWW, L"",
                     LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_BORDER | WS_TABSTOP,
                     kIdList);
        ListView_SetExtendedListViewStyle(list_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
        LVCOLUMNW column = {};
        column.mask = LVCF_TEXT | LVCF_WIDTH;
        column.pszText = const_cast<wchar_t*>(L"Button");
        column.cx = 150;
        ListView_InsertColumn(list_, 0, &column);
        column.pszText = const_cast<wchar_t*>(L"Key");
        column.cx = 150;
        ListView_InsertColumn(list_, 1, &column);
        for (int i = 0; i < kPadButtons; ++i) {
            LVITEMW item = {};
            item.mask = LVIF_TEXT;
            item.iItem = i;
            item.pszText = const_cast<wchar_t*>(kKeyBindings[i].label);
            ListView_InsertItem(list_, &item);
        }

        set_ = make(L"BUTTON", L"Set Key...", BS_PUSHBUTTON | WS_TABSTOP, kIdSet);
        clear_ = make(L"BUTTON", L"Clear", BS_PUSHBUTTON | WS_TABSTOP, kIdClear);
        defaults_ = make(L"BUTTON", L"Restore Defaults", BS_PUSHBUTTON | WS_TABSTOP, kIdDefaults);
        status_ = make(L"STATIC", kIdleHelp, SS_LEFT, 0);

        RECT client = {};
        GetClientRect(window_, &client);
        Layout(client.right, client.bottom);
        return true;
    }

    void KeyBindingsWindow::Show(const KeyMap& current) {
        if (window_ == nullptr)
            return;
        map_ = current;
        EndCapture(kIdleHelp);
        Refresh();
        ShowWindow(window_, SW_SHOW);
        SetForegroundWindow(window_);
        SetFocus(list_);
    }

    void KeyBindingsWindow::Refresh() {
        for (int i = 0; i < kPadButtons; ++i) {
            std::wstring label = (i == capturing_) ? std::wstring(L"press a key...") : KeyLabel(map_[i]);
            ListView_SetItemText(list_, i, 1, &label[0]);
        }
    }

    int KeyBindingsWindow::SelectedButton() const {
        return ListView_GetNextItem(list_, -1, LVNI_SELECTED);
    }

    void KeyBindingsWindow::BeginCapture() {
        const int button = SelectedButton();
        if (button < 0)
            return;
        capturing_ = button;
        Refresh();
        const std::wstring status = std::wstring(L"Press the key for ") +
                                    kKeyBindings[button].label + L". Escape cancels.";
        SetWindowTextW(status_, status.c_str());
        // The window itself takes the keyboard, so the key reaches WM_KEYDOWN here rather than
        // moving the list's selection.
        SetFocus(window_);
    }

    void KeyBindingsWindow::EndCapture(const wchar_t* status) {
        capturing_ = -1;
        SetWindowTextW(status_, status);
        Refresh();
    }

    void KeyBindingsWindow::Bind(int button, int key) {
        std::wstring status = kIdleHelp;
        if (key != 0) {
            // One button per key: taking it from wherever it was is what the person means, and
            // leaving it on two would press both.
            for (int i = 0; i < kPadButtons; ++i) {
                if (i != button && map_[i] == key) {
                    map_[i] = 0;
                    status = KeyLabel(key) + L" was " + kKeyBindings[i].label +
                             L"'s key; that button now has none.";
                }
            }
        }
        map_[button] = key;
        if (on_change_)
            on_change_(map_);
        EndCapture(status.c_str());
        SetFocus(list_);
    }

    void KeyBindingsWindow::Layout(int width, int height) {
        const int margin = 10;
        const int button_height = 28;
        const int status_height = 36;
        const int buttons_top = height - margin - button_height;
        const int status_top = buttons_top - 6 - status_height;
        MoveWindow(list_, margin, margin, width - margin * 2, status_top - 6 - margin, TRUE);
        MoveWindow(status_, margin, status_top, width - margin * 2, status_height, TRUE);
        MoveWindow(set_, margin, buttons_top, 100, button_height, TRUE);
        MoveWindow(clear_, margin + 106, buttons_top, 80, button_height, TRUE);
        MoveWindow(defaults_, width - margin - 130, buttons_top, 130, button_height, TRUE);
    }

    LRESULT CALLBACK KeyBindingsWindow::WindowProc(HWND window, UINT message, WPARAM wparam,
                                                   LPARAM lparam) {
        if (message == WM_NCCREATE) {
            const CREATESTRUCTW* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            SetWindowLongPtrW(window, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        }
        KeyBindingsWindow* self =
            reinterpret_cast<KeyBindingsWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (self == nullptr)
            return DefWindowProcW(window, message, wparam, lparam);

        switch (message) {
            case WM_SIZE:
                if (self->defaults_ != nullptr)
                    self->Layout(LOWORD(lparam), HIWORD(lparam));
                return 0;

            case WM_GETMINMAXINFO: {
                MINMAXINFO* limits = reinterpret_cast<MINMAXINFO*>(lparam);
                limits->ptMinTrackSize.x = 380;
                limits->ptMinTrackSize.y = 360;
                return 0;
            }

            case WM_KEYDOWN:
            case WM_SYSKEYDOWN: {
                if (self->capturing_ < 0)
                    break;
                const int key = static_cast<int>(wparam);
                if (key == VK_ESCAPE) {
                    self->EndCapture(L"Cancelled - the binding is unchanged.");
                    SetFocus(self->list_);
                } else if (IsReservedKey(key)) {
                    const std::wstring status =
                        KeyLabel(key) + L" is taken by the emulator itself (Space pauses, F1-F8 "
                                        L"load and save states, F11 is full screen). Press "
                                        L"another key, or Escape.";
                    SetWindowTextW(self->status_, status.c_str());
                } else {
                    self->Bind(self->capturing_, key);
                }
                return 0;
            }

            // Clicking anywhere else while a key is awaited cancels, rather than leaving the next
            // unrelated key press to rebind a button.
            case WM_KILLFOCUS:
                if (self->capturing_ >= 0)
                    self->EndCapture(L"Cancelled - the binding is unchanged.");
                break;

            case WM_NOTIFY: {
                const NMHDR* header = reinterpret_cast<const NMHDR*>(lparam);
                // Posted rather than started here: the list is still inside its own click
                // handling, and taking the focus from it now can see it taken straight back -
                // which would cancel the capture before a key was pressed.
                if (header->idFrom == kIdList && header->code == NM_DBLCLK)
                    PostMessageW(window, kMessageBeginCapture, 0, 0);
                return 0;
            }

            case kMessageBeginCapture:
                self->BeginCapture();
                return 0;

            case WM_COMMAND:
                switch (LOWORD(wparam)) {
                    case kIdSet:
                        self->BeginCapture();
                        break;
                    case kIdClear:
                        if (self->SelectedButton() >= 0)
                            self->Bind(self->SelectedButton(), 0);
                        break;
                    case kIdDefaults:
                        self->map_ = DefaultKeyMap();
                        if (self->on_change_)
                            self->on_change_(self->map_);
                        self->EndCapture(L"Restored the default keys.");
                        break;
                }
                return 0;

            case WM_CLOSE:
                self->EndCapture(kIdleHelp);
                ShowWindow(window, SW_HIDE);
                return 0;

            case WM_NCDESTROY:
                SetWindowLongPtrW(window, GWLP_USERDATA, 0);
                self->window_ = nullptr;
                break;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

}   // namespace psxemu
