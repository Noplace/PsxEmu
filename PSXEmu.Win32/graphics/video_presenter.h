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

// What the video thread draws with: a Direct3D or OpenGL engine, and the few things the menus can
// ask of it. (The class is still called D3DPresenter, from before OpenGL.)
//
// Everything here runs on that thread and nowhere else - the device or GL context, the swap chain
// and the filters all belong to it, which is what lets vsync block it without the machine or the
// window noticing (Docs/Threading-Plan.md). Anything it has to tell the user goes back through `to_ui`,
// which posts to the window's own thread; a message box raised from here would be a wait on the
// thread that is supposed to be free.

#include "framework.h"

#include "engine_factory.h"
#include "host/video_output.h"

#include <functional>

namespace psxemu {

    class D3DPresenter : public emulation::host::Presenter {
     public:
        // `to_ui` runs a piece of work on the UI thread - App::PostToUi.
        // `windows` says where each engine draws (App::CreateRenderSurfaces).
        D3DPresenter(const RenderWindows& windows,
                     std::function<void(std::function<void()>)> to_ui);
        ~D3DPresenter() override;

        // Brings up `renderer` ("d3d11", "d3d12", "opengl" or "vulkan") with `filter` on it, at the window's
        // current client size. False if no engine could be created at all, which is fatal to the
        // front end and is reported through `to_ui`.
        bool Open(const std::string& renderer, const std::string& filter);

        // host::Presenter, both on the video thread.
        void Present(const emulation::host::VideoFrame& frame) override;
        void Resize(int width, int height) override;

        // The menu's asks, run as requests on the video thread.
        void SetRenderer(const std::string& key);
        void SetFilter(const std::string& key);

        // What is actually running, which is not always what was asked for.
        const std::string& renderer() const { return renderer_; }
        const std::string& filter() const { return filter_; }

     private:
        // Creates an engine for `renderer`, loads the filters it supports, and tells the UI what
        // opened and anything it needs to know.
        bool Create(const std::string& renderer, const std::string& filter);

        HWND window_;
        RenderWindows windows_;
        std::function<void(std::function<void()>)> to_ui_;
        std::unique_ptr<IGraphicsEngine> engine_;
        std::string renderer_;
        std::string filter_;
        int width_ = 0;
        int height_ = 0;

        // Video > View VRAM: the machine ships the raw 16-bit VRAM and the conversion happens
        // here, where there is time for it.
        std::vector<uint32_t> vram_scratch_;
    };

}   // namespace psxemu
