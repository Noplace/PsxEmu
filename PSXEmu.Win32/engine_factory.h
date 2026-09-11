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

// Bringing up the two devices the front end needs, each with a fallback, so that neither one being
// unavailable is a reason to refuse to start.
//
// This is the only file that names a concrete backend. Everything else holds an IGraphicsEngine or
// an IAudioEngine and never learns which one it got - which is what lets the Video menu swap the
// renderer under a running machine without anything else being told.

#include "framework.h"

#include "igraphicsengine.h"
#include "audio/iaudioengine.h"

namespace psxemu {

    enum class GraphicsBackend { kD3D11, kD3D12 };

    // Tries `preferred` first; if that engine's own device creation fails, tries the other one and
    // warns that it did, rather than failing outright - a machine that can do one almost always can
    // do the other. Only if both fail does this return null, which the caller treats as a hard
    // failure (at startup) or a "could not switch, and could not go back either" one (mid session,
    // from the Video menu).
    //
    // `*active_backend` is set to whichever engine actually ended up running, which the caller uses
    // instead of the requested one for menu ticks and persisted state from here on.
    std::unique_ptr<IGraphicsEngine> CreateGraphicsEngine(GraphicsBackend preferred, HWND window,
                                                          int width, int height, HWND message_owner,
                                                          std::string* active_backend);

    // Compiles every ported filter into the engine at once - cheap (startup-cost D3DCompile calls,
    // not per-frame work), so there is no reason to defer any of them until first selected. Only
    // worth calling on an engine that supports filters at all; see IGraphicsEngine.
    void LoadAllFilters(IGraphicsEngine& engine);

    // Tries the modern output first and falls back. Audio is optional: a machine with no working
    // output device should still run, silently, rather than refusing to start - so null here is an
    // ordinary outcome, not a failure the caller has to report.
    std::unique_ptr<IAudioEngine> CreateAudioEngine();

}   // namespace psxemu
