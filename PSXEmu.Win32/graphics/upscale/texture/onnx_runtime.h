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
#pragma once

// ONNX Runtime with its DirectML execution provider, for the AI texture upscaler
// (Docs/Texture-Upscaling-Plan.md).
//
// Nothing links against it: onnxruntime.dll and DirectML.dll are loaded from a folder - the
// executable's - only when an upscaler is asked for, after Microsoft's signature on each has been
// checked, and the API is taken from OrtGetApiBase. Without the DLLs it says why and nothing else
// changes. DirectML is loaded first, by its full path, so that ONNX Runtime's import of it is that
// module and never Windows' own older copy in System32.
//
// A model runs on a Direct3D 12 device it is given - one made on the card the rasteriser draws on -
// through a DirectML device and a compute queue of that device's, so which card it runs on is ours
// to say, by LUID, as for everything else that draws.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include "graphics/third_party/onnxruntime/onnxruntime_c_api.h"
#include "graphics/upscale/texture/texture_upscaler.h"

#include <memory>
#include <string>
#include <vector>

struct OrtDmlApi;

namespace psxemu {

    class OnnxRuntime {
     public:
        // The one per process: the DLLs are loaded once and never let go of.
        static OnnxRuntime& Get();

        // Loads DirectML.dll and onnxruntime.dll from `folder` once, after checking Microsoft's
        // signature on them and on onnxruntime_providers_shared.dll. False, with `error` saying
        // why, if any step fails; true again at once once loaded.
        bool Load(const std::wstring& folder, std::string* error);
        bool loaded() const { return api_ != nullptr; }

        const OrtApi* api() const { return api_; }
        const OrtDmlApi* dml() const { return dml_; }
        OrtEnv* env() const { return env_; }
        // ONNX Runtime's own version, "1.24.4".
        const std::string& version() const { return version_; }
        // DirectML's DMLCreateDevice, from the DLL loaded beside it.
        HRESULT CreateDmlDevice(ID3D12Device* device, REFIID iid, void** dml_device) const;

        // `status` as words, released; empty for success.
        std::string Error(OrtStatus* status) const;

     private:
        OnnxRuntime() = default;
        const OrtApi* api_ = nullptr;
        const OrtDmlApi* dml_ = nullptr;
        OrtEnv* env_ = nullptr;
        void* create_dml_device_ = nullptr;
        std::string version_;
        std::wstring folder_;
    };

    // Whether the file at `path` carries a valid Authenticode signature by Microsoft
    // (SignedByOrganisation, "Microsoft Corporation").
    bool SignedByMicrosoft(const std::wstring& path);

    // One upscaling model, loaded on one device (TextureUpscaler).
    class OnnxUpscaler : public TextureUpscaler {
     public:
        // Loads the model at `path` onto `device`, with a compute queue of its own. Null, with
        // `error` saying why, if it cannot: ONNX Runtime not loaded, a model that is not one
        // picture in and one out, or anything DirectML refuses.
        static std::unique_ptr<OnnxUpscaler> Create(const std::wstring& path, ID3D12Device* device,
                                                    std::string* error);
        ~OnnxUpscaler() override;

        // The model's input as it declares it: 0 for a side that may be any size, otherwise the
        // only size it takes. Channels are 3.
        int fixed_width() const override { return fixed_width_; }
        int fixed_height() const override { return fixed_height_; }
        // Whether it takes and gives half floats (converted here, either way).
        bool half() const { return half_; }
        // Learnt from the first Run, 0 before.
        int scale() const override { return scale_; }
        std::string description() const override;

        // False also if the output is not a whole multiple of the input.
        bool Run(const std::vector<float>& rgb, int width, int height, std::vector<float>* out,
                 int* out_width, int* out_height, std::string* error) override;

     private:
        OnnxUpscaler() = default;
        OrtSession* session_ = nullptr;
        OrtMemoryInfo* memory_ = nullptr;
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue_;
        Microsoft::WRL::ComPtr<IUnknown> dml_device_;
        std::string input_name_, output_name_;
        int fixed_width_ = 0, fixed_height_ = 0;
        bool half_ = false;
        int scale_ = 0;
    };

}   // namespace psxemu
