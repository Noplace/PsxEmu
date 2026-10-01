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
#pragma once

// Per-monitor DPI for the front end's windows.
//
// The process is per-monitor aware (version 2, set first thing in wWinMain), so Windows never
// stretches a window's pixels: each window is told its monitor's DPI and lays itself out for it,
// and is sent WM_DPICHANGED when it moves to a monitor with another. Every layout here is written
// in pixels at 96 DPI - 100% scaling - and scaled by the window's DPI as it is made.
//
// Two kinds of window use this. One laid out once, at creation (the settings windows), answers
// WM_DPICHANGED with RescaleForDpi: every control moved and sized by the ratio, its fonts swapped
// for ones made at the new DPI. One that lays itself out on WM_SIZE (the debugger, the memory card
// editor) only needs its fonts swapped and its list columns scaled; the new size it is given lays
// it out again.

#include "app/framework.h"

#include <functional>
#include <initializer_list>
#include <utility>

namespace psxemu {

    // The DPI a window is shown at, 96 if it cannot say.
    int WindowDpi(HWND window);

    // `value` pixels at 96 DPI, at `dpi`.
    inline int ScaleForDpi(int value, int dpi) { return MulDiv(value, dpi, 96); }

    // The system's message font - what dialogs use - at `dpi`: bold or not, and `percent` of its
    // size. nullptr if the system will not say what it is.
    HFONT CreateMessageFont(int dpi, bool bold = false, int percent = 100);

    // Each font pair is (old, new): every control under `window` using an old font is given the
    // new one.
    using FontSwap = std::pair<HFONT, HFONT>;
    void SwapFonts(HWND window, std::initializer_list<FontSwap> swaps);

    // Every report list view directly under `window`, its column widths scaled by to/from.
    void ScaleListColumns(HWND window, int from, int to);

    // MoveWindow, the rect given in pixels at 96 DPI and placed at `dpi`: edges rather than sizes
    // scaled, so controls that touch at 96 DPI still touch.
    void MoveForDpi(HWND window, int dpi, int x, int y, int width, int height);

    // Sizes a window `width` x `height` - frame and all - in pixels at 96 DPI, at its own DPI,
    // and no bigger than its monitor's work area, onto which it is moved if it hangs off.
    void SizeForDpi(HWND window, int width, int height);

    // WM_DPICHANGED for a window that lays itself out on WM_SIZE: placed and sized as `suggested`
    // (the message's lParam) says, which keeps it the same size to the eye.
    void MoveToSuggested(HWND window, const RECT& suggested);

    // WM_DPICHANGED for a window whose layout is fixed at creation: its client area and every
    // control on it scaled by to/from, the window placed where `suggested` (the message's lParam)
    // puts it. `descend` says which controls have controls of their own to scale with them - a
    // canvas of buttons, say.
    void RescaleForDpi(HWND window, int from, int to, const RECT& suggested,
                       const std::function<bool(HWND)>& descend = {});

}   // namespace psxemu
