/*****************************************************************************************************************
* Copyright (c) 2014 Khalid Ali Al-Kooheji                                                                       *
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
#include "ui/cheats_window.h"
#include "app/app_icon.h"
#include "app/win32_paths.h"   // Widen, Narrow

#include <commctrl.h>
#pragma comment(lib, "comctl32.lib")

namespace psxemu {

    using emulation::psx::Cheat;

    namespace {

        const wchar_t kWindowClass[] = L"PSXEmuCheats";

        const int kIdList = 100;
        const int kIdName = 101;
        const int kIdCode = 102;
        const int kIdNew = 103;
        const int kIdDelete = 104;
        const int kIdImport = 105;
        const int kIdSave = 106;
        const int kIdClose = 107;
        const int kIdStatus = 108;

        // In 96-DPI pixels: the list on the left, the editor on the right, the buttons along the
        // bottom of each.
        const int kMargin = 12;
        const int kClientWidth = 760;
        const int kClientHeight = 470;
        const int kListWidth = 320;
        const int kEditorX = kMargin + kListWidth + 16;
        const int kEditorWidth = kClientWidth - kEditorX - kMargin;
        const int kButtonsY = kClientHeight - kMargin - 28;

        // An edit control wants \r\n; a cheat keeps \n.
        std::wstring ForEdit(const std::string& text) {
            std::wstring out;
            for (wchar_t c : Widen(text)) {
                if (c == L'\n')
                    out += L'\r';
                out += c;
            }
            return out;
        }

        std::string FromEdit(HWND edit) {
            const int length = GetWindowTextLengthW(edit);
            std::wstring text(static_cast<size_t>(length) + 1, L'\0');
            GetWindowTextW(edit, text.data(), length + 1);
            text.resize(static_cast<size_t>(length));
            std::wstring out;
            for (wchar_t c : text) {
                if (c != L'\r')
                    out += c;
            }
            return Narrow(out);
        }

    }   // namespace

    CheatsWindow::~CheatsWindow() {
        if (window_ != nullptr && IsWindow(window_))
            DestroyWindow(window_);
        for (HFONT font : { font_, bold_font_, code_font_ }) {
            if (font != nullptr)
                DeleteObject(font);
        }
    }

    bool CheatsWindow::Create(HINSTANCE instance, HWND owner, Host host) {
        host_ = std::move(host);

        INITCOMMONCONTROLSEX controls = { sizeof(controls),
                                          ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES };
        InitCommonControlsEx(&controls);

        WNDCLASSEXW window_class = {};
        window_class.cbSize = sizeof(window_class);
        if (!GetClassInfoExW(instance, kWindowClass, &window_class)) {
            window_class = {};
            window_class.cbSize = sizeof(window_class);
            window_class.lpfnWndProc = WindowProc;
            window_class.hInstance = instance;
            window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            window_class.hIcon = AppIcon(instance);
            window_class.hIconSm = AppIconSmall(instance);
            window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
            window_class.lpszClassName = kWindowClass;
            if (RegisterClassExW(&window_class) == 0)
                return false;
        }

        const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
        window_ = CreateWindowExW(0, kWindowClass, L"PSXEmu - Cheats", style, CW_USEDEFAULT,
                                  CW_USEDEFAULT, 100, 100, owner, nullptr, instance, this);
        if (window_ == nullptr)
            return false;
        dpi_ = static_cast<int>(GetDpiForWindow(window_));
        if (dpi_ <= 0)
            dpi_ = 96;
        RECT bounds = { 0, 0, Scale(kClientWidth), Scale(kClientHeight) };
        AdjustWindowRectExForDpi(&bounds, style, FALSE, 0, static_cast<UINT>(dpi_));
        SetWindowPos(window_, nullptr, 0, 0, bounds.right - bounds.left, bounds.bottom - bounds.top,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

        NONCLIENTMETRICSW metrics = { sizeof(metrics) };
        if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0,
                                       static_cast<UINT>(dpi_))) {
            font_ = CreateFontIndirectW(&metrics.lfMessageFont);
            LOGFONTW bold = metrics.lfMessageFont;
            bold.lfWeight = FW_BOLD;
            bold_font_ = CreateFontIndirectW(&bold);
            // Codes line up in columns in a fixed-width font, which is how they are printed.
            LOGFONTW mono = metrics.lfMessageFont;
            wcscpy_s(mono.lfFaceName, L"Consolas");
            mono.lfHeight = mono.lfHeight * 110 / 100;
            code_font_ = CreateFontIndirectW(&mono);
        }

        auto make = [&](DWORD ex_style, const wchar_t* cls, const wchar_t* text, DWORD control_style,
                        int id, int x, int y, int w, int h, HFONT font) {
            HWND control = CreateWindowExW(
                ex_style, cls, text, WS_CHILD | WS_VISIBLE | control_style, Scale(x), Scale(y),
                Scale(w), Scale(h), window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                instance, nullptr);
            if (control != nullptr && font != nullptr)
                SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
            return control;
        };

        heading_ = make(0, L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, 0, kMargin, 12,
                        kClientWidth - kMargin * 2, 20, bold_font_);

        list_ = make(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                     LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER | WS_TABSTOP,
                     kIdList, kMargin, 40, kListWidth, kButtonsY - 40 - 10, font_);
        ListView_SetExtendedListViewStyle(list_, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT |
                                                     LVS_EX_DOUBLEBUFFER);
        LVCOLUMNW column = {};
        column.mask = LVCF_TEXT | LVCF_WIDTH;
        column.cx = Scale(kListWidth - 80);
        column.pszText = const_cast<wchar_t*>(L"Cheat");
        ListView_InsertColumn(list_, 0, &column);
        column.cx = Scale(54);
        column.pszText = const_cast<wchar_t*>(L"Lines");
        ListView_InsertColumn(list_, 1, &column);

        new_ = make(0, L"BUTTON", L"&New", BS_PUSHBUTTON | WS_TABSTOP, kIdNew, kMargin, kButtonsY,
                    96, 28, font_);
        delete_ = make(0, L"BUTTON", L"&Delete", BS_PUSHBUTTON | WS_TABSTOP, kIdDelete,
                       kMargin + 102, kButtonsY, 96, 28, font_);
        import_ = make(0, L"BUTTON", L"&Import...", BS_PUSHBUTTON | WS_TABSTOP, kIdImport,
                       kMargin + 204, kButtonsY, 116, 28, font_);

        make(0, L"STATIC", L"Name:", SS_LEFT, 0, kEditorX, 42, kEditorWidth, 18, font_);
        name_ = make(WS_EX_CLIENTEDGE, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, kIdName,
                     kEditorX, 60, kEditorWidth, 24, font_);
        make(0, L"STATIC", L"GameShark code, one line each - like 80012345 0063:", SS_LEFT, 0,
             kEditorX, 94, kEditorWidth, 18, font_);
        code_ = make(WS_EX_CLIENTEDGE, L"EDIT", L"",
                     ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL | WS_TABSTOP,
                     kIdCode, kEditorX, 112, kEditorWidth, 250, code_font_);
        status_ = make(0, L"STATIC", L"", SS_LEFT, kIdStatus, kEditorX, 368, kEditorWidth, 52,
                       font_);
        save_ = make(0, L"BUTTON", L"&Save Cheat", BS_DEFPUSHBUTTON | WS_TABSTOP, kIdSave, kEditorX,
                     kButtonsY, 120, 28, font_);
        make(0, L"BUTTON", L"Close", BS_PUSHBUTTON | WS_TABSTOP, kIdClose,
             kClientWidth - kMargin - 100, kButtonsY, 100, 28, font_);
        return true;
    }

    void CheatsWindow::Show() {
        if (window_ == nullptr)
            return;
        Refresh(editing_);
        ShowWindow(window_, SW_SHOW);
        SetForegroundWindow(window_);
    }

    void CheatsWindow::OnCheatsChanged(int select) {
        if (window_ != nullptr && IsWindowVisible(window_))
            Refresh(select == -2 ? editing_ : select);
    }

    void CheatsWindow::Refresh(int select) {
        const GameScope game = host_.game ? host_.game() : GameScope();
        const std::vector<Cheat>& cheats = host_.cheats();
        const std::wstring heading =
            game.running ? game.name
                         : std::wstring(L"Start a game to see and add its cheats.");
        SetWindowTextW(heading_, heading.c_str());
        const std::wstring caption = game.running ? L"PSXEmu - Cheats - " + game.name
                                                  : std::wstring(L"PSXEmu - Cheats");
        SetWindowTextW(window_, caption.c_str());

        filling_ = true;
        ListView_DeleteAllItems(list_);
        for (size_t i = 0; i < cheats.size(); ++i) {
            std::wstring name = Widen(cheats[i].name);
            LVITEMW item = {};
            item.mask = LVIF_TEXT;
            item.iItem = static_cast<int>(i);
            item.pszText = name.data();
            ListView_InsertItem(list_, &item);
            std::wstring lines = std::to_wstring(cheats[i].lines.size());
            ListView_SetItemText(list_, static_cast<int>(i), 1, lines.data());
            ListView_SetCheckState(list_, static_cast<int>(i), cheats[i].enabled);
        }
        filling_ = false;

        for (HWND control : { list_, new_, import_, name_, code_, save_ })
            EnableWindow(control, game.running);
        if (select >= static_cast<int>(cheats.size()))
            select = -1;
        ShowCheat(game.running ? select : -1);
    }

    void CheatsWindow::ShowCheat(int index) {
        editing_ = index;
        const std::vector<Cheat>& cheats = host_.cheats();
        filling_ = true;
        if (index >= 0) {
            ListView_SetItemState(list_, index, LVIS_SELECTED | LVIS_FOCUSED,
                                  LVIS_SELECTED | LVIS_FOCUSED);
            ListView_EnsureVisible(list_, index, FALSE);
            SetWindowTextW(name_, Widen(cheats[static_cast<size_t>(index)].name).c_str());
            SetWindowTextW(code_, ForEdit(cheats[static_cast<size_t>(index)].code).c_str());
            SetWindowTextW(save_, L"&Save Cheat");
        } else {
            ListView_SetItemState(list_, -1, 0, LVIS_SELECTED);
            SetWindowTextW(name_, L"");
            SetWindowTextW(code_, L"");
            SetWindowTextW(save_, L"&Add Cheat");
        }
        filling_ = false;
        EnableWindow(delete_, index >= 0);
        const size_t on = static_cast<size_t>(
            std::count_if(cheats.begin(), cheats.end(), [](const Cheat& c) { return c.enabled; }));
        SetStatus(cheats.empty()
                      ? L"No cheats yet. Type one in and add it, or import a DuckStation or "
                        L"RetroArch cheat file."
                      : std::to_wstring(on) + L" of " + std::to_wstring(cheats.size()) +
                            L" on. Ticking one applies it at once, every frame.",
                  false);
    }

    void CheatsWindow::SetStatus(const std::wstring& text, bool error) {
        status_error_ = error;
        SetWindowTextW(status_, text.c_str());
        InvalidateRect(status_, nullptr, TRUE);
    }

    LRESULT CALLBACK CheatsWindow::WindowProc(HWND window, UINT message, WPARAM wparam,
                                              LPARAM lparam) {
        if (message == WM_NCCREATE) {
            const CREATESTRUCTW* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            SetWindowLongPtrW(window, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        }
        CheatsWindow* self =
            reinterpret_cast<CheatsWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (self == nullptr)
            return DefWindowProcW(window, message, wparam, lparam);

        switch (message) {
            case WM_CTLCOLORSTATIC: {
                HDC dc = reinterpret_cast<HDC>(wparam);
                const bool error = reinterpret_cast<HWND>(lparam) == self->status_ &&
                                   self->status_error_;
                SetTextColor(dc, error ? RGB(192, 0, 0) : GetSysColor(COLOR_BTNTEXT));
                SetBkColor(dc, GetSysColor(COLOR_BTNFACE));
                return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));
            }

            case WM_NOTIFY: {
                const NMHDR* header = reinterpret_cast<const NMHDR*>(lparam);
                if (header->idFrom != kIdList || header->code != LVN_ITEMCHANGED || self->filling_)
                    break;
                const NMLISTVIEW* change = reinterpret_cast<const NMLISTVIEW*>(lparam);
                if (change->iItem < 0 || (change->uChanged & LVIF_STATE) == 0)
                    break;
                // The check box: the cheat on or off, at once.
                if (((change->uNewState ^ change->uOldState) & LVIS_STATEIMAGEMASK) != 0 &&
                    (change->uOldState & LVIS_STATEIMAGEMASK) != 0) {
                    const bool on = ListView_GetCheckState(self->list_, change->iItem) != 0;
                    if (self->host_.set_enabled)
                        self->host_.set_enabled(change->iItem, on);
                    return 0;
                }
                // A new row chosen: into the editor.
                if ((change->uNewState & LVIS_SELECTED) != 0 &&
                    (change->uOldState & LVIS_SELECTED) == 0 && change->iItem != self->editing_)
                    self->ShowCheat(change->iItem);
                return 0;
            }

            case WM_COMMAND: {
                const int id = LOWORD(wparam);
                if (HIWORD(wparam) != BN_CLICKED)
                    break;
                switch (id) {
                    case kIdNew:
                        self->ShowCheat(-1);
                        SetFocus(self->name_);
                        break;
                    case kIdDelete:
                        if (self->editing_ >= 0 && self->host_.remove) {
                            const int index = self->editing_;
                            self->editing_ = -1;
                            self->host_.remove(index);
                        }
                        break;
                    case kIdImport:
                        if (self->host_.import)
                            self->host_.import();
                        break;
                    case kIdSave: {
                        if (!self->host_.save)
                            break;
                        const std::string name = FromEdit(self->name_);
                        const std::string code = FromEdit(self->code_);
                        const int index = self->editing_;
                        const std::string error = self->host_.save(index, name, code);
                        if (!error.empty()) {
                            self->SetStatus(Widen(error), true);
                            SetFocus(self->code_);
                        }
                        break;
                    }
                    case kIdClose:
                        SendMessageW(window, WM_CLOSE, 0, 0);
                        break;
                }
                return 0;
            }

            case WM_CLOSE:
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
