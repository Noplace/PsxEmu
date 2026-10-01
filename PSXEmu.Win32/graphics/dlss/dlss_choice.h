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

// What DLSS is asked for, and the arithmetic of sizes around it (Docs/DLSS-Plan.md, phase 4): the
// rasteriser's scale the front end picks for a mode, the length of the jitter, and the size DLSS
// draws its output at. Header-only and free of Streamline, so the front end and the harness
// (tools/dlss_choice_test.cpp) use it as the renderer does.
//
// What DLSS 310.9.1 takes, measured on the RTX 4060 with sl_probe --optimal: Quality, Balanced and
// Performance any input from half the output to all of it, each way; DLAA the output's size to
// within 1%; Ultra Performance exactly a third. The rasteriser's scales are whole numbers of the
// console's 240 lines, so a mode's own ratio is only ever approached.

#include <algorithm>
#include <cmath>
#include <string>

namespace psxemu {

    enum class DlssMode { kOff, kDlaa, kQuality, kBalanced, kPerformance, kUltraPerformance };
    // Auto is NVIDIA's own per mode: K for DLAA, Quality and Balanced, M for Performance, L for
    // Ultra Performance.
    enum class DlssPreset { kAuto, kK, kL, kM };

    // DLSS Frame Generation (phase 5): how many pictures it makes for each one drawn - 0 off, 1
    // for 2x, up to 5 for 6x (RTX 50) - or kDlssGenerationDynamic, to the screen's rate (RTX 50).
    // It runs only with Super Resolution (a mode other than Off): it takes the same motion and
    // depth.
    inline constexpr int kDlssGenerationDynamic = -1;

    struct DlssChoice {
        DlssMode mode = DlssMode::kOff;
        DlssPreset preset = DlssPreset::kAuto;
        int frame_generation = 0;
        bool operator==(const DlssChoice&) const = default;
        bool generating() const { return mode != DlssMode::kOff && frame_generation != 0; }
    };

    // Whether DLSS runs in a renderer, and if not why not - in words for the menu.
    struct DlssStatus {
        bool ready = false;
        std::string why;       // when not ready
        std::string version;   // DLSS's own, when ready: "310.9.1"
        // Frame Generation, when asked for: whether it runs, and if not why not; the most
        // pictures the card makes for one (1: 2x only; 5: up to 6x) and whether it makes as
        // many as the screen wants (Dynamic).
        bool generation_ready = false;
        std::string generation_why;
        int generation_most = 0;
        bool generation_dynamic = false;
        bool operator==(const DlssStatus&) const = default;
    };

    // As EmuConfig keeps dlss_frame_generation: "off", "2x" to "6x", "dynamic".
    inline int ParseDlssGeneration(const std::string& key) {
        if (key == "dynamic") return kDlssGenerationDynamic;
        if (key.size() == 2 && key[1] == 'x' && key[0] >= '2' && key[0] <= '6')
            return key[0] - '1';
        return 0;
    }

    // As EmuConfig keeps them: dlss_mode and dlss_preset.
    inline DlssMode ParseDlssMode(const std::string& key) {
        if (key == "dlaa") return DlssMode::kDlaa;
        if (key == "quality") return DlssMode::kQuality;
        if (key == "balanced") return DlssMode::kBalanced;
        if (key == "performance") return DlssMode::kPerformance;
        if (key == "ultra_performance") return DlssMode::kUltraPerformance;
        return DlssMode::kOff;
    }
    inline DlssPreset ParseDlssPreset(const std::string& key) {
        if (key == "k") return DlssPreset::kK;
        if (key == "l") return DlssPreset::kL;
        if (key == "m") return DlssPreset::kM;
        return DlssPreset::kAuto;
    }

    // How much larger DLSS's output is than its input in each mode, each way.
    inline float DlssRatio(DlssMode mode) {
        switch (mode) {
        case DlssMode::kQuality: return 1.5f;
        case DlssMode::kBalanced: return 1.72f;
        case DlssMode::kPerformance: return 2.0f;
        case DlssMode::kUltraPerformance: return 3.0f;
        default: return 1.0f;
        }
    }

    // The console's lines, which the scale multiplies: 240 for nearly every game. 480-line
    // pictures are interlaced, and DLSS leaves them alone.
    inline constexpr int kDlssNativeLines = 240;

    // How tall the picture is on a window's client area: the 4:3 letterbox every renderer draws
    // it in (tools/letterbox.h).
    inline int DlssShownHeight(int client_width, int client_height) {
        if (client_width <= 0 || client_height <= 0)
            return 0;
        return (std::min)(client_height, client_width * 3 / 4);
    }

    // The hardware rasteriser's scale for `mode`, the picture shown `shown_height` tall: the
    // mode's ratio as near as a whole scale gets it, within what DLSS takes - no less than half
    // the output (Quality to Performance) and no more than all of it. DLAA draws at the largest
    // scale that fits, and Ultra Performance at a third, since neither takes anything else. One of
    // the rasteriser's scales, 2-6 or 8: at 1x it hands over no picture of its own for DLSS to
    // take (native VRAM's picture is shown, exactly as the software rasteriser's).
    inline int DlssScale(DlssMode mode, int shown_height) {
        if (mode == DlssMode::kOff || shown_height <= 0)
            return 1;
        const float lines = static_cast<float>(shown_height) / kDlssNativeLines;
        const int most = (std::max)(1, static_cast<int>(std::floor(lines)));
        int scale;
        if (mode == DlssMode::kDlaa) {
            scale = most;
        } else if (mode == DlssMode::kUltraPerformance) {
            scale = (std::max)(1, static_cast<int>(std::lround(lines / 3.0f)));
        } else {
            const int least = (std::max)(1, static_cast<int>(std::ceil(lines / 2.0f)));
            scale = static_cast<int>(std::lround(lines / DlssRatio(mode)));
            scale = std::clamp(scale, least, (std::max)(least, most));
        }
        scale = std::clamp(scale, 2, 8);
        return scale == 7 ? 6 : scale;   // no 7x
    }

    // How many pictures the jitter runs through before it repeats: NVIDIA's rule, at least
    // 8 x (output / input)^2, for the ratio DLSS will actually work at.
    inline int DlssJitterPhases(DlssMode mode, int shown_height, int scale) {
        float ratio = 1.0f;
        if (mode == DlssMode::kUltraPerformance)
            ratio = 3.0f;
        else if (mode != DlssMode::kDlaa && scale > 0)
            ratio = (std::max)(1.0f, static_cast<float>(shown_height) /
                                       static_cast<float>(kDlssNativeLines * scale));
        return std::clamp(static_cast<int>(std::ceil(8.0f * ratio * ratio)), 8, 128);
    }

    // DLSS's range for one output size: the input sizes it takes, as slDLSSGetOptimalSettings
    // says them.
    struct DlssRange {
        int least_width = 0, least_height = 0;
        int most_width = 0, most_height = 0;
        bool Takes(int width, int height) const {
            return width >= least_width && width <= most_width && height >= least_height &&
                   height <= most_height;
        }
    };

    // The output first tried: the picture's rectangle on the screen, drawn one to one - except for
    // DLAA and Ultra Performance, which never take a whole scale of the console's lines for it,
    // and go straight to DlssOwnOutput.
    inline bool DlssTriesScreen(DlssMode mode) {
        return mode == DlssMode::kQuality || mode == DlssMode::kBalanced ||
               mode == DlssMode::kPerformance;
    }

    // ...and the output when the screen will not do: the input times the mode's own ratio, which
    // the renderer then scales onto the screen as it does any picture. DLAA is then
    // anti-aliasing at the rasteriser's resolution; a 256-wide picture, too narrow for the
    // screen's range, still goes through the mode.
    inline void DlssOwnOutput(DlssMode mode, int input_width, int input_height, int* width,
                              int* height) {
        const float ratio = DlssRatio(mode);
        *width = static_cast<int>(std::lround(input_width * ratio));
        *height = static_cast<int>(std::lround(input_height * ratio));
    }

}   // namespace psxemu
