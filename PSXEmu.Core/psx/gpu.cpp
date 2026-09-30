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
#include "psx/psx.h"

#include <algorithm>
#include <cmath>

namespace emulation {
    namespace psx {

        namespace {

            // The GPU runs at 53.222400 MHz against the CPU's 33.868800 MHz, so a GPU
            // dot clock is 11/7 of a CPU cycle. Tick() is handed CPU cycles and scales
            // them. The ratio and the scanline width live in the header now - the
            // display timing accessors the root counters use are inline and need them.
            using emulation::psx::Gpu;
            const uint32_t kGpuClockNumerator = Gpu::kGpuClockNumerator;
            const uint32_t kGpuClockDenominator = Gpu::kGpuClockDenominator;
            const uint32_t kDotsPerScanline = Gpu::kDotsPerScanline;
            const uint32_t kScanlinesNtsc = 263;
            const uint32_t kScanlinesPal = 314;

            inline int32_t SignExtend11(uint32_t value) {
                return static_cast<int32_t>(value << 21) >> 21;
            }

            inline uint16_t To15Bit(uint8_t r, uint8_t g, uint8_t b) {
                return static_cast<uint16_t>(((b >> 3) << 10) | ((g >> 3) << 5) | (r >> 3));
            }

            // 5-bit VRAM components are widened by replicating the top bits, so 0x1F maps
            // to 0xFF rather than 0xF8 and white stays white.
            inline uint8_t From5Bit(uint32_t c) {
                return static_cast<uint8_t>((c << 3) | (c >> 2));
            }

            // Motion's keys (psx/vertex_motion.h): `value` folded into `key`.
            inline uint64_t MixKey(uint64_t key, uint64_t value) {
                key ^= value + 0x9E3779B97F4A7C15ull + (key << 6) + (key >> 2);
                key ^= key >> 31;
                return key * 0xBF58476D1CE4E5B9ull;
            }

            inline uint32_t PackColour(const RasterVertex& v) {
                return v.r | (static_cast<uint32_t>(v.g) << 8) | (static_cast<uint32_t>(v.b) << 16);
            }

        }  // namespace

        Gpu::Gpu() : vram_(nullptr), framebuffer_(nullptr) {}

        // Stops the rasteriser if Deinitialize has not already. A std::thread that is still
        // joinable when it is destroyed calls std::terminate, so a Gpu torn down without
        // Deinitialize - three of the harnesses do exactly that - took the whole process
        // down with it once the thread became the default (bug 95). What is still queued is
        // drawn first; vram_ is only freed by Deinitialize, so it is still there to draw into.
        Gpu::~Gpu() { StopRasterThread(); }

        int Gpu::Initialize() {
            vram_ = new uint16_t[kVramWidth * kVramHeight];
            framebuffer_ = new uint32_t[kVramWidth * kVramHeight];
            memset(vram_, 0, sizeof(uint16_t) * kVramWidth * kVramHeight);
            memset(framebuffer_, 0, sizeof(uint32_t) * kVramWidth * kVramHeight);

            status_.raw = 0x14802000;
            pending_draw_ticks_ = 0;
            queue_head_ = queue_size_ = 0;
            prepaid_ticks_ = 0;
            // IOInterface hands this over again at its first batch, from the setting.
            exact_hblank_ = false;
            // A fresh rasteriser for this VRAM, before the thread that drives it starts.
            backend_.reset();
            ChooseRasteriser();
            jobs_head_ = jobs_count_ = 0;
            threaded_ = system().config().gpu_thread;
            if (threaded_)
                StartRasterThread();
            fifo_count_ = 0;
            fifo_needed_ = 0;
            transfer_mode_ = kTransferNone;
            memset(&transfer_, 0, sizeof(transfer_));
            read_latch_ = 0;
            current_command_ = 0;
            watch_x_ = watch_y_ = watch_w_ = watch_h_ = 0;
            PushWatch();

            draw_area_left_ = 0;
            draw_area_top_ = 0;
            draw_area_right_ = 0;
            draw_area_bottom_ = 0;
            draw_offset_x_ = 0;
            draw_offset_y_ = 0;
            texture_window_mask_x_ = 0;
            texture_window_mask_y_ = 0;
            texture_window_offset_x_ = 0;
            texture_window_offset_y_ = 0;
            force_set_mask_ = false;
            check_mask_ = false;
            rect_flip_x_ = false;
            rect_flip_y_ = false;

            display_vram_x_ = 0;
            display_vram_y_ = 0;
            horizontal_display_start_ = 0x200;
            horizontal_display_end_ = 0xC00;
            vertical_display_start_ = 0x10;
            vertical_display_end_ = 0x100;

            dot_accumulator_ = 0;
            dot_clock_remainder_ = 0;
            dot_clock_accum_ = 0;
            pending_dot_clocks_ = 0;
            pending_hblanks_ = 0;
            scanline_ = 0;
            was_in_vblank_ = false;
            frame_count_ = 0;
            memset(&stats_, 0, sizeof(stats_));

            UpdateDisplaySize();
            return S_OK;
        }

        // The hardware rasteriser if it is asked for and the front end can make one, and the
        // software one otherwise - including when making the hardware one fails, which
        // raster_error() then says why.
        //
        // Mid-game it is the path a save state takes: whatever the old rasteriser drew is
        // brought into native VRAM, and the new one starts from there. Nothing the game can
        // see changes - every cost was charged when its command was parsed.
        void Gpu::ChooseRasteriser() {
            if (vram_ == nullptr)
                return;   // not initialised; Initialize will choose
            if (backend_) {
                SyncRaster();
                backend_->PrepareRead(0, 0, kVramWidth, kVramHeight);
            }
            std::unique_ptr<RasterBackend> next;
            bool hardware = false;
            raster_error_.clear();
            if (system().config().gpu_rasteriser == "hardware") {
                const RasterFactory& factory = system().hardware_raster();
                RasterOptions options;
                options.scale = system().config().resolution_scale;
                options.true_color = system().config().true_color;
                if (!factory)
                    raster_error_ = "no hardware rasteriser in this build";
                else if ((next = factory(vram_, options, &raster_error_)) != nullptr)
                    hardware = true;
            }
            if (!next)
                next = std::make_unique<SoftwareRaster>(vram_);
            {
                // Nothing is queued, so the rasteriser's thread is not touching it.
                std::lock_guard<std::mutex> lock(jobs_mutex_);
                backend_ = std::move(next);
                hardware_raster_ = hardware;
            }
            picture_scale_ = 1;   // until the new one resolves a frame
            shared_picture_ = SharedPicture();
            PushWatch();
            backend_->SetPlanes(planes_keep_, plane_view_);
            backend_->set_motion_check(motion_check_);
            backend_->SetJitter(jitter_phases_);
            jitter_drawn_ = jitter_phases_;
            UpdateMotion();
            motion_fresh_ = true;   // the new rasteriser's plane knows nothing of the old
        }

        void Gpu::SetPlanes(bool keep, PlaneView view) {
            planes_keep_ = keep;
            plane_view_ = view;
            if (!backend_)
                return;   // not initialised; ChooseRasteriser will hand them over
            SyncRaster();
            backend_->SetPlanes(keep, view);
            UpdateMotion();
        }

        void Gpu::SetMotionCheck(bool on) {
            motion_check_ = on;
            if (!backend_)
                return;
            SyncRaster();
            backend_->set_motion_check(on);
        }

        void Gpu::SetJitter(int phases) {
            jitter_phases_ = phases;
            if (!backend_)
                return;
            SyncRaster();
            backend_->SetJitter(phases);
            jitter_drawn_ = phases;
        }

        void Gpu::UpdateMotion() {
            const bool on = hardware_raster_ && (planes_keep_ || plane_view_ != PlaneView::kPicture);
            if (on != motion_) {
                sprite_motion_.Forget();
                motion_fresh_ = true;
            }
            motion_ = on;
            system().gte().set_motion(on);
        }

        void Gpu::NextPicture(bool* is_new, bool* reset) {
            const bool flipped = display_vram_x_ != shown_x_ || display_vram_y_ != shown_y_;
            shown_x_ = display_vram_x_;
            shown_y_ = display_vram_y_;
            vblanks_since_flip_ = flipped ? 0 : vblanks_since_flip_ + 1;
            if (!flipped && vblanks_since_flip_ <= kSingleBuffered)
                return;
            *is_new = true;
            // A cut: few of the vertices projected since the last new picture were found in the
            // one before. That work is shown now by a game drawing into the buffer on screen, and
            // a flip from now by one drawing into the other - so a cut resets two pictures.
            VertexMotion& vertices = system().gte().vertex_motion();
            const uint64_t looked = vertices.looked(), found = vertices.found();
            vertices.ClearCounts();
            stats_.motion_vertices += looked;
            stats_.motion_vertices_found += found;
            const bool cut = looked >= kCutVertices && found * 4 < looked;
            const bool changed = display_width_ != shown_width_ ||
                                 display_height_ != shown_height_ ||
                                 (status_.display_depth != 0) != shown_depth_;
            // 480 lines interlaced: each picture is half new lines and half the last field's,
            // which no motion describes - Air Combat's title screen, 640x480. Every one starts
            // afresh; DLSS leaves them as they are (Docs/DLSS-Plan.md).
            const bool interlaced = status_.vres && status_.vertical_interlace;
            *reset = cut || cut_before_ || changed || interlaced || motion_fresh_;
            // ...and since they are shown as they are, they are not jittered either, which would
            // show as a shake. Back on from the first picture that is not.
            const int jitter = interlaced ? 0 : jitter_phases_;
            if (jitter != jitter_drawn_) {
                backend_->SetJitter(jitter);
                jitter_drawn_ = jitter;
            }
            cut_before_ = cut;
            motion_fresh_ = false;
            shown_width_ = display_width_;
            shown_height_ = display_height_;
            shown_depth_ = status_.display_depth != 0;
            ++stats_.pictures;
            if (*reset)
                ++stats_.picture_resets;
            last_picture_motion_ = { looked, found, *reset };
            system().gte().NewPicture();
            sprite_motion_.NewPicture();
            backend_->NewPicture(*reset);
        }

        bool Gpu::FindSprite(uint64_t key, float x, float y, float* dx, float* dy) {
            ++stats_.motion_sprites;
            if (!sprite_motion_.FindAndRemember(key, x - static_cast<float>(draw_offset_x_),
                                                y - static_cast<float>(draw_offset_y_), dx, dy))
                return false;
            ++stats_.motion_sprites_found;
            return true;
        }

        // A rasteriser that can no longer draw - the graphics card reset, or went - is replaced
        // by the software one, with nothing queued. What it had drawn since native VRAM was last
        // brought up to date is gone with the card; the game draws it again, usually by the next
        // frame. Better that than a picture that stops while the game goes on.
        void Gpu::FallBackToSoftware(const char* reason) {
            std::string why = reason;
            {
                std::lock_guard<std::mutex> lock(jobs_mutex_);
                backend_ = std::make_unique<SoftwareRaster>(vram_);
                hardware_raster_ = false;
            }
            picture_scale_ = 1;
            shared_picture_ = SharedPicture();
            raster_error_ = std::move(why);
            PushWatch();
            UpdateMotion();
        }

        void Gpu::Serialise(StateIO& io) {
            SyncRaster();
            // Native VRAM is what a state holds, whichever rasteriser drew it.
            if (io.saving())
                backend_->PrepareRead(0, 0, kVramWidth, kVramHeight);
            io.Bytes(vram_, sizeof(uint16_t) * kVramWidth * kVramHeight);
            io.Plain(status_.raw);
            io.Plain(fifo_);
            io.Plain(fifo_count_);
            io.Plain(fifo_needed_);
            io.Plain(transfer_mode_);
            io.Plain(transfer_);
            io.Plain(read_latch_);
            io.Plain(current_command_);
            io.Plain(watch_x_);
            io.Plain(watch_y_);
            io.Plain(watch_w_);
            io.Plain(watch_h_);
            io.Plain(draw_area_left_);
            io.Plain(draw_area_top_);
            io.Plain(draw_area_right_);
            io.Plain(draw_area_bottom_);
            io.Plain(draw_offset_x_);
            io.Plain(draw_offset_y_);
            io.Plain(texture_window_mask_x_);
            io.Plain(texture_window_mask_y_);
            io.Plain(texture_window_offset_x_);
            io.Plain(texture_window_offset_y_);
            io.Plain(force_set_mask_);
            io.Plain(check_mask_);
            io.Plain(rect_flip_x_);
            io.Plain(rect_flip_y_);
            io.Plain(display_vram_x_);
            io.Plain(display_vram_y_);
            io.Plain(horizontal_display_start_);
            io.Plain(horizontal_display_end_);
            io.Plain(vertical_display_start_);
            io.Plain(vertical_display_end_);
            io.Plain(display_width_);
            io.Plain(display_height_);
            io.Plain(dot_accumulator_);
            io.Plain(dot_clock_remainder_);
            io.Plain(dot_clock_accum_);
            io.Plain(pending_dot_clocks_);
            io.Plain(pending_hblanks_);
            io.Plain(scanline_);
            io.Plain(was_in_vblank_);
            io.Plain(frame_count_);
            // pending_draw_ticks_ is deliberately not saved. It is at most a few
            // thousand GPU clocks of work in flight, it is gone a scanline later, and
            // saving it would change the state format - which would refuse every save
            // state anyone already has, for a number that is worth nothing once the
            // machine has run for a frame. A loaded state resumes with an idle GPU.

            // framebuffer_ is derived, not saved - rebuild it now so a caller that
            // reads it right after a load (boot_runner --ppm, the front end's next
            // Present) sees the picture the restored VRAM actually holds, not
            // whatever the buffer held before the load.
            if (!io.saving()) {
                // PGXP's shadows are not in a state: the words loaded come without.
                for (PreciseVertex& precise : fifo_precise_)
                    precise.valid = false;
                for (PreciseVertex& precise : queue_precise_)
                    precise.valid = false;
                // Nor is motion: nothing drawn before the load is anything drawn after it.
                sprite_motion_.Forget();
                motion_fresh_ = true;
                backend_->Reloaded();
                PushWatch();
                ResolveFramebuffer();
            }
        }

        int Gpu::Deinitialize() {
            StopRasterThread();
            backend_.reset();   // before the VRAM it draws into
            delete[] vram_;
            delete[] framebuffer_;
            vram_ = nullptr;
            framebuffer_ = nullptr;
            return S_OK;
        }

        // ---------------------------------------------------------------------------
        // Register interface
        // ---------------------------------------------------------------------------

        uint32_t Gpu::ReadStatus() {
            GpuStatus s = status_;
            // Bits 26 and 28 - ready for a command word, ready for a DMA block - are
            // the GP0 queue's occupancy against the 16 words hardware holds. They drop
            // when the rasteriser falls behind, which is what makes drawing time
            // visible to software and what DMA channel 2 waits on. The DMA request
            // line below follows them.
            //
            // Bit 27 stays set. Reporting it only while a
            // VRAM-to-CPU transfer is in flight looks more honest, but it is not what
            // the bit means: it says the GPU is ready to hand VRAM over, not that a
            // transfer is already running. Software that checks readiness *before*
            // issuing the read command waits for a bit that this GPU would only set
            // afterwards, and spins for ever - which is exactly where the BIOS shell
            // was stopping.
            s.ready_cmd = ready_for_dma() ? 1 : 0;
            s.ready_dma = ready_for_dma() ? 1 : 0;
            s.ready_vram_send = 1;

            switch (s.dma_direction) {
            case 0:  s.dma_request = 0; break;              // off
            case 1:  s.dma_request = 1; break;              // FIFO status
            case 2:  s.dma_request = s.ready_dma; break;    // CPU -> GP0
            default: s.dma_request = s.ready_vram_send; break;  // GPUREAD -> CPU
            }
            return s.raw;
        }

        uint32_t Gpu::ReadData() {
            // Reading the port means software wants what the GPU has produced, and on
            // hardware it would have waited for that itself. Anything still queued is
            // run first - drawing time is given away rather than answering with a
            // stale latch, which is the one way the queue could turn into a wrong
            // picture instead of a slower one. All of it, up to the read's own command:
            // an ordinary drain stops after the first primitive, since that makes the
            // GPU busy again, and every word read before the read command had run came
            // back as the latch - the BIOS menu's spheres, drawn and read straight back,
            // arrived in RAM shifted by the words it had gone through (bug 130).
            if (queue_size_ > 0 && transfer_mode_ != kTransferFromVram) {
                const int32_t owed = pending_draw_ticks_;
                DrainQueue(Drain::ForReadback);
                pending_draw_ticks_ = owed;
            }
            if (transfer_mode_ != kTransferFromVram)
                return read_latch_;

            // Reading VRAM: everything handed to the rasteriser has to be in it first.
            SyncRaster();
            backend_->PrepareRead(transfer_.x, transfer_.y, transfer_.w, transfer_.h);

            // Two 16-bit pixels per 32-bit read, left to right, top to bottom.
            uint32_t result = 0;
            for (int half = 0; half < 2; ++half) {
                uint16_t pixel = VramAt(transfer_.x + transfer_.px, transfer_.y + transfer_.py);
                result |= static_cast<uint32_t>(pixel) << (half * 16);
                if (++transfer_.px >= transfer_.w) {
                    transfer_.px = 0;
                    if (++transfer_.py >= transfer_.h) {
                        transfer_mode_ = kTransferNone;
                        break;
                    }
                }
            }
            read_latch_ = result;
            return result;
        }

        // A word arriving at GP0. It goes into the queue and is acted on from there,
        // which is the whole point: a busy rasteriser leaves it waiting, exactly as a
        // real FIFO would, and GPUSTAT says so (bug 86).
        void Gpu::WriteData(uint32_t data) {
            ++stats_.gp0_words;
            PushQueue(data);
            DrainQueue();
        }

        void Gpu::WriteData(uint32_t data, const PreciseVertex* precise) {
            ++stats_.gp0_words;
            PushQueue(data, precise);
            DrainQueue();
        }

        void Gpu::PushQueue(uint32_t word, const PreciseVertex* precise) {
            if (queue_size_ >= kQueueCapacity) {
                // Nothing here can stall a CPU write, so a game that ignores the ready
                // bits must not lose words. Catch up by force - drawing time is given
                // away rather than data - and only then give up.
                const int32_t owed = pending_draw_ticks_;
                DrainQueue(Drain::Everything);
                pending_draw_ticks_ = (queue_size_ >= kQueueCapacity) ? 0 : owed;
                if (queue_size_ >= kQueueCapacity) {
                    ++stats_.queue_overflows;
                    return;
                }
            }
            const int slot = (queue_head_ + queue_size_) % kQueueCapacity;
            queue_[slot] = word;
            // PGXP's shadow travels beside its word, and only while it is the word's own.
            if (precise != nullptr && precise->Matches(word))
                queue_precise_[slot] = *precise;
            else
                queue_precise_[slot].valid = false;
            ++queue_size_;
            if (queue_size_ > stats_.queue_peak)
                stats_.queue_peak = queue_size_;
        }

        uint32_t Gpu::PopQueue(PreciseVertex* precise) {
            const uint32_t word = queue_[queue_head_];
            if (precise != nullptr)
                *precise = queue_precise_[queue_head_];
            queue_head_ = (queue_head_ + 1) % kQueueCapacity;
            --queue_size_;
            return word;
        }

        // Words leave the queue while the GPU has nothing else to do. A VRAM transfer's
        // data keeps flowing whatever the rasteriser is doing: the blitter is a separate
        // piece of the chip, and holding its words back behind a draw would deadlock a
        // game that uploads a texture between two primitives.
        void Gpu::DrainQueue(Drain how) {
            while (queue_size_ > 0) {
                if (transfer_mode_ == kTransferToVram) {
                    StepTransfer(PopQueue());
                    continue;
                }
                // A read is served from the point its command has run: what is queued
                // behind it is for after the read.
                if (how == Drain::ForReadback && transfer_mode_ == kTransferFromVram)
                    break;
                if (how == Drain::WhenIdle && drawing())
                    break;
                PreciseVertex precise;
                const uint32_t word = PopQueue(&precise);
                FeedCommand(word, precise);
            }
        }

        // The command assembler: collects a command's words and runs it once the last
        // one has arrived. This is what WriteData was before the queue went in front
        // of it, unchanged apart from taking its word from the queue.
        void Gpu::FeedCommand(uint32_t data, const PreciseVertex& precise) {
            if (fifo_count_ == 0) {
                fifo_needed_ = CommandLength(data >> 24);
                // A polyline runs until its terminator rather than for a fixed length.
                if (fifo_needed_ < 0) {
                    { fifo_precise_[fifo_count_] = precise; fifo_[fifo_count_++] = data; }
                    return;
                }
            }
            else if (fifo_needed_ < 0) {
                // Collecting a polyline. 0x55555555 (with the low bits masked) ends it -
                // the terminator carries no vertex data of its own and must not be
                // appended to the fifo. It used to fall through into the shared tail
                // below, which pushed it in as one more word and let CmdLine decode it
                // as a bogus final vertex - X=0x5000/Y=0x5000 masked to 11 bits is
                // (0,0), which is why a polyline's last real point grew a spurious
                // segment back to the screen origin.
                if ((data & 0xF000F000) == 0x50005000) {
                    ExecuteCommand();
                    fifo_count_ = 0;
                    fifo_needed_ = 0;
                    return;
                }
                if (fifo_count_ < static_cast<int>(sizeof(fifo_) / sizeof(fifo_[0])))
                    { fifo_precise_[fifo_count_] = precise; fifo_[fifo_count_++] = data; }
                return;
            }

            if (fifo_count_ < static_cast<int>(sizeof(fifo_) / sizeof(fifo_[0])))
                { fifo_precise_[fifo_count_] = precise; fifo_[fifo_count_++] = data; }

            if (fifo_count_ >= fifo_needed_) {
                ExecuteCommand();
                fifo_count_ = 0;
                fifo_needed_ = 0;
            }
        }

        void Gpu::WriteStatus(uint32_t data) {
            ++stats_.gp1_words;
            ExecuteGp1(data);
        }

        // Number of 32-bit words each GP0 command consumes, including the command word
        // itself. A negative result means "variable, terminated by 0x55555555".
        int Gpu::CommandLength(uint32_t command) {
            if (command == 0x02) return 3;                 // fill rectangle
            if (command < 0x20)  return 1;                 // nop / clear cache / irq

            if (command < 0x40) {                          // polygons
                const bool gouraud = (command & 0x10) != 0;
                const bool quad = (command & 0x08) != 0;
                const bool textured = (command & 0x04) != 0;
                const int verts = quad ? 4 : 3;
                return 1 + verts * (1 + (textured ? 1 : 0) + (gouraud ? 1 : 0)) -
                    (gouraud ? 1 : 0);
            }

            if (command < 0x60) {                          // lines
                const bool gouraud = (command & 0x10) != 0;
                const bool polyline = (command & 0x08) != 0;
                if (polyline) return -1;
                return gouraud ? 4 : 3;
            }

            if (command < 0x80) {                          // rectangles / sprites
                const uint32_t size = (command >> 3) & 3;
                const bool textured = (command & 0x04) != 0;
                return 2 + (textured ? 1 : 0) + (size == 0 ? 1 : 0);
            }

            if (command < 0xA0) return 4;                  // VRAM -> VRAM
            if (command < 0xC0) return 3;                  // CPU  -> VRAM
            if (command < 0xE0) return 3;                  // VRAM -> CPU
            return 1;                                      // E1..E6 rendering attributes
        }

        void Gpu::ExecuteCommand() {
            const uint32_t command = fifo_[0] >> 24;
            current_command_ = command;
            ++stats_.gp0_commands[command & 0xFF];
            if (command >= 0x20 && command < 0x80)
                ++stats_.primitives;

            if (command == 0x02) { CmdFillRectangle(); return; }
            if (command == 0x1F) {  // interrupt request
                // GPUSTAT.24 is a level, not a pulse: raising IRQ1 again while it is
                // already set is not a new edge for I_STAT to latch. GP1(02h) is the
                // only thing that clears it.
                if (!status_.irq) {
                    status_.irq = 1;
                    system().io().SetInterrupt(kInterruptGPU);
                }
                return;
            }
            if (command < 0x20) { return; }               // nop / clear cache
            if (command < 0x40) { CmdPolygon(); return; }
            if (command < 0x60) { CmdLine(); return; }
            if (command < 0x80) { CmdRectangle(); return; }
            if (command < 0xA0) { CmdVramToVramCopy(); return; }
            if (command < 0xC0) { CmdCpuToVram(); return; }
            if (command < 0xE0) { CmdVramToCpu(); return; }

            const uint32_t data = fifo_[0];
            switch (command) {
            case 0xE1:  // draw mode setting
                // GPUSTAT holds bits 0-10 as written and the texture-disable bit at 15.
                status_.raw = (status_.raw & ~0x87FF) | (data & 0x7FF) |
                    ((data & 0x800) << 4);
                // Bits 12-13 have nowhere to live in GPUSTAT, so they are kept here.
                rect_flip_x_ = (data & 0x1000) != 0;
                rect_flip_y_ = (data & 0x2000) != 0;
                break;
            case 0xE2:  // texture window
                texture_window_mask_x_ = (data >> 0) & 0x1F;
                texture_window_mask_y_ = (data >> 5) & 0x1F;
                texture_window_offset_x_ = (data >> 10) & 0x1F;
                texture_window_offset_y_ = (data >> 15) & 0x1F;
                break;
            case 0xE3:  // drawing area top-left
                draw_area_left_ = data & 0x3FF;
                draw_area_top_ = (data >> 10) & 0x1FF;
                break;
            case 0xE4:  // drawing area bottom-right
                draw_area_right_ = data & 0x3FF;
                draw_area_bottom_ = (data >> 10) & 0x1FF;
                break;
            case 0xE5:  // drawing offset
                draw_offset_x_ = SignExtend11(data & 0x7FF);
                draw_offset_y_ = SignExtend11((data >> 11) & 0x7FF);
                break;
            case 0xE6:  // mask bit setting
                force_set_mask_ = (data & 1) != 0;
                check_mask_ = (data & 2) != 0;
                status_.set_mask = force_set_mask_ ? 1 : 0;
                status_.check_mask = check_mask_ ? 1 : 0;
                break;
            default:
                break;
            }
        }

        void Gpu::ExecuteGp1(uint32_t data) {
            const uint32_t command = (data >> 24) & 0x3F;
            ++stats_.gp1_commands[command];
            const uint32_t params = data & 0xFFFFFF;

            switch (command) {
            case 0x00:  // reset GPU
                status_.raw = 0x14802000;
                fifo_count_ = 0;
                fifo_needed_ = 0;
                // A reset abandons whatever was being drawn, so nothing is owed.
                pending_draw_ticks_ = 0;
                queue_head_ = queue_size_ = 0;
                AbandonTransfer();
                draw_area_left_ = draw_area_top_ = 0;
                draw_area_right_ = draw_area_bottom_ = 0;
                draw_offset_x_ = draw_offset_y_ = 0;
                texture_window_mask_x_ = texture_window_mask_y_ = 0;
                texture_window_offset_x_ = texture_window_offset_y_ = 0;
                display_vram_x_ = display_vram_y_ = 0;
                horizontal_display_start_ = 0x200;
                horizontal_display_end_ = 0xC00;
                vertical_display_start_ = 0x10;
                vertical_display_end_ = 0x100;
                UpdateDisplaySize();
                break;

            case 0x01:  // reset command buffer
                fifo_count_ = 0;
                fifo_needed_ = 0;
                AbandonTransfer();
                break;

            case 0x02:  // acknowledge interrupt
                status_.irq = 0;
                break;

            case 0x03:  // display enable
                status_.display_disable = params & 1;
                break;

            case 0x04:  // DMA direction
                status_.dma_direction = params & 3;
                break;

            case 0x05:  // start of display area in VRAM
                display_vram_x_ = params & 0x3FE;
                display_vram_y_ = (params >> 10) & 0x1FF;
                break;

            case 0x06:  // horizontal display range
                horizontal_display_start_ = params & 0xFFF;
                horizontal_display_end_ = (params >> 12) & 0xFFF;
                UpdateDisplaySize();
                break;

            case 0x07:  // vertical display range
                vertical_display_start_ = params & 0x3FF;
                vertical_display_end_ = (params >> 10) & 0x3FF;
                UpdateDisplaySize();
                break;

            case 0x08:  // display mode
                status_.hres1 = params & 3;
                status_.vres = (params >> 2) & 1;
                status_.video_mode = (params >> 3) & 1;
                status_.display_depth = (params >> 4) & 1;
                status_.vertical_interlace = (params >> 5) & 1;
                status_.hres2 = (params >> 6) & 1;
                status_.reverse = (params >> 7) & 1;
                UpdateDisplaySize();
                break;

            case 0x10:  // get GPU info
                switch (params & 0xF) {
                case 2: read_latch_ = (texture_window_mask_y_ << 15) |
                    (texture_window_mask_x_ << 10) |
                    (texture_window_offset_y_ << 5) |
                    texture_window_offset_x_; break;
                case 3: read_latch_ = (draw_area_top_ << 10) | draw_area_left_; break;
                case 4: read_latch_ = (draw_area_bottom_ << 10) | draw_area_right_; break;
                case 5: read_latch_ = ((draw_offset_y_ & 0x7FF) << 11) |
                    (draw_offset_x_ & 0x7FF); break;
                case 7: read_latch_ = 2; break;  // GPU version
                default: break;
                }
                break;

            default:
                break;
            }
        }

        // ---------------------------------------------------------------------------
        // CPU <-> VRAM transfers
        // ---------------------------------------------------------------------------

        void Gpu::StepTransfer(uint32_t data) {
            for (int half = 0; half < 2; ++half) {
                const uint16_t pixel = static_cast<uint16_t>(data >> (half * 16));
                VramAt(transfer_.x + transfer_.px, transfer_.y + transfer_.py) = pixel;
                if (transfer_timing_)
                    ChargeTransfer(1);
                backend_->NoteWatchWrite(transfer_.x + transfer_.px, transfer_.y + transfer_.py);
                if (stats_.transfer_log_count > 0 &&
                    stats_.transfer_log_count <= Stats::kTransferCapacity)
                    ++stats_.transfers[stats_.transfer_log_count - 1].written;
                if (++transfer_.px >= transfer_.w) {
                    transfer_.px = 0;
                    if (++transfer_.py >= transfer_.h) {
                        transfer_mode_ = kTransferNone;
                        // The whole rectangle is in native VRAM now; a rasteriser drawing
                        // somewhere else takes it from there.
                        backend_->Written(transfer_.x, transfer_.y, transfer_.w, transfer_.h);
                        return;
                    }
                }
            }
        }

        // A reset ends a transfer where it is. An upload cut short has still written the pixels
        // it got to, so the rasteriser hears about its rectangle as if it had finished.
        void Gpu::AbandonTransfer() {
            if (transfer_mode_ == kTransferToVram)
                backend_->Written(transfer_.x, transfer_.y, transfer_.w, transfer_.h);
            transfer_mode_ = kTransferNone;
        }

        void Gpu::CmdCpuToVram() {
            // The words that follow are written straight into VRAM from this thread,
            // so whatever is still queued has to land first or the upload would be
            // drawn over by a primitive that came before it.
            SyncRaster();
            // Close off the previous entry before starting a new one, so a transfer that
            // never finished is visible as a short pixel count.

            transfer_.x = fifo_[1] & 0x3FF;
            transfer_.y = (fifo_[1] >> 16) & 0x1FF;
            // A width or height field of zero means the maximum, not nothing.
            transfer_.w = ((fifo_[2] & 0xFFFF) - 1 & 0x3FF) + 1;
            transfer_.h = (((fifo_[2] >> 16) & 0xFFFF) - 1 & 0x1FF) + 1;
            transfer_.px = 0;
            transfer_.py = 0;
            transfer_mode_ = kTransferToVram;
            transfer_timing_ = system().config().gpu_transfer_timing;
            // Native VRAM is about to be written a pixel at a time, and must be the newer copy
            // of the rectangle while it is: a rasteriser drawing somewhere else would otherwise
            // hand back what it drew there over the upload - say when a state is saved halfway
            // through. Nothing for the software rasteriser.
            backend_->PrepareRead(transfer_.x, transfer_.y, transfer_.w, transfer_.h);

            if (stats_.transfer_log_count < Stats::kTransferCapacity) {
                Stats::Transfer& entry = stats_.transfers[stats_.transfer_log_count++];
                entry.x = static_cast<uint16_t>(transfer_.x);
                entry.y = static_cast<uint16_t>(transfer_.y);
                entry.w = static_cast<uint16_t>(transfer_.w);
                entry.h = static_cast<uint16_t>(transfer_.h);
                entry.written = 0;
            }
        }

        void Gpu::CmdVramToCpu() {
            SyncRaster();   // ReadData brings the backend's copy up to date as it reads
            transfer_.x = fifo_[1] & 0x3FF;
            transfer_.y = (fifo_[1] >> 16) & 0x1FF;
            transfer_.w = ((fifo_[2] & 0xFFFF) - 1 & 0x3FF) + 1;
            transfer_.h = (((fifo_[2] >> 16) & 0xFFFF) - 1 & 0x1FF) + 1;
            transfer_.px = 0;
            transfer_.py = 0;
            transfer_mode_ = kTransferFromVram;
            // Charged up front: the pixels are read out as software asks for them, but
            // the GPU is fetching them from VRAM for the whole transfer.
            if (system().config().gpu_transfer_timing)
                ChargeTransfer(static_cast<int32_t>(transfer_.w * transfer_.h));
        }

        void Gpu::CmdVramToVramCopy() {
            const uint32_t sx = fifo_[1] & 0x3FF;
            const uint32_t sy = (fifo_[1] >> 16) & 0x1FF;
            const uint32_t dx = fifo_[2] & 0x3FF;
            const uint32_t dy = (fifo_[2] >> 16) & 0x1FF;
            const uint32_t w = ((fifo_[3] & 0xFFFF) - 1 & 0x3FF) + 1;
            const uint32_t h = (((fifo_[3] >> 16) & 0xFFFF) - 1 & 0x1FF) + 1;

            // Each pixel is read and then written, hence twice the area.
            AddDrawTicks(static_cast<int32_t>(w * h * 2));

            DrawJob job;
            job.kind = DrawJob::kVramCopy;
            job.env = CaptureDrawEnv();
            job.src_x = static_cast<int32_t>(sx);
            job.src_y = static_cast<int32_t>(sy);
            job.x = static_cast<int32_t>(dx);
            job.y = static_cast<int32_t>(dy);
            job.w = static_cast<int32_t>(w);
            job.h = static_cast<int32_t>(h);
            SubmitJob(job);
        }

        void Gpu::CmdFillRectangle() {
            const uint8_t r = static_cast<uint8_t>(fifo_[0]);
            const uint8_t g = static_cast<uint8_t>(fifo_[0] >> 8);
            const uint8_t b = static_cast<uint8_t>(fifo_[0] >> 16);
            const uint16_t colour = To15Bit(r, g, b);

            // Fill is aligned to 16-pixel columns and ignores the drawing area and the
            // mask bits entirely, which is what makes it the fast way to clear VRAM.
            const uint32_t x = fifo_[1] & 0x3F0;
            const uint32_t y = (fifo_[1] >> 16) & 0x1FF;
            const uint32_t w = ((fifo_[2] & 0x3FF) + 0x0F) & ~0x0F;
            const uint32_t h = (fifo_[2] >> 16) & 0x1FF;

            // A fill writes VRAM directly and in wide bursts, so it is charged by the
            // row rather than by the pixel.
            AddDrawTicks(46 + static_cast<int32_t>((w / 8 + 9) * h));

            // Clipped at the right and bottom edges rather than wrapped. VramAt masks
            // its coordinates, so running off an edge used to come back round and land
            // on whatever was at the other side - and Silent Hill leans on that not
            // happening: it clears with fills that overhang the right edge by a few
            // pixels (x=512 w=544 reaches 1056, 32 past the 1024 VRAM is wide) while
            // keeping its texture palettes in the strip at x=0..31. Wrapping painted
            // the fill colour straight over those palettes, so every 4-bit texture
            // sharing them drew in one flat colour - a bright green figure in the dark
            // alley, because the fill that clobbered the palette was green. A shipped
            // game would be broken on real hardware if the fill wrapped, which is the
            // argument that it clips.
            DrawJob job;
            job.kind = DrawJob::kFill;
            job.env = CaptureDrawEnv();
            job.x = static_cast<int32_t>(x);
            job.y = static_cast<int32_t>(y);
            job.w = static_cast<int32_t>(w);
            job.h = static_cast<int32_t>(h);
            job.fill_colour = colour;
            SubmitJob(job);
        }

        // ---------------------------------------------------------------------------
        // Drawing time
        //
        // A real GPU takes time to rasterise, and software can tell: GPUSTAT's ready
        // bits drop while it is busy, and with them the DMA request line. This core
        // drew everything instantly and reported ready for ever, which is the "no
        // drawing time" entry in Docs/Gaps.md.
        //
        // The costs below are DuckStation's, and they are not Sony's: nobody published
        // a rasteriser timing table, so these are what the emulator community measured
        // and agreed on. Two parts to each primitive - a fixed setup, then a cost per
        // pixel that depends on what the pixel costs to produce.
        // ---------------------------------------------------------------------------

        // See the declaration: a transfer's own cycles, spent on the rasteriser now
        // rather than at the next batch boundary, and remembered so Tick does not
        // spend them again.
        void Gpu::AdvanceDrawing(uint32_t cpu_cycles) {
            if (cpu_cycles == 0)
                return;
            const uint32_t ticks =
                (cpu_cycles * kGpuClockNumerator) / kGpuClockDenominator;
            // Only what the rasteriser actually had work for is remembered. Time it
            // spent idle is not bankable: crediting it here would have Tick spend its
            // whole budget paying the bank back instead of drawing, and since the
            // bank only ever grows the rasteriser would fall further behind the
            // longer a game ran (bug 87).
            if (pending_draw_ticks_ > 0) {
                const uint32_t used =
                    (ticks < static_cast<uint32_t>(pending_draw_ticks_))
                        ? ticks : static_cast<uint32_t>(pending_draw_ticks_);
                pending_draw_ticks_ -= static_cast<int32_t>(used);
                prepaid_ticks_ += used;
            }
            DrainQueue();
        }

        // A transfer's cost, as drawing time the rasteriser owes (bug 93). One tick a
        // pixel, in one direction: the VRAM-to-VRAM copy is charged two a pixel, a
        // read and a write, and an upload is only the write while a readback is
        // only the read. DuckStation charges neither - the bus time of moving the
        // words is on the DMA side, which this core charges too - so this is a
        // derived figure, not a measured one, and it is off unless asked for.
        //
        // It goes on the same bill as a primitive, so what it delays is the
        // commands queued behind it. The transfer's own words are not held back:
        // they flow past a busy rasteriser, for the reason in Gpu::DrainQueue.
        void Gpu::ChargeTransfer(int32_t pixels) {
            if (pixels <= 0)
                return;
            AddDrawTicks(pixels);
            stats_.transfer_ticks += static_cast<uint64_t>(pixels);
        }

        void Gpu::AddDrawTicks(int32_t ticks) {

            if (ticks <= 0)
                return;
            pending_draw_ticks_ += ticks;
            stats_.draw_ticks += static_cast<uint64_t>(ticks);
        }

        // Setup, by primitive shape: a flat untextured triangle is cheap, a shaded
        // textured quad is more than ten times dearer.
        int32_t Gpu::PolygonSetupTicks(bool quad, bool shaded, bool textured) {
            static const int32_t kSetup[2][2][2] = {
                // [quad][shaded][textured]
                { {  46, 226 }, { 334, 496 } },
                { {  82, 262 }, { 370, 532 } },
            };
            return kSetup[quad ? 1 : 0][shaded ? 1 : 0][textured ? 1 : 0];
        }

        // One triangle: its area in pixels, doubled if it samples a texture, and half
        // as much again if each pixel has to read the framebuffer back - which is what
        // blending and mask-checking both do.
        int32_t Gpu::TriangleDrawTicks(const Vertex& a, const Vertex& b, const Vertex& c,
            const DrawState& state) const {
            // Clamped to the drawing area first: what a primitive costs is what it
            // actually rasterises, and a game that throws big polygons at a small
            // viewport - Silent Hill's 3D view is one - pays only for the part that
            // lands. Charging the unclipped area made it seven times dearer than the
            // hardware and left the GPU permanently behind (bug 87). It is still an
            // approximation for a triangle only partly outside: clamping the corners
            // undershoots rather than intersecting the edges, which is what
            // DuckStation does too.
            const int32_t ax = ClampToDrawArea(a.x, true),  ay = ClampToDrawArea(a.y, false);
            const int32_t bx = ClampToDrawArea(b.x, true),  by = ClampToDrawArea(b.y, false);
            const int32_t cx = ClampToDrawArea(c.x, true),  cy = ClampToDrawArea(c.y, false);
            const int64_t twice_area =
                static_cast<int64_t>(ax) * by + static_cast<int64_t>(bx) * cy +
                static_cast<int64_t>(cx) * ay - static_cast<int64_t>(ax) * cy -
                static_cast<int64_t>(bx) * ay - static_cast<int64_t>(cx) * by;
            int64_t pixels = (twice_area < 0 ? -twice_area : twice_area) / 2;
            if (state.textured)
                pixels += pixels;
            if (state.semi_transparent || check_mask_)
                pixels += (pixels + 1) / 2;
            if (DrawsOneFieldOnly())
                pixels /= 2;
            if (pixels > 0x00FFFFFF)
                pixels = 0x00FFFFFF;   // a primitive larger than VRAM is a bad packet
            return static_cast<int32_t>(pixels);
        }

        // One rectangle or sprite: a cost per row times the rows. A textured row costs
        // more, and how much more depends on the depth - the texture cache reloads
        // every few pixels, and reloads less often when the sprite is narrow enough for
        // a row to hit what the last one fetched.
        int32_t Gpu::RectangleDrawTicks(int32_t x, int32_t y, int32_t width,
            int32_t height, const DrawState& state) const {
            // Clipped to the drawing area, for the same reason the triangle above is.
            const int32_t left   = (x > draw_area_left_) ? x : draw_area_left_;
            const int32_t top    = (y > draw_area_top_) ? y : draw_area_top_;
            const int32_t right  = ((x + width - 1) < draw_area_right_)
                                       ? (x + width - 1) : draw_area_right_;
            const int32_t bottom = ((y + height - 1) < draw_area_bottom_)
                                       ? (y + height - 1) : draw_area_bottom_;
            width = right - left + 1;
            height = bottom - top + 1;
            if (width <= 0 || height <= 0)
                return 0;
            int64_t ticks_per_row = width;
            if (state.textured) {
                switch (state.texpage_colors) {
                case 0:   // 4-bit CLUT
                    ticks_per_row += width;
                    break;
                case 1:   // 8-bit CLUT: 8 bytes fetched every 4 pixels
                    if (width > 128)
                        ticks_per_row += (width / 4) * 8;
                    else if ((width * height) > 2048)
                        ticks_per_row += (width / 4) * (4 * (128 / (width ? width : 1)));
                    else
                        ticks_per_row += width;
                    break;
                default:  // 15-bit direct: the same again, in 2x2 blocks
                    if (width > 128)
                        ticks_per_row += (width / 2) * 8;
                    else if ((width * height) > 1024)
                        ticks_per_row += (width / 4) * (8 * (128 / (width ? width : 1)));
                    else
                        ticks_per_row += width;
                    break;
                }
            }
            if (state.semi_transparent || check_mask_)
                ticks_per_row += (width + 1) / 2;
            if (DrawsOneFieldOnly())
                height = (height / 2 > 0) ? (height / 2) : 1;
            const int64_t total = ticks_per_row * height;
            return static_cast<int32_t>(total > 0x00FFFFFF ? 0x00FFFFFF : total);
        }

        // ---------------------------------------------------------------------------
        // Primitives
        // ---------------------------------------------------------------------------

        void Gpu::CmdPolygon() {
            const uint32_t command = fifo_[0] >> 24;
            const bool gouraud = (command & 0x10) != 0;
            const bool quad = (command & 0x08) != 0;
            const bool textured = (command & 0x04) != 0;
            const bool semi = (command & 0x02) != 0;
            const bool raw = (command & 0x01) != 0;
            const int verts = quad ? 4 : 3;

            DrawState state;
            state.textured = textured;
            state.raw_texture = raw;
            state.semi_transparent = semi;
            state.gouraud = gouraud;
            state.clut_x = state.clut_y = 0;
            state.texpage_x = status_.texpage_x * 64;
            state.texpage_y = status_.texpage_y * 256;
            state.texpage_colors = status_.texpage_colors;
            state.semi_mode = status_.semi_mode;
            state.dither = status_.dither != 0;
            state.flip_x = false;
            state.flip_y = false;

            Vertex v[4];
            uint32_t raw_page = 0;
            uint32_t raw_clut = 0;
            int word = 0;
            uint32_t colour = fifo_[word++] & 0xFFFFFF;

            for (int i = 0; i < verts; ++i) {
                if (gouraud && i > 0)
                    colour = fifo_[word++] & 0xFFFFFF;

                const PreciseVertex& precise = fifo_precise_[word];
                const uint32_t position = fifo_[word++];
                v[i].x = SignExtend11(position & 0x7FF) + draw_offset_x_;
                v[i].y = SignExtend11((position >> 16) & 0x7FF) + draw_offset_y_;
                // PGXP (psx/pgxp.h): a vertex word that arrived with its shadow is drawn where
                // the GTE put it, offset the same way; the depth goes too if textures are to be
                // mapped in perspective. Only the hardware rasteriser reads these. A word that
                // arrived without one - moved about by a way the shadows cannot follow, which is
                // about half of them in Ridge Racer and Spyro 3 - takes the vertex the GTE last
                // projected to that same word, if it did so this frame or the last: the same
                // vertex, almost always, and the same fraction wherever else it is drawn, which
                // keeps the edges it shares from opening up.
                ++stats_.polygon_vertices;
                const PreciseVertex* found = precise.valid ? &precise : nullptr;
                if (found == nullptr && system().pgxp().enabled()) {
                    found = system().gte().Recall(position);
                    if (found != nullptr)
                        ++stats_.recalled_vertices;
                }
                v[i].precise = found != nullptr;
                if (found != nullptr) {
                    ++stats_.precise_vertices;
                    v[i].fx = found->x + static_cast<float>(draw_offset_x_);
                    v[i].fy = found->y + static_cast<float>(draw_offset_y_);
                    v[i].w = system().config().pgxp_textures ? found->w : 0.0f;
                    // And where the GTE found it in the last picture (Docs/DLSS-Plan.md).
                    v[i].moved = found->moved;
                    v[i].mx = found->mx;
                    v[i].my = found->my;
                }
                v[i].r = static_cast<uint8_t>(colour);
                v[i].g = static_cast<uint8_t>(colour >> 8);
                v[i].b = static_cast<uint8_t>(colour >> 16);
                v[i].u = 0;
                v[i].v = 0;

                if (textured) {
                    const uint32_t coord = fifo_[word++];
                    v[i].u = static_cast<uint8_t>(coord);
                    v[i].v = static_cast<uint8_t>(coord >> 8);
                    // The CLUT rides on the first vertex, the texpage on the second.
                    if (i == 0) {
                        const uint32_t clut = (coord >> 16) & 0xFFFF;
                        raw_clut = clut;
                        state.clut_x = (clut & 0x3F) * 16;
                        state.clut_y = (clut >> 6) & 0x1FF;
                    }
                    else if (i == 1) {
                        const uint32_t page = (coord >> 16) & 0xFFFF;
                        raw_page = page;
                        state.texpage_x = (page & 0x0F) * 64;
                        state.texpage_y = ((page >> 4) & 1) * 256;
                        state.semi_mode = (page >> 5) & 3;
                        state.texpage_colors = (page >> 7) & 3;

                        // Bit 11 disables texturing for this primitive: it is drawn with its
                        // own colour and the texture page is not read at all. Ignoring it
                        // meant sampling whatever happened to be at the texpage and painting
                        // it on screen.
                        if (page & 0x0800)
                            state.textured = false;

                        // A polygon's texpage also updates the persistent draw mode. Only
                        // bits 0-8 map straight across; the texture-disable bit lands at
                        // GPUSTAT bit 15, not bit 11 - bit 11 is the mask-set flag, and
                        // writing texture-disable into it corrupted the mask setting that
                        // software reads back.
                        status_.raw = (status_.raw & ~0x81FF) | (page & 0x01FF) |
                            ((page & 0x0800) << 4);
                    }
                }
            }

            RecordSetup(command, state, raw_page, raw_clut);

            // Motion for a polygon the GTE did not project - 2D, drawn from whole pixels: the
            // same one in the last picture, by its texture and texture coordinates, or its colour
            // and shape untextured, nearest by its middle. The whole of it moved the same way.
            // Not by a textured one's shape: Spyro 3 draws Spyro himself from words PGXP cannot
            // follow, and matched by texture his polygons moved near enough right - warp ratio
            // 0.889 - where, with their shape changing as he turns, they were lost (0.958).
            if (motion_) {
                bool any_precise = false;
                for (int i = 0; i < verts; ++i)
                    any_precise = any_precise || v[i].precise;
                if (!any_precise) {
                    uint64_t key = MixKey(0x2D, (static_cast<uint64_t>(verts) << 1) |
                                                    (state.textured ? 1u : 0u));
                    float mid_x = 0.0f, mid_y = 0.0f;
                    for (int i = 0; i < verts; ++i) {
                        mid_x += static_cast<float>(v[i].x);
                        mid_y += static_cast<float>(v[i].y);
                        if (state.textured) {
                            key = MixKey(key, v[i].u | (static_cast<uint32_t>(v[i].v) << 8));
                        }
                        else {
                            key = MixKey(key, PackColour(v[i]));
                            key = MixKey(key, (static_cast<uint32_t>(v[i].x - v[0].x) & 0xFFFF) |
                                                  (static_cast<uint32_t>(v[i].y - v[0].y) << 16));
                        }
                    }
                    if (state.textured)
                        key = MixKey(key, raw_page | (static_cast<uint64_t>(raw_clut) << 16));
                    float dx = 0.0f, dy = 0.0f;
                    if (FindSprite(key, mid_x / verts, mid_y / verts, &dx, &dy)) {
                        for (int i = 0; i < verts; ++i) {
                            v[i].moved = true;
                            v[i].mx = dx;
                            v[i].my = dy;
                        }
                    }
                }
            }

            AddDrawTicks(PolygonSetupTicks(quad, gouraud, state.textured));
            AddDrawTicks(TriangleDrawTicks(v[0], v[1], v[2], state));
            if (quad)
                AddDrawTicks(TriangleDrawTicks(v[1], v[2], v[3], state));

            DrawJob job;
            job.kind = DrawJob::kTriangle;
            job.env = CaptureDrawEnv();
            job.state = state;
            job.v[0] = v[0]; job.v[1] = v[1]; job.v[2] = v[2];
            SubmitJob(job);
            if (quad) {
                job.v[0] = v[1]; job.v[1] = v[2]; job.v[2] = v[3];
                SubmitJob(job);
            }
        }

        void Gpu::CmdLine() {
            const uint32_t command = fifo_[0] >> 24;
            const bool gouraud = (command & 0x10) != 0;
            const bool semi = (command & 0x02) != 0;

            DrawState state;
            state.textured = false;
            state.raw_texture = false;
            state.semi_transparent = semi;
            state.gouraud = gouraud;
            state.clut_x = state.clut_y = 0;
            state.texpage_x = state.texpage_y = 0;
            state.texpage_colors = 0;
            state.semi_mode = status_.semi_mode;
            state.dither = status_.dither != 0;
            state.flip_x = false;
            state.flip_y = false;

            int word = 0;
            uint32_t colour = fifo_[word++] & 0xFFFFFF;

            Vertex previous;
            bool have_previous = false;

            while (word < fifo_count_) {
                if (gouraud && have_previous)
                    colour = fifo_[word++] & 0xFFFFFF;
                if (word >= fifo_count_)
                    break;

                const uint32_t position = fifo_[word++];
                Vertex current;
                current.x = SignExtend11(position & 0x7FF) + draw_offset_x_;
                current.y = SignExtend11((position >> 16) & 0x7FF) + draw_offset_y_;
                current.r = static_cast<uint8_t>(colour);
                current.g = static_cast<uint8_t>(colour >> 8);
                current.b = static_cast<uint8_t>(colour >> 16);
                current.u = current.v = 0;

                if (have_previous) {
                    // A line costs its longer dimension - it plots one pixel per step
                    // along whichever axis it travels furthest - plus a flat setup, the
                    // same 16 a rectangle pays.
                    const int32_t dx = current.x - previous.x;
                    const int32_t dy = current.y - previous.y;
                    const int32_t span_x = (dx < 0) ? -dx : dx;
                    const int32_t span_y = (dy < 0) ? -dy : dy;
                    const int32_t rows =
                        DrawsOneFieldOnly()
                            ? ((span_y / 2 > 0) ? (span_y / 2) : 1)
                            : span_y;
                    AddDrawTicks(16 + ((span_x > rows) ? span_x : rows));
                    DrawJob job;
                    job.kind = DrawJob::kLine;
                    job.env = CaptureDrawEnv();
                    job.state = state;
                    job.v[0] = previous;
                    job.v[1] = current;
                    SubmitJob(job);
                }
                previous = current;
                have_previous = true;
            }
        }

        void Gpu::CmdRectangle() {
            const uint32_t command = fifo_[0] >> 24;
            const uint32_t size = (command >> 3) & 3;
            const bool textured = (command & 0x04) != 0;
            const bool semi = (command & 0x02) != 0;
            const bool raw = (command & 0x01) != 0;

            DrawState state;
            state.textured = textured;
            state.raw_texture = raw;
            state.semi_transparent = semi;
            state.gouraud = false;
            state.clut_x = state.clut_y = 0;
            state.texpage_x = status_.texpage_x * 64;
            state.texpage_y = status_.texpage_y * 256;
            state.texpage_colors = status_.texpage_colors;
            state.semi_mode = status_.semi_mode;
            // Rectangles are never dithered on hardware.
            state.dither = false;
            state.flip_x = rect_flip_x_;
            state.flip_y = rect_flip_y_;
            // A rectangle has no texpage word of its own, so texture disable comes from
            // the persistent draw mode.
            if (status_.texture_disable)
                state.textured = false;

            int word = 0;
            const uint32_t colour = fifo_[word++] & 0xFFFFFF;
            const uint32_t position = fifo_[word++];
            const int32_t x = SignExtend11(position & 0x7FF) + draw_offset_x_;
            const int32_t y = SignExtend11((position >> 16) & 0x7FF) + draw_offset_y_;

            uint8_t base_u = 0, base_v = 0;
            if (textured) {
                const uint32_t coord = fifo_[word++];
                base_u = static_cast<uint8_t>(coord);
                base_v = static_cast<uint8_t>(coord >> 8);
                const uint32_t clut = (coord >> 16) & 0xFFFF;
                state.clut_x = (clut & 0x3F) * 16;
                state.clut_y = (clut >> 6) & 0x1FF;
            }

            int32_t w = 0, h = 0;
            switch (size) {
            case 0: {
                const uint32_t extent = fifo_[word++];
                w = extent & 0x3FF;
                h = (extent >> 16) & 0x1FF;
                break;
            }
            case 1: w = h = 1; break;
            case 2: w = h = 8; break;
            default: w = h = 16; break;
            }

            const uint8_t r = static_cast<uint8_t>(colour);
            const uint8_t g = static_cast<uint8_t>(colour >> 8);
            const uint8_t b = static_cast<uint8_t>(colour >> 16);

            // A rectangle's setup is flat-rate, unlike a polygon's.
            AddDrawTicks(16);
            AddDrawTicks(RectangleDrawTicks(x, y, w, h, state));

            RecordSetup(command, state, 0, 0);

            DrawJob job;
            job.kind = DrawJob::kRectangle;
            job.env = CaptureDrawEnv();
            job.state = state;
            job.x = x; job.y = y; job.w = w; job.h = h;
            job.r = r; job.g = g; job.b = b;
            job.base_u = base_u; job.base_v = base_v;
            // Motion (Docs/DLSS-Plan.md, phase 2): the same sprite in the last picture - its
            // texture, CLUT, texel and size, or its colour and size untextured - nearest to here.
            if (motion_) {
                uint64_t key = MixKey(0x5B, static_cast<uint32_t>(w) |
                                                (static_cast<uint32_t>(h) << 10) |
                                                (state.textured ? 1u << 20 : 0u) |
                                                (state.flip_x ? 1u << 21 : 0u) |
                                                (state.flip_y ? 1u << 22 : 0u));
                if (state.textured)
                    key = MixKey(key, base_u | (static_cast<uint64_t>(base_v) << 8) |
                                          (static_cast<uint64_t>(state.clut_x) << 16) |
                                          (static_cast<uint64_t>(state.clut_y) << 26) |
                                          (static_cast<uint64_t>(state.texpage_x) << 36) |
                                          (static_cast<uint64_t>(state.texpage_y) << 46) |
                                          (static_cast<uint64_t>(state.texpage_colors) << 56));
                else
                    key = MixKey(key, colour);
                job.moved = FindSprite(key, static_cast<float>(x), static_cast<float>(y), &job.mx,
                                       &job.my);
            }
            SubmitJob(job);
        }

        // ---------------------------------------------------------------------------
        // Handing rasterising over
        //
        // The machine thread parses a command, charges it for the GPU time it will
        // take and snapshots the state it was issued under; what actually puts pixels
        // in VRAM is one of these jobs. Today they are applied where they are
        // submitted, which is what makes this refactor provably a no-op: every
        // checksum in Test-Suite.md is unchanged by it. What it buys is that the
        // rasteriser no longer reads live state, which is what a thread behind a queue
        // needs (phase 7 of Docs/Threading-Plan.md).
        // ---------------------------------------------------------------------------

        Gpu::DrawEnv Gpu::CaptureDrawEnv() const {
            DrawEnv env;
            env.area_left = draw_area_left_;
            env.area_top = draw_area_top_;
            env.area_right = draw_area_right_;
            env.area_bottom = draw_area_bottom_;
            env.tw_mask_x = texture_window_mask_x_;
            env.tw_mask_y = texture_window_mask_y_;
            env.tw_offset_x = texture_window_offset_x_;
            env.tw_offset_y = texture_window_offset_y_;
            env.force_set_mask = force_set_mask_;
            env.check_mask = check_mask_;
            env.skip_field = DrawsOneFieldOnly();
            env.active_line_lsb = ActiveLineLsb();
            return env;
        }

        void Gpu::SubmitJob(const DrawJob& job) {
            if (!threaded_) {
                DrawJob now = job;
                now.command = static_cast<uint8_t>(current_command_);
                backend_->Apply(now);
                return;
            }
            std::unique_lock<std::mutex> lock(jobs_mutex_);
            // A full ring means the rasteriser is a thousand primitives behind, which
            // is far more than a frame: waiting here is the back pressure that keeps
            // it from falling arbitrarily far behind and makes a barrier bounded.
            while (jobs_count_ == kJobCapacity)
                jobs_drained_.wait(lock);
            DrawJob& slot = jobs_[(jobs_head_ + jobs_count_) % kJobCapacity];
            slot = job;
            slot.command = static_cast<uint8_t>(current_command_);
            ++jobs_count_;
            ++stats_.raster_jobs;
            jobs_added_.notify_one();
        }

        void Gpu::StartRasterThread() {
            if (raster_thread_.joinable())
                return;
            {
                std::lock_guard<std::mutex> lock(jobs_mutex_);
                raster_stop_ = false;
            }
            raster_thread_ = std::thread(&Gpu::RasterLoop, this);
        }

        void Gpu::StopRasterThread() {
            if (!raster_thread_.joinable())
                return;
            {
                std::lock_guard<std::mutex> lock(jobs_mutex_);
                raster_stop_ = true;
            }
            jobs_added_.notify_all();
            raster_thread_.join();
        }

        void Gpu::RasterLoop() {
            std::unique_lock<std::mutex> lock(jobs_mutex_);
            for (;;) {
                while (jobs_count_ == 0 && !raster_stop_)
                    jobs_added_.wait(lock);
                if (jobs_count_ == 0 && raster_stop_)
                    break;
                const DrawJob job = jobs_[jobs_head_];
                jobs_head_ = (jobs_head_ + 1) % kJobCapacity;
                --jobs_count_;
                raster_busy_ = true;
                lock.unlock();
                backend_->Apply(job);
                lock.lock();
                raster_busy_ = false;
                jobs_drained_.notify_all();
            }
        }

        void Gpu::MergeRasterCounters() const {
            if (!backend_)
                return;
            RasterCounters& counters = backend_->counters();
            stats_.pixels += counters.pixels;
            stats_.clipped += counters.clipped;
            stats_.field_skipped += counters.field_skipped;
            stats_.mask_rejected += counters.mask_rejected;
            stats_.transparent_texels += counters.transparent_texels;
            for (int i = 0; i < 4; ++i)
                stats_.texels_by_depth[i] += counters.texels_by_depth[i];
            stats_.watch_writes += counters.watch_writes;
            for (int i = 0; i < 256; ++i)
                stats_.watch_writers[i] += counters.watch_writers[i];
            stats_.warp_pictures += counters.warp_pictures;
            stats_.warp_pixels += counters.warp_pixels;
            stats_.warp_moved_pixels += counters.warp_moved_pixels;
            stats_.warp_error_moved += counters.warp_error_moved;
            stats_.warp_error_still += counters.warp_error_still;
            memset(&counters, 0, sizeof(counters));
        }

        void Gpu::SyncRaster() const {
            std::unique_lock<std::mutex> lock(jobs_mutex_);
            if (threaded_) {
                if (jobs_count_ > 0 || raster_busy_)
                    ++stats_.raster_waits;
                while (jobs_count_ > 0 || raster_busy_)
                    jobs_drained_.wait(lock);
            }
            // Under the same lock the rasteriser releases after every job, so what it
            // counted is visible here - and merged whether it is threaded or not, since
            // the two paths have to produce the same numbers.
            MergeRasterCounters();
        }

        // ---------------------------------------------------------------------------
        // Rasterisation
        // ---------------------------------------------------------------------------

        // Captures the first few textured primitive setups for the harnesses.
        void Gpu::RecordSetup(uint32_t command, const DrawState& state,
            uint32_t raw_page, uint32_t raw_clut) {
            // Only 15-bit direct-colour draws for now: those are the ones under
            // suspicion, and the 4-bit ones flood the log before they appear.
            if (!state.textured || state.texpage_colors != 2 ||
                stats_.setup_count >= Stats::kSetupCapacity)
                return;
            Stats::TexturedSetup& setup = stats_.setups[stats_.setup_count++];
            setup.command = static_cast<uint8_t>(command);
            setup.colors = static_cast<uint8_t>(state.texpage_colors);
            setup.semi_mode = static_cast<uint8_t>(state.semi_mode);
            setup.flags = static_cast<uint8_t>((state.raw_texture ? 1 : 0) |
                (state.semi_transparent ? 2 : 0));
            setup.texpage_x = static_cast<uint16_t>(state.texpage_x);
            setup.texpage_y = static_cast<uint16_t>(state.texpage_y);
            setup.clut_x = static_cast<uint16_t>(state.clut_x);
            setup.clut_y = static_cast<uint16_t>(state.clut_y);
            setup.raw_page = static_cast<uint16_t>(raw_page);
            setup.raw_clut = static_cast<uint16_t>(raw_clut);
        }

        // ---------------------------------------------------------------------------
        // Display
        // ---------------------------------------------------------------------------

        void Gpu::UpdateDisplaySize() {
            // Horizontal resolution comes from two separate fields: hres2 overrides
            // hres1 when set. This is only how fast pixels leave the GPU, though - it
            // is the width the mode would produce if the beam were on for a whole
            // standard line.
            int mode_width;
            if (status_.hres2) {
                mode_width = 368;
            }
            else {
                static const int kWidths[4] = { 256, 320, 512, 640 };
                mode_width = kWidths[status_.hres1 & 3];
            }

            // How many of those pixels actually get painted is GP1(06)'s business: the
            // beam is on between X1 and X2, so the visible width is that window divided
            // by the clocks one pixel takes. The height has always come from GP1(07)
            // this way; the width used to be assumed, which is what let a game whose
            // window is narrower than its mode - Metal Gear Solid runs the 368 mode but
            // opens only ~318 pixels' worth of window - show 50 columns of whatever
            // VRAM sits to the right of its framebuffer, changing every time the buffers
            // flipped.
            //
            // Capped at the mode width rather than allowed to grow past it: a window
            // wider than the mode is overscan the beam paints off the side of a TV, and
            // showing it would mean sampling the VRAM to the right of the framebuffer
            // for exactly the reason above.
            const int window = static_cast<int>(horizontal_display_end_) -
                static_cast<int>(horizontal_display_start_);
            int width = mode_width;
            if (window > 0) {
                const int active = window / static_cast<int>(dot_clock_divider());
                if (active > 0 && active < mode_width)
                    width = active;
            }
            display_width_ = width;

            int lines = static_cast<int>(vertical_display_end_) -
                static_cast<int>(vertical_display_start_);
            if (lines <= 0)
                lines = 240;
            if (status_.vres && status_.vertical_interlace)
                lines *= 2;
            if (lines > kVramHeight)
                lines = kVramHeight;
            display_height_ = lines;
        }

        // The inverse of what UpdateDisplaySize and ResolveFramebuffer do: the frame starts
        // where the beam turns on, X1 (rounded down to a whole dot, as the GPU paints it) and Y1,
        // and each of its pixels is one dot clock wide and one line tall - two frame rows to a
        // line when both fields are shown at once. The same arithmetic as DuckStation's light
        // guns, so a game calibrated against one lines up here too.
        bool Gpu::BeamPositionAt(float x, float y, uint32_t* dot, uint32_t* line) const {
            if (!(x >= 0.0f && x < 1.0f && y >= 0.0f && y < 1.0f))
                return false;
            const uint32_t divider = dot_clock_divider();
            const uint32_t start = (horizontal_display_start_ / divider) * divider;
            const float column = x * static_cast<float>(display_width_);
            const float row = y * static_cast<float>(display_height_);
            const bool both_fields = status_.vres && status_.vertical_interlace;
            *dot = start + static_cast<uint32_t>(std::lround(column * static_cast<float>(divider)));
            const uint32_t frame_row = static_cast<uint32_t>(std::lround(row));
            *line = vertical_display_start_ + (both_fields ? frame_row / 2 : frame_row);
            return true;
        }

        void Gpu::ResolveFramebuffer() {
            SyncRaster();
            // What is shown, when the rasteriser draws sharper than native VRAM: the display
            // area as it drew it. Not in 24-bit mode, which is films arriving by CPU upload at
            // native size anyway, and read here byte by byte.
            picture_scale_ = 1;
            shared_picture_ = SharedPicture();
            // Motion (Docs/DLSS-Plan.md, phase 2): whether this is a new picture, and a fresh
            // start, for DLSS - worked out before the rasteriser resolves it, which it tells.
            bool new_picture = false, reset = false;
            if (motion_)
                NextPicture(&new_picture, &reset);
            if (!status_.display_disable && !status_.display_depth) {
                int scale = 1;
                if (backend_->ResolveDisplay(display_vram_x_, display_vram_y_,
                                             static_cast<uint32_t>(display_width_),
                                             static_cast<uint32_t>(display_height_), &picture_,
                                             &shared_picture_, &scale))
                    picture_scale_ = scale;
            }
            shared_picture_.new_picture = new_picture;
            shared_picture_.reset = reset;
            shared_picture_.interlaced = status_.vres && status_.vertical_interlace;
            // framebuffer_ is the native picture, which the checksums and everything else that
            // measures the machine read. Bringing native VRAM up to date for it waits for the
            // hardware rasteriser to finish the frame, so when the sharper picture is what is
            // shown and nobody has asked for the native one - the front end - it is left.
            if (picture_scale_ > 1 && !native_picture_) {
                if (const char* reason = backend_->lost())
                    FallBackToSoftware(reason);
                return;
            }
            // The rows shown, and wide enough for 24-bit mode's three bytes a pixel.
            backend_->PrepareRead(display_vram_x_, display_vram_y_,
                                  static_cast<uint32_t>(display_width_) * 3 / 2 + 2,
                                  static_cast<uint32_t>(display_height_));
            if (const char* reason = backend_->lost())
                FallBackToSoftware(reason);
            if (status_.display_disable) {
                memset(framebuffer_, 0,
                    sizeof(uint32_t) * display_width_ * display_height_);
                return;
            }

            for (int y = 0; y < display_height_; ++y) {
                uint32_t* row = framebuffer_ + y * display_width_;
                const uint32_t vram_y = display_vram_y_ + y;

                if (!status_.display_depth) {
                    // 15 bit: one VRAM halfword per pixel.
                    for (int x = 0; x < display_width_; ++x) {
                        const uint16_t pixel = VramAt(display_vram_x_ + x, vram_y);
                        row[x] = 0xFF000000u |
                            (From5Bit(pixel & 0x1F) << 16) |
                            (From5Bit((pixel >> 5) & 0x1F) << 8) |
                            From5Bit((pixel >> 10) & 0x1F);
                    }
                }
                else {
                    // 24 bit: three bytes per pixel, so two pixels span three halfwords.
                    for (int x = 0; x < display_width_; ++x) {
                        const uint32_t byte_offset = x * 3;
                        const uint16_t w0 = VramAt(display_vram_x_ + (byte_offset / 2), vram_y);
                        const uint16_t w1 = VramAt(display_vram_x_ + (byte_offset / 2) + 1, vram_y);
                        uint8_t r, g, b;
                        if ((byte_offset & 1) == 0) {
                            r = static_cast<uint8_t>(w0);
                            g = static_cast<uint8_t>(w0 >> 8);
                            b = static_cast<uint8_t>(w1);
                        }
                        else {
                            r = static_cast<uint8_t>(w0 >> 8);
                            g = static_cast<uint8_t>(w1);
                            b = static_cast<uint8_t>(w1 >> 8);
                        }
                        row[x] = 0xFF000000u | (r << 16) | (g << 8) | b;
                    }
                }
            }
        }

        // Follows the setting rather than only reading it once: the front end can turn
        // it on mid-run, and a harness sets it after the machine is already built. Safe
        // here because this is the machine thread, the only one that submits.
        void Gpu::SyncThreadWithConfig() {
            const bool wanted = system().config().gpu_thread;
            if (wanted == threaded_)
                return;
            if (threaded_) {
                SyncRaster();
                StopRasterThread();
                threaded_ = false;
            } else {
                threaded_ = true;
                StartRasterThread();
            }
        }

        // Where the beam leaves the display window, in GPU clocks into the line - where
        // hblank begins. A window that never ends within the line ends with it.
        static uint32_t HblankStart(uint32_t display_end, uint32_t line) {
            return (display_end == 0 || display_end > line) ? line : display_end;
        }

        uint32_t Gpu::CyclesToNextEvent() const {
            // In units of a seventh of a GPU clock, as dot_accumulator_ is; a CPU cycle
            // is eleven of them.
            const uint64_t position = dot_accumulator_;
            uint64_t soonest = static_cast<uint64_t>(kDotsPerScanline) * kGpuClockDenominator;
            const uint32_t edges[2] = { horizontal_display_start_,
                                        HblankStart(horizontal_display_end_, kDotsPerScanline) };
            for (uint32_t edge : edges) {
                const uint64_t at = static_cast<uint64_t>(edge) * kGpuClockDenominator;
                if (at > position && at < soonest)
                    soonest = at;
            }
            uint64_t cycles = (soonest - position + kGpuClockNumerator - 1) / kGpuClockNumerator;

            // The rasteriser finishing: GPUSTAT's ready bits, and a DMA waiting on them.
            const uint64_t owed = static_cast<uint64_t>(prepaid_ticks_) +
                                  (pending_draw_ticks_ > 0 ? pending_draw_ticks_ : 0);
            if (pending_draw_ticks_ > 0) {
                const uint64_t draw = (owed * kGpuClockDenominator + kGpuClockNumerator - 1) /
                                      kGpuClockNumerator;
                if (draw < cycles)
                    cycles = draw;
            }
            if (cycles < 1)
                cycles = 1;
            return cycles > 0xFFFFFFFFu ? 0xFFFFFFFFu : static_cast<uint32_t>(cycles);
        }

        bool Gpu::Tick(uint32_t cycles) {
            // Where the beam was, in GPU clocks into the line, for counting hblanks as
            // it enters them (exact_hblank_).
            const uint32_t line_start_position = dot_accumulator_ / kGpuClockDenominator;
            dot_accumulator_ += cycles * kGpuClockNumerator;
            const uint32_t dots = dot_accumulator_ / kGpuClockDenominator;
            dot_accumulator_ -= dots * kGpuClockDenominator;

            // Counter 0 counts dot clocks, which are GPU clocks divided down by the
            // horizontal resolution - narrower modes spend more GPU clocks per pixel.
            // The remainder is carried, so a resolution change mid-line loses nothing.
            //
            // `dots` cannot be used for this. What the loop below leaves in
            // dot_accumulator_ is the beam position within the scanline, not a
            // fractional remainder, so `dots` is the position plus this call's elapsed
            // clocks - counting it as a delta counts most of the line again on every
            // single call. Hence a remainder of its own.
            dot_clock_remainder_ += cycles * kGpuClockNumerator;
            const uint32_t gpu_clocks = dot_clock_remainder_ / kGpuClockDenominator;
            dot_clock_remainder_ %= kGpuClockDenominator;

            dot_clock_accum_ += gpu_clocks;
            const uint32_t divider = dot_clock_divider();
            pending_dot_clocks_ += dot_clock_accum_ / divider;
            dot_clock_accum_ %= divider;

            const uint32_t total_lines =
                status_.video_mode ? kScanlinesPal : kScanlinesNtsc;

            // Exact event timing counts each time the beam crosses into hblank: from
            // where it was to where it is now, `dots` GPU clocks into the line it was on.
            if (exact_hblank_) {
                const uint32_t edge = HblankStart(horizontal_display_end_, kDotsPerScanline);
                const uint32_t crossed_before = (line_start_position >= edge) ? 1 : 0;
                const uint32_t crossed_by_now =
                    (dots >= edge) ? (dots - edge) / kDotsPerScanline + 1 : 0;
                pending_hblanks_ += crossed_by_now - crossed_before;
            }

            bool frame_completed = false;
            uint32_t remaining = dots;
            while (remaining >= kDotsPerScanline) {
                remaining -= kDotsPerScanline;
                ++scanline_;
                // Exactly one hblank per scanline. Counting them off completed lines
                // rather than off the gate below means the count is exact even when a
                // batch spans several lines; only the instant within the line they are
                // attributed to is approximate - which exact event timing, above,
                // replaces with the instant the beam enters hblank.
                if (!exact_hblank_)
                    ++pending_hblanks_;

                if (scanline_ >= total_lines) {
                    scanline_ = 0;
                    frame_completed = true;
                    // Bit 31 means two different things. With vertical interlace on it is
                    // the field being drawn, and flips once per frame; with it off it is
                    // simply the parity of the current scanline. Flipping it per scanline
                    // in interlace mode left software that waits for a particular field
                    // spinning on GPUSTAT forever.
                    if (status_.vertical_interlace)
                        status_.odd_line ^= 1;
                }

                if (!status_.vertical_interlace)
                    status_.odd_line = scanline_ & 1;

                const bool now_in_vblank = scanline_ >= vertical_display_end_;
                if (now_in_vblank && !was_in_vblank_) {
                    system().io().SetInterrupt(kInterruptVSYNC);
                    SyncThreadWithConfig();
                    ResolveFramebuffer();
                    ++frame_count_;
                    system().gte().NewFrame();   // PGXP's vertex cache ages a frame
                }
                was_in_vblank_ = now_in_vblank;
            }
            // Burn down whatever drawing is still owed. `gpu_clocks` above is this
            // call's elapsed time in the GPU's own clock, already carried across calls,
            // so it is what the rasteriser gets through too.
            // Whatever a transfer already paid for (AdvanceDrawing) comes off first,
            // so these cycles are not spent on the rasteriser twice. The display
            // timing above is not affected: that time passed either way.
            uint32_t payable = gpu_clocks;
            if (prepaid_ticks_ > 0) {
                const uint32_t used =
                    (prepaid_ticks_ < payable) ? prepaid_ticks_ : payable;
                prepaid_ticks_ -= used;
                payable -= used;
            }
            if (pending_draw_ticks_ > 0 && payable > 0) {
                pending_draw_ticks_ -= static_cast<int32_t>(payable);
                if (pending_draw_ticks_ < 0)
                    pending_draw_ticks_ = 0;
            }
            // Whatever the rasteriser has caught up on, the queue can now feed it.
            if (queue_size_ > 0 && !drawing())
                DrainQueue();

            dot_accumulator_ += remaining * kGpuClockDenominator;
            return frame_completed;
        }

    }
}
