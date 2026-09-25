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

    enum class GraphicsBackend { kD3D11, kD3D12, kOpenGL };

    // The settings key ("d3d11", "d3d12", "opengl") as that enum; anything else is Direct3D 11,
    // the default.
    GraphicsBackend ParseGraphicsBackend(const std::string& key);
    const char* GraphicsBackendKey(GraphicsBackend backend);

    // Tries `preferred` first; if that engine's own device creation fails, tries the others in turn
    // (Direct3D 11, then 12, then OpenGL) and warns that it did, rather than failing outright - a
    // machine that can do one almost always can do another. Only if both fail does this return null, which the caller treats as a hard
    // failure (at startup) or a "could not switch, and could not go back either" one (mid session,
    // from the Video menu).
    //
    // `*active_backend` is set to whichever engine actually ended up running, which the caller uses
    // instead of the requested one for menu ticks and persisted state from here on.
    //
    // A fallback does not put a dialog up itself: it writes what it would have said into
    // `*warning`, and the caller shows it. This runs on the video thread now, and a message box
    // from any thread but the window's is a wait on the window's thread - see Docs/Threading-
    // Plan.md's rules. Empty means nothing to say.
    // The Direct3D engines draw into `window`, OpenGL into `gl_window` - see App::CreateGlSurface.
    std::unique_ptr<IGraphicsEngine> CreateGraphicsEngine(GraphicsBackend preferred, HWND window,
                                                          HWND gl_window, int width, int height,
                                                          std::string* active_backend,
                                                          std::wstring* warning);

    // Compiles every ported filter into the engine at once - cheap (startup-cost shader compiles,
    // not per-frame work), so there is no reason to defer any of them until first selected. HLSL
    // for Direct3D 12, the GLSL ports for OpenGL (shaders/glsl_filters.h), under the same keys.
    // Only worth calling on an engine that supports filters at all; see RendererHasFilters.
    void LoadAllFilters(IGraphicsEngine& engine, GraphicsBackend backend);

    enum class AudioBackend { kWasapi, kDirectSound };

    // Tries `preferred` first and falls back to the other. Audio is optional: a machine with no
    // working output device should still run, silently, rather than refusing to start - so null here
    // is an ordinary outcome, not a failure the caller has to report.
    //
    // `*active_backend` is set to whichever engine actually ended up running ("wasapi" or "dsound"),
    // or cleared when neither did, which the caller uses for the menu tick and the saved setting
    // rather than the one it asked for - the same arrangement CreateGraphicsEngine has.
    std::unique_ptr<IAudioEngine> CreateAudioEngine(AudioBackend preferred,
                                                    std::string* active_backend);

}   // namespace psxemu
