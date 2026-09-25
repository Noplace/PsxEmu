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
#include "memcard_editor.h"

#include "const.h"
#include "win32_dialogs.h"

#include <commctrl.h>
#pragma comment(lib, "comctl32.lib")
// Version 6 of the common controls, so the list and the buttons look like the rest of Windows.
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace psxemu {

    namespace mcdir = emulation::psx::mcdir;

    namespace {

        const wchar_t kEditorClass[] = L"PSXEmuMemoryCardEditor";
        const UINT_PTR kRefreshTimer = 1;
        const UINT kRefreshMs = 1000;

        // Control ids: a block of sixteen per slot, then the shared ones.
        const int kIdSlotBase = 1000;
        const int kIdsPerSlot = 16;
        const int kIdList = 15;   // within a slot's block
        const int kIdSelector = 13;
        const int kIdShowDeleted = 2000;
        const int kIdRefresh = 2001;

        const int kIconPixels = 32;   // the card's 16x16, doubled

        const wchar_t* const kButtonLabels[] = {
            L"Delete", L"Undelete", L"Export...", L"Import...", L"Copy to %s", L"Format...",
        };

        std::wstring Widen(const std::string& text) {
            if (text.empty())
                return std::wstring();
            const int count = MultiByteToWideChar(CP_ACP, 0, text.data(),
                                                  static_cast<int>(text.size()), nullptr, 0);
            std::wstring wide(static_cast<size_t>(count), L'\0');
            MultiByteToWideChar(CP_ACP, 0, text.data(), static_cast<int>(text.size()), &wide[0],
                                count);
            return wide;
        }

        // One icon frame, doubled to 32x32, as a 32-bit bitmap an image list takes with its
        // alpha. Transparent pixels are 0 in all four channels, which is premultiplied already.
        HBITMAP IconBitmap(const uint32_t* pixels) {
            BITMAPINFO info = {};
            info.bmiHeader.biSize = sizeof(info.bmiHeader);
            info.bmiHeader.biWidth = kIconPixels;
            info.bmiHeader.biHeight = -kIconPixels;   // top-down
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            info.bmiHeader.biCompression = BI_RGB;
            void* bits = nullptr;
            HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
            if (bitmap == nullptr || bits == nullptr)
                return bitmap;
            uint32_t* out = static_cast<uint32_t*>(bits);
            for (int y = 0; y < kIconPixels; ++y) {
                for (int x = 0; x < kIconPixels; ++x) {
                    out[y * kIconPixels + x] =
                        pixels ? pixels[(y / 2) * mcdir::kIconSize + (x / 2)] : 0;
                }
            }
            return bitmap;
        }

        bool WriteBytes(const std::string& path, const std::vector<uint8_t>& bytes) {
            FILE* fp = fopen(path.c_str(), "wb");
            if (!fp)
                return false;
            const size_t written = fwrite(bytes.data(), 1, bytes.size(), fp);
            return fclose(fp) == 0 && written == bytes.size();
        }

        bool ReadBytes(const std::string& path, std::vector<uint8_t>* bytes) {
            FILE* fp = fopen(path.c_str(), "rb");
            if (!fp)
                return false;
            fseek(fp, 0, SEEK_END);
            const long size = ftell(fp);
            fseek(fp, 0, SEEK_SET);
            if (size <= 0 || size > static_cast<long>(mcdir::kCardSize)) {
                fclose(fp);
                return false;
            }
            bytes->resize(static_cast<size_t>(size));
            const size_t read = fread(bytes->data(), 1, bytes->size(), fp);
            fclose(fp);
            return read == bytes->size();
        }

        // What the selector calls a card: its port, and which socket - A is the port's own.
        std::wstring CardName(int card) {
            const wchar_t letter = static_cast<wchar_t>(L'A' + card % 4);
            std::wstring name = L"Port " + std::to_wstring(card / 4 + 1) + L", Card ";
            name += letter;
            if (card % 4 != 0)
                name += L" (multitap)";
            return name;
        }

    }   // namespace

    MemoryCardEditor::~MemoryCardEditor() {
        if (window_ != nullptr && IsWindow(window_))
            DestroyWindow(window_);
        for (Pane& pane : panes_) {
            if (pane.icons != nullptr)
                ImageList_Destroy(pane.icons);
        }
        if (font_ != nullptr)
            DeleteObject(font_);
    }

    bool MemoryCardEditor::Create(HINSTANCE instance, HWND owner, Host host) {
        host_ = std::move(host);

        INITCOMMONCONTROLSEX controls = { sizeof(controls), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES };
        InitCommonControlsEx(&controls);

        WNDCLASSEXW window_class = {};
        window_class.cbSize = sizeof(window_class);
        if (!GetClassInfoExW(instance, kEditorClass, &window_class)) {
            window_class = {};
            window_class.cbSize = sizeof(window_class);
            window_class.lpfnWndProc = WindowProc;
            window_class.hInstance = instance;
            window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
            window_class.lpszClassName = kEditorClass;
            if (RegisterClassExW(&window_class) == 0)
                return false;
        }

        window_ = CreateWindowExW(0, kEditorClass, L"PSXEmu - Memory Card Editor",
                                  WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1020, 560,
                                  owner, nullptr, instance, this);
        if (window_ == nullptr)
            return false;

        NONCLIENTMETRICSW metrics = { sizeof(metrics) };
        if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0))
            font_ = CreateFontIndirectW(&metrics.lfMessageFont);

        auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
            HWND control = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0,
                                           window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                           instance, nullptr);
            if (control != nullptr && font_ != nullptr)
                SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font_), FALSE);
            return control;
        };

        show_deleted_ = make(L"BUTTON", L"Show deleted saves", BS_AUTOCHECKBOX | WS_TABSTOP,
                             kIdShowDeleted);
        refresh_ = make(L"BUTTON", L"Refresh", BS_PUSHBUTTON | WS_TABSTOP, kIdRefresh);

        for (int slot = 0; slot < kSlots; ++slot) {
            Pane& pane = panes_[slot];
            const int base = kIdSlotBase + slot * kIdsPerSlot;
            // Left pane Port 1's own card, right pane Port 2's, as this has always opened.
            pane.which = (slot == 0) ? 0 : 4;
            pane.selector = make(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
                                 base + kIdSelector);
            for (int card = 0; card < kCards; ++card)
                SendMessageW(pane.selector, CB_ADDSTRING, 0,
                             reinterpret_cast<LPARAM>(CardName(card).c_str()));
            SendMessageW(pane.selector, CB_SETCURSEL, pane.which, 0);
            pane.label = make(L"STATIC", L"", SS_LEFT | SS_ENDELLIPSIS, base + 14);
            pane.list = make(WC_LISTVIEWW, L"",
                             LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_BORDER | WS_TABSTOP,
                             base + kIdList);
            ListView_SetExtendedListViewStyle(pane.list,
                                              LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
            pane.icons = ImageList_Create(kIconPixels, kIconPixels, ILC_COLOR32, 16, 16);
            ListView_SetImageList(pane.list, pane.icons, LVSIL_SMALL);

            const struct { const wchar_t* title; int width; } columns[] = {
                { L"Title", 230 }, { L"Save", 150 }, { L"Blocks", 50 }, { L"", 60 },
            };
            for (int c = 0; c < 4; ++c) {
                LVCOLUMNW column = {};
                column.mask = LVCF_TEXT | LVCF_WIDTH;
                column.pszText = const_cast<wchar_t*>(columns[c].title);
                column.cx = columns[c].width;
                ListView_InsertColumn(pane.list, c, &column);
            }

            for (int b = 0; b < kButtonCount; ++b) {
                wchar_t label[64];
                swprintf_s(label, kButtonLabels[b], slot == 0 ? L"Right" : L"Left");
                pane.buttons[b] = make(L"BUTTON", label, BS_PUSHBUTTON | WS_TABSTOP, base + b);
            }
        }

        RECT client = {};
        GetClientRect(window_, &client);
        Layout(client.right, client.bottom);
        UpdateButtons();
        return true;
    }

    void MemoryCardEditor::Show(bool on) {
        if (window_ == nullptr)
            return;
        if (on) {
            ShowWindow(window_, SW_SHOW);
            SetForegroundWindow(window_);
            SetTimer(window_, kRefreshTimer, kRefreshMs, nullptr);
            if (host_.refresh)
                host_.refresh();
        } else {
            KillTimer(window_, kRefreshTimer);
            ShowWindow(window_, SW_HIDE);
        }
    }

    bool MemoryCardEditor::visible() const {
        return window_ != nullptr && IsWindowVisible(window_);
    }

    void MemoryCardEditor::SetCards(const std::array<Snapshot, kCards>& cards) {
        cards_ = cards;
        for (int slot = 0; slot < kSlots; ++slot) {
            Pane& pane = panes_[slot];
            const Snapshot& card = cards_[pane.which];
            if (card.inserted == pane.card.inserted && card.filename == pane.card.filename &&
                card.image == pane.card.image)
                continue;
            pane.card = card;
            Fill(slot);
        }
        UpdateButtons();
    }

    void MemoryCardEditor::Fill(int slot) {
        Pane& pane = panes_[slot];

        // Keep the selection across a refresh, by the block the save starts at.
        int selected_block = 0;
        if (const mcdir::Save* save = Selected(slot))
            selected_block = save->first_block;

        SendMessageW(pane.list, WM_SETREDRAW, FALSE, 0);
        ListView_DeleteAllItems(pane.list);
        ImageList_RemoveAll(pane.icons);
        pane.saves.clear();

        // The selector already says which card this is; the label says what is in it.
        std::wstring label;
        const bool readable = pane.card.inserted &&
                              pane.card.image.size() == mcdir::kCardSize;
        if (!readable) {
            label += L"no card";
        } else {
            const uint8_t* image = pane.card.image.data();
            const bool deleted = SendMessageW(show_deleted_, BM_GETCHECK, 0, 0) == BST_CHECKED;
            pane.saves = mcdir::List(image, deleted);
            const size_t slash = pane.card.filename.find_last_of("\\/");
            label += Widen(slash == std::string::npos ? pane.card.filename
                                                      : pane.card.filename.substr(slash + 1));
            if (!mcdir::IsFormatted(image))
                label += L"  -  not formatted";
            else
                label += L"  -  " + std::to_wstring(mcdir::FreeBlocks(image)) + L" of 15 blocks free";

            for (size_t i = 0; i < pane.saves.size(); ++i) {
                const mcdir::Save& save = pane.saves[i];
                HBITMAP bitmap = IconBitmap(save.icon_frames > 0 ? save.icons.data() : nullptr);
                const int image_index = ImageList_Add(pane.icons, bitmap, nullptr);
                DeleteObject(bitmap);

                std::wstring title = save.title.empty() ? Widen(save.filename) : save.title;
                LVITEMW item = {};
                item.mask = LVIF_TEXT | LVIF_IMAGE | LVIF_PARAM;
                item.iItem = static_cast<int>(i);
                item.pszText = &title[0];
                item.iImage = image_index;
                item.lParam = static_cast<LPARAM>(i);
                ListView_InsertItem(pane.list, &item);

                std::wstring name = Widen(save.filename);
                ListView_SetItemText(pane.list, static_cast<int>(i), 1, &name[0]);
                std::wstring blocks = std::to_wstring(save.blocks);
                ListView_SetItemText(pane.list, static_cast<int>(i), 2, &blocks[0]);
                wchar_t state[] = L"deleted";
                if (save.deleted)
                    ListView_SetItemText(pane.list, static_cast<int>(i), 3, state);

                if (save.first_block == selected_block) {
                    ListView_SetItemState(pane.list, static_cast<int>(i),
                                          LVIS_SELECTED | LVIS_FOCUSED,
                                          LVIS_SELECTED | LVIS_FOCUSED);
                }
            }
        }
        SetWindowTextW(pane.label, label.c_str());
        SendMessageW(pane.list, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(pane.list, nullptr, TRUE);
    }

    const mcdir::Save* MemoryCardEditor::Selected(int slot) const {
        const Pane& pane = panes_[slot];
        const int index = ListView_GetNextItem(pane.list, -1, LVNI_SELECTED);
        if (index < 0)
            return nullptr;
        LVITEMW item = {};
        item.mask = LVIF_PARAM;
        item.iItem = index;
        if (!ListView_GetItem(pane.list, &item))
            return nullptr;
        const size_t at = static_cast<size_t>(item.lParam);
        return at < pane.saves.size() ? &pane.saves[at] : nullptr;
    }

    void MemoryCardEditor::UpdateButtons() {
        for (int slot = 0; slot < kSlots; ++slot) {
            const Pane& pane = panes_[slot];
            const bool inserted = pane.card.inserted;
            // Copying needs a card on the other side, and a different one: both panes can show
            // the same card.
            const bool other_inserted = panes_[1 - slot].card.inserted &&
                                        panes_[1 - slot].which != pane.which;
            const mcdir::Save* save = Selected(slot);
            EnableWindow(pane.buttons[kDelete], save != nullptr && !save->deleted);
            EnableWindow(pane.buttons[kUndelete], save != nullptr && save->deleted);
            EnableWindow(pane.buttons[kExport], save != nullptr);
            EnableWindow(pane.buttons[kImport], inserted);
            EnableWindow(pane.buttons[kCopy], save != nullptr && !save->deleted && other_inserted);
            EnableWindow(pane.buttons[kFormat], inserted);
        }
    }

    void MemoryCardEditor::OnButton(int slot, Button button) {
        const Pane& pane = panes_[slot];
        const mcdir::Save* save = Selected(slot);
        std::string error;

        switch (button) {
            case kDelete:
            case kUndelete: {
                if (save == nullptr)
                    return;
                const int block = save->first_block;
                const bool undo = (button == kUndelete);
                host_.edit(pane.which, [block, undo](uint8_t* card, std::string* e) {
                    return undo ? mcdir::Undelete(card, block, e) : mcdir::Delete(card, block, e);
                });
                break;
            }

            case kExport: {
                if (save == nullptr)
                    return;
                std::vector<uint8_t> mcs;
                if (!mcdir::Export(pane.card.image.data(), save->first_block, &mcs, &error)) {
                    ShowWarning(window_, Widen(error).c_str());
                    return;
                }
                const std::string path = ChooseFile(window_, FileDialog::kSave, kSaveFilter, "mcs");
                if (path.empty())
                    return;
                if (!WriteBytes(path, mcs))
                    ShowWarning(window_, L"Could not write that file.");
                break;
            }

            case kImport: {
                const std::string path = ChooseFile(window_, FileDialog::kOpen, kSaveFilter, "mcs");
                if (path.empty())
                    return;
                std::vector<uint8_t> mcs;
                if (!ReadBytes(path, &mcs)) {
                    ShowWarning(window_, L"Could not read that file.");
                    return;
                }
                host_.edit(pane.which, [mcs](uint8_t* card, std::string* e) {
                    return mcdir::Import(card, mcs, e);
                });
                break;
            }

            case kCopy: {
                if (save == nullptr)
                    return;
                std::vector<uint8_t> mcs;
                if (!mcdir::Export(pane.card.image.data(), save->first_block, &mcs, &error)) {
                    ShowWarning(window_, Widen(error).c_str());
                    return;
                }
                host_.edit(panes_[1 - slot].which, [mcs](uint8_t* card, std::string* e) {
                    return mcdir::Import(card, mcs, e);
                });
                break;
            }

            case kFormat: {
                const std::wstring question =
                    L"Format " + CardName(pane.which) +
                    L"?\n\nEvery save on it is erased, deleted ones included.";
                if (MessageBoxW(window_, question.c_str(), kWindowTitle,
                                MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) != IDOK)
                    return;
                host_.edit(pane.which, [](uint8_t* card, std::string*) {
                    mcdir::Format(card);
                    return true;
                });
                break;
            }

            default:
                break;
        }
    }

    void MemoryCardEditor::Layout(int width, int height) {
        const int margin = 10;
        const int bar = 26;           // the top row: show-deleted and refresh
        const int label_height = 26;   // the card selector sits on this row
        const int button_height = 28;
        const int pane_width = (width - margin * 3) / 2;

        MoveWindow(show_deleted_, margin, margin, 200, bar - 4, TRUE);
        MoveWindow(refresh_, width - margin - 90, margin - 2, 90, bar, TRUE);

        const int top = margin + bar + 6;
        const int list_top = top + label_height + 2;
        const int buttons_top = height - margin - button_height;
        const int list_height = buttons_top - 8 - list_top;
        for (int slot = 0; slot < kSlots; ++slot) {
            const Pane& pane = panes_[slot];
            const int x = margin + slot * (pane_width + margin);
            const int selector_width = 190;
            MoveWindow(pane.selector, x, top - 2, selector_width, 220, TRUE);
            MoveWindow(pane.label, x + selector_width + 8, top + 1, pane_width - selector_width - 8,
                       label_height, TRUE);
            MoveWindow(pane.list, x, list_top, pane_width, list_height > 50 ? list_height : 50,
                       TRUE);
            const int gap = 4;
            const int button_width = (pane_width - gap * (kButtonCount - 1)) / kButtonCount;
            for (int b = 0; b < kButtonCount; ++b) {
                MoveWindow(pane.buttons[b], x + b * (button_width + gap), buttons_top,
                           button_width, button_height, TRUE);
            }
        }
    }

    LRESULT CALLBACK MemoryCardEditor::WindowProc(HWND window, UINT message, WPARAM wparam,
                                                  LPARAM lparam) {
        if (message == WM_NCCREATE) {
            const CREATESTRUCTW* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            SetWindowLongPtrW(window, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        }
        MemoryCardEditor* self =
            reinterpret_cast<MemoryCardEditor*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (self == nullptr)
            return DefWindowProcW(window, message, wparam, lparam);

        switch (message) {
            case WM_SIZE:
                if (self->refresh_ != nullptr)
                    self->Layout(LOWORD(lparam), HIWORD(lparam));
                return 0;

            case WM_GETMINMAXINFO: {
                MINMAXINFO* limits = reinterpret_cast<MINMAXINFO*>(lparam);
                limits->ptMinTrackSize.x = 800;
                limits->ptMinTrackSize.y = 360;
                return 0;
            }

            case WM_TIMER:
                if (wparam == kRefreshTimer && self->host_.refresh)
                    self->host_.refresh();
                return 0;

            case WM_NOTIFY: {
                const NMHDR* header = reinterpret_cast<const NMHDR*>(lparam);
                if (header->code == LVN_ITEMCHANGED)
                    self->UpdateButtons();
                return 0;
            }

            case WM_COMMAND: {
                const int id = LOWORD(wparam);
                if (id == kIdRefresh) {
                    if (self->host_.refresh)
                        self->host_.refresh();
                } else if (id == kIdShowDeleted) {
                    for (int slot = 0; slot < kSlots; ++slot)
                        self->Fill(slot);
                    self->UpdateButtons();
                } else if (id >= kIdSlotBase && id < kIdSlotBase + kSlots * kIdsPerSlot) {
                    const int slot = (id - kIdSlotBase) / kIdsPerSlot;
                    const int button = (id - kIdSlotBase) % kIdsPerSlot;
                    if (button == kIdSelector) {
                        if (HIWORD(wparam) == CBN_SELCHANGE) {
                            Pane& pane = self->panes_[slot];
                            const LRESULT pick = SendMessageW(pane.selector, CB_GETCURSEL, 0, 0);
                            if (pick >= 0 && pick < kCards) {
                                pane.which = static_cast<int>(pick);
                                pane.card = self->cards_[pane.which];
                                self->Fill(slot);
                                self->UpdateButtons();
                            }
                        }
                    } else if (button < kButtonCount) {
                        self->OnButton(slot, static_cast<Button>(button));
                    }
                }
                return 0;
            }

            case WM_CLOSE:
                self->Show(false);
                if (self->host_.on_closed)
                    self->host_.on_closed();
                return 0;

            case WM_NCDESTROY:
                SetWindowLongPtrW(window, GWLP_USERDATA, 0);
                self->window_ = nullptr;
                break;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

}   // namespace psxemu
