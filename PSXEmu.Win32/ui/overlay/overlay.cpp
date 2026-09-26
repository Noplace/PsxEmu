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
#include "ui/overlay/overlay.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwctype>

namespace psxemu {

    namespace {

        using Clock = Overlay::Clock;

        // ---- the look ---------------------------------------------------------------------------
        const uint32_t kPanel = OverlayColor(18, 20, 24, 216);
        const uint32_t kPanelEdge = OverlayColor(255, 255, 255, 18);
        const uint32_t kTextMain = OverlayColor(240, 242, 246);
        const uint32_t kTextDim = OverlayColor(158, 166, 178);
        const uint32_t kInfo = OverlayColor(80, 160, 255);
        const uint32_t kSuccess = OverlayColor(56, 200, 124);
        const uint32_t kWarning = OverlayColor(255, 186, 38);
        const uint32_t kError = OverlayColor(255, 90, 90);
        const uint32_t kEmulate = OverlayColor(80, 160, 255);
        const uint32_t kHandoff = OverlayColor(255, 170, 64);
        const uint32_t kIdle = OverlayColor(74, 80, 92);
        const uint32_t kFpsLine = OverlayColor(110, 205, 255);
        const uint32_t kAudioLine = OverlayColor(120, 222, 150);
        const uint32_t kGuide = OverlayColor(255, 255, 255, 70);
        const uint32_t kGraphBack = OverlayColor(0, 0, 0, 70);

        // ---- timing -----------------------------------------------------------------------------
        const float kSlideIn = 0.22f;          // seconds
        const float kHoldInfo = 3.5f;
        const float kHoldWarning = 6.0f;
        const float kFadeOut = 0.7f;
        const float kControllersShown = 5.0f;
        const float kControllersFade = 0.8f;
        const size_t kMaxToasts = 5;

        float Seconds(Clock::duration d) { return std::chrono::duration<float>(d).count(); }
        float EaseOut(float t) {
            t = (std::min)((std::max)(t, 0.0f), 1.0f);
            const float u = 1.0f - t;
            return 1.0f - u * u * u;
        }
        float HoldFor(ToastKind kind) {
            return (kind == ToastKind::kWarning || kind == ToastKind::kError) ? kHoldWarning
                                                                              : kHoldInfo;
        }
        uint32_t Accent(ToastKind kind) {
            switch (kind) {
                case ToastKind::kSuccess: return kSuccess;
                case ToastKind::kWarning: return kWarning;
                case ToastKind::kError: return kError;
                default: return kInfo;
            }
        }
        std::wstring Format(const wchar_t* format, double a) {
            wchar_t text[64];
            swprintf(text, 64, format, a);
            return text;
        }
        std::wstring Widen(const std::string& text) {
            return std::wstring(text.begin(), text.end());
        }

    }   // namespace

    // ---------------------------------------------------------------------------------------------
    // What to show
    // ---------------------------------------------------------------------------------------------

    void Overlay::Notify(OverlayIcon icon, ToastKind kind, const std::wstring& title,
                         const std::wstring& detail) {
        if (!notifications_)
            return;
        // The same thing twice in a row - a pad dropping in and out - is one notification,
        // started again.
        if (!toasts_.empty() && toasts_.back().title == title && toasts_.back().detail == detail) {
            toasts_.back().born = Clock::now();
            dirty_ = true;
            return;
        }
        Toast toast;
        toast.icon = icon;
        toast.kind = kind;
        toast.title = title;
        toast.detail = detail;
        toast.born = Clock::now();
        toasts_.push_back(toast);
        while (toasts_.size() > kMaxToasts)
            toasts_.pop_front();
        dirty_ = true;
    }

    void Overlay::SetControllers(const std::vector<ControllerSlot>& slots, bool announce) {
        slots_ = slots;
        if (announce)
            controllers_until_ = Clock::now() + std::chrono::milliseconds(
                                                     static_cast<int>(kControllersShown * 1000));
        dirty_ = true;
    }

    void Overlay::SetNotificationsEnabled(bool on) {
        notifications_ = on;
        if (!on)
            toasts_.clear();
        dirty_ = true;
    }

    void Overlay::SetControllersAlwaysVisible(bool on) {
        controllers_always_ = on;
        dirty_ = true;
    }

    void Overlay::SetStatsMode(StatsMode mode) {
        stats_mode_ = mode;
        dirty_ = true;
    }

    void Overlay::SetPaused(bool paused) {
        if (paused != paused_)
            dirty_ = true;
        paused_ = paused;
    }

    void Overlay::SetCounters(uint64_t frames_dropped, uint64_t audio_short, uint64_t audio_dropped) {
        frames_dropped_ = frames_dropped;
        audio_short_ = audio_short;
        audio_dropped_ = audio_dropped;
    }

    void Overlay::SetRendererInfo(const std::string& renderer, const std::string& filter) {
        renderer_ = renderer;
        filter_ = filter;
        dirty_ = true;
    }

    void Overlay::AddSample(const emulation::host::FrameSample& sample) {
        if (sample.refresh_hz > 0.0f)
            last_refresh_hz_ = sample.refresh_hz;
        recent_.push_back(sample);
        while (recent_.size() > kRecentFrames)
            recent_.pop_front();
        minute_frame_ms_.push_back(sample.frame_ms);
        while (minute_frame_ms_.size() > 60 * 60)
            minute_frame_ms_.pop_front();

        bucket_ms_ += sample.frame_ms;
        ++bucket_frames_;
        bucket_audio_ += static_cast<float>(sample.audio_queued_frames) * 1000.0f / 44100.0f;
        if (bucket_ms_ >= 500.0f) {
            Bucket bucket;
            bucket.fps = static_cast<float>(bucket_frames_) * 1000.0f / bucket_ms_;
            bucket.audio_ms = bucket_audio_ / static_cast<float>(bucket_frames_);
            buckets_.push_back(bucket);
            while (buckets_.size() > kBuckets)
                buckets_.pop_front();
            bucket_ms_ = 0.0f;
            bucket_frames_ = 0;
            bucket_audio_ = 0.0f;
            // The 1% low: the frame rate the slowest one frame in a hundred ran at, over the
            // last minute. Worked out twice a second rather than every picture.
            if (!minute_frame_ms_.empty()) {
                std::vector<float> sorted(minute_frame_ms_.begin(), minute_frame_ms_.end());
                const size_t at = sorted.size() - 1 - sorted.size() / 100;
                std::nth_element(sorted.begin(), sorted.begin() + static_cast<ptrdiff_t>(at),
                                 sorted.end());
                one_percent_low_ = sorted[at] > 0.0f ? 1000.0f / sorted[at] : 0.0f;
            }
        }
        if (stats_mode_ != StatsMode::kOff)
            dirty_ = true;
    }

    void Overlay::NotePresent(double milliseconds) {
        presents_.push_back(static_cast<float>(milliseconds));
        while (presents_.size() > kRecentFrames)
            presents_.pop_front();
    }

    // ---------------------------------------------------------------------------------------------
    // Drawing
    // ---------------------------------------------------------------------------------------------

    bool Overlay::NeedsRedraw(Clock::time_point now) const {
        if (dirty_)
            return true;
        if (!toasts_.empty())
            return true;
        if (!controllers_always_ && now < controllers_until_ +
                                              std::chrono::milliseconds(
                                                  static_cast<int>(kControllersFade * 1000) + 50))
            return true;
        return false;
    }

    // Sized for the window: 1 at 720 lines, in steps of a quarter so a drag to resize does not
    // rebuild the fonts on every pixel.
    float Overlay::Scale(int height) const {
        float scale = static_cast<float>(height) / 720.0f;
        scale = std::round(scale * 4.0f) / 4.0f;
        return (std::min)((std::max)(scale, 0.75f), 2.5f);
    }

    const OverlayDrawData& Overlay::Build(int width, int height, int frame_width, int frame_height,
                                          Clock::time_point now) {
        dirty_ = false;
        vertices_.clear();
        indices_.clear();
        data_ = OverlayDrawData();

        // Toasts that have finished fading.
        while (!toasts_.empty()) {
            const Toast& oldest = toasts_.front();
            if (Seconds(now - oldest.born) < HoldFor(oldest.kind) + kFadeOut)
                break;
            toasts_.pop_front();
        }
        // Newer ones can outlive older ones (a warning holds longer), so any finished one goes.
        for (auto it = toasts_.begin(); it != toasts_.end();) {
            if (Seconds(now - it->born) >= HoldFor(it->kind) + kFadeOut)
                it = toasts_.erase(it);
            else
                ++it;
        }

        if (width <= 0 || height <= 0 || atlas_failed_)
            return data_;
        const float scale = Scale(height);
        if (atlas_.scale() != scale) {
            if (!atlas_.Build(scale)) {
                atlas_failed_ = true;
                return data_;
            }
        }
        s_ = scale;

        const float w = static_cast<float>(width);
        const float h = static_cast<float>(height);
        if (stats_mode_ == StatsMode::kFull)
            DrawStats(w, h, frame_width, frame_height);
        else if (stats_mode_ == StatsMode::kCompact)
            DrawCompactStats(16.0f * s_, 16.0f * s_);
        DrawControllers(w, now);
        DrawToasts(w, h, now);

        data_.vertices = vertices_.data();
        data_.vertex_count = vertices_.size();
        data_.indices = indices_.data();
        data_.index_count = indices_.size();
        data_.atlas = atlas_.pixels().data();
        data_.atlas_width = atlas_.width();
        data_.atlas_height = atlas_.height();
        data_.atlas_version = atlas_.version();
        return data_;
    }

    // ---- building blocks ------------------------------------------------------------------------

    void Overlay::Quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1,
                       float v1, uint32_t color) {
        if ((color >> 24) == 0)
            return;
        const uint32_t base = static_cast<uint32_t>(vertices_.size());
        vertices_.push_back({ x0, y0, u0, v0, color });
        vertices_.push_back({ x1, y0, u1, v0, color });
        vertices_.push_back({ x1, y1, u1, v1, color });
        vertices_.push_back({ x0, y1, u0, v1, color });
        const uint32_t quad[6] = { base, base + 1, base + 2, base, base + 2, base + 3 };
        indices_.insert(indices_.end(), quad, quad + 6);
    }

    void Overlay::Rect(float x, float y, float w, float h, uint32_t color) {
        const float u = atlas_.white_u(), v = atlas_.white_v();
        Quad(x, y, x + w, y + h, u, v, u, v, color);
    }

    // A fan from the centre round the four corners' arcs.
    void Overlay::RoundRect(float x, float y, float w, float h, float radius, uint32_t color) {
        if ((color >> 24) == 0)
            return;
        radius = (std::min)(radius, (std::min)(w, h) * 0.5f);
        if (radius < 1.0f) {
            Rect(x, y, w, h, color);
            return;
        }
        const float u = atlas_.white_u(), v = atlas_.white_v();
        const uint32_t centre = static_cast<uint32_t>(vertices_.size());
        vertices_.push_back({ x + w * 0.5f, y + h * 0.5f, u, v, color });
        const int kSteps = 6;
        const float corners[4][3] = {
            { x + w - radius, y + radius, -90.0f },       // top right, from straight up
            { x + w - radius, y + h - radius, 0.0f },     // bottom right
            { x + radius, y + h - radius, 90.0f },        // bottom left
            { x + radius, y + radius, 180.0f },           // top left
        };
        const uint32_t first = static_cast<uint32_t>(vertices_.size());
        for (const auto& corner : corners) {
            for (int i = 0; i <= kSteps; ++i) {
                const float angle = (corner[2] + 90.0f * i / kSteps) * 3.14159265f / 180.0f;
                vertices_.push_back({ corner[0] + std::cos(angle) * radius,
                                      corner[1] + std::sin(angle) * radius, u, v, color });
            }
        }
        const uint32_t count = static_cast<uint32_t>(vertices_.size()) - first;
        for (uint32_t i = 0; i < count; ++i) {
            indices_.push_back(centre);
            indices_.push_back(first + i);
            indices_.push_back(first + (i + 1) % count);
        }
    }

    void Overlay::Line(float x0, float y0, float x1, float y1, float thickness, uint32_t color) {
        const float dx = x1 - x0, dy = y1 - y0;
        const float length = std::sqrt(dx * dx + dy * dy);
        if (length <= 0.0f)
            return;
        const float nx = -dy / length * thickness * 0.5f;
        const float ny = dx / length * thickness * 0.5f;
        const float u = atlas_.white_u(), v = atlas_.white_v();
        const uint32_t base = static_cast<uint32_t>(vertices_.size());
        vertices_.push_back({ x0 + nx, y0 + ny, u, v, color });
        vertices_.push_back({ x1 + nx, y1 + ny, u, v, color });
        vertices_.push_back({ x1 - nx, y1 - ny, u, v, color });
        vertices_.push_back({ x0 - nx, y0 - ny, u, v, color });
        const uint32_t quad[6] = { base, base + 1, base + 2, base, base + 2, base + 3 };
        indices_.insert(indices_.end(), quad, quad + 6);
    }

    void Overlay::Icon(OverlayIcon icon, float x, float y, float size, uint32_t color) {
        const OverlayAtlas::Icon& i = atlas_.icon(icon);
        Quad(x, y, x + size, y + size, i.u0, i.v0, i.u1, i.v1, color);
    }

    float Overlay::Text(OverlayFont font, float x, float y, const std::wstring& text,
                        uint32_t color) {
        const OverlayAtlas::Font& f = atlas_.font(font);
        float pen = std::round(x);
        const float top = std::round(y);
        for (wchar_t c : text) {
            const OverlayAtlas::Glyph& glyph = f.Get(c);
            if (c != L' ' && glyph.width > 0.0f) {
                const float gx = pen + glyph.offset_x;
                Quad(gx, top, gx + glyph.width, top + glyph.height, glyph.u0, glyph.v0, glyph.u1,
                     glyph.v1, color);
            }
            pen += glyph.advance;
        }
        return pen - std::round(x);
    }

    std::wstring Overlay::Fit(OverlayFont font, const std::wstring& text, float width) const {
        if (atlas_.Measure(font, text) <= width)
            return text;
        const float dots = atlas_.Measure(font, L"...");
        std::wstring cut = text;
        while (!cut.empty() && atlas_.Measure(font, cut) + dots > width)
            cut.pop_back();
        while (!cut.empty() && cut.back() == L' ')
            cut.pop_back();
        return cut + L"...";
    }

    void Overlay::Graph(float x, float y, float w, float h, const std::vector<float>& values,
                        float low, float high, uint32_t color, float guide, uint32_t guide_color) {
        RoundRect(x, y, w, h, 4.0f * s_, kGraphBack);
        const float range = (high > low) ? high - low : 1.0f;
        auto to_y = [&](float value) {
            const float t = (std::min)((std::max)((value - low) / range, 0.0f), 1.0f);
            return y + h - t * h;
        };
        if (guide > low && guide < high) {
            const float gy = to_y(guide);
            const float dash = 6.0f * s_;
            for (float gx = x; gx < x + w; gx += dash * 2.0f)
                Rect(gx, gy, (std::min)(dash, x + w - gx), (std::max)(1.0f, s_), guide_color);
        }
        if (values.size() < 2)
            return;
        // The newest value at the right edge; a graph not yet full starts partway along.
        const size_t capacity = (std::max)(values.size(), kBuckets);
        const float step = w / static_cast<float>(capacity - 1);
        const float start = x + w - step * static_cast<float>(values.size() - 1);
        for (size_t i = 1; i < values.size(); ++i) {
            Line(start + step * (i - 1), to_y(values[i - 1]), start + step * i, to_y(values[i]),
                 1.6f * s_, color);
        }
    }

    // ---- the three corners ----------------------------------------------------------------------

    void Overlay::DrawToasts(float width, float height, Clock::time_point now) {
        (void)width;
        const float margin = 16.0f * s_;
        const float toast_w = 340.0f * s_;
        const float toast_h = 62.0f * s_;
        const float gap = 8.0f * s_;
        const float pad = 12.0f * s_;
        const float icon = 34.0f * s_;

        // Newest at the bottom, the rest stacked above, each easing to its place as the ones
        // below it leave.
        float slot = height - margin - toast_h;
        for (auto it = toasts_.rbegin(); it != toasts_.rend(); ++it) {
            Toast& toast = *it;
            if (toast.y < 0.0f)
                toast.y = slot;
            toast.y += (slot - toast.y) * 0.25f;
            if (std::fabs(toast.y - slot) < 0.5f)
                toast.y = slot;
            slot -= toast_h + gap;

            const float age = Seconds(now - toast.born);
            const float slide = EaseOut(age / kSlideIn);
            const float hold = HoldFor(toast.kind);
            float opacity = slide;
            if (age > hold)
                opacity = 1.0f - (age - hold) / kFadeOut;
            opacity = (std::min)((std::max)(opacity, 0.0f), 1.0f);
            if (opacity <= 0.0f)
                continue;

            const float x = margin - (1.0f - slide) * (toast_w * 0.35f);
            const float y = toast.y;
            const uint32_t accent = Accent(toast.kind);
            RoundRect(x, y, toast_w, toast_h, 10.0f * s_, OverlayFade(kPanel, opacity));
            RoundRect(x, y, 4.0f * s_, toast_h, 2.0f * s_, OverlayFade(accent, opacity));
            Icon(toast.icon, x + pad + 2.0f * s_, y + (toast_h - icon) * 0.5f, icon,
                 OverlayFade(accent, opacity));

            const float text_x = x + pad + icon + 14.0f * s_;
            const float text_w = toast_w - (text_x - x) - pad;
            const OverlayAtlas::Font& bold = atlas_.font(OverlayFont::kBold);
            const OverlayAtlas::Font& small = atlas_.font(OverlayFont::kSmall);
            if (toast.detail.empty()) {
                Text(OverlayFont::kBold, text_x, y + (toast_h - bold.line_height) * 0.5f,
                     Fit(OverlayFont::kBold, toast.title, text_w), OverlayFade(kTextMain, opacity));
            } else {
                const float block = bold.line_height + small.line_height;
                const float top = y + (toast_h - block) * 0.5f;
                Text(OverlayFont::kBold, text_x, top, Fit(OverlayFont::kBold, toast.title, text_w),
                     OverlayFade(kTextMain, opacity));
                Text(OverlayFont::kSmall, text_x, top + bold.line_height,
                     Fit(OverlayFont::kSmall, toast.detail, text_w), OverlayFade(kTextDim, opacity));
            }
        }
    }

    void Overlay::DrawControllers(float width, Clock::time_point now) {
        if (slots_.empty())
            return;
        float opacity = 1.0f;
        if (!controllers_always_) {
            const float left = Seconds(controllers_until_ - now);
            if (left <= -kControllersFade)
                return;
            if (left < 0.0f)
                opacity = 1.0f + left / kControllersFade;
        }

        const float margin = 16.0f * s_;
        const float card_w = 196.0f * s_;
        const float card_h = 58.0f * s_;
        const float tap_h = 30.0f * s_;   // the row of a multitap's four players
        const float gap = 8.0f * s_;
        const float pad = 10.0f * s_;
        const float icon = 38.0f * s_;
        const OverlayAtlas::Font& bold = atlas_.font(OverlayFont::kBold);
        const OverlayAtlas::Font& small = atlas_.font(OverlayFont::kSmall);

        // Port 2 at the far right, port 1 to its left - the order they sit on the console.
        float right = width - margin;
        for (int port = 1; port >= 0; --port) {
            const ControllerSlot* main = nullptr;
            std::vector<const ControllerSlot*> players;
            for (const ControllerSlot& slot : slots_) {
                if (slot.port != port)
                    continue;
                if (slot.player < 0)
                    main = &slot;
                else
                    players.push_back(&slot);
            }
            if (main == nullptr)
                continue;
            const bool tap = !players.empty();
            const float h = card_h + (tap ? tap_h : 0.0f);
            const float x = right - card_w;
            const float y = margin;
            right = x - gap;

            RoundRect(x, y, card_w, h, 10.0f * s_, OverlayFade(kPanel, opacity));
            const bool none = main->icon == OverlayIcon::kNone;
            const uint32_t tint = !main->connected ? kError : none ? kTextDim : kTextMain;
            Icon(main->icon, x + pad, y + (card_h - icon) * 0.5f, icon, OverlayFade(tint, opacity));

            // The port number, as a badge on the icon's corner.
            const std::wstring number = port == 0 ? L"1" : L"2";
            const float badge = 16.0f * s_;
            const float bx = x + pad + icon - badge * 0.6f;
            const float by = y + (card_h - icon) * 0.5f - badge * 0.25f;
            RoundRect(bx, by, badge, badge, badge * 0.5f, OverlayFade(kInfo, opacity));
            const float nw = atlas_.Measure(OverlayFont::kSmall, number);
            Text(OverlayFont::kSmall, bx + (badge - nw) * 0.5f,
                 by + (badge - small.line_height) * 0.5f, number, OverlayFade(kTextMain, opacity));

            const float text_x = x + pad + icon + 12.0f * s_;
            const float text_w = card_w - (text_x - x) - pad;
            const float block = bold.line_height + small.line_height;
            const float top = y + (card_h - block) * 0.5f;
            Text(OverlayFont::kBold, text_x, top, Fit(OverlayFont::kBold, main->type, text_w),
                 OverlayFade(none ? kTextDim : kTextMain, opacity));
            const std::wstring line = main->connected ? main->source : main->source + L" - not connected";
            Text(OverlayFont::kSmall, text_x, top + bold.line_height,
                 Fit(OverlayFont::kSmall, line, text_w),
                 OverlayFade(main->connected ? kTextDim : kError, opacity));

            if (tap) {
                const float mini = 20.0f * s_;
                const float cell = (card_w - 2.0f * pad) / 4.0f;
                for (size_t i = 0; i < players.size() && i < 4; ++i) {
                    const ControllerSlot& p = *players[i];
                    const float cx = x + pad + cell * static_cast<float>(i);
                    const float cy = y + card_h - 4.0f * s_;
                    const bool empty = p.icon == OverlayIcon::kNone;
                    const uint32_t c = !p.connected ? kError : empty ? kTextDim : kTextMain;
                    const std::wstring letter(1, static_cast<wchar_t>(L'A' + p.player));
                    Text(OverlayFont::kSmall, cx, cy + (mini - small.line_height) * 0.5f, letter,
                         OverlayFade(kTextDim, opacity));
                    Icon(p.icon, cx + 12.0f * s_, cy, mini, OverlayFade(c, opacity));
                }
            }
        }
    }

    void Overlay::DrawCompactStats(float x, float y) {
        const float pad = 8.0f * s_;
        const float graph_w = 110.0f * s_;
        const float graph_h = 22.0f * s_;
        const OverlayAtlas::Font& bold = atlas_.font(OverlayFont::kBold);
        const float fps = buckets_.empty() ? 0.0f : buckets_.back().fps;
        const float refresh = last_refresh_hz_ > 0.0f ? last_refresh_hz_ : 60.0f;
        std::wstring label = paused_ ? std::wstring(L"Paused")
                                     : Format(L"%.1f FPS", fps) +
                                           Format(L"  %.0f%%", fps / refresh * 100.0f);
        const float text_w = (std::max)(atlas_.Measure(OverlayFont::kBold, label), 96.0f * s_);
        const float w = pad + text_w + pad + graph_w + pad;
        const float h = (std::max)(bold.line_height, graph_h) + 2.0f * pad;
        RoundRect(x, y, w, h, 8.0f * s_, kPanel);
        Text(OverlayFont::kBold, x + pad, y + (h - bold.line_height) * 0.5f, label, kTextMain);
        std::vector<float> values;
        const size_t take = (std::min)(buckets_.size(), static_cast<size_t>(20));
        for (size_t i = buckets_.size() - take; i < buckets_.size(); ++i)
            values.push_back(buckets_[i].fps);
        float high = refresh * 1.25f;
        for (float v : values)
            high = (std::max)(high, v * 1.05f);
        // A short graph: its own capacity, so twenty points fill it.
        const float gx = x + pad + text_w + pad;
        const float gy = y + (h - graph_h) * 0.5f;
        RoundRect(gx, gy, graph_w, graph_h, 4.0f * s_, kGraphBack);
        if (values.size() >= 2) {
            const float step = graph_w / 19.0f;
            const float start = gx + graph_w - step * static_cast<float>(values.size() - 1);
            for (size_t i = 1; i < values.size(); ++i) {
                auto to_y = [&](float value) {
                    return gy + graph_h - (std::min)(value / high, 1.0f) * graph_h;
                };
                Line(start + step * (i - 1), to_y(values[i - 1]), start + step * i,
                     to_y(values[i]), 1.5f * s_, kFpsLine);
            }
        }
    }

    void Overlay::DrawStats(float width, float height, int frame_width, int frame_height) {
        (void)width;
        (void)height;
        const OverlayAtlas::Font& large = atlas_.font(OverlayFont::kLarge);
        const OverlayAtlas::Font& regular = atlas_.font(OverlayFont::kRegular);
        const OverlayAtlas::Font& small = atlas_.font(OverlayFont::kSmall);
        const float margin = 16.0f * s_;
        const float pad = 12.0f * s_;
        const float w = 392.0f * s_;
        const float inner = w - 2.0f * pad;
        const float graph_fps = 64.0f * s_;
        const float graph_frames = 64.0f * s_;
        const float graph_audio = 30.0f * s_;
        const float x = margin;
        const float y0 = margin;

        const float h = pad + large.line_height + 4.0f * s_ +                   // headline
                        small.line_height + 3.0f * s_ + graph_fps + 10.0f * s_ +   // fps
                        small.line_height + 3.0f * s_ + graph_frames + 6.0f * s_ +
                        small.line_height + 10.0f * s_ +                           // legend
                        small.line_height + 3.0f * s_ + graph_audio + 10.0f * s_ +   // audio
                        regular.line_height + small.line_height + pad;             // footer
        RoundRect(x, y0, w, h, 12.0f * s_, kPanel);
        RoundRect(x, y0, w, 2.0f * s_, 1.0f * s_, kPanelEdge);

        // Averages over the last second or so of frames.
        const size_t window = (std::min)(recent_.size(), static_cast<size_t>(60));
        double emulate = 0, handoff = 0, idle = 0, instructions = 0, frame_ms = 0;
        for (size_t i = recent_.size() - window; i < recent_.size(); ++i) {
            emulate += recent_[i].emulate_ms;
            handoff += recent_[i].handoff_ms;
            idle += recent_[i].idle_ms;
            instructions += recent_[i].instructions;
            frame_ms += recent_[i].frame_ms;
        }
        if (window > 0) {
            emulate /= window;
            handoff /= window;
            idle /= window;
            instructions /= window;
            frame_ms /= window;
        }
        double present = 0;
        for (float p : presents_)
            present += p;
        if (!presents_.empty())
            present /= static_cast<double>(presents_.size());
        const float refresh = last_refresh_hz_ > 0.0f ? last_refresh_hz_ : 60.0f;
        const float fps = frame_ms > 0.0 ? static_cast<float>(1000.0 / frame_ms) : 0.0f;

        // The headline: frames a second, and how that compares with the console's own rate.
        float y = y0 + pad;
        if (paused_) {
            Text(OverlayFont::kLarge, x + pad, y, L"Paused", kTextMain);
        } else {
            const float used = Text(OverlayFont::kLarge, x + pad, y, Format(L"%.1f", fps), kTextMain);
            Text(OverlayFont::kRegular, x + pad + used + 6.0f * s_,
                 y + large.ascent - regular.ascent, L"FPS", kTextDim);
            const std::wstring speed = Format(L"%.0f%%", fps / refresh * 100.0f);
            const std::wstring target = Format(L"of %.2f Hz", refresh);
            const float sw = atlas_.Measure(OverlayFont::kBold, speed);
            const float tw = atlas_.Measure(OverlayFont::kSmall, target);
            Text(OverlayFont::kBold, x + w - pad - sw, y + 2.0f * s_, speed, kTextMain);
            Text(OverlayFont::kSmall, x + w - pad - tw, y + 2.0f * s_ + regular.line_height, target,
                 kTextDim);
        }
        y += large.line_height + 4.0f * s_;

        // Frames a second over the last minute.
        {
            std::vector<float> values;
            float low = 1e9f, high = refresh * 1.2f, sum = 0.0f;
            for (const Bucket& b : buckets_) {
                values.push_back(b.fps);
                low = (std::min)(low, b.fps);
                high = (std::max)(high, b.fps * 1.05f);
                sum += b.fps;
            }
            Text(OverlayFont::kSmall, x + pad, y, L"FPS \x00B7 last minute", kTextDim);
            if (!values.empty()) {
                const std::wstring numbers =
                    Format(L"min %.1f", low) + Format(L"   avg %.1f", sum / values.size()) +
                    Format(L"   1%% low %.1f", one_percent_low_);
                const float nw = atlas_.Measure(OverlayFont::kSmall, numbers);
                Text(OverlayFont::kSmall, x + w - pad - nw, y, numbers, kTextDim);
            }
            y += small.line_height + 3.0f * s_;
            Graph(x + pad, y, inner, graph_fps, values, 0.0f, high, kFpsLine, refresh, kGuide);
            y += graph_fps + 10.0f * s_;
        }

        // Where each frame's time went, frame by frame: a bar per frame, stacked.
        {
            Text(OverlayFont::kSmall, x + pad, y, L"Frame time \x00B7 last 5 seconds", kTextDim);
            const std::wstring now_ms = Format(L"%.1f ms", frame_ms);
            const float nw = atlas_.Measure(OverlayFont::kSmall, now_ms);
            Text(OverlayFont::kSmall, x + w - pad - nw, y, now_ms, kTextDim);
            y += small.line_height + 3.0f * s_;
            const float budget = 1000.0f / refresh;
            float high = budget * 2.0f;
            for (const auto& f : recent_)
                high = (std::max)(high, (std::min)(f.frame_ms * 1.05f, 100.0f));
            RoundRect(x + pad, y, inner, graph_frames, 4.0f * s_, kGraphBack);
            const float bar = inner / static_cast<float>(kRecentFrames);
            const float bottom = y + graph_frames;
            float bx = x + pad + inner - bar * static_cast<float>(recent_.size());
            for (const auto& f : recent_) {
                float top = bottom;
                const float parts[3] = { f.emulate_ms, f.handoff_ms, f.idle_ms };
                const uint32_t colors[3] = { kEmulate, kHandoff, kIdle };
                for (int p = 0; p < 3; ++p) {
                    const float ph = (std::min)(parts[p] / high * graph_frames, top - y);
                    if (ph > 0.0f) {
                        Rect(bx, top - ph, (std::max)(bar, 1.0f), ph, colors[p]);
                        top -= ph;
                    }
                }
                bx += bar;
            }
            // The frame's budget: a bar that reaches this line took a whole frame's time.
            const float gy = bottom - budget / high * graph_frames;
            const float dash = 6.0f * s_;
            for (float gx = x + pad; gx < x + pad + inner; gx += dash * 2.0f)
                Rect(gx, gy, (std::min)(dash, x + pad + inner - gx), (std::max)(1.0f, s_), kGuide);
            y += graph_frames + 6.0f * s_;

            // The legend doubles as the averages.
            const float sq = 8.0f * s_;
            float lx = x + pad;
            const struct { uint32_t color; const wchar_t* name; double ms; } legend[] = {
                { kEmulate, L"Emulate", emulate },
                { kHandoff, L"Hand-off", handoff },
                { kIdle, L"Idle", idle },
            };
            for (const auto& item : legend) {
                Rect(lx, y + (small.line_height - sq) * 0.5f, sq, sq, item.color);
                lx += sq + 5.0f * s_;
                lx += Text(OverlayFont::kSmall, lx, y, std::wstring(item.name) + Format(L" %.1f", item.ms),
                           kTextDim);
                lx += 12.0f * s_;
            }
            Text(OverlayFont::kSmall, lx, y, Format(L"Present %.1f ms", present), kTextDim);
            y += small.line_height + 10.0f * s_;
        }

        // How much sound is waiting for the device - dips toward zero are crackles.
        {
            std::vector<float> values;
            float high = 80.0f;
            for (const Bucket& b : buckets_) {
                values.push_back(b.audio_ms);
                high = (std::max)(high, b.audio_ms * 1.1f);
            }
            Text(OverlayFont::kSmall, x + pad, y, L"Audio buffer \x00B7 last minute", kTextDim);
            const std::wstring counts =
                Format(L"%.0f ms", values.empty() ? 0.0 : values.back()) +
                Format(L"   short %.0f", static_cast<double>(audio_short_)) +
                Format(L"   dropped %.0f", static_cast<double>(audio_dropped_));
            const float cw = atlas_.Measure(OverlayFont::kSmall, counts);
            Text(OverlayFont::kSmall, x + w - pad - cw, y, counts, kTextDim);
            y += small.line_height + 3.0f * s_;
            Graph(x + pad, y, inner, graph_audio, values, 0.0f, high, kAudioLine, 40.0f, kGuide);
            y += graph_audio + 10.0f * s_;
        }

        // The machine, and what draws it.
        {
            const double mips = instructions * fps / 1e6;
            const std::wstring cpu = Format(L"CPU %.1f MIPS", mips) +
                                     Format(L"   frames not shown %.0f",
                                            static_cast<double>(frames_dropped_));
            Text(OverlayFont::kRegular, x + pad, y, cpu, kTextMain);
            y += regular.line_height;
            std::wstring engine = Widen(renderer_);
            for (wchar_t& c : engine)
                c = static_cast<wchar_t>(towupper(c));
            if (!filter_.empty())
                engine += L" \x00B7 " + Widen(filter_);
            if (frame_width > 0 && frame_height > 0)
                engine += Format(L" \x00B7 %.0f", frame_width) + Format(L"\x00D7%.0f", frame_height);
            Text(OverlayFont::kSmall, x + pad, y, Fit(OverlayFont::kSmall, engine, inner), kTextDim);
        }
    }

}   // namespace psxemu
