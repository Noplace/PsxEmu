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
#include "graphics/fsr/fsr_download.h"

#include "graphics/fsr/fidelityfx.h"   // SignedByAmd

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>

#include <cstdio>
#include <vector>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")

namespace psxemu {

    namespace {

        // Closes a WinHTTP handle when it goes.
        struct Internet {
            HINTERNET handle = nullptr;
            explicit Internet(HINTERNET h) : handle(h) {}
            ~Internet() {
                if (handle != nullptr)
                    WinHttpCloseHandle(handle);
            }
            Internet(const Internet&) = delete;
            Internet& operator=(const Internet&) = delete;
            explicit operator bool() const { return handle != nullptr; }
        };

        // Git's id for a file, as it streams in: SHA-1 over "blob <size>\0" and the bytes.
        class BlobId {
         public:
            BlobId(uint64_t size) {
                if (BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_SHA1_ALGORITHM, nullptr, 0) !=
                        0 ||
                    BCryptCreateHash(algorithm_, &hash_, nullptr, 0, nullptr, 0, 0) != 0) {
                    hash_ = nullptr;
                    return;
                }
                char header[48];
                const int length = snprintf(header, sizeof(header), "blob %llu",
                                            static_cast<unsigned long long>(size));
                Add(header, static_cast<size_t>(length) + 1);   // and its terminating zero
            }
            ~BlobId() {
                if (hash_ != nullptr)
                    BCryptDestroyHash(hash_);
                if (algorithm_ != nullptr)
                    BCryptCloseAlgorithmProvider(algorithm_, 0);
            }
            void Add(const void* data, size_t bytes) {
                if (hash_ != nullptr)
                    BCryptHashData(hash_, static_cast<PUCHAR>(const_cast<void*>(data)),
                                   static_cast<ULONG>(bytes), 0);
            }
            std::string Finish() {
                unsigned char digest[20] = {};
                if (hash_ == nullptr || BCryptFinishHash(hash_, digest, sizeof(digest), 0) != 0)
                    return std::string();
                std::string text;
                char hex[3];
                for (unsigned char byte : digest) {
                    snprintf(hex, sizeof(hex), "%02x", byte);
                    text += hex;
                }
                return text;
            }

         private:
            BCRYPT_ALG_HANDLE algorithm_ = nullptr;
            BCRYPT_HASH_HANDLE hash_ = nullptr;
        };

        std::wstring Number(unsigned long value) { return std::to_wstring(value); }

        // Whether the file at `path` is already the release's: its size, then its blob id.
        bool IsReleaseFile(const std::wstring& path, const FsrDownloadFile& file) {
            HANDLE in = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (in == INVALID_HANDLE_VALUE)
                return false;
            LARGE_INTEGER size = {};
            bool same = GetFileSizeEx(in, &size) &&
                        static_cast<uint64_t>(size.QuadPart) == file.size;
            if (same) {
                BlobId id(file.size);
                std::vector<char> buffer(1 << 16);
                DWORD read = 0;
                while (ReadFile(in, buffer.data(), static_cast<DWORD>(buffer.size()), &read,
                                nullptr) &&
                       read > 0)
                    id.Add(buffer.data(), read);
                same = id.Finish() == file.blob;
            }
            CloseHandle(in);
            return same;
        }

        // The SDK's licence for these files (graphics/fsr/fidelityfx/LICENSE.txt).
        const char kFsrLicence[] =
            "AMD FidelityFX SDK (FSR SDK) v2.3.0 - amd_fidelityfx_loader_dx12.dll,\r\n"
            "amd_fidelityfx_upscaler_dx12.dll and amd_fidelityfx_framegeneration_dx12.dll, from\r\n"
            "https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK (Kits/FidelityFX/signedbin),\r\n"
            "are listed in the SDK's docs/license.md under the following licence:\r\n"
            "\r\n"
            "Copyright (C) Advanced Micro Devices, Inc.\r\n"
            "\r\n"
            "Permission is hereby granted, free of charge, to any person obtaining a copy of this "
            "software and associated documentation files (the \"Software\"), to deal in the "
            "Software without restriction, including without limitation the rights to use, copy, "
            "modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, "
            "and to permit persons to whom the Software is furnished to do so, subject to the "
            "following conditions:\r\n"
            "\r\n"
            "The above copyright notice and this permission notice shall be included in all copies "
            "or substantial portions of the Software.\r\n"
            "\r\n"
            "THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, "
            "INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A "
            "PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT "
            "HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF "
            "CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE "
            "OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.\r\n";

        // One file into `part`, its blob id worked out as it comes. Adds what arrives to
        // progress->done.
        bool Fetch(HINTERNET session, const FsrDownloadFile& file, const std::wstring& part,
                   FsrDownloadProgress* progress, std::wstring* error) {
            Internet connection(WinHttpConnect(session, kFsrDownloadHost,
                                               INTERNET_DEFAULT_HTTPS_PORT, 0));
            const std::wstring path = std::wstring(kFsrDownloadPath) + file.name;
            Internet request(connection ? WinHttpOpenRequest(connection.handle, L"GET",
                                                             path.c_str(), nullptr,
                                                             WINHTTP_NO_REFERER,
                                                             WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                             WINHTTP_FLAG_SECURE)
                                        : nullptr);
            if (!request || !WinHttpSendRequest(request.handle, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                                WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
                !WinHttpReceiveResponse(request.handle, nullptr)) {
                *error = L"Could not reach " + std::wstring(kFsrDownloadHost) + L" (error " +
                         Number(GetLastError()) + L").";
                return false;
            }
            DWORD status = 0, size = sizeof(status);
            WinHttpQueryHeaders(request.handle,
                                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &status, &size,
                                WINHTTP_NO_HEADER_INDEX);
            if (status != 200) {
                *error = std::wstring(file.name) + L": the server answered " + Number(status) +
                         L".";
                return false;
            }

            HANDLE out = CreateFileW(part.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                     FILE_ATTRIBUTE_NORMAL, nullptr);
            if (out == INVALID_HANDLE_VALUE) {
                *error = L"Could not write into PSXEmu's folder (error " +
                         Number(GetLastError()) +
                         L"). If it is under Program Files, copy the files in by hand.";
                return false;
            }
            BlobId id(file.size);
            uint64_t got = 0;
            std::vector<char> buffer(1 << 16);
            bool ok = true;
            for (;;) {
                if (progress->cancel.load()) {
                    *error = L"Cancelled.";
                    ok = false;
                    break;
                }
                DWORD read = 0;
                if (!WinHttpReadData(request.handle, buffer.data(),
                                     static_cast<DWORD>(buffer.size()), &read)) {
                    *error = std::wstring(file.name) + L": the download broke off (error " +
                             Number(GetLastError()) + L").";
                    ok = false;
                    break;
                }
                if (read == 0)
                    break;
                got += read;
                if (got > file.size) {
                    *error = std::wstring(file.name) + L" is larger than the release's.";
                    ok = false;
                    break;
                }
                id.Add(buffer.data(), read);
                DWORD written = 0;
                if (!WriteFile(out, buffer.data(), read, &written, nullptr) || written != read) {
                    *error = L"Could not write into PSXEmu's folder (error " +
                             Number(GetLastError()) + L").";
                    ok = false;
                    break;
                }
                progress->done.fetch_add(read);
            }
            CloseHandle(out);
            if (ok && got != file.size) {
                *error = std::wstring(file.name) + L" came short: " + std::to_wstring(got) +
                         L" bytes of " + std::to_wstring(file.size) + L".";
                ok = false;
            }
            if (ok && id.Finish() != file.blob) {
                *error = std::wstring(file.name) + L" is not the release's file.";
                ok = false;
            }
            return ok;
        }

    }   // namespace

    bool DownloadFsrFiles(const std::wstring& folder, FsrDownloadProgress* progress,
                          std::wstring* error) {
        Internet session(WinHttpOpen(L"PSXEmu", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
        if (!session) {
            *error = L"Windows' HTTP service would not start (error " + Number(GetLastError()) +
                     L").";
            return false;
        }
        for (int i = 0; i < static_cast<int>(std::size(kFsrDownloadFiles)); ++i) {
            const FsrDownloadFile& file = kFsrDownloadFiles[i];
            progress->file.store(i);
            const std::wstring target = folder + file.name;
            // One there already and the release's is left alone - it may be loaded, and then
            // could not be replaced anyway.
            if (IsReleaseFile(target, file)) {
                progress->done.fetch_add(file.size);
                continue;
            }
            const std::wstring part = target + L".part";
            bool fetched = Fetch(session.handle, file, part, progress, error);
            // AMD's signature, as the loader is checked before it is loaded: a file that is the
            // release's should be, and one that is not is never put where it would load.
            if (fetched && !SignedByAmd(part)) {
                *error = std::wstring(file.name) + L" is not signed by AMD.";
                fetched = false;
            }
            if (!fetched) {
                DeleteFileW(part.c_str());
                return false;
            }
            if (!MoveFileExW(part.c_str(), target.c_str(),
                             MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                *error = std::wstring(file.name) + L" could not be put in place (error " +
                         Number(GetLastError()) + L") - is PSXEmu using it already?";
                DeleteFileW(part.c_str());
                return false;
            }
        }
        // The notice AMD's licence asks to go with the DLLs, as the build puts beside them.
        const std::wstring notice = folder + L"fidelityfx.license.txt";
        if (GetFileAttributesW(notice.c_str()) == INVALID_FILE_ATTRIBUTES) {
            FILE* text = _wfopen(notice.c_str(), L"wb");
            if (text != nullptr) {
                fputs(kFsrLicence, text);
                fclose(text);
            }
        }
        return true;
    }

}   // namespace psxemu
