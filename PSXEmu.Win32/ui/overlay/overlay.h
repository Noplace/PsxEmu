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

// What the front end draws over the picture: notifications that slide in at the lower left
// and fade, the controllers plugged into each port at the top right, and a performance panel
// with graphs over time at the top left (F3).
//
// It builds triangles (overlay_draw.h) that every graphics engine draws the same way, so it
// looks the same on all four. It lives on the video thread, in the presenter; the window and
// the machine reach it only through requests posted to that thread, which is why nothing here
// takes a lock.

#include "host/machine.h"
#include "ui/overlay/overlay_atlas.h"
#include "ui/overlay/overlay_draw.h"

#include <chrono>
#include <deque>
#include <string>
#include <vector>

namespace psxemu {

    enum class ToastKind { kInfo, kSuccess, kWarning, kError };

    // How much of the performance panel is shown - F3 steps through them.
    enum class StatsMode { kOff, kCompact, kFull };

    // One controller socket as the top-right corner shows it: a port, or a player behind a
    // multitap on it.
    struct ControllerSlot {
        int port = 0;          // 0 or 1
        int player = -1;       // -1 for the port itself; 0-3 for a multitap's A-D
        OverlayIcon icon = OverlayIcon::kNone;
        std::wstring type;     // "DualShock", "Mouse", ...
        std::wstring source;   // "Keyboard", "Gamepad 2", ...
        bool connected = true; // whether what drives it is there - a pad can be unplugged
    };

    class Overlay {
     public:
        typedef std::chrono::steady_clock Clock;

        // ---- what to show ------------------------------------------------------------------
        void Notify(OverlayIcon icon, ToastKind kind, const std::wstring& title,
                    const std::wstring& detail = std::wstring());
        // The ports as they are now. `announce` shows the corner for a few seconds even when
        // it is not set to stay up - a pad plugged in, a port's type changed, a game started.
        void SetControllers(const std::vector<ControllerSlot>& slots, bool announce);

        // ---- settings (the View menu) ---------------------------------------------------------
        void SetNotificationsEnabled(bool on);
        void SetControllersAlwaysVisible(bool on);
        void SetStatsMode(StatsMode mode);
        StatsMode stats_mode() const { return stats_mode_; }

        // ---- what the performance panel reads -------------------------------------------------
        void AddSample(const emulation::host::FrameSample& sample);
        void NotePresent(double milliseconds);   // this thread's own time, per picture shown
        void SetPaused(bool paused);
        void SetCounters(uint64_t frames_dropped, uint64_t audio_short, uint64_t audio_dropped);
        void SetRendererInfo(const std::string& renderer, const std::string& filter);

        // ---- drawing ------------------------------------------------------------------------
        // Whether anything is moving, or something changed since the last Build - the video
        // thread draws again for it even with no new frame (host::Presenter::WantsRefresh).
        bool NeedsRedraw(Clock::time_point now) const;
        // The overlay for a `width` x `height` window over a `frame_width` x `frame_height`
        // picture. Valid until the next call.
        const OverlayDrawData& Build(int width, int height, int frame_width, int frame_height,
                                     Clock::time_point now);

     private:
        struct Toast {
            OverlayIcon icon;
            ToastKind kind;
            std::wstring title;
            std::wstring detail;
            Clock::time_point born;
            float y = -1.0f;   // where it is drawn, easing towards its place in the stack
        };

        // A half-second of frames, for the minute-long graphs.
        struct Bucket {
            float fps = 0.0f;
            float audio_ms = 0.0f;
        };

        // ---- building blocks ----------------------------------------------------------------
        void Rect(float x, float y, float w, float h, uint32_t color);
        void RoundRect(float x, float y, float w, float h, float radius, uint32_t color);
        void Line(float x0, float y0, float x1, float y1, float thickness, uint32_t color);
        void Icon(OverlayIcon icon, float x, float y, float size, uint32_t color);
        // Returns the width drawn. Snaps to whole pixels so glyphs stay crisp.
        float Text(OverlayFont font, float x, float y, const std::wstring& text, uint32_t color);
        // `text` cut to fit `width`, with "..." where it was cut.
        std::wstring Fit(OverlayFont font, const std::wstring& text, float width) const;
        void Quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1,
                  uint32_t color);

        void DrawToasts(float width, float height, Clock::time_point now);
        void DrawControllers(float width, Clock::time_point now);
        void DrawStats(float width, float height, int frame_width, int frame_height);
        void DrawCompactStats(float x, float y);
        // A line graph of `values` over a box, `low`..`high` bottom to top, with an optional
        // dashed guide at `guide`.
        void Graph(float x, float y, float w, float h, const std::vector<float>& values,
                   float low, float high, uint32_t color, float guide, uint32_t guide_color);

        float Scale(int height) const;

        OverlayAtlas atlas_;
        bool atlas_failed_ = false;
        float s_ = 1.0f;   // the atlas's scale: everything is sized by it

        std::vector<OverlayVertex> vertices_;
        std::vector<uint32_t> indices_;
        OverlayDrawData data_;

        bool notifications_ = true;
        std::deque<Toast> toasts_;

        std::vector<ControllerSlot> slots_;
        bool controllers_always_ = false;
        Clock::time_point controllers_until_{};

        StatsMode stats_mode_ = StatsMode::kOff;
        bool paused_ = false;
        std::string renderer_, filter_;
        uint64_t frames_dropped_ = 0, audio_short_ = 0, audio_dropped_ = 0;

        // The last few seconds, frame by frame.
        static const size_t kRecentFrames = 300;
        std::deque<emulation::host::FrameSample> recent_;
        std::deque<float> presents_;
        // The last minute, half a second at a time, and every frame of it for the 1% low.
        static const size_t kBuckets = 120;
        std::deque<Bucket> buckets_;
        std::deque<float> minute_frame_ms_;
        float bucket_ms_ = 0.0f;
        int bucket_frames_ = 0;
        float bucket_audio_ = 0.0f;
        float one_percent_low_ = 0.0f;
        float last_refresh_hz_ = 0.0f;

        bool dirty_ = true;
    };

}   // namespace psxemu
