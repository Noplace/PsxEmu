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
#include "graphics/dlss/streamline.h"

#include "graphics/adapters.h"

// NVIDIA's check of its own signature, compiled here and only here: the header defines its
// functions rather than declaring them.
#include "graphics/dlss/streamline/sl_security.h"

namespace psxemu {

    std::string Streamline::last_message_;

    std::wstring ExecutableFolder() {
        wchar_t path[MAX_PATH] = {};
        const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring folder(path, length);
        const size_t slash = folder.find_last_of(L"\\/");
        return slash == std::wstring::npos ? std::wstring() : folder.substr(0, slash);
    }

    void DlssNote(const std::string& line) {
        static const std::wstring folder = [] {
            wchar_t path[MAX_PATH] = {};
            return GetEnvironmentVariableW(L"PSXEMU_DLSS_LOG", path, MAX_PATH) > 0
                       ? std::wstring(path) : std::wstring();
        }();
        if (folder.empty())
            return;
        FILE* file = _wfopen((folder + L"\\dlss.log").c_str(), L"a");
        if (file == nullptr)
            return;
        SYSTEMTIME now = {};
        GetLocalTime(&now);
        fprintf(file, "[%02d:%02d:%02d.%03d] %s\n", now.wHour, now.wMinute, now.wSecond,
                now.wMilliseconds, line.c_str());
        fclose(file);
    }

    const char* StreamlineResultText(sl::Result result) {
        switch (result) {
        case sl::Result::eOk: return "ok";
        case sl::Result::eErrorDriverOutOfDate: return "the graphics driver is too old for it";
        case sl::Result::eErrorOSOutOfDate: return "Windows is too old for it";
        case sl::Result::eErrorOSDisabledHWS: return "hardware-accelerated GPU scheduling is off";
        case sl::Result::eErrorNoSupportedAdapterFound: return "no graphics card here supports it";
        case sl::Result::eErrorAdapterNotSupported: return "this graphics card does not support it";
        case sl::Result::eErrorNoPlugins: return "its plugins were not found";
        case sl::Result::eErrorNGXFailed: return "NVIDIA's NGX failed";
        case sl::Result::eErrorFeatureMissing: return "DLSS was not loaded";
        case sl::Result::eErrorFeatureNotSupported: return "not supported";
        case sl::Result::eErrorFeatureFailedToLoad: return "DLSS failed to load";
        case sl::Result::eErrorNotInitialized: return "Streamline is not started";
        case sl::Result::eErrorInvalidParameter: return "an invalid parameter";
        case sl::Result::eErrorMissingInputParameter: return "an input is missing";
        case sl::Result::eErrorMissingResourceState: return "a resource state is missing";
        case sl::Result::eErrorCommonConstantsMissing: return "the constants are missing";
        case sl::Result::eWarnOutOfVRAM: return "out of video memory";
        default: return "an error";
        }
    }

    void Streamline::Log(sl::LogType type, const char* message) {
        if (type != sl::LogType::eInfo && message != nullptr)
            last_message_ = message;
    }

    // The DLL once per process, and never let go of: Streamline starts and stops as renderers
    // come and go, but what its plugins leave running is not this program's to know, and code
    // unloaded under a thread is a crash at some later moment.
    bool Streamline::Load(const std::wstring& folder, std::string* error) {
        static HMODULE loaded = nullptr;
        static std::wstring loaded_from;
        const std::wstring path = folder + L"\\sl.interposer.dll";
        if (loaded != nullptr) {
            if (_wcsicmp(path.c_str(), loaded_from.c_str()) != 0) {
                *error = "Streamline is already loaded from another folder";
                return false;
            }
        } else {
            if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
                *error = "NVIDIA's DLSS files are not beside the emulator (Streamline's "
                         "sl.interposer.dll)";
                return false;
            }
            // NVIDIA's signature, before a line of it runs: a DLL beside the executable could be
            // anyone's.
            if (!sl::security::verifyEmbeddedSignature(path.c_str())) {
                *error = "sl.interposer.dll is not signed by NVIDIA";
                return false;
            }
            loaded = LoadLibraryW(path.c_str());
            if (loaded == nullptr) {
                *error = "sl.interposer.dll could not be loaded";
                return false;
            }
            loaded_from = path;
        }
        bool found = true;
        auto take = [&](auto*& function, const char* name) {
            function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(
                GetProcAddress(loaded, name));
            found = found && function != nullptr;
        };
        take(slInit_, "slInit");
        take(slShutdown, "slShutdown");
        take(slIsFeatureSupported, "slIsFeatureSupported");
        take(slGetFeatureRequirements, "slGetFeatureRequirements");
        take(slSetD3DDevice, "slSetD3DDevice");
        take(slUpgradeInterface, "slUpgradeInterface");
        take(slGetNativeInterface, "slGetNativeInterface");
        take(slGetNewFrameToken, "slGetNewFrameToken");
        take(slSetConstants, "slSetConstants");
        take(slSetTagForFrame, "slSetTagForFrame");
        take(slEvaluateFeature, "slEvaluateFeature");
        take(slFreeResources, "slFreeResources");
        take(slGetFeatureVersion, "slGetFeatureVersion");
        take(slGetFeatureFunction, "slGetFeatureFunction");
        if (!found)
            *error = "sl.interposer.dll is not the Streamline this was built for";
        return found;
    }

    bool Streamline::Start(const std::wstring& folder, bool frame_generation, std::string* error) {
        Stop();
        if (!Load(folder, error))
            return false;

        // Manual hooking: only what a renderer hands over goes through it. No optional updates,
        // and no plugins but the ones shipped (Streamline's default has both on) - though it
        // still asks the driver's updater to look for updates (streamline.h).
        // Resources tagged frame by frame. The host keeps its own command-list state. No file
        // log; warnings and errors come back through Log.
        const sl::Feature all[] = { sl::kFeatureDLSS, sl::kFeatureDLSS_G, sl::kFeatureReflex,
                                    sl::kFeaturePCL };
        const wchar_t* paths[] = { folder.c_str() };
        // For finding out what went wrong: PSXEMU_DLSS_LOG names a folder Streamline writes its
        // own verbose log (sl.log) into.
        wchar_t log_folder[MAX_PATH] = {};
        const bool logging =
            GetEnvironmentVariableW(L"PSXEMU_DLSS_LOG", log_folder, MAX_PATH) > 0;
        sl::Preferences preferences{};
        preferences.showConsole = false;
        preferences.logLevel = logging ? sl::LogLevel::eVerbose : sl::LogLevel::eDefault;
        preferences.pathsToPlugins = paths;
        preferences.numPathsToPlugins = 1;
        preferences.pathToLogsAndData = logging ? log_folder : nullptr;
        preferences.logMessageCallback = &Log;
        preferences.flags = sl::PreferenceFlags::eDisableCLStateTracking |
                            sl::PreferenceFlags::eUseManualHooking |
                            sl::PreferenceFlags::eUseFrameBasedResourceTagging;
        preferences.featuresToLoad = all;
        preferences.numFeaturesToLoad = frame_generation ? 4 : 1;
        // No NVIDIA application id: a custom engine, named, with a project GUID of its own (any
        // fixed one; this is PSXEmu's).
        preferences.engine = sl::EngineType::eCustom;
        preferences.engineVersion = "PSXEmu";
        preferences.projectId = "b7c3e5a2-4f1d-4c8e-9a6b-2d5f8e1c3a70";
        preferences.renderAPI = sl::RenderAPI::eD3D12;
        const sl::Result result = slInit_(preferences, sl::kSDKVersion);
        if (result != sl::Result::eOk) {
            *error = std::string("Streamline would not start: ") + StreamlineResultText(result);
            return false;
        }
        started_ = true;
        frame_generation_ = frame_generation;
        return true;
    }

    bool Streamline::SetDevice(void* device, std::string* error) {
        const sl::Result result = slSetD3DDevice(device);
        if (result != sl::Result::eOk) {
            *error = std::string("Streamline would not take the device: ") +
                     StreamlineResultText(result);
            return false;
        }
        void* function = nullptr;
        if (slGetFeatureFunction(sl::kFeatureDLSS, "slDLSSGetOptimalSettings", function) ==
            sl::Result::eOk)
            slDLSSGetOptimalSettings = reinterpret_cast<PFun_slDLSSGetOptimalSettings*>(function);
        function = nullptr;
        if (slGetFeatureFunction(sl::kFeatureDLSS, "slDLSSSetOptions", function) == sl::Result::eOk)
            slDLSSSetOptions = reinterpret_cast<PFun_slDLSSSetOptions*>(function);
        if (slDLSSGetOptimalSettings == nullptr || slDLSSSetOptions == nullptr) {
            *error = "DLSS's functions are missing from Streamline";
            return false;
        }
        // Frame Generation's, and the Reflex and PC Latency it needs: all of them, or none, and
        // Super Resolution carries on either way.
        if (frame_generation_) {
            bool found = true;
            auto take = [&](sl::Feature feature, auto*& out, const char* name) {
                void* pointer = nullptr;
                if (slGetFeatureFunction(feature, name, pointer) != sl::Result::eOk)
                    pointer = nullptr;
                out = reinterpret_cast<std::remove_reference_t<decltype(out)>>(pointer);
                found = found && pointer != nullptr;
            };
            take(sl::kFeatureDLSS_G, slDLSSGSetOptions, "slDLSSGSetOptions");
            take(sl::kFeatureDLSS_G, slDLSSGGetState, "slDLSSGGetState");
            take(sl::kFeatureReflex, slReflexSetOptions, "slReflexSetOptions");
            take(sl::kFeatureReflex, slReflexSleep, "slReflexSleep");
            take(sl::kFeatureReflex, slReflexGetState, "slReflexGetState");
            take(sl::kFeaturePCL, slPCLSetMarker, "slPCLSetMarker");
            take(sl::kFeaturePCL, slPCLGetState, "slPCLGetState");
            take(sl::kFeaturePCL, slPCLSetOptions, "slPCLSetOptions");
            if (!found)
                ForgetGeneration();
        }
        return true;
    }

    void Streamline::ForgetGeneration() {
        slDLSSGSetOptions = nullptr;
        slDLSSGGetState = nullptr;
        slReflexSetOptions = nullptr;
        slReflexSleep = nullptr;
        slReflexGetState = nullptr;
        slPCLSetMarker = nullptr;
        slPCLGetState = nullptr;
        slPCLSetOptions = nullptr;
    }

    void Streamline::Stop() {
        if (!started_)
            return;
        slShutdown();
        started_ = false;
        frame_generation_ = false;
        slDLSSGetOptimalSettings = nullptr;
        slDLSSSetOptions = nullptr;
        ForgetGeneration();
    }

    bool Streamline::Supports(sl::Feature feature, uint64_t luid, std::string* why) const {
        if (!started_) {
            *why = "Streamline is not started";
            return false;
        }
        LUID adapter = UnpackLuid(luid);
        sl::AdapterInfo info{};
        info.deviceLUID = reinterpret_cast<uint8_t*>(&adapter);
        info.deviceLUIDSizeInBytes = sizeof(adapter);
        const sl::Result result = slIsFeatureSupported(feature, info);
        if (result == sl::Result::eOk)
            return true;
        *why = StreamlineResultText(result);
        return false;
    }

    std::string Streamline::Version(sl::Feature feature) const {
        if (!started_)
            return std::string();
        sl::FeatureVersion version{};
        if (slGetFeatureVersion(feature, version) != sl::Result::eOk)
            return std::string();
        return version.versionNGX ? version.versionNGX.toStr() : version.versionSL.toStr();
    }

}   // namespace psxemu
