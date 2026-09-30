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
#include <vector>

#include "graphics/dlss/dlss_choice.h"
#include "psx/shared_picture.h"
#include "ui/overlay/overlay_draw.h"

// The widest picture a multi-pass filter chain is run on. The console's own are at most 640 wide;
// anything wider has come from the hardware rasteriser already upscaled, and is shown as it is -
// a chain's passes multiply its size, and 4x of 2560 is past what a texture can be.
inline constexpr int kFilterChainMaxWidth = 1024;

// One stage of a multi-pass filter chain (see IGraphicsEngine::LoadShaderChain).
struct ShaderPass {
    // Key of a pixel shader already given to LoadCustomPixelShader or LoadPixelShaderFromString.
    std::string shader;
    // Size of this pass's render target, as a whole multiple of the emulator's framebuffer.
    // 0 means "draw straight into the letterboxed picture area of the window"; only the last
    // pass of a chain may use it. Any other last pass is followed by a linear-filtered blit.
    int scale;
};

// What a presenter has to be able to do, so the front end can hold one of
// these instead of a concrete D3D11 or D3D12 type and switch between them at
// run time. Adapted from GBAEmu's own interface of the same name - this is
// the ImGui-free half of it: PSXEmu.Win32 uses native Win32 menus, and its
// on-screen overlay (ui/overlay) is triangles each engine draws with one small
// pass of its own (SetOverlay), so there is no GUI backend lifecycle here.
class IGraphicsEngine {
 public:
    virtual ~IGraphicsEngine() = default;

    // The graphics card to draw on, by LUID (graphics/adapters.h) and by name - 0 and empty leave
    // it to the engine, which is what it does with no preference. Called before Initialize. The
    // Direct3D engines make their device on that card and Vulkan picks it; OpenGL cannot: on
    // Windows the driver and Windows' own per-app graphics setting decide which card a GL
    // context lands on, and nothing an application asks changes that.
    virtual void SetPreferredAdapter(uint64_t luid, const std::string& name) {
        (void)luid;
        (void)name;
    }

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

    // The hardware rasteriser's picture, left on the graphics card (psx/shared_picture.h), drawn
    // in RenderFramebuffer's place - filters, overlay and all - once its source says it is ready.
    // False if this engine cannot open it, and nothing was drawn. An engine that can hands the
    // picture back to its source (Release) once its own card has finished with it.
    virtual bool RenderSharedPicture(const emulation::psx::SharedPicture& picture) {
        (void)picture;
        return false;
    }
    // The graphics adapter, by LUID, whose shared pictures this engine can draw; 0 if it takes
    // none - and then the hardware rasteriser reads its pictures back for it.
    virtual uint64_t SharedPictureAdapter() const { return 0; }

    // NVIDIA DLSS (Docs/DLSS-Plan.md): made from the shared pictures and the plane beside them.
    // Given before Initialize - an engine may need to start something for it first - and again
    // whenever it changes. Only the Direct3D 12 engine has it; the rest say so.
    virtual void SetDlss(const psxemu::DlssChoice& choice) { (void)choice; }
    virtual psxemu::DlssStatus dlss_status() const {
        psxemu::DlssStatus status;
        status.why = "only the Direct3D 12 renderer has it";
        return status;
    }
    // Whether this engine has to be made again for DLSS to go from `from` to `to`: on or off,
    // for one that starts NVIDIA's Streamline with its device.
    virtual bool DlssNeedsRemaking(const psxemu::DlssChoice& from,
                                   const psxemu::DlssChoice& to) const {
        (void)from;
        (void)to;
        return false;
    }

    // The window was resized; follow the back buffer to the new client area.
    virtual void Resize(int width, int height) = 0;

    // What to draw over the picture this frame (ui/overlay): set before RenderFramebuffer, used
    // by the end of the frame, and pointing at nothing an engine may keep. Null or empty draws
    // nothing. Every engine draws it the same way - over the whole window, alpha-blended, after
    // the picture and any filter.
    virtual void SetOverlay(const psxemu::OverlayDrawData* overlay) = 0;

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

    // A filter made of several shaders run one after another, each drawing into its own texture
    // that the next one reads (t0), with the untouched emulator frame always available as well
    // (t1). Selected with SetPixelShader(name) like any single shader. Every pass shader must be
    // loaded first. Not pure: an engine without chain support just says no.
    virtual bool LoadShaderChain(const std::string& name, const std::vector<ShaderPass>& passes) {
        (void)name;
        (void)passes;
        return false;
    }
};
