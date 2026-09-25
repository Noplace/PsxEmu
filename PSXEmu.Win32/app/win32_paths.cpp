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
#include "app/win32_paths.h"

#include "app/const.h"

#include <shlobj.h>   // SHGetFolderPathA
#include <shellapi.h>   // CommandLineToArgvW

#include <algorithm>   // sort, for the BIOS folder listing

#pragma comment(lib, "shell32.lib")

namespace psxemu {

    CommandLine ParseCommandLine() {
        CommandLine result;
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argv == nullptr)
            return result;
        if (argc > 1)
            result.bios = Narrow(argv[1]);
        if (argc > 2)
            result.disc = Narrow(argv[2]);
        LocalFree(argv);
        return result;
    }

    std::string Narrow(const std::wstring& wide) {
        if (wide.empty())
            return std::string();
        const int size =
            WideCharToMultiByte(CP_ACP, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (size <= 1)
            return std::string();
        std::string narrow(static_cast<size_t>(size - 1), '\0');
        WideCharToMultiByte(CP_ACP, 0, wide.c_str(), -1, &narrow[0], size, nullptr, nullptr);
        return narrow;
    }

    std::wstring Widen(const std::string& narrow) {
        if (narrow.empty())
            return std::wstring();
        const int size = MultiByteToWideChar(CP_ACP, 0, narrow.c_str(), -1, nullptr, 0);
        if (size <= 1)
            return std::wstring();
        std::wstring wide(static_cast<size_t>(size - 1), L'\0');
        MultiByteToWideChar(CP_ACP, 0, narrow.c_str(), -1, &wide[0], size);
        return wide;
    }

    bool EnsureDirectory(const std::string& path) {
        if (CreateDirectoryA(path.c_str(), nullptr))
            return true;
        return GetLastError() == ERROR_ALREADY_EXISTS;
    }

    std::string ResolveDataRoot() {
        char documents[MAX_PATH] = { 0 };
        if (!SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_MYDOCUMENTS, nullptr, 0, documents)))
            return std::string();
        const std::string my_games = std::string(documents) + "\\My Games";
        if (!EnsureDirectory(my_games))
            return std::string();
        const std::string root = my_games + "\\PSXEmu";
        if (!EnsureDirectory(root))
            return std::string();
        return root;
    }

    std::wstring SettingsPathBesideExecutable() {
        wchar_t module[MAX_PATH] = { 0 };
        GetModuleFileNameW(nullptr, module, MAX_PATH);
        std::wstring path = module;
        const size_t slash = path.find_last_of(L"/\\");
        if (slash != std::wstring::npos)
            path.erase(slash + 1);
        return path + L"psxemu.ini";
    }

    std::string FindBios(const std::string& from_command_line) {
        if (!from_command_line.empty()) {
            char full_path[MAX_PATH] = { 0 };
            GetFullPathNameA(from_command_line.c_str(), MAX_PATH, full_path, nullptr);
            return full_path;
        }

        char module[MAX_PATH] = { 0 };
        GetModuleFileNameA(nullptr, module, MAX_PATH);
        std::string directory = module;
        const size_t slash = directory.find_last_of("/\\");
        directory = (slash == std::string::npos) ? std::string() : directory.substr(0, slash + 1);

        for (const char* candidate : kBiosCandidates) {
            const std::string path = directory + candidate;
            FILE* fp = fopen(path.c_str(), "rb");
            if (fp != nullptr) {
                fclose(fp);
                return path;
            }
        }
        return std::string();
    }

    std::vector<std::string> ScanBiosFolder(const std::string& folder) {
        std::vector<std::string> found;
        if (folder.empty())
            return found;

        WIN32_FIND_DATAA entry = {};
        HANDLE search = FindFirstFileA((folder + "\\*").c_str(), &entry);
        if (search == INVALID_HANDLE_VALUE)
            return found;
        do {
            if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
                continue;
            // Only the low word is looked at: a BIOS is half a megabyte, so anything with a high
            // word at all is far too big to be one and is rejected by the size test anyway.
            if (entry.nFileSizeHigh != 0 || entry.nFileSizeLow != kBiosImageBytes)
                continue;
            found.push_back(entry.cFileName);
        } while (FindNextFileA(search, &entry) != 0);
        FindClose(search);

        std::sort(found.begin(), found.end(), [](const std::string& a, const std::string& b) {
            return _stricmp(a.c_str(), b.c_str()) < 0;
        });
        return found;
    }

    std::string FileNameOf(const std::string& path) {
        const size_t slash = path.find_last_of("/\\");
        return (slash == std::string::npos) ? path : path.substr(slash + 1);
    }

    std::string DiscIdentifier(const std::string& disc_path) {
        std::string name = disc_path;
        const size_t slash = name.find_last_of("/\\");
        if (slash != std::string::npos)
            name = name.substr(slash + 1);
        const size_t dot = name.find_last_of('.');
        if (dot != std::string::npos)
            name = name.substr(0, dot);
        return name;
    }

    std::string SaveStateSlotPath(const std::string& savestates_root, const std::string& disc_path,
                                  int slot) {
        const std::string identifier = disc_path.empty() ? "bios" : DiscIdentifier(disc_path);
        return savestates_root + "\\" + identifier + ".st" + std::to_string(slot);
    }

    bool LooksLikePsExe(const std::string& path) {
        FILE* fp = fopen(path.c_str(), "rb");
        if (fp == nullptr)
            return false;
        char id[8] = {};
        const size_t read = fread(id, 1, sizeof(id), fp);
        fclose(fp);
        return read == sizeof(id) && memcmp(id, "PS-X EXE", sizeof(id)) == 0;
    }

}   // namespace psxemu
