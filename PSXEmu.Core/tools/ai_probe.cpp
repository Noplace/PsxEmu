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

// ai_probe: what the AI texture upscaler's backends can do on the cards here
// (Docs/Texture-Upscaling-Plan.md, phase 0).
//
//   ai_probe [folder] [--card n] [--all] [--convert in.pth out.onnx]
//            [--model m.onnx ...] [--rtx-video quality [--scale n]]
//            [--compare other.onnx] [--cost] [--sizes 16,64,256] [--runs n]
//            [--image in.png|in.ppm ... --out folder [--pad n] [--tile n]]
//
// Loads ONNX Runtime and DirectML from `folder` - by default its own, where build_tools.bat copies
// the fetched ones - the way the emulator will (each signature checked), and lists the cards.
//
// --rtx-video puts NVIDIA's RTX Video Super Resolution through the same tests as a model, at
// quality 0-4 (0 is bicubic) and --scale (4) times larger, with nvngx_vsr.dll from `folder`. Only a
// build that found the SDK in Temp\rtxvideo\ has it (emuwin32/upscale/texture/rtx_video.h).
//
// --convert reads a PyTorch .pth upscaler as data (emuwin32/upscale/texture/torch_model.h) and
// writes it as ONNX; no card is needed.
//
// --model, as many as wanted, loads each ONNX upscaler onto the first card asked about - the
// integrated one unless --card or --all says otherwise: a device made on the NVIDIA card is one
// more on a card that has not liked being cycled (the 0x9F of 2026-09-29), so it is asked about
// only when wanted, and every model shares one device a run. --compare runs a second model on the same picture and says how far apart the two are: a
// .pth converted here against its author's own ONNX export checks the conversion, and passes
// within half an 8-bit step. --cost then times it on square pictures of each size, the first run
// of a size apart from the rest (DirectML compiles the graph for each new size), and the median of
// --runs (10) after.
//
// --image, as many as wanted, upscales each picture the way the emulator will a texture:
// transparent texels take the colour of the nearest opaque one first, so the model draws no dark
// fringe into a cut-out's edge; the picture goes through in tiles of --tile texels (0, the default:
// all at once), each with --pad (8) texels of the picture round it, repeated at its edges and
// thrown away after; its coverage goes through the model too and is thresholded at a half. Into
// the --out folder go `<picture>.png` and the same picture scaled by repeating texels,
// `<picture>.nearest.png`, to compare with. A heavy model wants tiles on a weak card: one call
// that keeps the card busy past Windows' two seconds has the card reset (an ESRGAN on 256x256 on
// the Radeon 780M, 2026-10-16).
//
// Exits 0 when ONNX Runtime loaded and everything asked for worked, 1 otherwise.

#include "emuwin32/present/adapters.h"
#include "emuwin32/upscale/texture/onnx_runtime.h"
#include "emuwin32/upscale/texture/rtx_video.h"
#include "emuwin32/upscale/texture/torch_model.h"

#include <d3d12.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "windowscodecs.lib")

namespace {

    using Microsoft::WRL::ComPtr;


    std::wstring ExecutableFolder() {
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring folder = path;
        return folder.substr(0, folder.find_last_of(L'\\'));
    }

    double Now() {
        static LARGE_INTEGER frequency = [] {
            LARGE_INTEGER f;
            QueryPerformanceFrequency(&f);
            return f;
        }();
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        return static_cast<double>(t.QuadPart) * 1000.0 / static_cast<double>(frequency.QuadPart);
    }

    // ---- Pictures: RGBA8 rows -----------------------------------------------------------------

    struct Picture {
        int width = 0, height = 0;
        std::vector<uint8_t> rgba;
    };

    bool ReadPpm(const std::wstring& path, Picture* picture) {
        FILE* f = _wfopen(path.c_str(), L"rb");
        if (f == nullptr)
            return false;
        int width = 0, height = 0, max = 0;
        const bool ok = fscanf(f, "P6 %d %d %d", &width, &height, &max) == 3 && max == 255 &&
                        width > 0 && height > 0;
        if (ok) {
            fgetc(f);
            std::vector<uint8_t> rgb(static_cast<size_t>(width) * height * 3);
            if (fread(rgb.data(), 1, rgb.size(), f) == rgb.size()) {
                picture->width = width;
                picture->height = height;
                picture->rgba.resize(static_cast<size_t>(width) * height * 4);
                for (size_t i = 0; i < static_cast<size_t>(width) * height; ++i) {
                    memcpy(&picture->rgba[i * 4], &rgb[i * 3], 3);
                    picture->rgba[i * 4 + 3] = 255;
                }
            }
        }
        fclose(f);
        return !picture->rgba.empty();
    }

    bool ReadImage(const std::wstring& path, Picture* picture) {
        if (path.size() > 4 && _wcsicmp(path.c_str() + path.size() - 4, L".ppm") == 0)
            return ReadPpm(path, picture);
        ComPtr<IWICImagingFactory> factory;
        ComPtr<IWICBitmapDecoder> decoder;
        ComPtr<IWICBitmapFrameDecode> frame;
        ComPtr<IWICFormatConverter> converter;
        UINT width = 0, height = 0;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&factory))) ||
            FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                      WICDecodeMetadataCacheOnDemand, &decoder)) ||
            FAILED(decoder->GetFrame(0, &frame)) ||
            FAILED(factory->CreateFormatConverter(&converter)) ||
            FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
                                         WICBitmapDitherTypeNone, nullptr, 0.0,
                                         WICBitmapPaletteTypeCustom)) ||
            FAILED(converter->GetSize(&width, &height)))
            return false;
        picture->width = static_cast<int>(width);
        picture->height = static_cast<int>(height);
        picture->rgba.resize(static_cast<size_t>(width) * height * 4);
        return SUCCEEDED(converter->CopyPixels(nullptr, width * 4,
                                               static_cast<UINT>(picture->rgba.size()),
                                               picture->rgba.data()));
    }

    bool WritePng(const std::wstring& path, const Picture& picture) {
        ComPtr<IWICImagingFactory> factory;
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapEncoder> encoder;
        ComPtr<IWICBitmapFrameEncode> frame;
        // WIC's PNG encoder takes BGRA, not RGBA.
        WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
        std::vector<uint8_t> bgra = picture.rgba;
        for (size_t i = 0; i < bgra.size(); i += 4)
            std::swap(bgra[i], bgra[i + 2]);
        int step = 0;
        HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&factory));
        if (SUCCEEDED(hr) && ++step) hr = factory->CreateStream(&stream);
        if (SUCCEEDED(hr) && ++step) hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
        if (SUCCEEDED(hr) && ++step) hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
        if (SUCCEEDED(hr) && ++step) hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
        if (SUCCEEDED(hr) && ++step) hr = encoder->CreateNewFrame(&frame, nullptr);
        if (SUCCEEDED(hr) && ++step) hr = frame->Initialize(nullptr);
        if (SUCCEEDED(hr) && ++step) hr = frame->SetSize(picture.width, picture.height);
        if (SUCCEEDED(hr) && ++step) hr = frame->SetPixelFormat(&format);
        if (SUCCEEDED(hr) && ++step && !IsEqualGUID(format, GUID_WICPixelFormat32bppBGRA)) hr = E_FAIL;
        if (SUCCEEDED(hr) && ++step)
            hr = frame->WritePixels(picture.height, picture.width * 4,
                                    static_cast<UINT>(bgra.size()), bgra.data());
        if (SUCCEEDED(hr) && ++step) hr = frame->Commit();
        if (SUCCEEDED(hr) && ++step) hr = encoder->Commit();
        if (FAILED(hr))
            printf("    PNG step %d failed: %08lx\n", step, static_cast<unsigned long>(hr));
        return SUCCEEDED(hr);
    }

    // Each transparent texel given the colour of the nearest opaque one - a breadth-first fill
    // out from every opaque texel at once - so an upscaler never sees the black under a hole.
    void Dilate(Picture* picture) {
        const int w = picture->width, h = picture->height;
        std::vector<int> from(static_cast<size_t>(w) * h, -1);
        std::deque<int> queue;
        for (int i = 0; i < w * h; ++i)
            if (picture->rgba[i * 4 + 3] >= 128) {
                from[i] = i;
                queue.push_back(i);
            }
        if (queue.empty())
            return;
        while (!queue.empty()) {
            const int i = queue.front();
            queue.pop_front();
            const int x = i % w, y = i / w;
            const int next[4][2] = { { x - 1, y }, { x + 1, y }, { x, y - 1 }, { x, y + 1 } };
            for (const auto& n : next) {
                if (n[0] < 0 || n[1] < 0 || n[0] >= w || n[1] >= h)
                    continue;
                const int j = n[1] * w + n[0];
                if (from[j] < 0) {
                    from[j] = from[i];
                    queue.push_back(j);
                }
            }
        }
        for (int i = 0; i < w * h; ++i)
            if (picture->rgba[i * 4 + 3] < 128)
                memcpy(&picture->rgba[i * 4], &picture->rgba[from[i] * 4], 3);
    }

    // The `w` x `h` part of the picture from (x0, y0), as planar floats - the colour, or with
    // `coverage` the opacity in all three channels. Anything outside the picture repeats its edge.
    std::vector<float> Planar(const Picture& picture, int x0, int y0, int w, int h, bool coverage) {
        std::vector<float> planes(static_cast<size_t>(w) * h * 3);
        for (int y = 0; y < h; ++y) {
            const int sy = std::clamp(y0 + y, 0, picture.height - 1);
            for (int x = 0; x < w; ++x) {
                const int sx = std::clamp(x0 + x, 0, picture.width - 1);
                const uint8_t* p = &picture.rgba[(static_cast<size_t>(sy) * picture.width + sx) * 4];
                for (int c = 0; c < 3; ++c)
                    planes[(static_cast<size_t>(c) * h + y) * w + x] =
                        coverage ? (p[3] >= 128 ? 1.0f : 0.0f) : p[c] / 255.0f;
            }
        }
        return planes;
    }

    // ---- The tests ----------------------------------------------------------------------------

    bool Cost(emulation::win32::TextureUpscaler& upscaler, const std::vector<int>& sizes, int runs) {
        printf("    %-11s %-11s %10s %10s %10s\n", "in", "out", "first ms", "median ms",
               "Mpx/s out");
        bool ok = true;
        for (int size : sizes) {
            const int w = upscaler.fixed_width() != 0 ? upscaler.fixed_width() : size;
            const int h = upscaler.fixed_height() != 0 ? upscaler.fixed_height() : size;
            std::vector<float> rgb(static_cast<size_t>(w) * h * 3);
            for (size_t i = 0; i < rgb.size(); ++i)
                rgb[i] = static_cast<float>((i * 2654435761u) >> 24) / 255.0f;
            std::vector<float> out;
            std::vector<double> times;
            int ow = 0, oh = 0;
            std::string error;
            for (int run = 0; run <= runs; ++run) {
                const double start = Now();
                if (!upscaler.Run(rgb, w, h, &out, &ow, &oh, &error)) {
                    printf("    %dx%d: %s\n", w, h, error.c_str());
                    ok = false;
                    break;
                }
                times.push_back(Now() - start);
            }
            if (times.size() != static_cast<size_t>(runs) + 1)
                continue;
            const double first = times[0];
            std::vector<double> rest(times.begin() + 1, times.end());
            std::sort(rest.begin(), rest.end());
            const double median = rest.empty() ? first : rest[rest.size() / 2];
            char in_size[32], out_size[32];
            snprintf(in_size, sizeof(in_size), "%dx%d", w, h);
            snprintf(out_size, sizeof(out_size), "%dx%d", ow, oh);
            printf("    %-11s %-11s %10.1f %10.2f %10.1f\n", in_size, out_size, first, median,
                   static_cast<double>(ow) * oh / (median * 1000.0));
            if (upscaler.fixed_width() != 0)
                break;   // one size is all it takes
        }
        return ok;
    }

    // Two models on the same 64x48 picture - not square, so a height and width swapped shows - and
    // how far apart their outputs are.
    bool Compare(emulation::win32::TextureUpscaler& a, emulation::win32::TextureUpscaler& b) {
        const int w = 64, h = 48;
        std::vector<float> rgb(static_cast<size_t>(w) * h * 3);
        for (int c = 0; c < 3; ++c)
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                    rgb[(static_cast<size_t>(c) * h + y) * w + x] =
                        0.5f + 0.4f * std::sin(x * 0.37f + y * 0.21f * (c + 1)) *
                                   (((x / 6 + y / 5) & 1) ? 1.0f : -0.6f);
        std::vector<float> out_a, out_b;
        int aw = 0, ah = 0, bw = 0, bh = 0;
        std::string error;
        if (!a.Run(rgb, w, h, &out_a, &aw, &ah, &error) || !b.Run(rgb, w, h, &out_b, &bw, &bh, &error)) {
            printf("    %s\n", error.c_str());
            return false;
        }
        if (aw != bw || ah != bh) {
            printf("    the outputs differ in size: %dx%d and %dx%d\n", aw, ah, bw, bh);
            return false;
        }
        // Compared as they are used, clamped to 0-1: some exports clamp in the graph, some not.
        double largest = 0.0, total = 0.0;
        for (size_t i = 0; i < out_a.size(); ++i) {
            const double d = std::fabs(static_cast<double>(std::clamp(out_a[i], 0.0f, 1.0f)) -
                                       std::clamp(out_b[i], 0.0f, 1.0f));
            largest = std::max(largest, d);
            total += d;
        }
        printf("    %dx%d to %dx%d: largest difference %.6f, mean %.7f (in 0-1; one 8-bit step is "
               "%.4f)\n", w, h, aw, ah, largest, total / out_a.size(), 1.0 / 255.0);
        return largest < 0.5 / 255.0;
    }

    bool Upscale(emulation::win32::TextureUpscaler& upscaler, const std::wstring& in,
                 const std::wstring& folder, int pad, int tile) {
        Picture picture;
        if (!ReadImage(in, &picture)) {
            printf("    could not read %ls\n", in.c_str());
            return false;
        }
        bool holes = false;
        for (size_t i = 3; i < picture.rgba.size(); i += 4)
            holes = holes || picture.rgba[i] < 128;
        Picture filled = picture;
        Dilate(&filled);

        // Tile by tile - the whole picture as one tile when `tile` is 0 - each tile with `pad`
        // texels of the picture round it, which are thrown away after.
        const int tile_w = tile > 0 ? tile : picture.width, tile_h = tile > 0 ? tile : picture.height;
        Picture upscaled;
        int s = 0, tiles = 0;
        std::string error;
        const double start = Now();
        for (int ty = 0; ty < picture.height; ty += tile_h) {
            for (int tx = 0; tx < picture.width; tx += tile_w) {
                const int w = std::min(tile_w, picture.width - tx) + pad * 2;
                const int h = std::min(tile_h, picture.height - ty) + pad * 2;
                std::vector<float> result, coverage;
                int ow = 0, oh = 0;
                if (!upscaler.Run(Planar(filled, tx - pad, ty - pad, w, h, false), w, h, &result,
                                  &ow, &oh, &error) ||
                    (holes && !upscaler.Run(Planar(picture, tx - pad, ty - pad, w, h, true), w, h,
                                            &coverage, &ow, &oh, &error))) {
                    printf("    %s\n", error.c_str());
                    return false;
                }
                ++tiles;
                if (s == 0) {
                    s = ow / w;
                    upscaled.width = picture.width * s;
                    upscaled.height = picture.height * s;
                    upscaled.rgba.resize(static_cast<size_t>(upscaled.width) * upscaled.height * 4);
                }
                for (int y = pad * s; y < oh - pad * s; ++y) {
                    for (int x = pad * s; x < ow - pad * s; ++x) {
                        const size_t from = static_cast<size_t>(y) * ow + x;
                        const int dx = tx * s + x - pad * s, dy = ty * s + y - pad * s;
                        uint8_t* p = &upscaled.rgba[(static_cast<size_t>(dy) * upscaled.width + dx) * 4];
                        for (int c = 0; c < 3; ++c) {
                            const float v = result[static_cast<size_t>(c) * ow * oh + from];
                            p[c] = static_cast<uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f));
                        }
                        float alpha = 1.0f;
                        if (holes) {
                            alpha = 0.0f;
                            for (int c = 0; c < 3; ++c)
                                alpha += coverage[static_cast<size_t>(c) * ow * oh + from] / 3.0f;
                        }
                        p[3] = alpha >= 0.5f ? 255 : 0;
                    }
                }
            }
        }
        const double took = Now() - start;
        Picture nearest;
        nearest.width = upscaled.width;
        nearest.height = upscaled.height;
        nearest.rgba.resize(upscaled.rgba.size());
        for (int y = 0; y < nearest.height; ++y)
            for (int x = 0; x < nearest.width; ++x)
                memcpy(&nearest.rgba[(static_cast<size_t>(y) * nearest.width + x) * 4],
                       &picture.rgba[(static_cast<size_t>(y / s) * picture.width + x / s) * 4], 4);
        std::wstring stem = in.substr(in.find_last_of(L"\\/") == std::wstring::npos
                                          ? 0
                                          : in.find_last_of(L"\\/") + 1);
        stem = folder + L"\\" + stem.substr(0, stem.find_last_of(L'.'));
        if (!WritePng(stem + L".png", upscaled) || !WritePng(stem + L".nearest.png", nearest)) {
            printf("    could not write %ls.png\n", stem.c_str());
            return false;
        }
        printf("    %ls: %dx%d to %dx%d in %.1f ms, %d tile%s%s\n", in.c_str(), picture.width,
               picture.height, upscaled.width, upscaled.height, took, tiles, tiles == 1 ? "" : "s",
               holes ? ", coverage too" : "");
        return true;
    }

}   // namespace

int wmain(int argc, wchar_t** argv) {
    std::wstring folder = argc > 1 && argv[1][0] != L'-' ? argv[1] : ExecutableFolder();
    wchar_t full[MAX_PATH] = {};
    if (GetFullPathNameW(folder.c_str(), MAX_PATH, full, nullptr) != 0)
        folder = full;

    int card = -1, runs = 10, pad = 8, tile = 0, rtx_quality = -1, rtx_scale = 4;
    bool all = false, cost = false;
    std::wstring convert_in, convert_out, compare, out = L".";
    std::vector<std::wstring> models;
    std::vector<std::wstring> images;
    std::vector<int> sizes = { 16, 32, 64, 128, 256, 512 };
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        const bool more = i + 1 < argc;
        if (arg == L"--card" && more)
            card = _wtoi(argv[++i]);
        else if (arg == L"--runs" && more)
            runs = std::max(1, _wtoi(argv[++i]));
        else if (arg == L"--pad" && more)
            pad = std::max(0, _wtoi(argv[++i]));
        else if (arg == L"--model" && more)
            models.push_back(argv[++i]);
        else if (arg == L"--rtx-video" && more)
            rtx_quality = std::clamp(_wtoi(argv[++i]), 0, 4);
        else if (arg == L"--scale" && more)
            rtx_scale = std::clamp(_wtoi(argv[++i]), 1, 8);
        else if (arg == L"--compare" && more)
            compare = argv[++i];
        else if (arg == L"--image" && more)
            images.push_back(argv[++i]);
        else if (arg == L"--tile" && more)
            tile = std::max(0, _wtoi(argv[++i]));
        else if (arg == L"--out" && more)
            out = argv[++i];
        else if (arg == L"--convert" && i + 2 < argc) {
            convert_in = argv[++i];
            convert_out = argv[++i];
        } else if (arg == L"--sizes" && more) {
            sizes.clear();
            for (const wchar_t* c = argv[++i]; *c;) {
                sizes.push_back(_wtoi(c));
                while (*c && *c != L',')
                    ++c;
                if (*c == L',')
                    ++c;
            }
        }
        all = all || arg == L"--all";
        cost = cost || arg == L"--cost";
    }

    bool ok = true;
    if (!convert_in.empty()) {
        std::map<std::string, emulation::win32::TorchTensor> weights;
        std::string error, onnx;
        emulation::win32::UpscalerArchitecture architecture;
        const double start = Now();
        if (!emulation::win32::ReadTorchStateDict(convert_in, &weights, &error) ||
            !emulation::win32::ExportUpscalerOnnx(weights, &onnx, &architecture, &error)) {
            printf("%ls: %s\n", convert_in.c_str(), error.c_str());
            return 1;
        }
        size_t parameters = 0;
        for (const auto& weight : weights)
            parameters += weight.second.data.size();
        FILE* f = _wfopen(convert_out.c_str(), L"wb");
        if (f == nullptr || fwrite(onnx.data(), 1, onnx.size(), f) != onnx.size()) {
            printf("could not write %ls\n", convert_out.c_str());
            if (f != nullptr)
                fclose(f);
            return 1;
        }
        fclose(f);
        printf("%ls: %s, %dx, %d features, %d blocks, %zu tensors, %.2fM parameters; %zu bytes of "
               "ONNX in %.0f ms\n",
               convert_in.c_str(), architecture.name.c_str(), architecture.scale,
               architecture.features, architecture.blocks, weights.size(), parameters / 1e6,
               onnx.size(), Now() - start);
        if (models.empty())
            return 0;
    }

    printf("ONNX Runtime from %ls\n", folder.c_str());
    std::string error;
    if (!models.empty() || !compare.empty() || rtx_quality < 0) {
        if (!emulation::win32::OnnxRuntime::Get().Load(folder, &error)) {
            printf("Not loaded: %s\n", error.c_str());
            return 1;
        }
        printf("Loaded ONNX Runtime %s with DirectML, every DLL signed by Microsoft.\n",
               emulation::win32::OnnxRuntime::Get().version().c_str());
    }
    printf("RTX Video: %s.\n", emulation::win32::RtxVideoUpscaler::Built()
                                   ? "built in"
                                   : "not built in (no SDK in Temp\\rtxvideo when built)");
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    const std::vector<emulation::win32::GraphicsAdapter> adapters = emulation::win32::EnumerateGraphicsAdapters();
    bool tested = false;
    for (size_t i = 0; i < adapters.size(); ++i) {
        const emulation::win32::GraphicsAdapter& adapter = adapters[i];
        printf("\n[%zu] %s, %llu MB\n", i, adapter.name.c_str(),
               static_cast<unsigned long long>(adapter.video_memory >> 20));
        const bool nvidia = adapter.name.find("NVIDIA") != std::string::npos;
        const bool asked = card >= 0 ? static_cast<int>(i) == card : (all || !nvidia);
        if (!asked) {
            printf("    not asked about (--all, or --card %zu)\n", i);
            continue;
        }
        if ((models.empty() && rtx_quality < 0) || tested)
            continue;
        tested = true;
        ComPtr<IDXGIAdapter1> dxgi = emulation::win32::OpenAdapter(adapter.luid);
        ComPtr<ID3D12Device> device;
        if (!dxgi || FAILED(D3D12CreateDevice(dxgi.Get(), D3D_FEATURE_LEVEL_11_0,
                                              IID_PPV_ARGS(&device)))) {
            printf("    no Direct3D 12 device\n");
            ok = false;
            continue;
        }
        // What each backend is put through; its pictures go into a folder of its own under --out.
        auto exercise = [&](emulation::win32::TextureUpscaler& upscaler, const std::wstring& name) {
            if (!compare.empty()) {
                std::unique_ptr<emulation::win32::OnnxUpscaler> other =
                    emulation::win32::OnnxUpscaler::Create(compare, device.Get(), &error);
                if (!other) {
                    printf("    %ls: %s\n", compare.c_str(), error.c_str());
                    ok = false;
                } else {
                    printf("    against %ls:\n", compare.c_str());
                    ok = Compare(upscaler, *other) && ok;
                }
            }
            if (cost)
                ok = Cost(upscaler, sizes, runs) && ok;
            if (!images.empty()) {
                const std::wstring folder_for = out + L"\\" + name;
                CreateDirectoryW(folder_for.c_str(), nullptr);
                for (const std::wstring& image : images)
                    ok = Upscale(upscaler, image, folder_for, pad, tile) && ok;
            }
        };
        // Every backend on the one device: a run makes one device on a card, however many.
        for (const std::wstring& model : models) {
            const double start = Now();
            std::unique_ptr<emulation::win32::OnnxUpscaler> upscaler =
                emulation::win32::OnnxUpscaler::Create(model, device.Get(), &error);
            if (!upscaler) {
                printf("    %ls: %s\n", model.c_str(), error.c_str());
                ok = false;
                continue;
            }
            printf("    %ls: loaded in %.0f ms; %s\n", model.c_str(), Now() - start,
                   upscaler->description().c_str());
            std::wstring name = model.substr(model.find_last_of(L"\\/") == std::wstring::npos
                                                 ? 0
                                                 : model.find_last_of(L"\\/") + 1);
            exercise(*upscaler, name.substr(0, name.find_last_of(L'.')));
        }
        if (rtx_quality >= 0) {
            const double start = Now();
            std::unique_ptr<emulation::win32::RtxVideoUpscaler> upscaler = emulation::win32::RtxVideoUpscaler::Create(
                device.Get(), folder, rtx_scale, rtx_quality, &error);
            if (!upscaler) {
                printf("    RTX Video: %s\n", error.c_str());
                ok = false;
                continue;
            }
            printf("    RTX Video: made in %.0f ms; %s\n", Now() - start,
                   upscaler->description().c_str());
            exercise(*upscaler, L"rtxvideo-q" + std::to_wstring(rtx_quality) + L"-" +
                                    std::to_wstring(rtx_scale) + L"x");
        }
    }
    return ok ? 0 : 1;
}
