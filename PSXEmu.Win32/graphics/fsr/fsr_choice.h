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

// What AMD FSR is asked for, and the arithmetic of sizes around it (Docs/FSR-Plan.md): the
// rasteriser's scale the front end picks for a mode, the length of the jitter, and the size FSR
// draws its output at. Header-only and free of AMD's headers, so the front end and the harness
// (tools/fsr_choice_test.cpp) use it as the renderer does.
//
// Unlike DLSS, FSR takes any input size for any output - its quality modes are only names for
// ratios, and its dispatch is told the two sizes and nothing else - so the scale is the mode's
// ratio as near as a whole scale gets it, and the output is always the picture's rectangle on
// the screen unless the input is larger than that.

#include "graphics/dlss/dlss_choice.h"   // DlssShownHeight, kDlssNativeLines

#include <algorithm>
#include <cmath>
#include <string>

namespace psxemu {

    enum class FsrMode { kOff, kNativeAa, kQuality, kBalanced, kPerformance, kUltraPerformance };
    // Which of AMD's upscalers: Auto is AMD's own choice for the card - FSR 4, the machine-learned
    // one, on a Radeon RX 7000 (discrete) or 9000; FSR 3.1 on everything else. FSR 3.1 runs on
    // any Direct3D 12 card.
    enum class FsrVersion { kAuto, kFsr4, kFsr3 };

    struct FsrChoice {
        FsrMode mode = FsrMode::kOff;
        FsrVersion version = FsrVersion::kAuto;
        // AMD's RCAS sharpening after the upscale, 0 (off) to 1.
        float sharpness = 0.5f;
        // FSR Frame Generation: one picture made between each two, with FSR Upscaling on.
        bool frame_generation = false;
        bool operator==(const FsrChoice&) const = default;
        bool generating() const { return mode != FsrMode::kOff && frame_generation; }
    };

    // AMD's files FSR needs beside the executable. MIT-licensed, but fetched rather than kept in
    // the repository (graphics/fsr/fetch_fidelityfx.ps1); the loader first, then the upscaler,
    // then Frame Generation's.
    inline constexpr const wchar_t* kFsrFiles[] = {
        L"amd_fidelityfx_loader_dx12.dll",
        L"amd_fidelityfx_upscaler_dx12.dll",
        L"amd_fidelityfx_framegeneration_dx12.dll",
    };
    inline constexpr wchar_t kFsrSdkLatest[] =
        L"https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/releases/latest";
    inline constexpr wchar_t kFsrBuiltWith[] = L"2.3.0";

    // Whether FSR runs in a renderer, and if not why not - in words for the menu.
    struct FsrStatus {
        bool ready = false;
        std::string why;        // when not ready
        std::string version;    // the upscaler running, as AMD names it: "4.1.1", "3.1.5"
        // A note beside a running FSR: the version asked for that the card does not run.
        std::string note;
        bool fsr4_available = false;   // the card runs FSR 4
        // Frame Generation, when asked for: whether it runs, and if not why not, and its version.
        bool generation_ready = false;
        std::string generation_why;
        std::string generation_version;
        bool operator==(const FsrStatus&) const = default;
    };

    // As EmuConfig keeps them: fsr_mode, fsr_version, fsr_sharpness.
    inline FsrMode ParseFsrMode(const std::string& key) {
        if (key == "native_aa") return FsrMode::kNativeAa;
        if (key == "quality") return FsrMode::kQuality;
        if (key == "balanced") return FsrMode::kBalanced;
        if (key == "performance") return FsrMode::kPerformance;
        if (key == "ultra_performance") return FsrMode::kUltraPerformance;
        return FsrMode::kOff;
    }
    inline FsrVersion ParseFsrVersion(const std::string& key) {
        if (key == "fsr4") return FsrVersion::kFsr4;
        if (key == "fsr3") return FsrVersion::kFsr3;
        return FsrVersion::kAuto;
    }
    inline float ParseFsrSharpness(const std::string& key) {
        if (key == "off") return 0.0f;
        if (key == "low") return 0.25f;
        if (key == "high") return 0.8f;
        if (key == "max") return 1.0f;
        return 0.5f;   // "medium"
    }

    // How much larger FSR's output is than its input in each mode, each way: AMD's own ratios.
    inline float FsrRatio(FsrMode mode) {
        switch (mode) {
        case FsrMode::kQuality: return 1.5f;
        case FsrMode::kBalanced: return 1.7f;
        case FsrMode::kPerformance: return 2.0f;
        case FsrMode::kUltraPerformance: return 3.0f;
        default: return 1.0f;
        }
    }

    // The hardware rasteriser's scale for `mode`, the picture shown `shown_height` tall: the
    // mode's ratio as near as a whole scale gets it. Native AA draws at the largest scale that
    // fits. One of the rasteriser's scales, 2-6 or 8: at 1x it hands over no picture of its own.
    inline int FsrScale(FsrMode mode, int shown_height) {
        if (mode == FsrMode::kOff || shown_height <= 0)
            return 1;
        const float lines = static_cast<float>(shown_height) / kDlssNativeLines;
        int scale = mode == FsrMode::kNativeAa
                        ? static_cast<int>(std::floor(lines))
                        : static_cast<int>(std::lround(lines / FsrRatio(mode)));
        scale = std::clamp(scale, 2, 8);
        return scale == 7 ? 6 : scale;   // no 7x
    }

    // How many pictures the jitter runs through before it repeats: AMD's rule, 8 x (output /
    // input)^2 rounded up, for the ratio FSR will actually work at - never under 8.
    inline int FsrJitterPhases(int shown_height, int scale) {
        float ratio = 1.0f;
        if (scale > 0)
            ratio = (std::max)(1.0f, static_cast<float>(shown_height) /
                                       static_cast<float>(kDlssNativeLines * scale));
        return std::clamp(static_cast<int>(std::ceil(8.0f * ratio * ratio)), 8, 128);
    }

    // The output for an input `input_width` x `input_height` shown in a screen rectangle
    // `screen_width` x `screen_height`: that rectangle, drawn one to one - unless the input is
    // larger than it either way (Native AA on a 640-wide mode, say), when FSR works at the
    // input's own size and the picture is scaled onto the screen as any is.
    inline void FsrOutput(int input_width, int input_height, int screen_width, int screen_height,
                          int* width, int* height) {
        if (input_width <= screen_width && input_height <= screen_height) {
            *width = screen_width;
            *height = screen_height;
        } else {
            *width = input_width;
            *height = input_height;
        }
    }

}   // namespace psxemu
