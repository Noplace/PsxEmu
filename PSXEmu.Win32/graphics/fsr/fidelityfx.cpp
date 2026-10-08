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
#include "graphics/fsr/fidelityfx.h"

#include "graphics/fsr/fsr_choice.h"

#include <softpub.h>
#include <wintrust.h>

#include <cstdio>
#include <mutex>

#pragma comment(lib, "wintrust.lib")
#pragma comment(lib, "crypt32.lib")

namespace psxemu {

    namespace {

        bool Exists(const std::wstring& path) {
            return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
        }

        std::string Narrow(const std::wstring& text) {
            std::string out;
            for (wchar_t c : text)
                out += c < 128 ? static_cast<char>(c) : '?';
            return out;
        }

    }   // namespace

    FidelityFx& FidelityFx::Get() {
        static FidelityFx instance;
        return instance;
    }

    bool SignedByAmd(const std::wstring& path) {
        WINTRUST_FILE_INFO file = {};
        file.cbStruct = sizeof(file);
        file.pcwszFilePath = path.c_str();
        WINTRUST_DATA data = {};
        data.cbStruct = sizeof(data);
        data.dwUIChoice = WTD_UI_NONE;
        // Revocation is not asked about: that would go to the network at every renderer made.
        data.fdwRevocationChecks = WTD_REVOKE_NONE;
        data.dwUnionChoice = WTD_CHOICE_FILE;
        data.pFile = &file;
        data.dwStateAction = WTD_STATEACTION_VERIFY;
        data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
        GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
        const LONG trust = WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data);
        bool amd = false;
        if (trust == ERROR_SUCCESS) {
            // ...and signed by AMD, not merely by someone.
            CRYPT_PROVIDER_DATA* provider = WTHelperProvDataFromStateData(data.hWVTStateData);
            CRYPT_PROVIDER_SGNR* signer =
                provider != nullptr ? WTHelperGetProvSignerFromChain(provider, 0, FALSE, 0) : nullptr;
            if (signer != nullptr && signer->csCertChain > 0 && signer->pasCertChain[0].pCert) {
                wchar_t name[256] = {};
                CertGetNameStringW(signer->pasCertChain[0].pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0,
                                   nullptr, name, 256);
                amd = wcscmp(name, L"Advanced Micro Devices") == 0 ||
                      wcscmp(name, L"Advanced Micro Devices, Inc.") == 0;
            }
        }
        data.dwStateAction = WTD_STATEACTION_CLOSE;
        WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data);
        return amd;
    }

    // Once per process, and never let go of: what the effect DLLs leave running - the frame
    // generation swap chain's threads among it - is not this program's to know, and code unloaded
    // under a thread is a crash at some later moment.
    bool FidelityFx::Load(const std::wstring& folder, std::string* error) {
        static std::mutex lock;
        std::lock_guard<std::mutex> guard(lock);
        if (loaded()) {
            if (_wcsicmp(folder.c_str(), folder_.c_str()) != 0) {
                *error = "AMD FidelityFX is already loaded from another folder";
                return false;
            }
            return true;
        }
        const std::wstring loader = folder + L"\\" + kFsrFiles[0];
        const std::wstring upscaler = folder + L"\\" + kFsrFiles[1];
        const std::wstring generation = folder + L"\\" + kFsrFiles[2];
        if (!Exists(loader) || !Exists(upscaler)) {
            *error = "AMD's FSR files are not beside the emulator (" +
                     Narrow(Exists(loader) ? kFsrFiles[1] : kFsrFiles[0]) + ")";
            return false;
        }
        // AMD's signature, before a line of any of them runs: a DLL beside the executable could
        // be anyone's, and the loader loads the effects from the same folder by name.
        for (const std::wstring* path : { &loader, &upscaler }) {
            if (!SignedByAmd(*path)) {
                const size_t slash = path->find_last_of(L'\\');
                *error = Narrow(path->substr(slash + 1)) + " is not signed by AMD";
                return false;
            }
        }
        const bool has_generation = Exists(generation) && SignedByAmd(generation);
        if (Exists(generation) && !has_generation) {
            *error = Narrow(kFsrFiles[2]) + " is not signed by AMD";
            return false;
        }
        HMODULE module = LoadLibraryW(loader.c_str());
        if (module == nullptr) {
            *error = Narrow(kFsrFiles[0]) + " could not be loaded";
            return false;
        }
        auto create = reinterpret_cast<PfnFfxCreateContext>(GetProcAddress(module, "ffxCreateContext"));
        auto destroy =
            reinterpret_cast<PfnFfxDestroyContext>(GetProcAddress(module, "ffxDestroyContext"));
        auto configure = reinterpret_cast<PfnFfxConfigure>(GetProcAddress(module, "ffxConfigure"));
        auto query = reinterpret_cast<PfnFfxQuery>(GetProcAddress(module, "ffxQuery"));
        auto dispatch = reinterpret_cast<PfnFfxDispatch>(GetProcAddress(module, "ffxDispatch"));
        if (create == nullptr || destroy == nullptr || configure == nullptr || query == nullptr ||
            dispatch == nullptr) {
            *error = Narrow(kFsrFiles[0]) + " is missing a function";
            return false;
        }
        destroy_ = destroy;
        configure_ = configure;
        query_ = query;
        dispatch_ = dispatch;
        frame_generation_ = has_generation;
        folder_ = folder;
        create_ = create;   // last: loaded() from here on

        // AMD's own warnings and errors, into the log when there is one.
        if (FsrLogging()) {
            ffxConfigureDescGlobalDebug1 debug = {};
            debug.header.type = FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG1;
            debug.fpMessage = [](uint32_t type, const wchar_t* message) {
                FsrNote(std::string(type == FFX_API_MESSAGE_TYPE_ERROR ? "AMD error: "
                                                                       : "AMD warning: ") +
                        Narrow(message != nullptr ? message : L""));
            };
            debug.debugLevel = FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_WARNINGS;
            configure_(nullptr, &debug.header);
        }
        return true;
    }

    std::vector<FidelityFx::Version> FidelityFx::Versions(uint64_t create_desc_type,
                                                          ID3D12Device* device) const {
        std::vector<Version> versions;
        if (!loaded())
            return versions;
        ffxQueryDescGetVersions query = {};
        query.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
        query.createDescType = create_desc_type;
        query.device = device;
        uint64_t count = 0;
        query.outputCount = &count;
        if (query_(nullptr, &query.header) != FFX_API_RETURN_OK || count == 0)
            return versions;
        std::vector<uint64_t> ids(count);
        std::vector<const char*> names(count);
        query.versionIds = ids.data();
        query.versionNames = names.data();
        if (query_(nullptr, &query.header) != FFX_API_RETURN_OK)
            return versions;
        // Copied at once: AMD notes some names live in memory a later query overwrites.
        for (uint64_t i = 0; i < count; ++i)
            versions.push_back({ ids[i], names[i] != nullptr ? names[i] : "" });
        return versions;
    }

    const char* FfxResultText(ffxReturnCode_t code) {
        switch (code) {
        case FFX_API_RETURN_OK: return "ok";
        case FFX_API_RETURN_ERROR: return "an error";
        case FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE: return "a description it does not know";
        case FFX_API_RETURN_ERROR_RUNTIME_ERROR: return "Direct3D 12 or the effect failed";
        case FFX_API_RETURN_NO_PROVIDER: return "no version of it runs on this graphics card";
        case FFX_API_RETURN_ERROR_MEMORY: return "out of memory";
        case FFX_API_RETURN_ERROR_PARAMETER: return "an invalid parameter";
        case FFX_API_RETURN_PROVIDER_NO_SUPPORT_NEW_DESCTYPE:
            return "a description too new for its DLL or driver";
        default: return "an unknown error";
        }
    }

    namespace {
        const std::wstring& LogFolder() {
            static const std::wstring folder = [] {
                wchar_t path[MAX_PATH] = {};
                return GetEnvironmentVariableW(L"PSXEMU_FSR_LOG", path, MAX_PATH) > 0
                           ? std::wstring(path) : std::wstring();
            }();
            return folder;
        }
    }   // namespace

    bool FsrLogging() { return !LogFolder().empty(); }

    void FsrNote(const std::string& line) {
        if (LogFolder().empty())
            return;
        static std::mutex lock;   // AMD's messages come from its own threads too
        std::lock_guard<std::mutex> guard(lock);
        FILE* file = _wfopen((LogFolder() + L"\\fsr.log").c_str(), L"a");
        if (file == nullptr)
            return;
        SYSTEMTIME now = {};
        GetLocalTime(&now);
        fprintf(file, "[%02d:%02d:%02d.%03d] %s\n", now.wHour, now.wMinute, now.wSecond,
                now.wMilliseconds, line.c_str());
        fclose(file);
    }

    FfxApiResource FfxResource(ID3D12Resource* resource, uint32_t state) {
        return ffxApiGetResourceDX12(resource, state);
    }

}   // namespace psxemu
