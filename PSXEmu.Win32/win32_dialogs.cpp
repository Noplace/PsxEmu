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
#include "win32_dialogs.h"

#include "const.h"

#include <commdlg.h>

#pragma comment(lib, "comdlg32.lib")

namespace psxemu {

    void ShowError(HWND owner, const wchar_t* message) {
        MessageBoxW(owner, message, kWindowTitle, MB_OK | MB_ICONERROR);
    }

    void ShowWarning(HWND owner, const wchar_t* message) {
        MessageBoxW(owner, message, kWindowTitle, MB_OK | MB_ICONWARNING);
    }

    std::string ChooseFile(HWND window, FileDialog mode, const char* filter,
                           const char* default_extension) {
        char file[MAX_PATH] = { 0 };
        OPENFILENAMEA dialog = {};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = window;
        dialog.lpstrFilter = filter;
        dialog.lpstrFile = file;
        dialog.nMaxFile = sizeof(file);
        dialog.lpstrDefExt = default_extension;
        dialog.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (mode == FileDialog::kOpen) {
            dialog.Flags |= OFN_FILEMUSTEXIST;
            if (!GetOpenFileNameA(&dialog))
                return std::string();
        } else {
            dialog.Flags |= OFN_OVERWRITEPROMPT;
            if (!GetSaveFileNameA(&dialog))
                return std::string();
        }
        return std::string(file);
    }

}   // namespace psxemu
