/*****************************************************************************************************************
* Copyright (c) 2014 Khalid Ali Al-Kooheji                                                                       *
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

// The hardware rasteriser, whichever graphics API draws it: Direct3D 11 (d3d11_raster) or
// Direct3D 12 (d3d12_raster). The two draw the same pictures from the same shaders
// (raster_common.h); which one is the user's choice - Settings > Video's rasteriser - and the
// tools' (`--d3d12`). What they share beyond RasterBackend is what the front end and the test
// tools ask of them.

#include "psx/raster.h"
#include "psx/shared_picture.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace psxemu {

    class HardwareRaster : public emulation::psx::RasterBackend {
     public:
        enum class Api { kD3D11, kD3D12 };

        // A rasteriser drawing with `api` for `vram` (Gpu's native VRAM, 1024x512) at
        // `options`' scale, on `options`' adapter or Windows' default - or on WARP, Windows' own
        // software Direct3D, which is deterministic and needs no graphics card: what the headless
        // comparisons run on. Null, with `error` saying why, if it cannot be made. Asked for
        // shared pictures and unable to make them, it reads back instead.
        static std::unique_ptr<HardwareRaster> Create(Api api, uint16_t* vram,
                                                      const emulation::psx::RasterOptions& options,
                                                      bool warp, std::string* error);

        // A shared picture's pixels, 0xFFRRGGBB rows, read back through a device of its own on
        // the picture's card - for a screenshot now and then, from any thread, whichever
        // rasteriser made it. False if it cannot be opened, or is not drawn within a second.
        static bool ReadSharedPicture(const emulation::psx::SharedPicture& picture,
                                      std::vector<uint32_t>* pixels);

        // How many textures a shared picture can be in at once (psx/shared_picture.h). With none
        // free the picture is read back.
        static constexpr int kSharedPictures = emulation::psx::kSharedTextureCount;

        // Which API it draws with.
        virtual Api api() const = 0;
        // Whether it hands the picture over on the card; false once it has fallen back to
        // reading it back, or was never asked.
        virtual bool sharing_pictures() const = 0;
        // The jitter being drawn with now, in the target's pixels (RasterBackend::SetJitter).
        virtual float jitter_x() const = 0;
        virtual float jitter_y() const = 0;
        // Whether the plane beside VRAM is being drawn (psx/shared_picture.h).
        virtual bool planes() const = 0;
        // The plane over a rectangle of VRAM, every sub-pixel of it as four floats - R, G, B, A,
        // rows of w x scale - read back, for hw_raster_test. False if it is not being drawn.
        virtual bool ReadPlanes(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                                std::vector<float>* rgba) = 0;
        virtual int scale() const = 0;
        // For hw_raster_test: from here on, report the device lost, as a driver reset would.
        virtual void SimulateLoss() = 0;
    };

}   // namespace psxemu
