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
#include "graphics/hw_raster/hardware_raster.h"

#include "graphics/hw_raster/d3d11_raster.h"
#include "graphics/hw_raster/d3d12_raster.h"

namespace psxemu {

    std::unique_ptr<HardwareRaster> HardwareRaster::Create(
        Api api, uint16_t* vram, const emulation::psx::RasterOptions& options, bool warp,
        std::string* error) {
        if (api == Api::kD3D12)
            return D3D12Raster::Create(vram, options, warp, error);
        return D3D11Raster::Create(vram, options, warp, error);
    }

    bool HardwareRaster::ReadSharedPicture(const emulation::psx::SharedPicture& picture,
                                           std::vector<uint32_t>* pixels) {
        if (!picture)
            return false;
        return picture.source->d3d12() ? D3D12Raster::ReadSharedPicture(picture, pixels)
                                       : D3D11Raster::ReadSharedPicture(picture, pixels);
    }

}   // namespace psxemu
