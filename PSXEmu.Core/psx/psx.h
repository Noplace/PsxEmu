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

// Aggregate header for the PSX core. Order is load-bearing: a type used inline
// in another header must be included before it.

#define WIN32_LEAN_AND_MEAN
// windows.h defines min/max as macros, which collides with <algorithm>.
#define NOMINMAX
#include <windows.h>
#define _USE_MATH_DEFINES
#include <math.h>
#include <memory.h>
#include <eh.h>
#include <functional>

#include <cstddef>
#include <cstdint>

#include "emucore/timing/frame_limiter.h"
#include "emucore/timing/speed_resampler.h"

#include "psx/types.h"
#include "emu/state.h"
#include "emu/emuconfig.h"
#include "debug/debug.h"
#include "psx/component.h"
#include "enhance/pgxp.h"
#include "enhance/vertex_motion.h"
#include "psx/cpu_context.h"
#include "psx/cpu.h"
#include "psx/gte.h"
#include "psx/gpu_core.h"
#include "render/raster.h"
#include "render/software_raster.h"
#include "psx/gpu.h"
#include "media/disc.h"
#include "psx/cdrom.h"
#include "psx/sio.h"
#include "psx/sio1.h"
#include "psx/spu.h"
#include "psx/mdec.h"
#include "psx/root_counter.h"
#include "psx/dma.h"
#include "psx/io_interface.h"
#include "bios/bios_calls.h"
#include "bios/kernel.h"
#include "debug/debugger.h"
#include "psx/mc.h"
#include "media/iso9660.h"
#include "bios/hle/hle_bios.h"
#include "psx/system.h"
#include "emu/settings.h"
