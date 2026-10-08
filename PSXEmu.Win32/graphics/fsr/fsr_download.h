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

// AMD's FSR files fetched by the emulator itself, from the Video Settings window's "Get AMD's FSR
// files..." (Docs/FSR-Plan.md). Unlike NVIDIA's, AMD's DLLs are MIT and published one by one in
// AMD's repository, so they can be downloaded as they are rather than out of a 280 MB archive -
// the same three files, from the same release, checked the same way, as
// graphics/fsr/fetch_fidelityfx.ps1 fetches for the build: each one's size and git blob id against
// the release's tree, then AMD's Authenticode signature, and only then moved beside the emulator.
// Each goes to "<name>.part" first, so a download stopped half-way leaves nothing that loads.

#include <atomic>
#include <cstdint>
#include <string>

namespace psxemu {

    // The three, as the v2.3.0 tree lists them: name, size, git blob id.
    struct FsrDownloadFile {
        const wchar_t* name;
        uint64_t size;
        const char* blob;
    };
    inline constexpr FsrDownloadFile kFsrDownloadFiles[] = {
        { L"amd_fidelityfx_loader_dx12.dll", 26376, "144916ba922c2a66149ad5cb1037a4f86d2b6491" },
        { L"amd_fidelityfx_upscaler_dx12.dll", 28761864,
          "199de3a500a1d153b7e73fa5ca0adbd2a4ac1229" },
        { L"amd_fidelityfx_framegeneration_dx12.dll", 40085776,
          "1a06b72761086cb5561f8984ce0dc6a24a370e9e" },
    };
    inline constexpr wchar_t kFsrDownloadHost[] = L"raw.githubusercontent.com";
    inline constexpr wchar_t kFsrDownloadPath[] =
        L"/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v2.3.0/Kits/FidelityFX/signedbin/";

    inline uint64_t FsrDownloadTotal() {
        uint64_t total = 0;
        for (const FsrDownloadFile& file : kFsrDownloadFiles)
            total += file.size;
        return total;
    }

    // How far a download has got, for a progress bar on another thread.
    struct FsrDownloadProgress {
        std::atomic<uint64_t> done{ 0 };    // bytes of all three
        std::atomic<int> file{ 0 };         // which one, 0-2
        std::atomic<bool> cancel{ false };  // set to stop it
    };

    // Downloads the three into `folder` (with its trailing backslash), each checked and then put
    // in place of any there. Blocking - run it off the UI thread. False, with `error` in words,
    // if any step fails or it is cancelled; files already put in place stay, being good.
    bool DownloadFsrFiles(const std::wstring& folder, FsrDownloadProgress* progress,
                          std::wstring* error);

}   // namespace psxemu
