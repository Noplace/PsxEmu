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

// The application icon (Resource\app_icon.ico, compiled in by psxemu.rc), for every window class the
// front end registers: the title bar, the taskbar and Alt+Tab all take it from the class.
//
// The .ico holds a single 256x256 image. LoadIconWithScaleDown makes the 16- and 32-pixel sizes
// from it with proper filtering, where LoadIcon would shrink it crudely. It is a Common Controls 6
// function, which the manifest dependency in memcard_editor.cpp brings in.

#include "framework.h"
#include "resource.h"

#include <commctrl.h>
#pragma comment(lib, "comctl32.lib")

namespace psxemu {

    namespace detail {
        inline HICON LoadAppIconAt(HINSTANCE instance, int width_metric, int height_metric) {
            HICON icon = nullptr;
            if (FAILED(LoadIconWithScaleDown(instance, MAKEINTRESOURCEW(IDI_APP_ICON),
                                             GetSystemMetrics(width_metric),
                                             GetSystemMetrics(height_metric), &icon)))
                icon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP_ICON));
            return icon;
        }
    }   // namespace detail

    // Loaded once each and kept for the life of the process, like any class icon.
    inline HICON AppIcon(HINSTANCE instance) {
        static const HICON icon = detail::LoadAppIconAt(instance, SM_CXICON, SM_CYICON);
        return icon;
    }

    inline HICON AppIconSmall(HINSTANCE instance) {
        static const HICON icon = detail::LoadAppIconAt(instance, SM_CXSMICON, SM_CYSMICON);
        return icon;
    }

}   // namespace psxemu
