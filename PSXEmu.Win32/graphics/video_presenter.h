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

#include "app/framework.h"

#include "app/engine_factory.h"
#include "host/video_output.h"
#include "ui/overlay/frame_stats_ring.h"
#include "ui/overlay/overlay.h"

#include <chrono>
#include <functional>

namespace psxemu {

    class D3DPresenter : public emulation::host::Presenter {
     public:
        // `to_ui` runs a piece of work on the UI thread - App::PostToUi.
        // `windows` says where each engine draws (App::CreateRenderSurfaces).
        // `stats` is where the machine leaves its per-frame timings for the overlay's graphs;
        // it outlives this.
        D3DPresenter(const RenderWindows& windows,
                     std::function<void(std::function<void()>)> to_ui, FrameStatsRing* stats);
        ~D3DPresenter() override;

        // Brings up `renderer` ("d3d11", "d3d12", "opengl" or "vulkan") with `filter` on it, at the window's
        // current client size. False if no engine could be created at all, which is fatal to the
        // front end and is reported through `to_ui`.
        bool Open(const std::string& renderer, const std::string& filter);

        // host::Presenter, all on the video thread.
        void Present(const emulation::host::VideoFrame& frame) override;
        void Resize(int width, int height) override;
        bool WantsRefresh() override;
        void Refresh(const emulation::host::VideoFrame* last) override;

        // What is drawn over the picture. The video thread's, like everything here: the window
        // reaches it through requests posted to that thread.
        Overlay& overlay() { return overlay_; }

        // The menu's asks, run as requests on the video thread.
        void SetRenderer(const std::string& key);
        void SetFilter(const std::string& key);
        // The graphics card to draw on (graphics/adapters.h): before Open, and from Settings >
        // Video > Graphics Card. 0 and empty leave it to the engine.
        void SetGraphicsCard(uint64_t luid, const std::string& name);
        // The card actually asked of the engines - 0 once it turned out no engine would start on
        // the one chosen, and Windows' pick was used instead.
        uint64_t card_luid() const { return card_luid_; }
        // Settings > Video, NVIDIA DLSS: before Open, and after. The engine is made again when
        // DLSS goes on or off, if it has to be (IGraphicsEngine::DlssNeedsRemaking).
        void SetDlss(const DlssChoice& choice);
        // Whether DLSS runs in the engine now, and if not why not.
        DlssStatus dlss_status() const {
            return engine_ != nullptr ? engine_->dlss_status() : DlssStatus();
        }
        // Told whenever that changes while pictures are drawn - Frame Generation stopping, say -
        // on this thread; not for a change the caller made itself (SetRenderer and the like).
        void set_dlss_listener(std::function<void(const DlssStatus&)> listener) {
            dlss_listener_ = std::move(listener);
        }
        // DLSS Frame Generation only at full speed (IGraphicsEngine::SetFrameGenerationAllowed).
        void SetFrameGenerationAllowed(bool allowed);

        // What is actually running, which is not always what was asked for.
        const std::string& renderer() const { return renderer_; }
        const std::string& filter() const { return filter_; }
        // The graphics adapter (LUID) whose shared pictures the running engine can draw, or 0 if
        // it takes none (psx/shared_picture.h) - for the hardware rasteriser to draw there.
        uint64_t shared_adapter() const {
            return engine_ != nullptr ? engine_->SharedPictureAdapter() : 0;
        }

     private:
        // Creates an engine for `renderer`, loads the filters it supports, and tells the UI what
        // opened and anything it needs to know.
        bool Create(const std::string& renderer, const std::string& filter);
        // Closes the engine and makes one for `renderer` on the current card, keeping the filter.
        void Rebuild(const std::string& renderer);

        uint64_t card_luid_ = 0;
        std::string card_name_;
        DlssChoice dlss_;
        bool generation_allowed_ = true;
        std::function<void(const DlssStatus&)> dlss_listener_;
        DlssStatus dlss_reported_;
        // Under Frame Generation: the last picture presented, and when - repeats of it are not
        // presented, and while pictures come the overlay waits for them rather than presenting
        // in between.
        uint32_t last_picture_ = 0;
        std::chrono::steady_clock::time_point last_picture_time_;
        void ReportDlss();

        HWND window_;
        RenderWindows windows_;
        std::function<void(std::function<void()>)> to_ui_;
        std::unique_ptr<IGraphicsEngine> engine_;
        std::string renderer_;
        std::string filter_;
        int width_ = 0;
        int height_ = 0;

        // View > VRAM: the machine ships the raw 16-bit VRAM and the conversion happens
        // here, where there is time for it.
        std::vector<uint32_t> vram_scratch_;

        // Draws `pixels` with the overlay over it.
        void Draw(const uint32_t* pixels, int width, int height);
        // Draws a picture left on the card, with the overlay over it.
        void DrawShared(const emulation::psx::SharedPicture& picture);
        // Present, but a repeat under Frame Generation too when `again` - the overlay over a
        // paused picture.
        void PresentFrame(const emulation::host::VideoFrame& frame, bool again);
        // How long a shared picture is waited for before it is given up on: a frame's drawing
        // is milliseconds, so this is only reached when the rasteriser's card has gone.
        static constexpr uint32_t kSharedPictureWaitMs = 250;
        Overlay overlay_;
        FrameStatsRing* stats_ = nullptr;
        // What is shown before the first frame, so the overlay has something to go over.
        std::vector<uint32_t> blank_;
    };

}   // namespace psxemu
