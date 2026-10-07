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

// AMD's FidelityFX API, loaded when FSR is asked for (Docs/FSR-Plan.md).
//
// Nothing links against it: amd_fidelityfx_loader_dx12.dll is loaded from beside the executable
// only when a renderer wants FSR, after AMD's signature on it - and on the effect DLLs it loads
// from the same folder - has been checked, and its five functions are taken by name. Without the
// DLLs it says why and the emulator runs as ever. Unlike NVIDIA's Streamline there is nothing to
// start or stop: each effect is a context made on the renderer's own device, and nothing else in
// the process passes through AMD's code.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>

#include "graphics/fsr/fidelityfx/api/include/ffx_api.h"
#include "graphics/fsr/fidelityfx/api/include/ffx_api_types.h"
#include "graphics/fsr/fidelityfx/api/include/dx12/ffx_api_dx12.h"
#include "graphics/fsr/fidelityfx/upscalers/include/ffx_upscale.h"
#include "graphics/fsr/fidelityfx/framegeneration/include/ffx_framegeneration.h"
#include "graphics/fsr/fidelityfx/framegeneration/include/dx12/ffx_api_framegeneration_dx12.h"

#include <cstdint>
#include <string>
#include <vector>

namespace psxemu {

    class FidelityFx {
     public:
        // The one per process: the DLL is loaded once and never let go of.
        static FidelityFx& Get();

        // Loads the loader DLL from `folder` - the executable's - once, after checking AMD's
        // signature on it and on the upscaler beside it (and Frame Generation's, if there). False,
        // with `error` saying why, if any step fails; true again at once once loaded.
        bool Load(const std::wstring& folder, std::string* error);
        bool loaded() const { return create_ != nullptr; }
        // Whether Frame Generation's DLL was beside the loader, signed, when it was loaded.
        bool has_frame_generation() const { return frame_generation_; }

        // The five functions; null until Load.
        ffxReturnCode_t CreateContext(ffxContext* context, ffxApiHeader* desc) const {
            return create_(context, desc, nullptr);
        }
        ffxReturnCode_t DestroyContext(ffxContext* context) const {
            return destroy_(context, nullptr);
        }
        ffxReturnCode_t Configure(ffxContext* context, const ffxApiHeader* desc) const {
            return configure_(context, desc);
        }
        ffxReturnCode_t Query(ffxContext* context, ffxApiHeader* desc) const {
            return query_(context, desc);
        }
        ffxReturnCode_t Dispatch(ffxContext* context, const ffxApiHeader* desc) const {
            return dispatch_(context, desc);
        }

        // The versions of an effect that run on `device`, best first as AMD lists them - for the
        // upscaler "4.1.1" (FSR 4) where the card takes it, then "3.1.5" and "2.3.4" - each with
        // the id that asks for it (ffxOverrideVersion). Empty if none, or not loaded.
        struct Version {
            uint64_t id = 0;
            std::string name;
        };
        std::vector<Version> Versions(uint64_t create_desc_type, ID3D12Device* device) const;

     private:
        FidelityFx() = default;
        PfnFfxCreateContext create_ = nullptr;
        PfnFfxDestroyContext destroy_ = nullptr;
        PfnFfxConfigure configure_ = nullptr;
        PfnFfxQuery query_ = nullptr;
        PfnFfxDispatch dispatch_ = nullptr;
        bool frame_generation_ = false;
        std::wstring folder_;
    };

    // Whether the file at `path` carries a valid Authenticode signature by Advanced Micro
    // Devices: Windows' own check, with the signer's name read from its certificate.
    bool SignedByAmd(const std::wstring& path);

    // An ffx return code as words.
    const char* FfxResultText(ffxReturnCode_t code);

    // A line for fsr.log in the folder PSXEMU_FSR_LOG names: what the renderer made of FSR, and
    // what AMD's runtime said, for finding out why it did not run. Nothing without it.
    void FsrNote(const std::string& line);
    bool FsrLogging();

    // An ffx resource for a Direct3D 12 texture in `state` (FFX_API_RESOURCE_STATE_*).
    FfxApiResource FfxResource(ID3D12Resource* resource, uint32_t state);

}   // namespace psxemu
