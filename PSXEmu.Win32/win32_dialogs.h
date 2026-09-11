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

// The two things the front end puts in front of a person that are not the emulated picture: a file
// picker, and a message box saying something went wrong.
//
// Both are here rather than in the app because neither needs to know anything about it - a picker
// needs a filter and an owner window, and a message box needs text. What is worth saying is said at
// the call site; this only owns the fact that all of them are titled the same and that the four
// pickers differ by two flags.

#include "framework.h"

namespace psxemu {

    // ---------------------------------------------------------------------------------------------
    // Message boxes
    // ---------------------------------------------------------------------------------------------

    // Titled kWindowTitle, because every one of them is. `owner` may be null, which is what the
    // failures before there is a window to own them pass.
    //
    // An error is something that stopped: the BIOS would not load, the state would not save. A
    // warning is something that carried on anyway: the disc would not read, so nothing was mounted;
    // the preferred renderer was not there, so the other one is running.
    void ShowError(HWND owner, const wchar_t* message);
    void ShowWarning(HWND owner, const wchar_t* message);

    // ---------------------------------------------------------------------------------------------
    // File pickers
    // ---------------------------------------------------------------------------------------------

    enum class FileDialog { kOpen, kSave };

    // One implementation for all four file pickers. There used to be a copy of this per dialog,
    // differing only in the filter and two flags. `filter` is one of the k*Filter strings in
    // const.h; `default_extension` may be null. Empty if the person cancelled.
    std::string ChooseFile(HWND window, FileDialog mode, const char* filter,
                           const char* default_extension);

}   // namespace psxemu
