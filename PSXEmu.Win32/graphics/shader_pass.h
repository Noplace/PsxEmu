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

// One stage of a multi-pass filter chain (IGraphicsEngine::LoadShaderChain), on its own so the
// arithmetic that builds chains (graphics/filter_chain.h) can be tested without an engine.

#include <string>
#include <vector>

namespace psxemu {

    struct ShaderPass {
        // Key of a pixel shader already given to LoadCustomPixelShader or LoadPixelShaderFromString.
        std::string shader;
        // Size of this pass's render target, as a whole multiple of the emulator's framebuffer.
        // 0 means "draw straight into the letterboxed picture area of the window"; only the last
        // pass of a chain may use it. Any other last pass is followed by a linear-filtered blit.
        int scale = 0;
        // What the pass reads as its original (t1): the output of the pass at this index - an
        // earlier one, with a target of its own - or the emulator's frame, -1. A filter of several
        // passes that looks back at its own input (Super-xBR's second) needs that input to be what
        // the filter was given, which is a pass's output once the filter is not first in a chain.
        int original = -1;
    };

    // Whether `passes` is a chain an engine can run: every pass but the last has a target of its
    // own, and each original is the frame or an earlier pass's target. Engines check this in
    // LoadShaderChain, before anything of their own.
    inline bool IsRunnableChain(const std::vector<ShaderPass>& passes) {
        if (passes.empty())
            return false;
        for (size_t i = 0; i < passes.size(); ++i) {
            const ShaderPass& pass = passes[i];
            if (pass.scale < 0 || (pass.scale == 0 && i + 1 != passes.size()))
                return false;
            if (pass.original < -1 || pass.original >= static_cast<int>(i))
                return false;
        }
        return true;
    }

}   // namespace psxemu
