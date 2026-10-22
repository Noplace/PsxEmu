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
#include "emuwin32/upscale/texture/ai_download.h"

#include "emuwin32/upscale/texture/onnx_runtime.h"   // SignedByMicrosoft
#include "emuwin32/upscale/texture/torch_model.h"

#include "zlib.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>

#include <cstdio>
#include <functional>
#include <map>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")

namespace emulation::win32 {

    namespace {

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

        // What is fetched, and where from.
        constexpr wchar_t kOnnxRuntimePackage[] =
            L"https://api.nuget.org/v3-flatcontainer/microsoft.ml.onnxruntime.directml/1.24.4/"
            L"microsoft.ml.onnxruntime.directml.1.24.4.nupkg";
        constexpr wchar_t kDirectMlPackage[] =
            L"https://api.nuget.org/v3-flatcontainer/microsoft.ai.directml/1.15.4/"
            L"microsoft.ai.directml.1.15.4.nupkg";
        struct Wanted {
            const char* entry;      // in the package
            const wchar_t* file;    // in PSXEmu's folder
            bool signed_dll;
        };
        const Wanted kOnnxRuntimeFiles[] = {
            { "runtimes/win-x64/native/onnxruntime.dll", L"onnxruntime.dll", true },
            { "runtimes/win-x64/native/onnxruntime_providers_shared.dll",
              L"onnxruntime_providers_shared.dll", true },
            { "LICENSE", L"onnxruntime.license.txt", false },
            { "ThirdPartyNotices.txt", L"onnxruntime.notices.txt", false },
        };
        const Wanted kDirectMlFiles[] = {
            { "bin/x64-win/DirectML.dll", L"DirectML.dll", true },
            { "LICENSE.txt", L"directml.license.txt", false },
        };
        struct Model {
            const char* key;
            const wchar_t* pth;
            const wchar_t* onnx;
            const char* sha256;
            uint64_t size;
        };
        const Model kModels[] = {
            { "anime", L"realesr-animevideov3.pth", L"realesr-animevideov3.onnx",
              "b8a8376811077954d82ca3fcf476f1ac3da3e8a68a4f4d71363008000a18b75d", 2504012 },
            { "general", L"realesr-general-x4v3.pth", L"realesr-general-x4v3.onnx",
              "8dc7edb9ac80ccdc30c3a5dca6616509367f05fbc184ad95b731f05bece96292", 4885111 },
        };
        constexpr wchar_t kModelRelease[] =
            L"https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/";

        // Real-ESRGAN's licence, as its repository has it.
        const char kRealEsrganLicence[] =
            "Real-ESRGAN models realesr-animevideov3 and realesr-general-x4v3, from\r\n"
            "https://github.com/xinntao/Real-ESRGAN (release v0.2.5.0), converted to ONNX by PSXEmu.\r\n"
            "\r\n"
            "BSD 3-Clause License\r\n"
            "\r\n"
            "Copyright (c) 2021, Xintao Wang\r\n"
            "All rights reserved.\r\n"
            "\r\n"
            "Redistribution and use in source and binary forms, with or without modification, are "
            "permitted provided that the following conditions are met:\r\n"
            "\r\n"
            "1. Redistributions of source code must retain the above copyright notice, this list of "
            "conditions and the following disclaimer.\r\n"
            "\r\n"
            "2. Redistributions in binary form must reproduce the above copyright notice, this list "
            "of conditions and the following disclaimer in the documentation and/or other materials "
            "provided with the distribution.\r\n"
            "\r\n"
            "3. Neither the name of the copyright holder nor the names of its contributors may be "
            "used to endorse or promote products derived from this software without specific prior "
            "written permission.\r\n"
            "\r\n"
            "THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS \"AS IS\" AND ANY "
            "EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF "
            "MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL "
            "THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, "
            "SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT "
            "OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS "
            "INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT "
            "LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE "
            "OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.\r\n";

        bool Exists(const std::wstring& path) {
            return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
        }

        // One GET of `url`, bytes `from` to `to` (inclusive) - or, with `from` negative, the last
        // -`from` bytes; with both 0, all of it - each piece handed to `sink`, which may stop it by
        // returning false. `total` is the whole file's size, from the server's Content-Range or
        // Content-Length.
        bool Get(HINTERNET session, const std::wstring& url, int64_t from, int64_t to,
                 const std::function<bool(const uint8_t*, size_t)>& sink, uint64_t* total,
                 std::wstring* error) {
            URL_COMPONENTS parts = {};
            parts.dwStructSize = sizeof(parts);
            wchar_t host[256] = {}, path[2048] = {};
            parts.lpszHostName = host;
            parts.dwHostNameLength = static_cast<DWORD>(std::size(host));
            parts.lpszUrlPath = path;
            parts.dwUrlPathLength = static_cast<DWORD>(std::size(path));
            if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts)) {
                *error = L"not an address: " + url;
                return false;
            }
            Internet connection(WinHttpConnect(session, host, parts.nPort, 0));
            Internet request(connection ? WinHttpOpenRequest(connection.handle, L"GET", path, nullptr,
                                                             WINHTTP_NO_REFERER,
                                                             WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                             WINHTTP_FLAG_SECURE)
                                        : nullptr);
            if (!request) {
                *error = L"could not reach " + std::wstring(host);
                return false;
            }
            std::wstring range;
            if (from < 0)
                range = L"Range: bytes=" + std::to_wstring(from);
            else if (from != 0 || to != 0)
                range = L"Range: bytes=" + std::to_wstring(from) + L"-" + std::to_wstring(to);
            if (!WinHttpSendRequest(request.handle,
                                    range.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : range.c_str(),
                                    range.empty() ? 0 : static_cast<DWORD>(-1L),
                                    WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
                !WinHttpReceiveResponse(request.handle, nullptr)) {
                *error = L"no answer from " + std::wstring(host);
                return false;
            }
            DWORD status = 0, size = sizeof(status);
            WinHttpQueryHeaders(request.handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
            if (status != (range.empty() ? 200u : 206u)) {
                *error = std::wstring(host) + L" answered " + std::to_wstring(status);
                return false;
            }
            wchar_t header[128] = {};
            size = sizeof(header);
            if (!range.empty() &&
                WinHttpQueryHeaders(request.handle, WINHTTP_QUERY_CUSTOM, L"Content-Range", header,
                                    &size, WINHTTP_NO_HEADER_INDEX)) {
                const wchar_t* slash = wcschr(header, L'/');
                if (slash != nullptr && total != nullptr)
                    *total = _wcstoui64(slash + 1, nullptr, 10);
            } else if (range.empty() && total != nullptr) {
                size = sizeof(header);
                if (WinHttpQueryHeaders(request.handle, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX,
                                        header, &size, WINHTTP_NO_HEADER_INDEX))
                    *total = _wcstoui64(header, nullptr, 10);
            }
            std::vector<uint8_t> buffer(1 << 16);
            for (;;) {
                DWORD read = 0;
                if (!WinHttpReadData(request.handle, buffer.data(), static_cast<DWORD>(buffer.size()),
                                     &read)) {
                    *error = L"the download from " + std::wstring(host) + L" broke off";
                    return false;
                }
                if (read == 0)
                    return true;
                if (!sink(buffer.data(), read)) {
                    *error = L"cancelled";
                    return false;
                }
            }
        }

        bool GetAll(HINTERNET session, const std::wstring& url, int64_t from, int64_t to,
                    std::vector<uint8_t>* out, uint64_t* total, std::wstring* error) {
            out->clear();
            return Get(session, url, from, to,
                       [out](const uint8_t* data, size_t n) {
                           out->insert(out->end(), data, data + n);
                           return true;
                       },
                       total, error);
        }

        uint32_t U16(const uint8_t* p) { return p[0] | (p[1] << 8); }
        uint32_t U32(const uint8_t* p) {
            return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24);
        }

        struct ZipEntry {
            uint32_t method = 0, crc = 0, packed = 0, size = 0, offset = 0;
        };

        // A zip's directory, read from the end of it: entries by name.
        bool ReadDirectory(HINTERNET session, const std::wstring& url,
                           std::map<std::string, ZipEntry>* entries, std::wstring* error) {
            std::vector<uint8_t> tail;
            uint64_t total = 0;
            if (!GetAll(session, url, -65558, 0, &tail, &total, error))
                return false;
            if (total == 0 || tail.size() < 22) {
                *error = L"the package's size is not known";
                return false;
            }
            size_t end = std::string::npos;
            for (size_t i = tail.size() - 22 + 1; i-- > 0;)
                if (U32(&tail[i]) == 0x06054b50) {
                    end = i;
                    break;
                }
            if (end == std::string::npos) {
                *error = L"the package is not a zip";
                return false;
            }
            const uint32_t directory_size = U32(&tail[end + 12]);
            const uint32_t directory_at = U32(&tail[end + 16]);
            const uint64_t tail_at = total - tail.size();
            std::vector<uint8_t> directory;
            if (directory_at >= tail_at) {
                directory.assign(tail.begin() + static_cast<ptrdiff_t>(directory_at - tail_at),
                                 tail.begin() + static_cast<ptrdiff_t>(directory_at - tail_at + directory_size));
            } else if (!GetAll(session, url, directory_at, directory_at + directory_size - 1,
                               &directory, nullptr, error)) {
                return false;
            }
            for (size_t at = 0; at + 46 <= directory.size() && U32(&directory[at]) == 0x02014b50;) {
                const uint32_t name_length = U16(&directory[at + 28]);
                const uint32_t extra = U16(&directory[at + 30]), comment = U16(&directory[at + 32]);
                if (at + 46 + name_length > directory.size())
                    break;
                ZipEntry entry;
                entry.method = U16(&directory[at + 10]);
                entry.crc = U32(&directory[at + 16]);
                entry.packed = U32(&directory[at + 20]);
                entry.size = U32(&directory[at + 24]);
                entry.offset = U32(&directory[at + 42]);
                (*entries)[std::string(reinterpret_cast<const char*>(&directory[at + 46]), name_length)] = entry;
                at += 46 + name_length + extra + comment;
            }
            return true;
        }

        // One entry of a zip on a server, inflated into `path` - by way of "<path>.part" - and
        // checked against its CRC-32.
        bool FetchEntry(HINTERNET session, const std::wstring& url, const ZipEntry& entry,
                        const std::wstring& path, AiDownloadProgress* progress, std::wstring* error) {
            std::vector<uint8_t> local;
            if (!GetAll(session, url, entry.offset, entry.offset + 29, &local, nullptr, error))
                return false;
            if (local.size() < 30 || U32(&local[0]) != 0x04034b50) {
                *error = L"the package's entry is not where its directory says";
                return false;
            }
            const uint64_t data = entry.offset + 30ull + U16(&local[26]) + U16(&local[28]);
            const std::wstring part = path + L".part";
            FILE* out = _wfopen(part.c_str(), L"wb");
            if (out == nullptr) {
                *error = L"could not write " + part;
                return false;
            }
            z_stream z = {};
            const bool deflated = entry.method == 8;
            if (deflated)
                inflateInit2(&z, -15);
            uLong crc = crc32(0, nullptr, 0);
            bool wrote = true;
            std::vector<uint8_t> unpacked(1 << 16);
            const bool got = Get(
                session, url, static_cast<int64_t>(data), static_cast<int64_t>(data + entry.packed - 1),
                [&](const uint8_t* bytes, size_t n) {
                    progress->done += n;
                    if (progress->cancel.load())
                        return false;
                    if (!deflated) {
                        crc = crc32(crc, bytes, static_cast<uInt>(n));
                        wrote = wrote && fwrite(bytes, 1, n, out) == n;
                        return wrote;
                    }
                    z.next_in = const_cast<Bytef*>(bytes);
                    z.avail_in = static_cast<uInt>(n);
                    while (z.avail_in > 0) {
                        z.next_out = unpacked.data();
                        z.avail_out = static_cast<uInt>(unpacked.size());
                        const int result = inflate(&z, Z_NO_FLUSH);
                        const size_t made = unpacked.size() - z.avail_out;
                        crc = crc32(crc, unpacked.data(), static_cast<uInt>(made));
                        wrote = wrote && fwrite(unpacked.data(), 1, made, out) == made;
                        if (result == Z_STREAM_END)
                            break;
                        if (result != Z_OK)
                            return false;
                    }
                    return wrote;
                },
                nullptr, error);
            if (deflated)
                inflateEnd(&z);
            fclose(out);
            if (!got || !wrote || crc != entry.crc) {
                if (got)
                    *error = L"a file in the package did not check out";
                DeleteFileW(part.c_str());
                return false;
            }
            if (!MoveFileExW(part.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
                *error = L"could not put " + path + L" in place";
                return false;
            }
            return true;
        }

        bool FetchPackage(HINTERNET session, const std::wstring& url, const Wanted* files, size_t count,
                          const std::wstring& folder, AiDownloadProgress* progress,
                          std::wstring* error) {
            std::map<std::string, ZipEntry> entries;
            if (!ReadDirectory(session, url, &entries, error))
                return false;
            for (size_t i = 0; i < count; ++i) {
                const auto found = entries.find(files[i].entry);
                if (found == entries.end()) {
                    *error = L"the package has no " + std::wstring(files[i].file);
                    return false;
                }
                progress->total += found->second.packed;
            }
            for (size_t i = 0; i < count; ++i) {
                const std::wstring path = folder + files[i].file;
                if (!FetchEntry(session, url, entries[files[i].entry], path, progress, error))
                    return false;
                if (files[i].signed_dll && !SignedByMicrosoft(path)) {
                    DeleteFileW(path.c_str());
                    *error = std::wstring(files[i].file) + L" is not signed by Microsoft";
                    return false;
                }
            }
            return true;
        }

        std::string Sha256(const std::vector<uint8_t>& data) {
            BCRYPT_ALG_HANDLE algorithm = nullptr;
            BCRYPT_HASH_HANDLE hash = nullptr;
            unsigned char digest[32] = {};
            if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
                return std::string();
            if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
                BCryptHashData(hash, const_cast<PUCHAR>(data.data()), static_cast<ULONG>(data.size()), 0);
                BCryptFinishHash(hash, digest, sizeof(digest), 0);
                BCryptDestroyHash(hash);
            }
            BCryptCloseAlgorithmProvider(algorithm, 0);
            std::string text;
            char hex[3];
            for (unsigned char byte : digest) {
                snprintf(hex, sizeof(hex), "%02x", byte);
                text += hex;
            }
            return text;
        }

        bool WriteFile(const std::wstring& path, const void* data, size_t size) {
            const std::wstring part = path + L".part";
            FILE* out = _wfopen(part.c_str(), L"wb");
            if (out == nullptr)
                return false;
            const bool wrote = fwrite(data, 1, size, out) == size;
            fclose(out);
            return wrote && MoveFileExW(part.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
        }

        // A model from its release, checked, converted to ONNX into models\.
        bool FetchModel(HINTERNET session, const Model& model, const std::wstring& folder,
                        AiDownloadProgress* progress, std::wstring* error) {
            progress->step = 2;
            progress->total += model.size;
            std::vector<uint8_t> pth;
            const bool got = Get(session, std::wstring(kModelRelease) + model.pth, 0, 0,
                                 [&](const uint8_t* data, size_t n) {
                                     progress->done += n;
                                     pth.insert(pth.end(), data, data + n);
                                     return !progress->cancel.load();
                                 },
                                 nullptr, error);
            if (!got)
                return false;
            if (pth.size() != model.size || Sha256(pth) != model.sha256) {
                *error = std::wstring(model.pth) + L" is not the file phase 0 checked";
                return false;
            }
            const std::wstring models = folder + L"models\\";
            CreateDirectoryW(models.c_str(), nullptr);
            const std::wstring pth_path = models + model.pth;
            if (!WriteFile(pth_path, pth.data(), pth.size())) {
                *error = L"could not write " + pth_path;
                return false;
            }
            progress->step = 3;
            std::map<std::string, TorchTensor> weights;
            std::string onnx, why;
            UpscalerArchitecture architecture;
            const bool converted = ReadTorchStateDict(pth_path, &weights, &why) &&
                                   ExportUpscalerOnnx(weights, &onnx, &architecture, &why);
            DeleteFileW(pth_path.c_str());
            if (!converted) {
                *error = L"the model could not be converted: " +
                         std::wstring(why.begin(), why.end());
                return false;
            }
            const std::wstring licence = models + L"Real-ESRGAN.license.txt";
            if (!WriteFile(models + model.onnx, onnx.data(), onnx.size()) ||
                !WriteFile(licence, kRealEsrganLicence, sizeof(kRealEsrganLicence) - 1)) {
                *error = L"could not write the model into " + models;
                return false;
            }
            return true;
        }

    }   // namespace

    std::wstring AiModelFileName(const std::string& model) {
        for (const Model& m : kModels)
            if (model == m.key)
                return m.onnx;
        return kModels[0].onnx;
    }

    bool DownloadAiFiles(const std::wstring& folder, const std::vector<std::string>& models,
                         AiDownloadProgress* progress, std::wstring* error) {
        Internet session(WinHttpOpen(L"PSXEmu", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
        if (!session) {
            *error = L"no internet session could be opened";
            return false;
        }
        if (!Exists(folder + L"onnxruntime.dll") || !Exists(folder + L"onnxruntime_providers_shared.dll")) {
            progress->step = 0;
            if (!FetchPackage(session.handle, kOnnxRuntimePackage, kOnnxRuntimeFiles,
                              std::size(kOnnxRuntimeFiles), folder, progress, error))
                return false;
        }
        if (!Exists(folder + L"DirectML.dll")) {
            progress->step = 1;
            if (!FetchPackage(session.handle, kDirectMlPackage, kDirectMlFiles,
                              std::size(kDirectMlFiles), folder, progress, error))
                return false;
        }
        for (const std::string& key : models)
            for (const Model& model : kModels)
                if (key == model.key && !Exists(folder + L"models\\" + model.onnx) &&
                    !FetchModel(session.handle, model, folder, progress, error))
                    return false;
        return true;
    }

}   // namespace emulation::win32
