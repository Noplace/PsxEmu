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

// NVIDIA's RTX Video Super Resolution as a texture upscaler backend (Docs/Texture-Upscaling-Plan.md,
// phases 0 and 4).
//
// The RTX Video SDK sits behind NVIDIA's developer sign-in and its headers and libraries are
// NVIDIA's own, not open: none of it is committed. It is unpacked by hand into Temp\rtxvideo\, and
// only a build that finds it there defines PSXEMU_RTX_VIDEO and compiles the backend; any other
// build compiles this file to a Create that says why there is none. The feature DLL, nvngx_vsr.dll
// (the SDK's `rel` build - the `dev` one draws a watermark into its output), is loaded by NGX from
// beside the executable after NVIDIA's signature on it has been checked here.
//
// VSR is a video model: it upscales, sharpens and takes out compression's blocks, any size in to
// any size out, at quality 1-4 (0 is plain bicubic). Here it is given RGBA8 and asked for a whole
// number of times larger, like the ONNX models, so the two can be compared on the same pictures.

#include "emuwin32/upscale/texture/texture_upscaler.h"

#include <memory>
#include <string>

struct ID3D12Device;

namespace emulation::win32 {

    class RtxVideoUpscaler : public TextureUpscaler {
     public:
        // Whether this build has the backend at all (the SDK was there when it was built).
        static bool Built();

        // VSR on `device`, `scale` times larger at `quality` (0-4), the feature DLL looked for in
        // `folder`. Null, with `error` saying why: not built in, the DLL missing or not NVIDIA's,
        // not an RTX card or too old a driver, or NGX refusing.
        static std::unique_ptr<RtxVideoUpscaler> Create(ID3D12Device* device,
                                                        const std::wstring& folder, int scale,
                                                        int quality, std::string* error);
        ~RtxVideoUpscaler() override;

        int scale() const override { return scale_; }
        std::string description() const override;
        bool Run(const std::vector<float>& rgb, int width, int height, std::vector<float>* out,
                 int* out_width, int* out_height, std::string* error) override;

     private:
        RtxVideoUpscaler() = default;
        struct State;
        std::unique_ptr<State> state_;
        int scale_ = 0, quality_ = 0;
    };

}   // namespace emulation::win32
