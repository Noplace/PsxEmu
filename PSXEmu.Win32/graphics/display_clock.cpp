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
#include "graphics/display_clock.h"

#include <dxgi.h>
#include <wrl/client.h>

#include <vector>

#pragma comment(lib, "dxgi.lib")

namespace psxemu {

    using Microsoft::WRL::ComPtr;

    namespace {

        // The DXGI output showing `monitor`, on whichever adapter drives it.
        ComPtr<IDXGIOutput> OutputFor(HMONITOR monitor) {
            ComPtr<IDXGIFactory1> factory;
            if (monitor == nullptr || FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
                return nullptr;
            ComPtr<IDXGIAdapter1> adapter;
            for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
                ComPtr<IDXGIOutput> output;
                for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o) {
                    DXGI_OUTPUT_DESC desc = {};
                    if (SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor == monitor)
                        return output;
                }
            }
            return nullptr;
        }

        // A wake further than this from where the anchor says a vblank was is not taken in: the
        // thread was held up, or a vblank went by unwatched.
        constexpr double kOutlierFraction = 0.25;
        // How much of each wake's offset the anchor takes: about a third of a second's worth of
        // vblanks to settle, which smooths a wake's lateness without trailing the display.
        constexpr double kAnchorGain = 0.05;
        // Wakes out of place in a row before the anchor is given up and started again - the
        // display's timing really did move (a mode change the message has not come for yet).
        constexpr int kOutliersToRestart = 30;

    }   // namespace

    double DisplayClock::RefreshRateOf(HMONITOR monitor) {
        MONITORINFOEXW info = {};
        info.cbSize = sizeof(info);
        if (monitor == nullptr || !GetMonitorInfoW(monitor, &info))
            return 0.0;

        // The display configuration's own rational rate, for the path whose source is this
        // monitor's: the target mode's vsync frequency if there is one, else the path's.
        UINT32 path_count = 0;
        UINT32 mode_count = 0;
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count) ==
            ERROR_SUCCESS) {
            std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
            std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
            if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(), &mode_count,
                                   modes.data(), nullptr) == ERROR_SUCCESS) {
                for (UINT32 p = 0; p < path_count; ++p) {
                    const DISPLAYCONFIG_PATH_INFO& path = paths[p];
                    DISPLAYCONFIG_SOURCE_DEVICE_NAME source = {};
                    source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
                    source.header.size = sizeof(source);
                    source.header.adapterId = path.sourceInfo.adapterId;
                    source.header.id = path.sourceInfo.id;
                    if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS ||
                        wcscmp(source.viewGdiDeviceName, info.szDevice) != 0)
                        continue;
                    DISPLAYCONFIG_RATIONAL rate = path.targetInfo.refreshRate;
                    const UINT32 index = path.targetInfo.modeInfoIdx;
                    if (index != DISPLAYCONFIG_PATH_MODE_IDX_INVALID && index < mode_count &&
                        modes[index].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_TARGET &&
                        modes[index].targetMode.targetVideoSignalInfo.vSyncFreq.Denominator != 0)
                        rate = modes[index].targetMode.targetVideoSignalInfo.vSyncFreq;
                    if (rate.Denominator != 0 && rate.Numerator != 0)
                        return static_cast<double>(rate.Numerator) / rate.Denominator;
                }
            }
        }

        // The whole-number rate the old API knows, as a last resort.
        DEVMODEW mode = {};
        mode.dmSize = sizeof(mode);
        if (EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode) &&
            mode.dmDisplayFrequency > 1)
            return static_cast<double>(mode.dmDisplayFrequency);
        return 0.0;
    }

    void DisplayClock::SetRunning(bool on) {
        if (on == thread_.joinable())
            return;
        if (!on) {
            Stop();
            return;
        }
        {
            std::lock_guard<std::mutex> hold(lock_);
            ++monitor_serial_;   // read the rate afresh
            has_anchor_ = false;
        }
        stop_.store(false);
        thread_ = std::thread(&DisplayClock::Run, this);
        running_.store(true);
    }

    void DisplayClock::Stop() {
        if (!thread_.joinable())
            return;
        // A wait for a vblank ends at the next one, a refresh or two away at most - unless the
        // display is off, which the wait's own failure covers (Run).
        running_.store(false);
        stop_.store(true);
        thread_.join();
        std::lock_guard<std::mutex> hold(lock_);
        has_anchor_ = false;
    }

    void DisplayClock::SetMonitor(HMONITOR monitor, bool changed) {
        std::lock_guard<std::mutex> hold(lock_);
        if (monitor == monitor_ && !changed)
            return;
        monitor_ = monitor;
        ++monitor_serial_;
        has_anchor_ = false;
    }

    bool DisplayClock::Sample(utilities::DisplayTiming* timing) const {
        std::lock_guard<std::mutex> hold(lock_);
        if (!running_.load() || refresh_hz_ <= 0.0)
            return false;
        timing->refresh_hz = refresh_hz_;
        timing->has_vblank = has_anchor_;
        timing->last_vblank = anchor_;
        return true;
    }

    double DisplayClock::refresh_hz() const {
        std::lock_guard<std::mutex> hold(lock_);
        return refresh_hz_;
    }

    void DisplayClock::NoteVblank(Clock::time_point now) {
        std::lock_guard<std::mutex> hold(lock_);
        if (refresh_hz_ <= 0.0)
            return;
        const long long period = std::chrono::duration_cast<Clock::duration>(
                                     std::chrono::duration<double>(1.0 / refresh_hz_))
                                     .count();
        if (!has_anchor_) {
            anchor_ = now;
            has_anchor_ = true;
            outliers_ = 0;
            return;
        }
        // How far this wake is from the nearest vblank the anchor predicts.
        long long offset = (now - anchor_).count() % period;
        if (offset < 0)
            offset += period;
        if (offset >= period / 2)
            offset -= period;
        if (offset > period * kOutlierFraction || offset < -period * kOutlierFraction) {
            if (++outliers_ >= kOutliersToRestart)
                has_anchor_ = false;
            return;
        }
        outliers_ = 0;
        // Brought forward to this wake's vblank, so the anchor stays recent, and nudged by a
        // little of the offset.
        const long long whole = ((now - anchor_).count() - offset) / period;
        anchor_ += Clock::duration(whole * period +
                                   static_cast<long long>(offset * kAnchorGain));
    }

    void DisplayClock::Run() {
        ComPtr<IDXGIOutput> output;
        Clock::time_point last_wake;
        uint64_t serial = 0;
        bool first = true;
        while (!stop_.load()) {
            HMONITOR monitor = nullptr;
            bool moved = false;
            {
                std::lock_guard<std::mutex> hold(lock_);
                if (first || serial != monitor_serial_) {
                    serial = monitor_serial_;
                    monitor = monitor_;
                    moved = true;
                }
            }
            first = false;
            if (moved) {
                // Off the lock: both of these ask the driver, and Sample must not wait on that.
                output = OutputFor(monitor);
                const double hz = RefreshRateOf(monitor);
                std::lock_guard<std::mutex> hold(lock_);
                if (serial == monitor_serial_) {
                    refresh_hz_ = hz;
                    has_anchor_ = false;
                }
            }
            if (output == nullptr) {
                // No output to watch - the window not placed yet, or on a display DXGI does not
                // list. The rate alone still serves, without the phase.
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                continue;
            }
            if (FAILED(output->WaitForVBlank())) {
                // The display off, or the output gone with a mode change: looked for again.
                output.Reset();
                {
                    std::lock_guard<std::mutex> hold(lock_);
                    ++monitor_serial_;
                    has_anchor_ = false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                continue;
            }
            const Clock::time_point now = Clock::now();
            // A driver that answers at once rather than at the vblank (some do while the panel
            // refreshes itself) would have this spin: a millisecond's rest between such wakes.
            if (now - last_wake < std::chrono::milliseconds(2))
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            last_wake = now;
            NoteVblank(now);
        }
    }

}   // namespace psxemu
