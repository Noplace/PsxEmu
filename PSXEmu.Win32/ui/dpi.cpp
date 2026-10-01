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
#include "ui/dpi.h"

#include <algorithm>
#include <commctrl.h>
#pragma comment(lib, "comctl32.lib")

namespace psxemu {

    namespace {

        bool IsClass(HWND window, const wchar_t* name) {
            wchar_t text[64] = {};
            return GetClassNameW(window, text, static_cast<int>(std::size(text))) > 0 &&
                   _wcsicmp(text, name) == 0;
        }

        // Moves and sizes each child of `parent` by to/from, edges rather than sizes scaled so
        // controls that touched still do.
        void ScaleChildren(HWND parent, int from, int to,
                           const std::function<bool(HWND)>& descend) {
            for (HWND child = GetWindow(parent, GW_CHILD); child != nullptr;
                 child = GetWindow(child, GW_HWNDNEXT)) {
                RECT rect = {};
                GetWindowRect(child, &rect);
                MapWindowPoints(HWND_DESKTOP, parent, reinterpret_cast<POINT*>(&rect), 2);
                // A drop-down list's window is as tall as the list it drops, which GetWindowRect
                // does not say: keep that, not the height of the closed box.
                if (IsClass(child, WC_COMBOBOXW)) {
                    RECT dropped = {};
                    if (SendMessageW(child, CB_GETDROPPEDCONTROLRECT, 0,
                                     reinterpret_cast<LPARAM>(&dropped)))
                        rect.bottom = rect.top + (dropped.bottom - dropped.top);
                }
                const int left = MulDiv(rect.left, to, from);
                const int top = MulDiv(rect.top, to, from);
                SetWindowPos(child, nullptr, left, top, MulDiv(rect.right, to, from) - left,
                             MulDiv(rect.bottom, to, from) - top,
                             SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW);
                if (descend && descend(child))
                    ScaleChildren(child, from, to, descend);
            }
        }

    }   // namespace

    int WindowDpi(HWND window) {
        const UINT dpi = window != nullptr ? GetDpiForWindow(window) : 0;
        return dpi == 0 ? 96 : static_cast<int>(dpi);
    }

    HFONT CreateMessageFont(int dpi, bool bold, int percent) {
        NONCLIENTMETRICSW metrics = { sizeof(metrics) };
        if (!SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0,
                                        static_cast<UINT>(dpi)))
            return nullptr;
        LOGFONTW font = metrics.lfMessageFont;
        if (bold)
            font.lfWeight = FW_BOLD;
        font.lfHeight = font.lfHeight * percent / 100;
        return CreateFontIndirectW(&font);
    }

    void SwapFonts(HWND window, std::initializer_list<FontSwap> swaps) {
        struct Walk {
            std::initializer_list<FontSwap> swaps;
        } walk = { swaps };
        EnumChildWindows(
            window,
            [](HWND child, LPARAM param) -> BOOL {
                const Walk* walk = reinterpret_cast<const Walk*>(param);
                const HFONT font = reinterpret_cast<HFONT>(SendMessageW(child, WM_GETFONT, 0, 0));
                for (const FontSwap& swap : walk->swaps) {
                    if (font != nullptr && font == swap.first && swap.second != nullptr) {
                        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(swap.second),
                                     FALSE);
                        break;
                    }
                }
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&walk));
    }

    void ScaleListColumns(HWND window, int from, int to) {
        for (HWND child = GetWindow(window, GW_CHILD); child != nullptr;
             child = GetWindow(child, GW_HWNDNEXT)) {
            if (!IsClass(child, WC_LISTVIEWW))
                continue;
            const HWND header = ListView_GetHeader(child);
            const int columns = header != nullptr ? Header_GetItemCount(header) : 0;
            for (int c = 0; c < columns; ++c) {
                ListView_SetColumnWidth(child, c,
                                        MulDiv(ListView_GetColumnWidth(child, c), to, from));
            }
        }
    }

    void MoveForDpi(HWND window, int dpi, int x, int y, int width, int height) {
        const int left = ScaleForDpi(x, dpi);
        const int top = ScaleForDpi(y, dpi);
        MoveWindow(window, left, top, ScaleForDpi(x + width, dpi) - left,
                   ScaleForDpi(y + height, dpi) - top, TRUE);
    }

    void SizeForDpi(HWND window, int width, int height) {
        const int dpi = WindowDpi(window);
        RECT bounds = {};
        GetWindowRect(window, &bounds);
        bounds.right = bounds.left + ScaleForDpi(width, dpi);
        bounds.bottom = bounds.top + ScaleForDpi(height, dpi);
        // On its monitor's work area: no bigger, and moved back onto it if it hangs off - a size
        // meant for a big screen, on a small or very scaled one.
        MONITORINFO monitor = { sizeof(monitor) };
        if (GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor)) {
            const RECT& work = monitor.rcWork;
            bounds.right = bounds.left + std::min(bounds.right - bounds.left,
                                                  static_cast<LONG>(work.right - work.left));
            bounds.bottom = bounds.top + std::min(bounds.bottom - bounds.top,
                                                  static_cast<LONG>(work.bottom - work.top));
            const LONG dx = std::max(work.left - bounds.left,
                                     std::min(0L, work.right - bounds.right));
            const LONG dy = std::max(work.top - bounds.top,
                                     std::min(0L, work.bottom - bounds.bottom));
            OffsetRect(&bounds, dx, dy);
        }
        SetWindowPos(window, nullptr, bounds.left, bounds.top, bounds.right - bounds.left,
                     bounds.bottom - bounds.top, SWP_NOZORDER | SWP_NOACTIVATE);
    }

    void MoveToSuggested(HWND window, const RECT& suggested) {
        SetWindowPos(window, nullptr, suggested.left, suggested.top,
                     suggested.right - suggested.left, suggested.bottom - suggested.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }

    void RescaleForDpi(HWND window, int from, int to, const RECT& suggested,
                       const std::function<bool(HWND)>& descend) {
        if (from <= 0 || to <= 0 || from == to)
            return;
        RECT client = {};
        GetClientRect(window, &client);
        ScaleChildren(window, from, to, descend);
        ScaleListColumns(window, from, to);

        // The client area scaled exactly, rather than the suggested size, which scales the frame
        // too and so is a pixel or two out; only its position is taken.
        RECT bounds = { 0, 0, MulDiv(client.right, to, from), MulDiv(client.bottom, to, from) };
        AdjustWindowRectExForDpi(&bounds, static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE)),
                                 GetMenu(window) != nullptr,
                                 static_cast<DWORD>(GetWindowLongPtrW(window, GWL_EXSTYLE)),
                                 static_cast<UINT>(to));
        SetWindowPos(window, nullptr, suggested.left, suggested.top, bounds.right - bounds.left,
                     bounds.bottom - bounds.top, SWP_NOZORDER | SWP_NOACTIVATE);
        RedrawWindow(window, nullptr, nullptr,
                     RDW_ERASE | RDW_FRAME | RDW_INVALIDATE | RDW_ALLCHILDREN);
    }

}   // namespace psxemu
