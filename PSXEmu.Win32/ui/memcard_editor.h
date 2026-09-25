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

// File > Memory Cards > Memory Card Editor: both slots side by side - what is on each card, with
// icon and title, and delete, undelete, export and import of single saves, copy between slots,
// and format.
//
// The cards belong to the machine's thread (Docs/Threading-Plan.md, rule 4), so this window never
// touches one. It is shown snapshots - copies of both 128 KB images, taken on the machine thread -
// and asks for every change through Host::edit, which runs it against the live card there. That
// is also what makes an edit safe under a running game: the change lands between frames, and the
// card is flagged as swapped so the game reads its directory again. The parsing is the core's,
// psx/mc_directory.h.

#include "framework.h"

#include "psx/mc_directory.h"

#include <commctrl.h>

#include <array>
#include <functional>

namespace psxemu {

    class MemoryCardEditor {
     public:
        struct Snapshot {
            bool inserted = false;
            std::string filename;
            std::vector<uint8_t> image;   // 128 KB, empty with no card in
        };

        // A change to one card: given the live image, change it or say why not.
        typedef std::function<bool(uint8_t* card, std::string* error)> Edit;

        struct Host {
            std::function<void()> refresh;               // ask for fresh snapshots
            std::function<void(int card, Edit edit)> edit;   // card = port * 4 + slot
            std::function<void()> on_closed;
        };

        MemoryCardEditor() = default;
        ~MemoryCardEditor();

        MemoryCardEditor(const MemoryCardEditor&) = delete;
        MemoryCardEditor& operator=(const MemoryCardEditor&) = delete;

        bool Create(HINSTANCE instance, HWND owner, Host host);
        void Show(bool on);
        bool visible() const;

        // Every card slot there is (bug 99): two ports of four, a port's own card first and
        // then the three sockets a multitap adds - card = port * 4 + slot. Each pane shows the
        // one its selector picks, which starts as Port 1 and Port 2's own cards, the two this
        // editor always showed.
        static const int kCards = 8;

        // Fresh snapshots of every slot. Redraws a pane only if its card changed, so the
        // once-a-second refresh does not flicker or lose the selection.
        void SetCards(const std::array<Snapshot, kCards>& cards);

     private:
        static const int kSlots = 2;
        enum Button { kDelete, kUndelete, kExport, kImport, kCopy, kFormat, kButtonCount };

        struct Pane {
            HWND selector = nullptr;   // which of the eight cards this pane shows
            int which = 0;
            HWND label = nullptr;
            HWND list = nullptr;
            HWND buttons[kButtonCount] = {};
            HIMAGELIST icons = nullptr;
            Snapshot card;
            std::vector<emulation::psx::mcdir::Save> saves;   // in list order
        };

        static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam,
                                           LPARAM lparam);
        void Layout(int width, int height);
        void Fill(int slot);
        void UpdateButtons();
        void OnButton(int slot, Button button);
        const emulation::psx::mcdir::Save* Selected(int slot) const;

        HWND window_ = nullptr;
        HWND show_deleted_ = nullptr;
        HWND refresh_ = nullptr;
        HFONT font_ = nullptr;
        Pane panes_[kSlots];
        std::array<Snapshot, kCards> cards_;
        Host host_;
    };

}   // namespace psxemu
