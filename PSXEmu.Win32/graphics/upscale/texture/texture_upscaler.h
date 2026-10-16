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

// What the AI texture upscaler asks of a backend (Docs/Texture-Upscaling-Plan.md): an RGB picture
// in, planar floats 0-1 - all the reds, then the greens, then the blues - and one a whole number of
// times larger out. Two backends: an ONNX model on DirectML (onnx_runtime.h), and NVIDIA's RTX
// Video Super Resolution (rtx_video.h). Neither knows about transparency; the caller fills holes
// before and puts the coverage through separately.

#include <string>
#include <vector>

namespace psxemu {

    class TextureUpscaler {
     public:
        virtual ~TextureUpscaler() = default;

        // The only size it takes, or 0 for a side that may be any size.
        virtual int fixed_width() const { return 0; }
        virtual int fixed_height() const { return 0; }
        // How many times larger the output is - 0 until the first Run if the backend learns it then.
        virtual int scale() const = 0;
        // A line saying what it is, for the tools' output.
        virtual std::string description() const = 0;

        // Upscales `rgb`, `width` x `height`, into `out`, planar as it came. False, with `error`,
        // if the backend fails.
        virtual bool Run(const std::vector<float>& rgb, int width, int height,
                         std::vector<float>* out, int* out_width, int* out_height,
                         std::string* error) = 0;
    };

    // Whether the file at `path` carries a valid Authenticode signature whose signer's
    // organisation is `organisation` ("Microsoft Corporation", "NVIDIA Corporation"): Windows' own
    // check, without asking the network about revocation.
    bool SignedByOrganisation(const std::wstring& path, const char* organisation);

}   // namespace psxemu
