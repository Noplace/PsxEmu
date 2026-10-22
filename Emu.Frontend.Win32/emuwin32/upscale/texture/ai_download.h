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

// The AI texture upscaler's files fetched by the emulator itself (Docs/Texture-Upscaling-Plan.md,
// phase 3), from Settings > Video > Textures' "Get the AI files...": what
// fetch_onnxruntime.ps1 fetches for the tools, and the models phase 0 chose.
//
// - **ONNX Runtime and DirectML** come in NuGet packages, the second 202 MB for one 18 MB DLL. So
//   neither is downloaded whole: the zip's directory is read from its end, and only the entries
//   wanted are fetched, by HTTP ranges, from nuget.org - each checked against the CRC-32 the
//   directory gives, and each DLL against Microsoft's Authenticode signature, before it is put in
//   place. Version for version what phase 0 measured: ONNX Runtime 1.24.4, DirectML 1.15.4.
// - **The models** come from their author's GitHub release, xinntao's Real-ESRGAN v0.2.5.0, as
//   PyTorch .pth files, each checked against the SHA-256 it had when phase 0 read it, and converted
//   to ONNX here (torch_model.h) into models\, with Real-ESRGAN's licence (BSD-3) beside them.
//
// Everything goes to "<name>.part" first, so a download stopped half-way leaves nothing that loads.

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace emulation::win32 {

    // How far it has got, for a dialog on another thread.
    struct AiDownloadProgress {
        std::atomic<uint64_t> done{ 0 };    // bytes, all files
        std::atomic<uint64_t> total{ 0 };   // ...known so far
        std::atomic<int> step{ 0 };         // which of AiDownloadSteps
        std::atomic<bool> cancel{ false };  // set to stop it
    };
    inline constexpr const wchar_t* kAiDownloadSteps[] = {
        L"ONNX Runtime", L"DirectML", L"The model", L"Converting the model",
    };

    // The files PSXEmu's folder (`folder`, with its trailing backslash) lacks, fetched and put in
    // place: ONNX Runtime and DirectML, and each of `models` ("anime", "general") not yet in
    // models\. Blocking - run it off the UI thread. False, with `error` in words, if a step fails
    // or it is cancelled; files already put in place stay, being good.
    bool DownloadAiFiles(const std::wstring& folder, const std::vector<std::string>& models,
                         AiDownloadProgress* progress, std::wstring* error);

    // A model's file name in models\: "realesr-animevideov3.onnx" for "anime".
    std::wstring AiModelFileName(const std::string& model);

}   // namespace emulation::win32
