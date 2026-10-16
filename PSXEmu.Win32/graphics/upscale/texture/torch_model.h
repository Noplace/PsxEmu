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

// A PyTorch upscaling model's weights, read from its .pth file, and the same network written out as
// ONNX for ONNX Runtime (Docs/Texture-Upscaling-Plan.md, phase 0).
//
// The upscalers whose licences let the emulator offer them - Real-ESRGAN's (BSD-3), and the
// pixel-art and game-texture ESRGANs under CC0 and WTFPL - are published only as .pth files, and
// there is no Python here to convert them. A .pth is a pickle: PyTorch's own loader runs whatever
// the file names, which is why this one does not. It reads the pickle as data - dictionaries,
// tuples, numbers, strings and tensors - and any other object it names stays an inert placeholder;
// nothing in the file is ever called. Both of PyTorch's layouts are read: the zip of a pickle and
// raw storages it has used since 1.6, and the single stream of pickles before that.
//
// Two architectures are known, which between them are every candidate the plan names:
//   - ESRGAN's RRDBNet, under the original repository's names (model.0, model.1.sub.N.RDBk...) and
//     BasicSR's (conv_first, body.N.rdbk...), 1x to 8x
//   - Real-ESRGAN's SRVGGNetCompact (body.N), with PReLU or leaky ReLU

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace psxemu {

    struct TorchTensor {
        std::vector<int64_t> shape;
        std::vector<float> data;   // converted to float whatever it was stored as
    };

    // The state dict in the .pth at `path`: each parameter by its name. A file holding the dict
    // under a key ("params_ema", "params", "state_dict" - Real-ESRGAN's and BasicSR's) gives that
    // dict, the exponential moving average first. False, with `error` saying why, if the file is
    // neither layout or holds something other than tensors.
    bool ReadTorchStateDict(const std::wstring& path, std::map<std::string, TorchTensor>* weights,
                            std::string* error);

    // What a state dict was recognised as.
    struct UpscalerArchitecture {
        std::string name;   // "esrgan" or "compact"
        int scale = 0;
        int features = 0;   // ESRGAN's nf, compact's num_feat
        int blocks = 0;     // ESRGAN's RRDBs, compact's convolutions
        int in_channels = 0, out_channels = 0;
    };

    // The network `weights` describe, as an ONNX model (opset 17, NCHW float, height and width
    // left free), into `onnx`. False, with `error`, if the weights are not one of the two
    // architectures above, or are and do not fit together.
    bool ExportUpscalerOnnx(const std::map<std::string, TorchTensor>& weights, std::string* onnx,
                            UpscalerArchitecture* architecture, std::string* error);

}   // namespace psxemu
