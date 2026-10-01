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

// NVIDIA Streamline, loaded when DLSS is asked for (Docs/DLSS-Plan.md, phase 4).
//
// Nothing links against it: sl.interposer.dll is loaded from beside the executable only when a
// renderer wants DLSS, after NVIDIA's signature on it has been checked, and every function is
// taken from it by name. Without the DLLs - on a build that never fetched them - or on a card
// that is not NVIDIA's, it says why and the emulator runs as ever.
//
// It is started for manual hooking: only the device, factory and swap chain a renderer hands it
// go through it, and nothing else in the process. And with neither optional updates nor
// downloaded plugins - both Streamline's default - so the DLLs shipped are the ones that run.
// What it does regardless, in its release builds, whenever an NVIDIA card is present: it starts
// the driver's NGX updater (nvngx_update.exe) to check NVIDIA's servers, which may download newer
// Streamline plugins into ProgramData\NVIDIA\NGX. Those are never loaded here. Every Streamline
// application does the same; there is no preference that stops it (source/core/sl.ota/ota.cpp,
// OTA::checkForOTA).

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "graphics/dlss/streamline/sl.h"
#include "graphics/dlss/streamline/sl_consts.h"
#include "graphics/dlss/streamline/sl_dlss.h"
#include "graphics/dlss/streamline/sl_dlss_g.h"
#include "graphics/dlss/streamline/sl_pcl.h"
#include "graphics/dlss/streamline/sl_reflex.h"

#include <string>

namespace psxemu {

    class Streamline {
     public:
        Streamline() = default;
        ~Streamline() { Stop(); }
        Streamline(const Streamline&) = delete;
        Streamline& operator=(const Streamline&) = delete;

        // Loads sl.interposer.dll from `folder` - the executable's - checks its signature, and
        // starts Streamline with DLSS loaded, and Frame Generation (with the Reflex and PC
        // Latency it needs) if asked for. False, with `error` saying why, and nothing started,
        // if any step fails.
        bool Start(const std::wstring& folder, bool frame_generation, std::string* error);
        // Gives Streamline the renderer's device (an ID3D12Device, not upgraded), and then takes
        // DLSS's own functions, which only exist from here on - and Frame Generation's, Reflex's
        // and PC Latency's, if started with them; `generating()` says whether they were all there.
        bool SetDevice(void* device, std::string* error);
        // Shuts Streamline down: before the device it was given goes. The DLL stays loaded, for
        // the next Start.
        void Stop();
        bool started() const { return started_; }

        // Whether a feature runs on the card with this LUID; if not, why not.
        bool Supports(sl::Feature feature, uint64_t luid, std::string* why) const;
        bool SupportsDlss(uint64_t luid, std::string* why) const {
            return Supports(sl::kFeatureDLSS, luid, why);
        }
        // A feature's own version (NGX's where it has one), once started: "310.9.1".
        std::string Version(sl::Feature feature) const;
        std::string DlssVersion() const { return Version(sl::kFeatureDLSS); }

        // The functions, taken from the DLL by name; null until Start.
        PFun_slShutdown* slShutdown = nullptr;
        PFun_slIsFeatureSupported* slIsFeatureSupported = nullptr;
        PFun_slGetFeatureRequirements* slGetFeatureRequirements = nullptr;
        PFun_slSetD3DDevice* slSetD3DDevice = nullptr;
        PFun_slUpgradeInterface* slUpgradeInterface = nullptr;
        PFun_slGetNativeInterface* slGetNativeInterface = nullptr;
        PFun_slGetNewFrameToken* slGetNewFrameToken = nullptr;
        PFun_slSetConstants* slSetConstants = nullptr;
        PFun_slSetTagForFrame* slSetTagForFrame = nullptr;
        PFun_slEvaluateFeature* slEvaluateFeature = nullptr;
        PFun_slFreeResources* slFreeResources = nullptr;
        PFun_slGetFeatureVersion* slGetFeatureVersion = nullptr;
        PFun_slGetFeatureFunction* slGetFeatureFunction = nullptr;
        // ...and DLSS's, through slGetFeatureFunction once SetDevice has been.
        PFun_slDLSSGetOptimalSettings* slDLSSGetOptimalSettings = nullptr;
        PFun_slDLSSSetOptions* slDLSSSetOptions = nullptr;
        // Frame Generation's, Reflex's and PC Latency's, when started with them: all or none.
        PFun_slDLSSGSetOptions* slDLSSGSetOptions = nullptr;
        PFun_slDLSSGGetState* slDLSSGGetState = nullptr;
        PFun_slReflexSetOptions* slReflexSetOptions = nullptr;
        PFun_slReflexSleep* slReflexSleep = nullptr;
        PFun_slReflexGetState* slReflexGetState = nullptr;
        PFun_slPCLSetMarker* slPCLSetMarker = nullptr;
        PFun_slPCLGetState* slPCLGetState = nullptr;
        PFun_slPCLSetOptions* slPCLSetOptions = nullptr;
        bool generating() const { return slDLSSGSetOptions != nullptr; }

        // Streamline's last warning or error, from its log.
        const std::string& last_message() const { return last_message_; }

     private:
        // Loads the DLL - once per process, after its signature is checked - and takes the
        // functions from it.
        bool Load(const std::wstring& folder, std::string* error);
        void ForgetGeneration();
        static void Log(sl::LogType type, const char* message);

        bool started_ = false;
        bool frame_generation_ = false;   // started with Frame Generation, Reflex and PCL
        PFun_slInit* slInit_ = nullptr;
        static std::string last_message_;
    };

    // The folder the executable is in, for Streamline::Start.
    std::wstring ExecutableFolder();

    // A line for dlss.log in the folder PSXEMU_DLSS_LOG names, beside Streamline's own sl.log:
    // what the renderer made of DLSS, for finding out why it did not run. Nothing without it.
    void DlssNote(const std::string& line);

    // A Streamline result as words.
    const char* StreamlineResultText(sl::Result result);

}   // namespace psxemu
