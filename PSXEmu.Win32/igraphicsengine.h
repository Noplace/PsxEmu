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

#include <windows.h>
#include <stdint.h>
#include <string>

// What a presenter has to be able to do, so the front end can hold one of
// these instead of a concrete D3D11 or D3D12 type and switch between them at
// run time. Adapted from GBAEmu's own interface of the same name - this is
// the ImGui-free half of it: PSXEmu.Win32 uses native Win32 menus, not an
// immediate-mode overlay, so there is no GUI backend lifecycle here.
class IGraphicsEngine {
 public:
    virtual ~IGraphicsEngine() = default;

    // Lifecycle. `width`/`height` are the window's client area at the moment
    // of construction - the caller resolves that once, rather than each
    // implementation calling GetClientRect on its own.
    virtual bool Initialize(HWND window_handle, int width, int height) = 0;
    virtual void Shutdown() = 0;

    // One frame: clear and bind the target, upload and draw the emulator's
    // framebuffer, present. Three calls rather than one `Present(pixels, w,
    // h)` because a filter change or a live renderer switch needs to happen
    // between frames, not inside one.
    virtual void BeginFrame() = 0;
    virtual void RenderFramebuffer(const void* data, int width, int height) = 0;
    virtual void EndFrame() = 0;

    // The window was resized; follow the back buffer to the new client area.
    virtual void Resize(int width, int height) = 0;

    virtual void SetVsync(bool enabled) = 0;

    // Video filters. `name` is a stable key ("xbrz", "scanline", ...) chosen
    // by whoever is loading shaders, not a file name - an empty name always
    // means the engine's own built-in pass-through. An engine that does not
    // support filters (the D3D11 path, in this project) may simply no-op
    // `SetPixelShader` and return false from both loaders; the caller is
    // responsible for not offering filters as an option when that is so.
    virtual void SetPixelShader(const std::string& name) = 0;
    virtual bool LoadCustomPixelShader(const std::string& name, const uint8_t* bytecode,
                                       size_t size) = 0;
    virtual bool LoadPixelShaderFromString(const std::string& name, const char* hlsl) = 0;
};
