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

// The custom filter chain - Settings > Video > Filter, "Custom chain": up to four of the filters
// run one after another, each on the picture the one before made (EmuConfig::filter_chain).
//
// Every filter here was written to be the only one, reading the console's picture and drawing
// into the window. In a chain all but the last draw into a texture instead, and this is where
// each one's texture is sized:
//
// - The upscalers - Super-xBR, SuperEagle, xBRZ, and the two placeholders under those names -
//   make a picture twice the size of the one they are given. Super-xBR is three passes, each
//   at twice its input, the second looking back at that input as its original.
// - The rest - Nearest, Bilinear, CRT, Scanline - keep the size they are given.
// - The last stage draws into the window, as the filter alone would - except Super-xBR, whose
//   last pass is a texture, stretched onto the window by the engine's own blit.
//
// The picture is never taken past four times the console's: two upscalers. A third is left
// out, and said to be (FilterChainPlan::dropped) - eight times a 640-line picture is a 5120 by
// 3840 texture per pass, a quarter of a gigabyte for Super-xBR's three.
//
// CRT and Scanline after an upscaler work on its picture as they would on the console's: a
// scanline for every other line of it, twice as many as the console drew. That is the chain
// asked for, and the Video Settings window's hint says so; put them first, or alone, for the
// console's own lines.

#include "graphics/shader_pass.h"

#include <string>
#include <vector>

namespace psxemu {

    // The key the custom chain is loaded under, and EmuConfig::video_filter's value for it.
    inline constexpr const char* kCustomChainKey = "chain";
    // The most the picture is scaled, in multiples of the console's.
    inline constexpr int kFilterChainMaxScale = 4;

    // Whether a filter doubles the picture it is given.
    inline bool FilterUpscales(const std::string& key) {
        return key == "superxbr" || key == "eagle" || key == "xbrz" || key == "hq2x" ||
               key == "xbrz_legacy";
    }

    struct FilterChainPlan {
        std::vector<ShaderPass> passes;   // for LoadShaderChain; empty draws the picture plain
        std::vector<std::string> dropped; // stages left out, past kFilterChainMaxScale
    };

    // The passes for `stages`, each a filter key (EmuConfig::kValidVideoFilters but "" and
    // "chain"; anything else is skipped). The pass shaders are LoadAllFilters' keys.
    inline FilterChainPlan PlanFilterChain(const std::vector<std::string>& stages) {
        FilterChainPlan plan;
        // The stages that run: those that fit, so the last one is known before any is planned.
        std::vector<std::string> running;
        int scale = 1;
        for (const std::string& key : stages) {
            if (key.empty() || key == kCustomChainKey)
                continue;
            if (FilterUpscales(key)) {
                if (scale * 2 > kFilterChainMaxScale) {
                    plan.dropped.push_back(key);
                    continue;
                }
                scale *= 2;
            }
            running.push_back(key);
        }

        scale = 1;
        int input = -1;   // the pass whose output the next stage reads: -1 for the frame
        for (size_t s = 0; s < running.size(); ++s) {
            const std::string& key = running[s];
            const bool last = s + 1 == running.size();
            if (key == "superxbr") {
                scale *= 2;
                for (const char* pass : { "superxbr_pass0", "superxbr_pass1", "superxbr_pass2" })
                    plan.passes.push_back({ pass, scale, input });
                input = static_cast<int>(plan.passes.size()) - 1;
                continue;
            }
            if (FilterUpscales(key))
                scale *= 2;
            plan.passes.push_back({ key, last ? 0 : scale, -1 });
            input = static_cast<int>(plan.passes.size()) - 1;
        }
        return plan;
    }

}   // namespace psxemu
