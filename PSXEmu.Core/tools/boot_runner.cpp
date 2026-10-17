// boot_runner - headless boot harness for the PSX core.
//
//   boot_runner <bios.bin> [options]
//
//     --disc <path>      mount a disc: a .cue, an image file, or a drive letter
//     --boot-disc        read SYSTEM.CNF from the disc and start its executable
//     --auto-boot        let the BIOS run, then take over at pc=80030000
//     --exe <file.exe>   side-load a PS-EXE. Combined with --auto-boot, the
//                        BIOS runs for real first and the side-load happens
//                        when it reports its kernel initialised (POST 7)
//                        instead of immediately - the BIOS clearing BEV and
//                        Isolate Cache and setting up the kernel is what a
//                        standalone test program can assume already
//                        happened. Without --auto-boot the side-load is
//                        immediate, before the BIOS has run at all - for a
//                        synthetic snippet that sets up everything it needs
//                        itself and does not want the intro run first.
//     --frames <n>       run for n frames and stop      (default 300)
//     --ppm <file>       write the final frame as a PPM
//     --ppm-every <n>    ...and the native picture every n frames on the way, each as
//                        <file>_<frame> with the same extension - for watching a long run
//     --vram <file>      write the whole of VRAM as a PPM
//     --wav <file>       write everything the SPU produced as a 44100 Hz WAV
//     --trace-spu <file> log every voice key-on and the registers behind it
//     --spu-ram <file>   dump the 512 KB of sound RAM at the end of the run
//     --trace <n>        print the first n instructions as they execute
//     --trace-skip <n>   start tracing only after n instructions
//     --trace-at <hex>[:<n>]  start tracing when the pc reaches an address,
//                        skipping the first n visits (decimal) if given
//     --trace-irq [n]    start tracing at the nth hardware interrupt (default 1)
//     --hot <n>          print the n most-executed addresses at the end
//     --idle-range <lo>:<hi>[,<lo>:<hi>]  the pc range of a game's idle loop, and of a
//                        routine it calls: a line each time the pc has stayed there, with the
//                        scanlines of work before it and the frame and scanline it began at
//     --frame-log <n>    a line every n frames: checksum, size, and running counts of the
//                        game's own frames (flips), MDEC blocks, CD sectors and GP0 words
//     --dis <hex>:<n>    disassemble n instructions of RAM from an address
//     --watch-vram x,y,w,h   report which GP0 command wrote into a VRAM area
//     --justifier x,y    port 1 holds a Konami Justifier aimed at x,y (fractions of the
//                        picture); --press then pulls its trigger with square, Back with
//                        cross and Start with start, the pad buttons psx-spx gives them
//     --press b@f[+h]    press a button at frame f, holding h frames
//                        e.g. --press start@1800 --press down+cross@2000+8
//     --movie <file.bk2> play a BizHawk input movie into the ports from the first frame
//                        (emu/movie.h): the pads it names plugged in, a new memory card in
//                        each slot it has one in unless --card1/--card2 gives one, and its
//                        resets and disc changes carried out. In place of --press
//     --movie-offset <n> ...waiting n frames before the movie's first - or, negative,
//                        leaving that many of its frames out
//     --movie-disc <path>  ...the game's next disc, for a movie that changes discs;
//                        repeatable, in order, with --disc the first
//     --cd-mechanical    charge the CD-ROM for spin-up, seek distance and
//                        rotational latency instead of moving the head for
//                        free (EmuConfig::cdrom_mechanical_timing). Every
//                        disc-dependent baseline moves with this on, so it is
//                        off by default here exactly as it is in the front end
//     --cd-mednafen      time the drive as Mednafen's is instead - its seeks, pauses and
//                        command delays (EmuConfig::cdrom_mednafen_timing) - which is what a
//                        BizHawk movie's loads were recorded under; --movie turns it on
//     --exact-timing     devices run to each event rather than in 32-cycle batches
//     --dma-stops-cpu    the CPU waits while a DMA holds the bus
//     --measured-bus     8/16-bit bus costs fitted to a console, not psx-spx's formula
//     --write-queue      stores go through a 4-deep write queue (interpreter only)
//                        - the four EmuConfig timing models, each off by default
//     --load-state <f>   resume from a save state instead of booting - skips
//                        --disc/--boot-disc/--auto-boot/--exe entirely
//     --save-state <f>   write a save state after the run finishes
//     --bench-state <n>  at the very end, time a save state kept in memory - saves, loads, the
//                        frame after a load, run-ahead and what a state a frame later has
//                        changed - each over n tries (Docs/Rewind-and-Run-Ahead-Plan.md)
//     --memory-state-every <n>  every n frames, save a state in memory (cards and all) and
//                        load it straight back: the run must be exactly the run without it
//     --rewind-test <n>  keep a state every frame in rewind's store; at the end step back n of
//                        them, check it is the frame n before byte for byte, and play n frames
//                        on from it to the run's own end (Docs/Rewind-and-Run-Ahead-Plan.md)
//     --run-ahead <n>    after every frame, run n frames ahead on its input, take their last
//                        picture as the one shown, and go back: everything the run reports
//                        is still the real frames' (Docs/Rewind-and-Run-Ahead-Plan.md)
//     --shown-log <f>    a line per frame to f: the frame, and the checksum and non-black
//                        pixels of the picture shown - with --run-ahead, the one n frames ahead
//     --hw-raster        draw with the Direct3D 11 hardware rasteriser rather than the software
//                        one (Docs/Hardware-Renderer-Plan.md)
//     --d3d12            ...the Direct3D 12 one instead; on its own it means --hw-raster too
//     --one-device       ...with --shared-picture, drawing on a Direct3D 12 device made here, as
//                        it draws on the Direct3D 12 renderer's own (graphics/shared_device.h),
//                        and handing its pictures over as they are; implies --d3d12
//     --warp             ...on WARP, Windows' software Direct3D: no graphics card needed, and
//                        the same pictures every run
//     --scale <n>        ...at n times the console's resolution (1-8); --ppm writes that
//                        picture, and everything measured stays the native one
//     --no-true-color    ...at the console's colours exactly, dithered: then an upscaled run's
//                        checksums are the software rasteriser's too
//     --shown-only       ...keeping only the sharper picture, as the front end does - for
//                        timing that path; the checksums are then of a stale native picture
//     --shared-picture   ...handing that picture over on the card (render/shared_picture.h), as
//                        to a presenter that can take it there, rather than reading it back
//     --planes           ...keeping the plane beside VRAM that DLSS will use - depth and
//                        motion - and handing it over with a shared picture (Docs/DLSS-Plan.md)
//     --view depth|motion  ...showing that plane in place of the picture, as View > Depth
//                        and Motion do: above 1x, --ppm writes it
//     --motion           ...with the plane kept, the warp check: each new picture compared with
//                        the last one moved by its motion and left still (Docs/DLSS-Plan.md,
//                        phase 2) - slow, since it reads each picture back; with --pgxp
//     --motion-key model|order|address  ...what the GTE finds a vertex in the last picture by
//                        - its model's coordinates (the default), its place in the picture, or
//                        where in RAM it was loaded from
//     --motion-log       ...a line for each new picture: vertices found, and whether afresh
//     --motion-reach n   ...how far, in screen pixels, a vertex may move and still be found
//     --jitter n         ...with the plane kept, triangles jittered through n offsets of a
//                        Halton sequence, a new one each new picture (Docs/DLSS-Plan.md,
//                        phase 3); the report's pictures and checksums are then jittered too
//     --textures         which texture each textured draw used, by a name that is the same
//                        every run - the CPU-to-VRAM upload it reads and its palette - and
//                        why for the rest; the share of textured pixels named
//                        (render/texture_tracker.h, Docs/Texture-Upscaling-Plan.md, phase 1)
//     --texture-dump <dir>  ...writing each texture named the first time as a PNG there
//     --texture-log <n>  ...a line for the first n draws of each reason a texture is not named:
//                        what it reads, and the uploads there
//     --texture-trace x,y,w,h  ...a line for everything that happens to an upload touching
//                        that rectangle of VRAM: recorded, joined, patched, uploaded over, drawn over
//     --texture-pack <dir>  ...and drawn with the replacements in that folder, as the front end's
//                        Use Replacement Textures does - with the hardware rasteriser - each image
//                        read the first time a job wants it, so the pictures do not depend on a
//                        loader's speed (render/texture_pack.h, phase 2)
//     --texture-console  ...replacements cut to 15 bits and drawn as the texels they replace:
//                        the identity check's debug switch
//     --texture-pack-scale <k>  ...each replacement made k times as large as it is read,
//                        nearest: a dump at 1x drawn as a pack at 2x-8x, for the identity check
//     --texture-ai <model.onnx | rtx-video>  ...and every texture drawn upscaled by the model as
//                        it is read - or by RTX Video Super Resolution at --texture-ai-quality
//                        1-4 (4), nvngx_vsr.dll beside this program
//                        (graphics/upscale/texture/ai_textures.h, phase 3), on the hardware
//                        rasteriser's card, into --texture-ai-out <dir> (needed), at
//                        --texture-ai-scale 2 or 4 (4); the upscaler held to --texture-ai-share
//                        of the time (1: all of it), --texture-ai-grid n tiles to a call each
//                        way (3; 1 is one tile, as the front end's on the game's own card),
//                        ONNX Runtime from --texture-ai-runtime
//                        <dir> (this program's folder). The report waits for the queue at the end.
//     --upscale-folder <dumps>  ...with --texture-ai and its out folder, and no disc: every dump in
//                        the folder upscaled - Upscale All - and nothing run
//     --gpu <name>       ...drawing on the graphics card with <name> in its name (any case),
//                        as Settings > Video > Graphics Card does; the report names the card
//     --list-gpus        list the graphics cards and their LUIDs, and exit
//     --quiet            suppress the per-100-frame progress lines
//     --break <hex>[,<hex>...]  stop before these addresses run (debug/debugger.h), print the
//                        registers and the code around the pc, and carry on; repeatable
//     --watchpoint <hex>[:<len>][:r|w|rw]  stop after a read or write of a range - by the CPU,
//                        or a DMA channel's write - print it as a break does, and carry on;
//                        four bytes and writes unless said otherwise; repeatable
//     --track-calls      keep the debugger's approximate call stack all run, and print it
//     --break-print <n>  how many halts to print (default 20); the rest are only counted
//
// Takes no window, no input and no audio device, so it can be run from a shell
// and diffed. Prints a framebuffer checksum, which is the cheap regression
// signal: a change to the CPU, timing or the renderer that moves the checksum
// for a fixed frame number needs explaining.

#include "psx/psx.h"
#include "emu/recompiler_bridge.h"
#include "debug/disasm.h"
#include "emu/movie.h"
#include "emu/rewind_buffer.h"
#include "render/texture_pack.h"
#include "render/texture_tracker.h"
#ifdef PSXEMU_HW_RASTER
#include "graphics/adapters.h"
#include "graphics/hw_raster/hardware_raster.h"
#include "graphics/upscale/texture/ai_textures.h"
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#endif

#include "zlib.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <chrono>
#include <ctime>
#include <unordered_map>
#include <vector>

using emulation::psx::System;
using emulation::psx::TrapCounter;

namespace {

// A scheduled button press. Everything measured in this project so far has
// come from a machine whose controller was connected and permanently idle, so
// nothing past a title screen was reachable at all.
struct Press {
  uint16_t buttons;
  int frame;        // the frame it goes down on
  int hold;         // how many frames it stays down
};

struct ButtonName {
  const char* name;
  uint16_t mask;
};

const ButtonName kButtonNames[] = {
  { "up",       emulation::psx::Sio::kUp },
  { "down",     emulation::psx::Sio::kDown },
  { "left",     emulation::psx::Sio::kLeft },
  { "right",    emulation::psx::Sio::kRight },
  { "cross",    emulation::psx::Sio::kCross },
  { "circle",   emulation::psx::Sio::kCircle },
  { "square",   emulation::psx::Sio::kSquare },
  { "triangle", emulation::psx::Sio::kTriangle },
  { "l1",       emulation::psx::Sio::kL1 },
  { "l2",       emulation::psx::Sio::kL2 },
  { "r1",       emulation::psx::Sio::kR1 },
  { "r2",       emulation::psx::Sio::kR2 },
  { "start",    emulation::psx::Sio::kStart },
  { "select",   emulation::psx::Sio::kSelect },
};

uint16_t ButtonMask(const std::string& name) {
  for (const ButtonName& button : kButtonNames) {
    if (name == button.name)
      return button.mask;
  }
  return 0;
}

// Parses "start@1800" or "start@1800+6", or several buttons at once as
// "start+cross@1800". A press with no hold gets a default long enough that
// software which debounces its input still sees it - a single frame is very
// often missed.
bool ParsePress(const char* text, Press* out) {
  const std::string input = text;
  const size_t at = input.find('@');
  if (at == std::string::npos)
    return false;

  const std::string when = input.substr(at + 1);
  const size_t plus = when.find('+');
  out->frame = atoi(when.substr(0, plus).c_str());
  out->hold = (plus == std::string::npos)
                  ? 6
                  : atoi(when.substr(plus + 1).c_str());
  if (out->hold < 1)
    out->hold = 1;

  // The button half may name several, joined by '+'.
  out->buttons = 0;
  std::string names = input.substr(0, at);
  size_t start = 0;
  while (start <= names.size()) {
    const size_t sep = names.find('+', start);
    const std::string one = names.substr(
        start, (sep == std::string::npos) ? std::string::npos : sep - start);
    const uint16_t mask = ButtonMask(one);
    if (mask == 0)
      return false;
    out->buttons |= mask;
    if (sep == std::string::npos)
      break;
    start = sep + 1;
  }
  return out->buttons != 0 && out->frame >= 0;
}

// Which buttons are down on a given frame.
uint16_t ButtonsOnFrame(const std::vector<Press>& presses, int frame) {
  uint16_t buttons = 0;
  for (size_t i = 0; i < presses.size(); ++i) {
    if (frame >= presses[i].frame &&
        frame < presses[i].frame + presses[i].hold) {
      buttons |= presses[i].buttons;
    }
  }
  return buttons;
}

struct Options {
  const char* bios;
  const char* exe;
  const char* disc;
  const char* ppm;
  const char* vram;
  const char* wav;
  const char* trace_spu;
  const char* spu_ram;
  int frames;
  // A disc put in after the machine is already running, the way someone
  // reaches the CD player: boot with the tray empty, pick CD PLAYER, then
  // insert. The disc is a different one from --disc, which is mounted at
  // boot and makes the BIOS start the game on it instead.
  const char* insert;
  int insert_at;
  int trace;
  uint64_t trace_skip;
  uint32_t trace_at;
  bool trace_at_set;
  // How many times to let the address go past before arming. A routine that
  // runs hundreds of times is usually only interesting on a late call - the
  // first one is the one that worked.
  uint64_t trace_at_skip;
  uint64_t trace_irq;   // which interrupt to trace from; 0 means none
  const char* dis;
  const char* watch;
  uint32_t watch_ram;
  int hot;
  // --idle-range <lo>:<hi>: the pc range of a game's idle loop. Each time the pc comes into it
  // or leaves it is a line - the frame, the scanline, the cycle - which is when the game's work
  // for a frame ended and when it started again. idle_from skips the frames before it.
  uint32_t idle_lo;
  uint32_t idle_hi;
  uint32_t idle2_lo;   // ...and a second range, after a comma: a routine the loop calls
  uint32_t idle2_hi;
  bool boot_disc;
  bool auto_boot;
  bool cd_mechanical;
  bool cd_mednafen;   // --cd-mednafen: the drive timed as Mednafen times it (a BizHawk movie)
  bool quiet;
  bool recompiler;
  bool gte_x64;              // --gte-x64: with the recompiler, GTE commands emitted as x64 (the default)
  bool gte_cpp;              // --no-gte-x64: the C++ for each command instead
  int gte_x64_toggle;        // --gte-x64-toggle n: switch that every n frames
  bool gpu_thread;
  bool gpu_transfer_timing;
  bool icache_timing;
  bool exact_event_timing;
  bool dma_stops_cpu;
  bool measured_bus_timing;
  bool write_queue_timing;
  int recompiler_toggle;
  bool recompiler_diff;
  bool hle;                 // --hle: the HLE BIOS (psx/hle_bios.h) rather than a dump
  const char* hle_trace;    // --call-trace <file>: every kernel call, HLE or a dump
  const char* time_calls;   // --time-calls <csv>: every kernel call and the cycles it took
  const char* cards[2];     // --card1/--card2 <file>: a memory card in each slot
  // --break: execute breakpoints (debug/debugger.h). Each hit prints the registers and the code
  // around the pc, up to break_print of them, then carries on.
  std::vector<uint32_t> breaks;
  // --watchpoint: stop after a read or write of a range, by the CPU or a DMA channel. Printed
  // and counted with the breaks.
  struct Watch {
    uint32_t address;
    uint32_t length;
    bool read;
    bool write;
  };
  std::vector<Watch> watches;
  // --track-calls: keep the debugger's approximate call stack for the whole run.
  bool track_calls;
  int break_print;
  int frame_log;
  int ppm_every;   // --ppm-every: a picture every this many frames, beside --ppm's
  float volume;
  std::vector<Press> presses;
  // --justifier x,y: port 1 holds a Konami Justifier aimed there (fractions of the picture);
  // negative for none.
  float justifier_x = -1.0f;
  float justifier_y = -1.0f;
  bool justifier = false;
  // --movie: a BizHawk .bk2 played into the ports (emu/movie.h), --movie-offset frames late,
  // with --movie-disc naming the discs after --disc for a movie that changes them.
  const char* movie;
  int movie_offset;
  std::vector<const char*> movie_discs;
  // Applied once, right after the BIOS is loaded and before any other setup
  // (--boot-disc/--auto-boot/--exe are skipped when this is set - the state
  // load overwrites everything they would have done anyway). Lets a run
  // resume from a saved point instead of booting from scratch, which is how
  // Docs/Save-States-Plan.md's own round-trip test is actually driven: save
  // at the end of one run, load at the start of another, and diff the
  // pictures --frames further on in each.
  const char* load_state;
  // Applied once, after the frame loop finishes, alongside --ppm/--vram.
  const char* save_state;
  int bench_state;   // --bench-state n: tries per measurement, 0 for none
  int memory_state_every;   // --memory-state-every n: a memory state round trip every n frames
  int run_ahead;            // --run-ahead n: frames run ahead of the real one, 0 for none
  int rewind_test;          // --rewind-test n: rewind's store, stepped back n states at the end
  const char* shown_log;    // --shown-log f: the picture shown, frame by frame
  // --hw-raster: draw with the Direct3D 11 rasteriser the front end has (12 with --d3d12), on
  // the graphics card or, with --warp, on WARP - Windows' software Direct3D, which needs no card and gives the
  // same pictures every run. Only in a build that compiles it in (PSXEMU_HW_RASTER).
  bool hw_raster;
  bool warp;
  bool d3d12;   // --d3d12: the Direct3D 12 rasteriser rather than 11 (implies --hw-raster)
  bool one_device;   // --one-device: on a device of this program's, as on the renderer's
  // --scale n: its internal resolution; --no-true-color: at it, the console's colours exactly,
  // dithering and all - which keeps even the upscaled run's checksums the software ones.
  int scale;
  bool shown_only;   // --shown-only: the front end's path, for timing it
  bool shared_picture;   // --shared-picture: the picture left on the card
  const char* gpu;       // --gpu <part of a card's name>: which card the rasteriser draws on
  // --pgxp [--pgxp-culling] [--no-pgxp-textures]: PGXP's precise vertices for the hardware
  // rasteriser (enhance/pgxp.h).
  bool pgxp, pgxp_culling, pgxp_textures;
  bool true_color;
  // --planes: the plane beside VRAM kept; --view: shown in place of the picture.
  bool planes;
  emulation::psx::PlaneView view;
  // --motion: the warp check; --motion-log: a line per new picture; --motion-key: how the GTE
  // finds a vertex in the last picture.
  bool motion, motion_log;
  emulation::psx::Gte::MotionKey motion_key;
  float motion_reach;   // --motion-reach: VertexMotion's, in screen pixels; 0 leaves it
  int jitter;           // --jitter: the sequence's length, 0 for none
  // --textures: which texture each draw used (render/texture_tracker.h); --texture-dump: each
  // new one written as a PNG into this folder.
  bool textures;
  const char* texture_dump;
  int texture_log;   // --texture-log: lines for the first n of each reason a texture is not named
  int texture_trace[4];   // --texture-trace x,y,w,h: what happens to uploads there; w 0 for none
  const char* texture_pack;   // --texture-pack: replacements from this folder
  bool texture_console;       // --texture-console: drawn as the console's texels
  int texture_pack_scale;     // --texture-pack-scale: each replacement made this much larger
  const char* texture_ai;           // --texture-ai: the model
  const char* texture_ai_out;       // --texture-ai-out: where its images go
  const char* texture_ai_runtime;   // --texture-ai-runtime: ONNX Runtime's folder
  int texture_ai_scale;             // --texture-ai-scale: 2 or 4
  double texture_ai_share;          // --texture-ai-share: of the time, 0-1
  int texture_ai_grid;              // --texture-ai-grid: tiles to a call each way, 1-3
  int texture_ai_quality;           // --texture-ai-quality: RTX Video's, 1-4
  const char* upscale_folder;       // --upscale-folder: dumps to upscale, nothing run
};

// FNV-1a over the visible framebuffer. Small, order-sensitive, and good enough
// to tell "the same picture" from "very nearly the same picture".
uint64_t Checksum(const uint32_t* pixels, int count) {
  uint64_t hash = 1469598103934665603ull;
  for (int i = 0; i < count; ++i) {
    const uint32_t pixel = pixels[i] & 0x00FFFFFF;
    for (int byte = 0; byte < 4; ++byte) {
      hash ^= (pixel >> (byte * 8)) & 0xFF;
      hash *= 1099511628211ull;
    }
  }
  return hash;
}
// Writes a 16-bit stereo WAV at the mixer's own rate. Audio is the one output
// of this machine that cannot be checked by looking at it, and a file that can
// be played settles arguments that a peak level does not.
bool WriteWav(const char* path, const std::vector<int16_t>& samples) {
  FILE* fp = fopen(path, "wb");
  if (fp == nullptr)
    return false;
  const uint32_t data_bytes = static_cast<uint32_t>(samples.size() * 2);
  const uint32_t rate = emulation::psx::Spu::kSampleRate;
  const uint16_t channels = 2;
  const uint32_t byte_rate = rate * channels * 2;
  const uint16_t block_align = channels * 2;
  const uint16_t bits = 16;
  const uint32_t riff_size = 36 + data_bytes;
  const uint32_t fmt_size = 16;
  const uint16_t format = 1;

  fwrite("RIFF", 1, 4, fp);
  fwrite(&riff_size, 4, 1, fp);
  fwrite("WAVEfmt ", 1, 8, fp);
  fwrite(&fmt_size, 4, 1, fp);
  fwrite(&format, 2, 1, fp);
  fwrite(&channels, 2, 1, fp);
  fwrite(&rate, 4, 1, fp);
  fwrite(&byte_rate, 4, 1, fp);
  fwrite(&block_align, 2, 1, fp);
  fwrite(&bits, 2, 1, fp);
  fwrite("data", 1, 4, fp);
  fwrite(&data_bytes, 4, 1, fp);
  if (!samples.empty())
    fwrite(&samples[0], 2, samples.size(), fp);
  fclose(fp);
  return true;
}


bool WritePpm(const char* path, const uint32_t* pixels, int width, int height) {
  FILE* fp = fopen(path, "wb");
  if (fp == nullptr)
    return false;
  fprintf(fp, "P6\n%d %d\n255\n", width, height);
  for (int i = 0; i < width * height; ++i) {
    const uint32_t pixel = pixels[i];
    const unsigned char rgb[3] = {
      static_cast<unsigned char>((pixel >> 16) & 0xFF),
      static_cast<unsigned char>((pixel >> 8) & 0xFF),
      static_cast<unsigned char>(pixel & 0xFF),
    };
    fwrite(rgb, 1, 3, fp);
  }
  fclose(fp);
  return true;
}

// A frame as a .bmp, because a .ppm cannot be looked at without converting it
// first and half of working out where the machine is stuck is looking at the
// screen. Uncompressed 24-bit, bottom-up, which is the format at its simplest.
bool WriteBmp(const char* path, const uint32_t* pixels, int width, int height) {
  FILE* fp = fopen(path, "wb");
  if (fp == nullptr)
    return false;
  const int row_bytes = width * 3;
  const int padding = (4 - (row_bytes % 4)) % 4;
  const uint32_t image_bytes =
      static_cast<uint32_t>((row_bytes + padding) * height);
  const uint32_t offset = 14 + 40;
  const uint32_t file_bytes = offset + image_bytes;

  uint8_t header[54];
  memset(header, 0, sizeof(header));
  header[0] = 'B'; header[1] = 'M';
  memcpy(&header[2], &file_bytes, 4);
  memcpy(&header[10], &offset, 4);
  const uint32_t info_size = 40;
  memcpy(&header[14], &info_size, 4);
  const int32_t w = width;
  const int32_t h = height;               // positive: rows run bottom to top
  memcpy(&header[18], &w, 4);
  memcpy(&header[22], &h, 4);
  const uint16_t planes = 1, bits = 24;
  memcpy(&header[26], &planes, 2);
  memcpy(&header[28], &bits, 2);
  memcpy(&header[34], &image_bytes, 4);
  fwrite(header, 1, sizeof(header), fp);

  const uint8_t pad[3] = { 0, 0, 0 };
  for (int y = height - 1; y >= 0; --y) {
    for (int x = 0; x < width; ++x) {
      const uint32_t pixel = pixels[y * width + x];
      const uint8_t bgr[3] = {
        static_cast<uint8_t>(pixel & 0xFF),
        static_cast<uint8_t>((pixel >> 8) & 0xFF),
        static_cast<uint8_t>((pixel >> 16) & 0xFF),
      };
      fwrite(bgr, 1, 3, fp);
    }
    if (padding > 0)
      fwrite(pad, 1, padding, fp);
  }
  fclose(fp);
  return true;
}

// A frame as a .png, because half of working out where a machine is stuck is
// looking at the screen, and a .ppm cannot be looked at without converting it
// first.
//
// No compression library is involved: a zlib stream is allowed to consist of
// stored - literally uncompressed - deflate blocks, which needs nothing but a
// length and its complement in front of each one. The file is bigger than it
// would otherwise be and every viewer reads it.
namespace png {

uint32_t Crc32(const uint8_t* data, size_t length, uint32_t crc) {
  static uint32_t table[256];
  static bool built = false;
  if (!built) {
    for (uint32_t n = 0; n < 256; ++n) {
      uint32_t c = n;
      for (int k = 0; k < 8; ++k)
        c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      table[n] = c;
    }
    built = true;
  }
  crc = ~crc;
  for (size_t i = 0; i < length; ++i)
    crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

void PutBigEndian(std::vector<uint8_t>& out, uint32_t value) {
  out.push_back(static_cast<uint8_t>(value >> 24));
  out.push_back(static_cast<uint8_t>(value >> 16));
  out.push_back(static_cast<uint8_t>(value >> 8));
  out.push_back(static_cast<uint8_t>(value));
}

void Chunk(std::vector<uint8_t>& out, const char* type,
           const std::vector<uint8_t>& data) {
  PutBigEndian(out, static_cast<uint32_t>(data.size()));
  const size_t crc_start = out.size();
  out.insert(out.end(), type, type + 4);
  out.insert(out.end(), data.begin(), data.end());
  const uint32_t crc =
      Crc32(&out[crc_start], out.size() - crc_start, 0);
  PutBigEndian(out, crc);
}

}  // namespace png

bool WritePng(const char* path, const uint32_t* pixels, int width,
              int height) {
  if (width <= 0 || height <= 0)
    return false;

  // Raw scanlines, each behind a zero filter byte.
  std::vector<uint8_t> raw;
  raw.reserve(static_cast<size_t>(height) * (1 + width * 3));
  for (int y = 0; y < height; ++y) {
    raw.push_back(0);
    for (int x = 0; x < width; ++x) {
      const uint32_t pixel = pixels[y * width + x];
      raw.push_back(static_cast<uint8_t>((pixel >> 16) & 0xFF));
      raw.push_back(static_cast<uint8_t>((pixel >> 8) & 0xFF));
      raw.push_back(static_cast<uint8_t>(pixel & 0xFF));
    }
  }

  // zlib: header, stored deflate blocks, adler32 of the raw bytes.
  std::vector<uint8_t> z;
  z.push_back(0x78);
  z.push_back(0x01);
  size_t offset = 0;
  while (offset < raw.size()) {
    const size_t block = (raw.size() - offset > 65535)
                             ? 65535 : (raw.size() - offset);
    const bool last = (offset + block) >= raw.size();
    z.push_back(last ? 1 : 0);
    z.push_back(static_cast<uint8_t>(block & 0xFF));
    z.push_back(static_cast<uint8_t>(block >> 8));
    z.push_back(static_cast<uint8_t>(~block & 0xFF));
    z.push_back(static_cast<uint8_t>((~block >> 8) & 0xFF));
    z.insert(z.end(), raw.begin() + offset, raw.begin() + offset + block);
    offset += block;
  }
  uint32_t a = 1, b = 0;
  for (size_t i = 0; i < raw.size(); ++i) {
    a = (a + raw[i]) % 65521;
    b = (b + a) % 65521;
  }
  png::PutBigEndian(z, (b << 16) | a);

  std::vector<uint8_t> out;
  static const uint8_t kSignature[8] = { 0x89, 'P', 'N', 'G',
                                         0x0D, 0x0A, 0x1A, 0x0A };
  out.insert(out.end(), kSignature, kSignature + 8);

  std::vector<uint8_t> ihdr;
  png::PutBigEndian(ihdr, static_cast<uint32_t>(width));
  png::PutBigEndian(ihdr, static_cast<uint32_t>(height));
  ihdr.push_back(8);    // bits per channel
  ihdr.push_back(2);    // truecolour
  ihdr.push_back(0);    // deflate
  ihdr.push_back(0);    // adaptive filtering
  ihdr.push_back(0);    // no interlace
  png::Chunk(out, "IHDR", ihdr);
  png::Chunk(out, "IDAT", z);
  png::Chunk(out, "IEND", std::vector<uint8_t>());

  FILE* fp = fopen(path, "wb");
  if (fp == nullptr)
    return false;
  const bool ok = fwrite(out.data(), 1, out.size(), fp) == out.size();
  fclose(fp);
  return ok;
}
// Writes whichever format the name asks for.
bool WriteFrame(const char* path, const uint32_t* pixels, int width,
                int height) {
  const size_t length = strlen(path);
  if (length >= 4 && _stricmp(path + length - 4, ".png") == 0)
    return WritePng(path, pixels, width, height);
  if (length >= 4 && _stricmp(path + length - 4, ".bmp") == 0)
    return WriteBmp(path, pixels, width, height);
  return WritePpm(path, pixels, width, height);
}


// How many pixels differ from black. A frame that is entirely black almost
// always means the run never got as far as drawing anything, and that is worth
// distinguishing from a frame that is merely wrong.
int NonBlackPixels(const uint32_t* pixels, int count) {
  int total = 0;
  for (int i = 0; i < count; ++i)
    if ((pixels[i] & 0x00FFFFFF) != 0)
      ++total;
  return total;
}

// Fetches the instruction word at a virtual address, straight out of the
// buffers, so tracing does not disturb the CPU it is watching.
uint32_t FetchCode(System* system, uint32_t address) {
  const uint32_t physical = address & 0x1FFFFFFF;
  uint32_t code = 0;
  if (physical <= 0x001FFFFF)
    memcpy(&code, system->ram() + (physical & 0x1FFFFC), sizeof(code));
  else if (physical >= 0x1FC00000 && physical <= 0x1FC7FFFF)
    memcpy(&code, system->bios() + (physical & 0x7FFFC), sizeof(code));
  return code;
}

// One --break hit: why it stopped, the registers, and the code around the pc with the pc marked.
void PrintBreak(System* system, uint64_t hit, uint64_t instructions) {
  const auto* ctx = system->cpu().context();
  const emulation::psx::Debugger& debugger = system->debugger();
  printf("\nbreak %llu at %08X after %llu instructions (breakpoint hits so far:",
         static_cast<unsigned long long>(hit), ctx->pc,
         static_cast<unsigned long long>(instructions));
  for (const auto& bp : debugger.breakpoints())
    printf(" %08X=%llu", bp.address, static_cast<unsigned long long>(bp.hits));
  printf(")\n");
  if (debugger.halt_reason() == emulation::psx::Debugger::HaltReason::kWatchpoint) {
    const auto& hit = debugger.watch_hit();
    char who[48];
    if (hit.dma_channel >= 0)
      snprintf(who, sizeof(who), "DMA channel %d", hit.dma_channel);
    else
      snprintf(who, sizeof(who), "the instruction at %08X", hit.pc);
    printf("  watchpoint %08X: %s %s %u bytes at %08X, value %08X%s\n", hit.watchpoint, who,
           hit.write ? "wrote" : "read", hit.size, hit.address, hit.value,
           hit.value_known ? "" : " (not peekable)");
  }
  for (int r = 0; r < 32; ++r) {
    printf("  %-4s %08X%s", emulation::psx::RegisterName(r), ctx->gp.reg[r], (r % 4 == 3) ? "\n" : "");
  }
  printf("  hi   %08X  lo   %08X  sr   %08X  cause %08X  epc %08X\n", ctx->high, ctx->low,
         ctx->ctrl.SR.raw, ctx->ctrl.Cause, ctx->ctrl.EPC);
  for (int i = -4; i <= 6; ++i) {
    const uint32_t address = ctx->pc + static_cast<uint32_t>(i * 4);
    char text[96];
    emulation::psx::Disassemble(address, FetchCode(system, address), text, sizeof(text));
    printf("  %s %08X  %08X  %s\n", i == 0 ? ">" : " ", address, FetchCode(system, address),
           text);
  }
}

bool ParseOptions(int argc, char** argv, Options* options) {
  options->bios = nullptr;
  options->exe = nullptr;
  options->disc = nullptr;
  options->ppm = nullptr;
  options->vram = nullptr;
  options->wav = nullptr;
  options->trace_spu = nullptr;
  options->spu_ram = nullptr;
  options->frames = 300;
  options->insert = nullptr;
  options->insert_at = 0;
  options->trace = 0;
  options->trace_skip = 0;
  options->trace_at = 0;
  options->trace_at_set = false;
  options->trace_at_skip = 0;
  options->trace_irq = 0;
  options->dis = nullptr;
  options->watch = nullptr;
  options->watch_ram = 0;
  options->hot = 0;
  options->idle_lo = options->idle_hi = options->idle2_lo = options->idle2_hi = 0;
  options->boot_disc = false;
  options->auto_boot = false;
  options->cd_mechanical = false;
  options->cd_mednafen = false;
  options->quiet = false;
  // These three had no default, so they held whatever the stack did - false by luck until
  // --watchpoint added a member and moved them, and --recompiler-diff came on by itself.
  options->recompiler = false;
  options->gte_x64 = false;
  options->gte_cpp = false;
  options->gte_x64_toggle = 0;
  options->gpu_thread = false;
  options->gpu_transfer_timing = false;
  options->icache_timing = false;
  options->exact_event_timing = false;
  options->dma_stops_cpu = false;
  options->measured_bus_timing = false;
  options->write_queue_timing = false;
  options->recompiler_toggle = 0;
  options->recompiler_diff = false;
  options->hle = false;
  options->hle_trace = nullptr;
  options->time_calls = nullptr;
  options->cards[0] = options->cards[1] = nullptr;
  options->movie = nullptr;
  options->movie_offset = 0;
  options->track_calls = false;
  options->frame_log = 0;
  options->ppm_every = 0;
  options->break_print = 20;
  options->volume = -1.0f;
  options->load_state = nullptr;
  options->save_state = nullptr;
  options->bench_state = 0;
  options->memory_state_every = 0;
  options->run_ahead = 0;
  options->rewind_test = 0;
  options->shown_log = nullptr;
  options->hw_raster = false;
  options->warp = false;
  options->d3d12 = false;
  options->one_device = false;
  options->scale = 1;
  options->shown_only = false;
  options->shared_picture = false;
  options->gpu = nullptr;
  options->pgxp = false;
  options->pgxp_culling = false;
  options->pgxp_textures = true;
  options->true_color = true;
  options->planes = false;
  options->view = emulation::psx::PlaneView::kPicture;
  options->motion = false;
  options->motion_log = false;
  options->motion_key = emulation::psx::Gte::MotionKey::kModel;
  options->motion_reach = 0.0f;
  options->jitter = 0;
  options->textures = false;
  options->texture_dump = nullptr;
  options->texture_log = 0;
  memset(options->texture_trace, 0, sizeof(options->texture_trace));
  options->texture_pack = nullptr;
  options->texture_console = false;
  options->texture_pack_scale = 1;
  options->texture_ai = nullptr;
  options->texture_ai_out = nullptr;
  options->texture_ai_runtime = nullptr;
  options->texture_ai_scale = 4;
  options->texture_ai_share = 1.0;
  options->texture_ai_grid = 3;
  options->texture_ai_quality = 4;
  options->upscale_folder = nullptr;

  for (int i = 1; i < argc; ++i) {
    const char* arg = argv[i];
    if (strcmp(arg, "--exe") == 0 && i + 1 < argc) {
      options->exe = argv[++i];
    } else if (strcmp(arg, "--disc") == 0 && i + 1 < argc) {
      options->disc = argv[++i];
    } else if (strcmp(arg, "--load-state") == 0 && i + 1 < argc) {
      options->load_state = argv[++i];
    } else if (strcmp(arg, "--save-state") == 0 && i + 1 < argc) {
      options->save_state = argv[++i];
    } else if (strcmp(arg, "--bench-state") == 0 && i + 1 < argc) {
      options->bench_state = atoi(argv[++i]);
    } else if (strcmp(arg, "--rewind-test") == 0 && i + 1 < argc) {
      options->rewind_test = atoi(argv[++i]);
    } else if (strcmp(arg, "--run-ahead") == 0 && i + 1 < argc) {
      options->run_ahead = atoi(argv[++i]);
    } else if (strcmp(arg, "--shown-log") == 0 && i + 1 < argc) {
      options->shown_log = argv[++i];
    } else if (strcmp(arg, "--memory-state-every") == 0 && i + 1 < argc) {
      options->memory_state_every = atoi(argv[++i]);
    } else if (strcmp(arg, "--frames") == 0 && i + 1 < argc) {
      options->frames = atoi(argv[++i]);
    } else if (strcmp(arg, "--insert") == 0 && i + 2 < argc) {
      options->insert = argv[++i];
      options->insert_at = atoi(argv[++i]);
    } else if (strcmp(arg, "--ppm") == 0 && i + 1 < argc) {
      options->ppm = argv[++i];
    } else if (strcmp(arg, "--vram") == 0 && i + 1 < argc) {
      options->vram = argv[++i];
    } else if (strcmp(arg, "--wav") == 0 && i + 1 < argc) {
      options->wav = argv[++i];
    } else if (strcmp(arg, "--trace-spu") == 0 && i + 1 < argc) {
      options->trace_spu = argv[++i];
    } else if (strcmp(arg, "--spu-ram") == 0 && i + 1 < argc) {
      options->spu_ram = argv[++i];
    } else if (strcmp(arg, "--trace") == 0 && i + 1 < argc) {
      options->trace = atoi(argv[++i]);
    } else if (strcmp(arg, "--trace-skip") == 0 && i + 1 < argc) {
      options->trace_skip = strtoull(argv[++i], nullptr, 0);
    } else if (strcmp(arg, "--trace-at") == 0 && i + 1 < argc) {
      // <hex>[:<n>] - n is how many visits to skip first, decimal, so
      // "8002C34C:150" arms on the 151st call rather than the first.
      const char* value = argv[++i];
      options->trace_at = strtoul(value, nullptr, 16);
      options->trace_at_set = true;
      const char* colon = strchr(value, ':');
      options->trace_at_skip = (colon != nullptr) ? strtoull(colon + 1, nullptr, 10) : 0;
    } else if (strcmp(arg, "--dis") == 0 && i + 1 < argc) {
      options->dis = argv[++i];
    } else if (strcmp(arg, "--watch-vram") == 0 && i + 1 < argc) {
      options->watch = argv[++i];
    } else if (strcmp(arg, "--volume") == 0 && i + 1 < argc) {
      options->volume = static_cast<float>(atof(argv[++i]));
    } else if (strcmp(arg, "--frame-log") == 0 && i + 1 < argc) {
      options->frame_log = atoi(argv[++i]);
    } else if (strcmp(arg, "--ppm-every") == 0 && i + 1 < argc) {
      options->ppm_every = atoi(argv[++i]);
    } else if (strcmp(arg, "--justifier") == 0 && i + 1 < argc) {
      if (sscanf(argv[++i], "%f,%f", &options->justifier_x, &options->justifier_y) != 2) {
        fprintf(stderr, "bad --justifier: %s (expected x,y as fractions, e.g. 0.5,0.5)\n",
                argv[i]);
        return false;
      }
      options->justifier = true;
    } else if (strcmp(arg, "--press") == 0 && i + 1 < argc) {
      Press press;
      if (!ParsePress(argv[++i], &press)) {
        fprintf(stderr, "bad --press: %s\n", argv[i]);
        fprintf(stderr, "  expected <button>[+<button>]@<frame>[+<hold>], "
                        "e.g. start@1800 or start@1800+6\n");
        return false;
      }
      options->presses.push_back(press);
    } else if (strcmp(arg, "--movie") == 0 && i + 1 < argc) {
      options->movie = argv[++i];
    } else if (strcmp(arg, "--movie-offset") == 0 && i + 1 < argc) {
      options->movie_offset = atoi(argv[++i]);
    } else if (strcmp(arg, "--movie-disc") == 0 && i + 1 < argc) {
      options->movie_discs.push_back(argv[++i]);
    } else if (strcmp(arg, "--watch-ram") == 0 && i + 1 < argc) {
      options->watch_ram = strtoul(argv[++i], nullptr, 16);
    } else if (strcmp(arg, "--hot") == 0 && i + 1 < argc) {
      options->hot = atoi(argv[++i]);
    } else if (strcmp(arg, "--idle-range") == 0 && i + 1 < argc) {
      const char* value = argv[++i];
      options->idle_lo = strtoul(value, nullptr, 16);
      const char* colon = strchr(value, ':');
      options->idle_hi = colon != nullptr ? strtoul(colon + 1, nullptr, 16) : options->idle_lo;
      if (const char* comma = strchr(value, ',')) {
        options->idle2_lo = strtoul(comma + 1, nullptr, 16);
        const char* second = strchr(comma, ':');
        options->idle2_hi = second != nullptr ? strtoul(second + 1, nullptr, 16) : options->idle2_lo;
      }
    } else if (strcmp(arg, "--trace-irq") == 0) {
      // Optionally followed by which interrupt to stop at, so a later one can
      // be reached without wading through the first.
      options->trace_irq = 1;
      if (i + 1 < argc && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '9')
        options->trace_irq = strtoull(argv[++i], nullptr, 0);
    } else if (strcmp(arg, "--boot-disc") == 0) {
      options->boot_disc = true;
    } else if (strcmp(arg, "--auto-boot") == 0) {
      options->auto_boot = true;
    } else if (strcmp(arg, "--cd-mechanical") == 0) {
      options->cd_mechanical = true;
    } else if (strcmp(arg, "--cd-mednafen") == 0) {
      options->cd_mednafen = true;
    } else if ((strcmp(arg, "--card1") == 0 || strcmp(arg, "--card2") == 0) && i + 1 < argc) {
      options->cards[arg[6] - '1'] = argv[++i];
    } else if (strcmp(arg, "--time-calls") == 0 && i + 1 < argc) {
      options->time_calls = argv[++i];
    } else if (strcmp(arg, "--call-trace") == 0 && i + 1 < argc) {
      options->hle_trace = argv[++i];
    } else if (strcmp(arg, "--hle") == 0) {
      options->hle = true;
    } else if (strcmp(arg, "--quiet") == 0) {
      options->quiet = true;
    } else if (strcmp(arg, "--gpu-thread") == 0) {
      options->gpu_thread = true;
    } else if (strcmp(arg, "--hw-raster") == 0) {
      options->hw_raster = true;
    } else if (strcmp(arg, "--d3d12") == 0) {
      options->hw_raster = true;
      options->d3d12 = true;
    } else if (strcmp(arg, "--one-device") == 0) {
      options->hw_raster = true;
      options->d3d12 = true;
      options->one_device = true;
    } else if (strcmp(arg, "--warp") == 0) {
      options->warp = true;
    } else if (strcmp(arg, "--scale") == 0 && i + 1 < argc) {
      options->scale = atoi(argv[++i]);
      if (options->scale < 1 || options->scale > 8) {
        fprintf(stderr, "--scale wants 1 to 8\n");
        return false;
      }
    } else if (strcmp(arg, "--pgxp") == 0) {
      options->pgxp = true;
    } else if (strcmp(arg, "--pgxp-culling") == 0) {
      options->pgxp_culling = true;
    } else if (strcmp(arg, "--no-pgxp-textures") == 0) {
      options->pgxp_textures = false;
    } else if (strcmp(arg, "--shown-only") == 0) {
      options->shown_only = true;
    } else if (strcmp(arg, "--shared-picture") == 0) {
      options->shared_picture = true;
    } else if (strcmp(arg, "--planes") == 0) {
      options->planes = true;
    } else if (strcmp(arg, "--textures") == 0) {
      options->textures = true;
    } else if (strcmp(arg, "--texture-dump") == 0 && i + 1 < argc) {
      options->textures = true;
      options->texture_dump = argv[++i];
    } else if (strcmp(arg, "--texture-log") == 0 && i + 1 < argc) {
      options->textures = true;
      options->texture_log = atoi(argv[++i]);
    } else if (strcmp(arg, "--texture-trace") == 0 && i + 1 < argc) {
      options->textures = true;
      int* t = options->texture_trace;
      if (sscanf(argv[++i], "%d,%d,%d,%d", &t[0], &t[1], &t[2], &t[3]) != 4) {
        fprintf(stderr, "--texture-trace wants x,y,w,h\n");
        return false;
      }
    } else if (strcmp(arg, "--texture-pack") == 0 && i + 1 < argc) {
      options->textures = true;
      options->texture_pack = argv[++i];
    } else if (strcmp(arg, "--texture-console") == 0) {
      options->texture_console = true;
    } else if (strcmp(arg, "--texture-ai") == 0 && i + 1 < argc) {
      options->textures = true;
      options->texture_ai = argv[++i];
    } else if (strcmp(arg, "--texture-ai-out") == 0 && i + 1 < argc) {
      options->texture_ai_out = argv[++i];
    } else if (strcmp(arg, "--texture-ai-runtime") == 0 && i + 1 < argc) {
      options->texture_ai_runtime = argv[++i];
    } else if (strcmp(arg, "--texture-ai-scale") == 0 && i + 1 < argc) {
      options->texture_ai_scale = atoi(argv[++i]);
      if (options->texture_ai_scale != 2 && options->texture_ai_scale != 4) {
        fprintf(stderr, "--texture-ai-scale is 2 or 4\n");
        return false;
      }
    } else if (strcmp(arg, "--texture-ai-share") == 0 && i + 1 < argc) {
      options->texture_ai_share = atof(argv[++i]);
    } else if (strcmp(arg, "--texture-ai-grid") == 0 && i + 1 < argc) {
      options->texture_ai_grid = atoi(argv[++i]);
    } else if (strcmp(arg, "--texture-ai-quality") == 0 && i + 1 < argc) {
      options->texture_ai_quality = atoi(argv[++i]);
      if (options->texture_ai_quality < 1 || options->texture_ai_quality > 4) {
        fprintf(stderr, "--texture-ai-quality is 1 to 4\n");
        return false;
      }
    } else if (strcmp(arg, "--upscale-folder") == 0 && i + 1 < argc) {
      options->upscale_folder = argv[++i];
    } else if (strcmp(arg, "--texture-pack-scale") == 0 && i + 1 < argc) {
      options->texture_pack_scale = atoi(argv[++i]);
      if (options->texture_pack_scale < 1 || options->texture_pack_scale > 8) {
        fprintf(stderr, "--texture-pack-scale wants 1 to 8\n");
        return false;
      }
    } else if (strcmp(arg, "--motion") == 0) {
      options->motion = true;
      options->planes = true;
    } else if (strcmp(arg, "--motion-log") == 0) {
      options->motion_log = true;
      options->planes = true;
    } else if (strcmp(arg, "--motion-reach") == 0 && i + 1 < argc) {
      options->motion_reach = static_cast<float>(atof(argv[++i]));
    } else if (strcmp(arg, "--jitter") == 0 && i + 1 < argc) {
      options->jitter = atoi(argv[++i]);
      options->planes = true;
      if (options->jitter < 1) {
        fprintf(stderr, "--jitter wants the sequence's length, 1 or more\n");
        return false;
      }
    } else if (strcmp(arg, "--motion-key") == 0 && i + 1 < argc) {
      const char* key = argv[++i];
      if (strcmp(key, "model") == 0) {
        options->motion_key = emulation::psx::Gte::MotionKey::kModel;
      } else if (strcmp(key, "order") == 0) {
        options->motion_key = emulation::psx::Gte::MotionKey::kOrder;
      } else if (strcmp(key, "address") == 0) {
        options->motion_key = emulation::psx::Gte::MotionKey::kAddress;
      } else {
        fprintf(stderr, "--motion-key wants model, order or address\n");
        return false;
      }
    } else if (strcmp(arg, "--view") == 0 && i + 1 < argc) {
      const char* view = argv[++i];
      if (strcmp(view, "depth") == 0) {
        options->view = emulation::psx::PlaneView::kDepth;
      } else if (strcmp(view, "motion") == 0) {
        options->view = emulation::psx::PlaneView::kMotion;
      } else {
        fprintf(stderr, "--view wants depth or motion\n");
        return false;
      }
    } else if (strcmp(arg, "--gpu") == 0 && i + 1 < argc) {
      options->gpu = argv[++i];
    } else if (strcmp(arg, "--no-true-color") == 0) {
      options->true_color = false;
    } else if (strcmp(arg, "--gpu-transfer-timing") == 0) {
      options->gpu_transfer_timing = true;
    } else if (strcmp(arg, "--icache-timing") == 0) {
      options->icache_timing = true;
    } else if (strcmp(arg, "--exact-timing") == 0) {
      options->exact_event_timing = true;
    } else if (strcmp(arg, "--dma-stops-cpu") == 0) {
      options->dma_stops_cpu = true;
    } else if (strcmp(arg, "--measured-bus") == 0) {
      options->measured_bus_timing = true;
    } else if (strcmp(arg, "--write-queue") == 0) {
      options->write_queue_timing = true;
    } else if (strcmp(arg, "--recompiler") == 0) {
      options->recompiler = true;
    } else if (strcmp(arg, "--gte-x64") == 0) {
      options->gte_x64 = true;
    } else if (strcmp(arg, "--no-gte-x64") == 0) {
      options->gte_cpp = true;
    } else if (strcmp(arg, "--gte-x64-toggle") == 0 && i + 1 < argc) {
      options->gte_x64_toggle = atoi(argv[++i]);
    } else if (strcmp(arg, "--recompiler-toggle") == 0 && i + 1 < argc) {
      // Switch CPU every N frames, mid-run. The front end offers this from its
      // Emulation menu, and the menu cannot be clicked from a headless harness -
      // so this is how "safe to change at any time" gets tested at all.
      options->recompiler_toggle = atoi(argv[++i]);
    } else if (strcmp(arg, "--recompiler-diff") == 0) {
      options->recompiler_diff = true;
    } else if (strcmp(arg, "--break") == 0 && i + 1 < argc) {
      // One address or a comma-separated list; the option can be given more than once.
      const char* list = argv[++i];
      while (*list != '\0') {
        char* end = nullptr;
        options->breaks.push_back(static_cast<uint32_t>(strtoul(list, &end, 16)));
        list = (*end == ',') ? end + 1 : end;
        if (end == nullptr || (*end != ',' && *end != '\0'))
          break;
      }
    } else if (strcmp(arg, "--track-calls") == 0) {
      options->track_calls = true;
    } else if (strcmp(arg, "--watchpoint") == 0 && i + 1 < argc) {
      // <hex>[:<length>][:r|w|rw] - four bytes and writes unless said otherwise.
      const char* spec = argv[++i];
      char* end = nullptr;
      Options::Watch watch = { static_cast<uint32_t>(strtoul(spec, &end, 16)), 4, false, true };
      if (end != nullptr && *end == ':') {
        const char* part = end + 1;
        if (*part >= '0' && *part <= '9') {
          watch.length = static_cast<uint32_t>(strtoul(part, &end, 0));
          part = (*end == ':') ? end + 1 : end;
        }
        if (*part != '\0') {
          watch.read = strchr(part, 'r') != nullptr;
          watch.write = strchr(part, 'w') != nullptr;
        }
      }
      if (watch.length == 0 || (!watch.read && !watch.write)) {
        fprintf(stderr, "--watchpoint wants <hex>[:<length>][:r|w|rw]\n");
        return false;
      }
      options->watches.push_back(watch);
    } else if (strcmp(arg, "--break-print") == 0 && i + 1 < argc) {
      options->break_print = atoi(argv[++i]);
    } else if (arg[0] == '-') {
      fprintf(stderr, "unknown option: %s\n", arg);
      return false;
    } else if (options->bios == nullptr) {
      options->bios = arg;
    } else {
      fprintf(stderr, "unexpected argument: %s\n", arg);
      return false;
    }
  }
  // Until the address is reached, nothing should trace, so park the skip
  // count out of reach rather than at zero.
  if (options->trace_at_set || options->trace_irq != 0)
    options->trace_skip = ~0ull;
  // "hle" in place of a BIOS file is the same as --hle.
  if (options->bios != nullptr && strcmp(options->bios, "hle") == 0) {
    options->hle = true;
    options->bios = "";
  }
  if (options->hle && options->bios == nullptr)
    options->bios = "";
  return options->bios != nullptr;
}

// ---------------------------------------------------------------------------
// --recompiler-diff: the two CPUs, side by side
// ---------------------------------------------------------------------------
//
// Docs/Recompiler-Plan.md has asked for this from the beginning: run every
// block both ways and compare, so that a disagreement names the instruction it
// happened at instead of turning up as a different framebuffer four hundred
// frames later.
//
// It runs **two whole machines**, not one machine twice. The plan's original
// sketch was to snapshot the CPU, run a block compiled, restore, run it
// interpreted and compare - but that performs every store twice, and a store
// to the GPU's FIFO or the SPU is not something to do twice. Two machines cost
// eight megabytes and have no such problem.
//
// They are aligned by pc, which is what makes the comparison meaningful: the
// recompiled machine runs a whole block at a time, so the interpreted one is
// stepped until it reaches wherever that block ended, and both are then in the
// same place by construction. Everything else - all 32 registers, HI, LO - has
// to agree there or something is wrong.
//
// **What a divergence here does and does not prove.** The two machines tick
// their devices differently (compiled code charges its cycles in a lump), so an
// interrupt can land on a different instruction in one than the other, and the
// two will then legitimately part company without either CPU being wrong. The
// report says whether an interrupt was taken just before the split, which is
// what tells those two cases apart.

struct DiffState {
  uint32_t pc;
  uint32_t reg[32];
  uint32_t low;
  uint32_t high;
};

DiffState CaptureDiff(System* system) {
  DiffState state;
  const emulation::psx::CpuContext* context = system->cpu().context();
  state.pc = context->pc;
  for (int i = 0; i < 32; ++i)
    state.reg[i] = context->gp.reg[i];
  state.low = context->low;
  state.high = context->high;

  // A load still in the pipeline belongs to this state even though it is not
  // in the register file yet: it reaches its register before the next
  // instruction runs, and compiled code has already written it out by the time
  // its block ends. Without this the two machines look different every time a
  // block's last-but-one instruction is a load - which is most of them, and is
  // exactly what this harness reported first.
  uint32_t index = 0;
  uint32_t value = 0;
  if (system->cpu().GetPendingLoad(&index, &value) && index != 0)
    state.reg[index] = value;
  return state;
}

const char* kRegisterNames[32] = {
    "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3",
    "t0",   "t1", "t2", "t3", "t4", "t5", "t6", "t7",
    "s0",   "s1", "s2", "s3", "s4", "s5", "s6", "s7",
    "t8",   "t9", "k0", "k1", "gp", "sp", "fp", "ra",
};

bool SetUpDiffMachine(System* system, const Options& options, bool recompiler) {
  system->config().hle_bios = options.hle;
  if (system->Initialize(options.bios) != 0)
    return false;
  if (options.cd_mechanical)
    system->config().cdrom_mechanical_timing = true;
  if (options.disc != nullptr && !system->LoadDisc(options.disc))
    return false;
  if (options.boot_disc)
    system->set_auto_boot(true);
  if (options.exe != nullptr)
    system->set_auto_boot_exe(true, options.exe);
  if (recompiler)
    system->EnableRecompiler(true);
  return true;
}

int RunRecompilerDiff(const Options& options) {
  printf("recompiler-diff  two machines, one per CPU, compared at every block\n");
  printf("bios             %s\n", options.bios);
  if (options.disc != nullptr)
    printf("disc             %s\n", options.disc);

  System* compiled = new System();
  System* interpreted = new System();
  if (!SetUpDiffMachine(compiled, options, true) ||
      !SetUpDiffMachine(interpreted, options, false)) {
    fprintf(stderr, "failed to initialise both machines\n");
    return 1;
  }

  // A short trail of where each machine has been, for the report. Knowing the
  // last few blocks is usually what identifies the code involved.
  const int kTrail = 8;
  uint32_t trail[kTrail] = {};
  int trail_next = 0;

  uint64_t blocks = 0;
  uint64_t instructions = 0;
  uint64_t interrupts_before = 0;
  int frames = 0;
  uint64_t last_frame = 0;

  while (frames < options.frames) {
    const uint32_t start_pc = compiled->cpu().context()->pc;
    trail[trail_next % kTrail] = start_pc;
    ++trail_next;

    const uint64_t taken_before = compiled->interrupts_taken();
    const uint64_t taken_before_interpreted = interpreted->interrupts_taken();

    // How many guest instructions the compiled machine is about to run, from
    // both halves of it: the compiled ones, which the engine counts exactly,
    // and any the engine handed to the interpreter, which Cpu::index counts.
    const uint64_t compiled_before =
        compiled->recompiler()->stats().instructions_compiled;
    const int fallback_before = compiled->cpu().index;

    compiled->StepInstruction();
    ++blocks;
    const uint32_t target = compiled->cpu().context()->pc;

    const uint64_t ran =
        (compiled->recompiler()->stats().instructions_compiled - compiled_before) +
        static_cast<uint64_t>(compiled->cpu().index - fallback_before);

    // Step the interpreted machine exactly that many instructions - not "until
    // its pc matches". The first version of this did the latter, and a loop
    // makes a liar of it: the interpreted machine stops the first time round
    // while the compiled one has been round five more, and the two are then
    // compared in different iterations. It reported a divergence in the BIOS
    // that the framebuffer said was not there, which is the only reason it was
    // caught.
    const int before_index = interpreted->cpu().index;
    while (static_cast<uint64_t>(interpreted->cpu().index - before_index) < ran)
      interpreted->StepInstruction();
    instructions += ran;

    const uint64_t interpreted_ran =
        static_cast<uint64_t>(interpreted->cpu().index - before_index);
    if (interpreted_ran != ran) {
      printf("\nDIVERGED: the two ran different amounts\n");
      printf("  compiled ran %llu instructions from %08X, interpreted %llu\n",
             static_cast<unsigned long long>(ran), start_pc,
             static_cast<unsigned long long>(interpreted_ran));
      printf("  which means a branch delay pair was split, or the block was\n"
             "  not what the interpreter walked.\n");
      break;
    }

    const DiffState a = CaptureDiff(compiled);
    const DiffState b = CaptureDiff(interpreted);
    bool same = (a.low == b.low) && (a.high == b.high);
    for (int i = 0; i < 32 && same; ++i)
      same = (a.reg[i] == b.reg[i]);

    // The pc is part of the comparison, not the thing that aligned it: both
    // machines ran the same number of instructions, so they should be in the
    // same place as well as holding the same values.
    const uint32_t interpreted_pc = interpreted->cpu().context()->pc;
    if (!same || interpreted_pc != target) {
      printf("\nDIVERGED: state at %08X\n", target);
      printf("  after the block starting at %08X\n", start_pc);
      if (interpreted_pc != target)
        printf("  the interpreted machine is at %08X instead\n", interpreted_pc);
      printf("  %llu blocks, %llu instructions, frame %d\n",
             static_cast<unsigned long long>(blocks),
             static_cast<unsigned long long>(instructions), frames);
      // The confound this harness cannot design away: the two machines tick
      // their devices differently, so an interrupt can land inside one
      // machine's window and not the other's. When that happens the two ran
      // different code and the comparison says nothing about the CPU.
      const uint64_t compiled_irqs = compiled->interrupts_taken() - taken_before;
      const uint64_t interpreted_irqs =
          interpreted->interrupts_taken() - taken_before_interpreted;
      printf("  interrupts in this window: %llu compiled, %llu interpreted\n",
             static_cast<unsigned long long>(compiled_irqs),
             static_cast<unsigned long long>(interpreted_irqs));
      if (compiled_irqs != interpreted_irqs) {
        printf("  -> they are not the same, so the two machines ran different\n"
               "     code here. This is when the interrupt landed, not what the\n"
               "     CPU computed.\n");
      } else {
        printf("  -> the same either way, so the interrupts are not the cause\n");
        printf("     (the other timing-dependent answer is a load from a\n");
        printf("      hardware register - 1F801xxx - since the two machines\n");
        printf("      tick their devices at different granularity. GPUSTAT's\n");
        printf("      bit 31 is the usual one. Check the listing below before\n");
        printf("      concluding the CPU is at fault.)\n");
      }
      printf("  interrupts so far: %llu compiled, %llu interpreted\n",
             static_cast<unsigned long long>(compiled->interrupts_taken()),
             static_cast<unsigned long long>(interpreted->interrupts_taken()));
      printf("\n  %-6s %-10s %-10s\n", "reg", "compiled", "interpreted");
      for (int i = 0; i < 32; ++i) {
        if (a.reg[i] != b.reg[i]) {
          printf("  %-6s %08X   %08X\n", kRegisterNames[i], a.reg[i], b.reg[i]);
        }
      }
      if (a.low != b.low)
        printf("  %-6s %08X   %08X\n", "lo", a.low, b.low);
      if (a.high != b.high)
        printf("  %-6s %08X   %08X\n", "hi", a.high, b.high);

      // The instructions themselves, which is what turns "these registers
      // differ" into something to read.
      printf("\n  the block:\n");
      for (uint32_t pc = start_pc; pc != target && pc - start_pc < 4 * 80;
           pc += 4) {
        const uint32_t word = FetchCode(interpreted, pc);
        char text[128];
        emulation::psx::Disassemble(pc, word, text, sizeof(text));
        printf("    %08X  %08X  %s\n", pc, word, text);
      }

      printf("\n  the blocks leading here:\n");
      for (int i = 0; i < kTrail; ++i) {
        const uint32_t pc = trail[(trail_next + i) % kTrail];
        if (pc != 0)
          printf("    %08X\n", pc);
      }
      delete compiled;
      delete interpreted;
      return 1;
    }

    const uint64_t now = compiled->gpu().frame_count();
    if (now != last_frame) {
      last_frame = now;
      ++frames;
      if ((frames % 100) == 0) {
        printf("frame %-5d %llu blocks, %llu instructions, still identical\n",
               frames, static_cast<unsigned long long>(blocks),
               static_cast<unsigned long long>(instructions));
        fflush(stdout);
      }
    }
  }

  printf("\n%llu blocks and %llu instructions over %d frames,\n",
         static_cast<unsigned long long>(blocks),
         static_cast<unsigned long long>(instructions), frames);
  printf("and the two CPUs agreed on every register at every block boundary.\n");
  delete compiled;
  delete interpreted;
  return 0;
}

// --bench-state <n>: how fast a save state is when it stays in memory, for
// Docs/Rewind-and-Run-Ahead-Plan.md. Runs once the run is over, from wherever it ended, and
// moves the machine on. Every figure is a mean over n tries. Where two ways of doing one thing
// are compared they take turns within one loop, so the scene and the host's clock - which on a
// laptop swings by tens of percent between batches - are the same for both.
namespace bench {
typedef std::chrono::steady_clock Clock;

double Ms(Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); }

// One frame, and the steps it took: an instruction interpreted, or a run of compiled blocks.
uint64_t RunFrame(System* system) {
  const uint64_t target = system->gpu().frame_count() + 1;
  uint64_t steps = 0;
  while (system->gpu().frame_count() < target && steps < 8000000) {
    system->StepInstructionUnarmed();
    ++steps;
  }
  return steps;
}

// Into the buffer kept from the last time, which allocates nothing once it is big enough.
void Save(System* system, std::vector<uint8_t>* buffer) {
  emulation::psx::StateIO state(/*saving=*/true);
  state.ReuseBuffer(buffer);
  system->SerialisePayload(state);
  state.TakeBytes(buffer);
}

// What LoadState does once the file is read, with `reopen` and `drop_code` as it has them;
// either false is what a state kept in memory could do instead.
void Load(System* system, emulation::psx::StateIO* state, const std::vector<uint8_t>& payload,
          bool reopen, bool drop_code) {
  system->cdrom().set_reopen_on_load(reopen);
  state->BeginLoad(payload.data(), payload.size());
  system->SerialisePayload(*state);
  system->cdrom().set_reopen_on_load(true);
  system->FinishLoad(drop_code);
}

// zlib at level 1. The vendored zlib has deflate but not compress.c.
size_t Deflate1(const uint8_t* in, size_t size, std::vector<uint8_t>* out) {
  out->resize(size + size / 8 + 4096);
  z_stream z = {};
  deflateInit(&z, 1);
  z.next_in = const_cast<Bytef*>(in);
  z.avail_in = static_cast<uInt>(size);
  z.next_out = out->data();
  z.avail_out = static_cast<uInt>(out->size());
  deflate(&z, Z_FINISH);
  const size_t written = z.total_out;
  deflateEnd(&z);
  return written;
}

void Line(const char* what, double total_ms, int n) {
  printf("bench  %-48s %8.3f ms\n", what, n > 0 ? total_ms / n : 0.0);
}

// The disc reads a stretch made, per try: reads that went to the image, how many of them
// stepped back, and the time they took.
void ReadsLine(const char* what, const emulation::psx::Disc::ReadStats& total, int n) {
  if (n <= 0)
    return;
  printf("bench    %-46s %6.2f reads (%.2f back) %7.3f ms\n", what,
         static_cast<double>(total.reads) / n, static_cast<double>(total.backward) / n,
         total.ms / n);
}
emulation::psx::Disc::ReadStats Since(const emulation::psx::Disc::ReadStats& now,
                                      const emulation::psx::Disc::ReadStats& then) {
  emulation::psx::Disc::ReadStats d;
  d.reads = now.reads - then.reads;
  d.backward = now.backward - then.backward;
  d.bytes = now.bytes - then.bytes;
  d.ms = now.ms - then.ms;
  return d;
}

void Run(System* system, int n) {
  const emulation::psx::Disc::ReadStats& disc = system->cdrom().disc().read_stats();
  printf("bench  recompiler %s, rasteriser %s, %d tries each\n",
         system->config().recompiler ? "on" : "off",
         system->gpu().hardware_raster() ? "hardware" : "software", n);
  for (int i = 0; i < 30; ++i)
    RunFrame(system);

  std::vector<uint8_t> state, scratch;
  emulation::psx::StateIO loader(/*saving=*/false);
  Save(system, &state);
  printf("bench  payload                                          %10zu bytes\n", state.size());

  Clock::time_point t;
  emulation::psx::Disc::ReadStats before = disc;
  t = Clock::now();
  for (int i = 0; i < n; ++i)
    RunFrame(system);
  Line("a frame", Ms(Clock::now() - t), n);
  ReadsLine("its disc reads", Since(disc, before), n);

  // Saving: as SaveState builds its payload, a vector grown a field at a time, then into one
  // kept from the last save.
  t = Clock::now();
  for (int i = 0; i < n; ++i) {
    emulation::psx::StateIO fresh(/*saving=*/true);
    system->SerialisePayload(fresh);
  }
  Line("save, a fresh vector (as SaveState)", Ms(Clock::now() - t), n);
  t = Clock::now();
  for (int i = 0; i < n; ++i)
    Save(system, &scratch);
  Line("save, a reused buffer", Ms(Clock::now() - t), n);
  t = Clock::now();
  for (int i = 0; i < n; ++i)
    memcpy(scratch.data(), state.data(), std::min(scratch.size(), state.size()));
  Line("memcpy of the payload (the floor)", Ms(Clock::now() - t), n);
  System::MemoryState memory, memory_scratch;
  system->SaveMemoryState(&memory_scratch, true);
  t = Clock::now();
  for (int i = 0; i < n; ++i)
    system->SaveMemoryState(&memory_scratch, false);
  Line("save, a memory state (SaveMemoryState)", Ms(Clock::now() - t), n);
  t = Clock::now();
  for (int i = 0; i < n; ++i)
    system->SaveMemoryState(&memory_scratch, true);
  Line("save, a memory state with the cards", Ms(Clock::now() - t), n);

  // Loading, taking turns: as LoadState does, with the disc kept open, and as a memory state.
  Save(system, &state);
  system->SaveMemoryState(&memory, false);
  {
    double ms[3] = {};
    for (int i = 0; i < 3 * n; ++i) {
      t = Clock::now();
      if (i % 3 == 2)
        system->LoadMemoryState(memory);
      else
        Load(system, &loader, state, i % 3 == 0, true);
      ms[i % 3] += Ms(Clock::now() - t);
    }
    Line("load, the disc reopened (as LoadState)", ms[0], n);
    Line("load, the disc kept open", ms[1], n);
    Line("load, a memory state (LoadMemoryState)", ms[2], n);
  }

  // The frame after a load, from the same state each time: compiled code thrown away as
  // LoadState does, all kept, or kept where its page did not change, as LoadMemoryState does.
  // The difference from the first is recompiling.
  {
    double ms[3] = {};
    for (int i = 0; i < 3 * n; ++i) {
      if (i % 3 == 2)
        system->LoadMemoryState(memory);
      else
        Load(system, &loader, state, false, i % 3 == 0);
      t = Clock::now();
      RunFrame(system);
      ms[i % 3] += Ms(Clock::now() - t);
    }
    Line("the frame after a load, compiled code dropped", ms[0], n);
    Line("the frame after a load, compiled code kept", ms[1], n);
    Line("the frame after a memory state's load", ms[2], n);
  }

  // Run-ahead of one frame, per frame shown: the frame the input is for runs, a state is kept,
  // one more frame runs and would be shown, and the state is loaded back - two frames, a save
  // and a load. Against no run-ahead, one frame per frame shown. Every way runs the same 30
  // frames shown from one starting state, taking turns, so each pays for the same scenes.
  {
    const int kStretch = 30;
    const int stretches = std::max(1, n / kStretch);
    const char* names[4] = {"no run-ahead", "run-ahead 1, a load as LoadState does",
                            "run-ahead 1, memory states",
                            "run-ahead 1, memory states with the cards"};
    double ms[4] = {};
    double part[4][4] = {};   // the first frame, the save, the second frame, the load
    uint64_t steps[4] = {};
    emulation::psx::Disc::ReadStats reads[4];
    std::vector<uint8_t> start;
    Save(system, &start);
    for (int s = 0; s < stretches; ++s) {
      for (int k = 0; k < 4; ++k) {
        Load(system, &loader, start, true, true);
        before = disc;
        for (int f = 0; f < kStretch; ++f) {
          Clock::time_point marks[5];
          marks[0] = Clock::now();
          steps[k] += RunFrame(system);
          marks[1] = Clock::now();
          if (k == 1)
            Save(system, &scratch);
          else if (k > 1)
            system->SaveMemoryState(&memory_scratch, k == 3);
          marks[2] = Clock::now();
          if (k > 0)
            steps[k] += RunFrame(system);
          marks[3] = Clock::now();
          if (k == 1)
            Load(system, &loader, scratch, true, true);
          else if (k > 1)
            system->LoadMemoryState(memory_scratch);
          marks[4] = Clock::now();
          ms[k] += Ms(marks[4] - marks[0]);
          for (int p = 0; p < 4; ++p)
            part[k][p] += Ms(marks[p + 1] - marks[p]);
        }
        const emulation::psx::Disc::ReadStats d = Since(disc, before);
        reads[k].reads += d.reads;
        reads[k].backward += d.backward;
        reads[k].ms += d.ms;
      }
    }
    const int shown = stretches * kStretch;
    for (int k = 0; k < 4; ++k) {
      Line(names[k], ms[k], shown);
      printf("bench    frame %.3f, save %.3f, frame %.3f, load %.3f ms; %.0f steps\n",
             part[k][0] / shown, part[k][1] / shown, part[k][2] / shown, part[k][3] / shown,
             static_cast<double>(steps[k]) / shown);
      ReadsLine("its disc reads", reads[k], shown);
    }
  }

  // Whether a load changes what the machine computes. From one state, 120 frames on: straight
  // on with no load at all, after a load that reopens the disc, after one that keeps it open,
  // and with a save and a load (disc reopened) after every frame, as run-ahead would. Each
  // ends hashed whole; a load that is invisible gives the same hash as no load.
  {
    const int kFrames = 120;
    auto hash_after = [&](int mode) {
      // 0: no load; 1: reopened; 2: kept open; 3: a save and load after every frame, as
      // LoadState does them; 4: the same, the disc and the compiled code kept; 5: a memory
      // state saved and loaded after every frame.
      if (mode == 1 || mode == 2)
        Load(system, &loader, state, mode == 1, true);
      for (int i = 0; i < kFrames; ++i) {
        RunFrame(system);
        if (mode == 3 || mode == 4) {
          Save(system, &scratch);
          Load(system, &loader, scratch, mode == 3, mode == 3);
        } else if (mode == 5) {
          system->SaveMemoryState(&memory_scratch, true);
          system->LoadMemoryState(memory_scratch);
        }
      }
      Save(system, &scratch);
      return emulation::psx::Fnv1a64(scratch.data(), scratch.size());
    };
    const char* modes[6] = {"no load", "a load, the disc reopened", "a load, the disc kept open",
                            "a save and load after every frame",
                            "...that, compiled code kept",
                            "a memory state after every frame"};
    Save(system, &state);
    uint64_t hashes[6];
    hashes[0] = hash_after(0);
    for (int mode = 1; mode < 6; ++mode) {
      Load(system, &loader, state, true, true);
      hashes[mode] = hash_after(mode);
    }
    for (int mode = 0; mode < 6; ++mode)
      printf("bench  %d frames after %-36s %016llx%s\n", kFrames, modes[mode],
             static_cast<unsigned long long>(hashes[mode]),
             hashes[mode] == hashes[0] ? "" : "  DIFFERS");
  }

  // Rewind's storage: what changes from one frame's state to the next.
  std::vector<uint8_t> last, now, changed, packed;
  Save(system, &last);
  double save_ms = 0.0, pages_ms = 0.0, whole_ms = 0.0;
  uint64_t bytes_differing = 0, pages_differing = 0, packed_bytes = 0;
  int samples = 0;
  for (int i = 0; i < n; ++i) {
    RunFrame(system);
    t = Clock::now();
    Save(system, &now);
    save_ms += Ms(Clock::now() - t);
    if (now.size() == last.size()) {
      changed.clear();
      for (size_t at = 0; at < now.size(); at += 4096) {
        const size_t length = std::min<size_t>(4096, now.size() - at);
        if (memcmp(&now[at], &last[at], length) == 0)
          continue;
        ++pages_differing;
        for (size_t k = 0; k < length; ++k)
          bytes_differing += now[at + k] != last[at + k];
        changed.insert(changed.end(), now.begin() + at, now.begin() + at + length);
      }
      t = Clock::now();
      packed_bytes += Deflate1(changed.data(), changed.size(), &packed);
      pages_ms += Ms(Clock::now() - t);
      ++samples;
    }
    last.swap(now);
  }
  t = Clock::now();
  const size_t whole = Deflate1(last.data(), last.size(), &packed);
  whole_ms = Ms(Clock::now() - t);
  printf("bench  frame to frame, %d states\n", samples);
  Line("  a save", save_ms, n);
  if (samples > 0) {
    printf("bench    bytes that differ                              %10.0f\n",
           static_cast<double>(bytes_differing) / samples);
    printf("bench    4 KB pages that differ                         %10.1f of %zu\n",
           static_cast<double>(pages_differing) / samples, (last.size() + 4095) / 4096);
    printf("bench    those pages, zlib-1                            %10.0f bytes\n",
           static_cast<double>(packed_bytes) / samples);
    Line("  ...taking", pages_ms, samples);
  }
  printf("bench    a whole state, zlib-1                          %10zu bytes\n", whole);
  Line("  ...taking", whole_ms, 1);
}
}  // namespace bench
}  // namespace

#ifdef PSXEMU_HW_RASTER
// --gpu's card, by part of its name; 0 for Windows' default, or when there is no such card.
uint64_t ChosenAdapter(const Options& options) {
  if (options.gpu == nullptr)
    return 0;
  const std::vector<psxemu::GraphicsAdapter> cards = psxemu::EnumerateGraphicsAdapters();
  const psxemu::GraphicsAdapter* card = psxemu::FindGraphicsAdapterLike(cards, options.gpu);
  return card != nullptr ? card->luid : 0;
}

// --texture-ai: the model on that card, ONNX Runtime from its folder - this program's unless
// --texture-ai-runtime says - writing into --texture-ai-out. Null, having said why.
std::unique_ptr<psxemu::AiTextures> MakeAiTextures(const Options& options) {
  if (options.texture_ai_out == nullptr) {
    fprintf(stderr, "--texture-ai needs --texture-ai-out <dir>\n");
    return nullptr;
  }
  auto wide = [](const char* text) {
    std::wstring out;
    for (const char* p = text; *p != '\0'; ++p)
      out += static_cast<wchar_t>(static_cast<unsigned char>(*p));
    return out;
  };
  std::wstring runtime;
  if (options.texture_ai_runtime != nullptr) {
    runtime = wide(options.texture_ai_runtime);
  } else {
    wchar_t self[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    runtime = self;
    runtime.erase(runtime.find_last_of(L"\\/") + 1);
  }
  std::string error;
  std::unique_ptr<psxemu::TextureUpscaler> backend =
      strcmp(options.texture_ai, "rtx-video") == 0
          ? psxemu::MakeRtxVideoBackend(runtime, options.texture_ai_scale,
                                        options.texture_ai_quality, ChosenAdapter(options), &error)
          : psxemu::MakeOnnxBackend(runtime, wide(options.texture_ai), ChosenAdapter(options),
                                    &error);
  if (!backend) {
    fprintf(stderr, "--texture-ai: %s\n", error.c_str());
    return nullptr;
  }
  psxemu::AiTextures::Options ai;
  ai.scale = options.texture_ai_scale;
  ai.output = options.texture_ai_out;
  ai.share = options.texture_ai_share;
  ai.grid = options.texture_ai_grid;
  printf("texture ai     %s at %dx into %s, %s, %.0f%% of the time\n", options.texture_ai,
         ai.scale, ai.output.c_str(), backend->description().c_str(), ai.share * 100.0);
  return std::make_unique<psxemu::AiTextures>(std::move(backend), ai);
}

void ReportAi(psxemu::AiTextures& ai, double seconds) {
  const psxemu::AiTextures::Counts c = ai.counts();
  printf("upscaled       %llu tiles in %llu calls, %.1f ms a call (%.1f s of %.1f), %llu tiles "
         "with nothing drawn; %llu sources told of, %llu images made; %llu images read through "
         "more than eight palettes%s%s\n",
         static_cast<unsigned long long>(c.tiles), static_cast<unsigned long long>(c.calls),
         c.calls > 0 ? c.call_ms / static_cast<double>(c.calls) : 0.0, c.call_ms / 1000.0, seconds,
         static_cast<unsigned long long>(c.empty_tiles), static_cast<unsigned long long>(c.sources),
         static_cast<unsigned long long>(c.published),
         static_cast<unsigned long long>(c.palettes_cycling),
         c.failures > 0 ? "; failed: " : "", c.failures > 0 ? c.last_error.c_str() : "");
}
#endif

int main(int argc, char** argv) {
#ifdef PSXEMU_HW_RASTER
  // The graphics cards, and the LUID each is known by - which is also how Windows' `\GPU Engine`
  // and `\GPU Process Memory` counters name them, for seeing which card a process is using.
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--list-gpus") == 0) {
      const std::vector<psxemu::GraphicsAdapter> cards = psxemu::EnumerateGraphicsAdapters();
      for (const psxemu::GraphicsAdapter& card : cards)
        printf("%-44s luid_%s  %llu MB\n", card.name.c_str(),
               psxemu::CounterLuid(card.luid).c_str(),
               static_cast<unsigned long long>(card.video_memory >> 20));
      return cards.empty() ? 1 : 0;
    }
  }
#endif
  Options options;
  if (!ParseOptions(argc, argv, &options)) {
    fprintf(stderr,
            "usage: boot_runner <bios.bin> [--exe f] [--frames n] [--ppm f]\n"
            "                              [--trace n] [--quiet]\n");
    return 2;
  }

  // Two machines rather than one, and nothing else about this run applies.
  if (options.recompiler_diff)
    return RunRecompilerDiff(options);

  // --upscale-folder: Upscale All, and no machine at all.
  if (options.upscale_folder != nullptr) {
#ifdef PSXEMU_HW_RASTER
    std::unique_ptr<psxemu::AiTextures> ai =
        options.texture_ai != nullptr ? MakeAiTextures(options) : nullptr;
    if (!ai) {
      fprintf(stderr, "--upscale-folder needs --texture-ai and --texture-ai-out\n");
      return 2;
    }
    const auto start = std::chrono::steady_clock::now();
    printf("upscaling      %zu dumps in %s\n", ai->QueueFolder(options.upscale_folder),
           options.upscale_folder);
    ai->Drain();
    ReportAi(*ai, std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
    return ai->counts().failures == 0 ? 0 : 1;
#else
    fprintf(stderr, "--upscale-folder: this build has no AI upscaler\n");
    return 2;
#endif
  }

  System* system = new System();
  // The rasteriser is made when the GPU is initialised, so it is chosen before.
  if (options.hw_raster) {
#ifdef PSXEMU_HW_RASTER
    const bool warp = options.warp;
    const bool shared = options.shared_picture;
    // --gpu: the card to draw on, by part of its name. Sharing wants a presenter on the same
    // card, which this has none of - it hands pictures over as a presenter that is on it would.
    uint64_t adapter = 0;
    if (options.gpu != nullptr) {
      const std::vector<psxemu::GraphicsAdapter> cards = psxemu::EnumerateGraphicsAdapters();
      const psxemu::GraphicsAdapter* card =
          psxemu::FindGraphicsAdapterLike(cards, options.gpu);
      if (card == nullptr) {
        fprintf(stderr, "--gpu: no graphics card here has \"%s\" in its name (--list-gpus)\n",
                options.gpu);
        return 2;
      }
      adapter = card->luid;
    }
    const psxemu::HardwareRaster::Api api =
        options.d3d12 ? psxemu::HardwareRaster::Api::kD3D12 : psxemu::HardwareRaster::Api::kD3D11;
    // --one-device: a device of this program's, on the card asked for or WARP, standing in for
    // the Direct3D 12 renderer's.
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    if (options.one_device) {
      if (!shared) {
        fprintf(stderr, "--one-device: only with --shared-picture\n");
        return 2;
      }
      Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
      Microsoft::WRL::ComPtr<IDXGIAdapter1> card;
      if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        if (warp)
          factory->EnumWarpAdapter(IID_PPV_ARGS(&card));
        else if (adapter != 0)
          card = psxemu::OpenAdapter(adapter);
      }
      if (FAILED(D3D12CreateDevice(card.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
        fprintf(stderr, "--one-device: no Direct3D 12 device could be made\n");
        return 2;
      }
    }
    const bool as_console = options.texture_console;
    system->set_hardware_raster([warp, shared, adapter, api, device, as_console](
                                    uint16_t* vram, const emulation::psx::RasterOptions& raster,
                                    std::string* error)
                                    -> std::unique_ptr<emulation::psx::RasterBackend> {
      emulation::psx::RasterOptions shown = raster;
      shown.shared_picture = shared;
      shown.adapter = adapter;
      shown.replacements_as_console = as_console;
      return psxemu::HardwareRaster::Create(api, vram, shown, warp, error, device.Get());
    });
    system->config().gpu_rasteriser = "hardware";
    system->config().resolution_scale = options.scale;
    system->config().true_color = options.true_color;
    // As the front end does: no native picture beside the sharper one. The checksums are then
    // of whatever the native picture last was - this is for timing the front end's path.
    system->gpu().set_native_picture(!options.shown_only);
    system->config().pgxp_vertices = options.pgxp;
    system->config().pgxp_culling = options.pgxp_culling;
    system->config().pgxp_textures = options.pgxp_textures;
#else
    fprintf(stderr, "--hw-raster: this build has no hardware rasteriser\n");
    return 2;
#endif
  }
  system->config().hle_bios = options.hle;
  if (system->Initialize(options.bios) != 0) {
    fprintf(stderr, "failed to initialise the core (bios: %s)\n", options.bios);
    return 1;
  }
  if (system->hle_active())
    printf("bios           HLE (psx/hle_bios.h)\n");
  if (options.justifier) {
    system->sio().set_controller_type(0, emulation::psx::Sio::kJustifier);
    system->sio().set_connected(0, true);
    system->sio().set_guncon(0, false, false, false, options.justifier_x, options.justifier_y);
    printf("port 1         Konami Justifier, aimed at %.3f,%.3f\n", options.justifier_x,
           options.justifier_y);
  }
  // --movie: read before anything is mounted, since it says which disc the machine boots on and
  // which slots have a card - new ones, in the temporary folder, so a run starts as the
  // recording did and writes to nothing of anyone's.
  emulation::psx::MoviePlayer movie;
  // Where the movie's cards are kept, taken away again when the run ends however it ends.
  struct MovieCards {
    std::string path;
    ~MovieCards() {
      if (path.empty())
        return;
      DeleteFileA((path + "\\movie_card1.mcr").c_str());
      DeleteFileA((path + "\\movie_card2.mcr").c_str());
      RemoveDirectoryA(path.c_str());
    }
  } movie_cards;
  if (options.movie != nullptr) {
    emulation::psx::Movie loaded;
    std::string error;
    if (!emulation::psx::LoadBk2(options.movie, &loaded, &error)) {
      fprintf(stderr, "--movie: %s: %s\n", options.movie, error.c_str());
      return 2;
    }
    if (!options.presses.empty()) {
      fprintf(stderr, "--movie: the movie holds the pads, so --press has nothing to press\n");
      return 2;
    }
    printf("movie          %s\n", loaded.Summary().c_str());
    for (const emulation::psx::Movie::Player& player : loaded.players) {
      printf("movie          P%d: port %d%s%s, %s\n", player.number, player.port + 1,
             loaded.multitap[player.port] ? " socket " : "",
             loaded.multitap[player.port]
                 ? std::string(1, static_cast<char>('A' + player.socket)).c_str() : "",
             player.device == emulation::psx::MovieDevice::kDualShock    ? "DualShock"
             : player.device == emulation::psx::MovieDevice::kDualAnalog ? "Dual Analog"
                                                                        : "digital pad");
    }
    for (const std::string& column : loaded.ignored)
      printf("movie          column not understood, ignored: %s\n", column.c_str());
    for (int slot = 0; slot < 2; ++slot) {
      if (loaded.memory_card[slot])
        printf("movie          card %d: %s\n", slot + 1,
               !loaded.card[slot].empty()   ? "the one saved in the movie"
               : loaded.new_cards_formatted ? "new, formatted"
                                            : "new, never formatted");
    }
    // A movie is in step only on the BIOS it was recorded on.
    if (!loaded.bios_sha1.empty() && !system->hle_active()) {
      const std::string have = emulation::psx::Sha1Hex(system->bios(), System::kBiosSize);
      printf("movie          recorded on BIOS %s - %s\n", loaded.bios_sha1.c_str(),
             have == loaded.bios_sha1 ? "this one" : "NOT this one, so expect it out of step");
    }
    // A folder of this run's own: two runs at once writing one card file is one of them
    // failing to start.
    char temp[MAX_PATH] = {};
    GetTempPathA(MAX_PATH, temp);
    std::string directory = temp;
    while (!directory.empty() && (directory.back() == '\\' || directory.back() == '/'))
      directory.pop_back();
    directory += "\\psxemu_movie_" + std::to_string(GetCurrentProcessId());
    CreateDirectoryA(directory.c_str(), nullptr);
    movie_cards.path = directory;
    if (!emulation::psx::InsertMovieCards(*system, loaded, directory, &error)) {
      fprintf(stderr, "--movie: %s\n", error.c_str());
      return 2;
    }
    movie.Start(std::move(loaded));
    // A movie's loads have to take the frames they took in BizHawk: the drive is timed as
    // Mednafen's whatever else was asked for. The CPU's options stay the command line's.
    system->config().cdrom_mednafen_timing = true;
    system->config().cdrom_mechanical_timing = false;
    printf("movie          the drive is timed as Mednafen's for it\n");
    movie.set_offset(options.movie_offset);
    if (options.movie_offset != 0)
      printf("movie          offset by %d frames\n", options.movie_offset);
  }
  // The nth disc of the movie's game: --disc, then each --movie-disc. Null if there is none.
  const char* const first_disc = options.disc;
  auto movie_disc = [&options, first_disc](int n) -> const char* {
    if (n == 1)
      return first_disc;
    return n >= 2 && static_cast<size_t>(n - 2) < options.movie_discs.size()
               ? options.movie_discs[static_cast<size_t>(n - 2)]
               : nullptr;
  };
  // A movie whose first frame has another disc in the tray, or none, boots on that.
  if (movie.playing() && movie.initial_disc() != 1)
    options.disc = movie_disc(movie.initial_disc());

  // Cards are written back to their files as the machine changes them: pass copies.
  for (int slot = 0; slot < 2; ++slot) {
    if (options.cards[slot] == nullptr)
      continue;
    if (system->mc(slot).LoadFile(options.cards[slot]) != S_OK) {
      fprintf(stderr, "--card%d: cannot load %s\n", slot + 1, options.cards[slot]);
      return 2;
    }
    printf("card %d         %s\n", slot + 1, options.cards[slot]);
  }
  if (options.hle_trace != nullptr) {
    FILE* trace = fopen(options.hle_trace, "w");
    if (trace == nullptr) {
      fprintf(stderr, "--call-trace: cannot write %s\n", options.hle_trace);
      return 2;
    }
    // Closed by the process exiting. On a dump, the calls through the vectors instead.
    if (system->hle_active())
      system->hle().set_trace(trace);
    else
      system->kernel().set_trace(trace);
  }
  if (options.hw_raster) {
    if (!system->gpu().hardware_raster()) {
      fprintf(stderr, "--hw-raster: %s\n", system->gpu().raster_error().c_str());
      return 1;
    }
    printf("gpu            hardware rasteriser (Direct3D %s%s, %s), %dx%s\n",
           options.d3d12 ? "12" : "11", options.one_device ? " on one device" : "",
           system->gpu().raster_device().c_str(), options.scale,
           options.scale > 1 && options.true_color ? ", true colour" : "");
    if (options.planes || options.view != emulation::psx::PlaneView::kPicture) {
      system->gte().set_motion_key(options.motion_key);
      if (options.motion_reach > 0.0f)
        system->gte().vertex_motion().set_reach(options.motion_reach);
      system->gpu().SetPlanes(options.planes, options.view);
      system->gpu().SetMotionCheck(options.motion);
      system->gpu().SetJitter(options.jitter);
      if (options.jitter > 0)
        printf("gpu            triangles jittered through %d offsets\n", options.jitter);
      printf("gpu            the plane beside VRAM kept%s\n",
             options.view == emulation::psx::PlaneView::kDepth    ? ", its depth shown"
             : options.view == emulation::psx::PlaneView::kMotion ? ", its motion shown"
                                                                  : "");
    }
  } else if (options.planes || options.view != emulation::psx::PlaneView::kPicture) {
    fprintf(stderr, "--planes and --view are the hardware rasteriser's (--hw-raster)\n");
    return 2;
  }

  // --textures: watching every draw from the first, so every upload is seen.
  std::unique_ptr<emulation::psx::TextureTracker> textures;
  std::unique_ptr<emulation::psx::TexturePack> pack;   // --texture-pack: after the tracker
#ifdef PSXEMU_HW_RASTER
  std::unique_ptr<psxemu::AiTextures> ai_textures;   // --texture-ai: after the pack it feeds
  std::chrono::steady_clock::time_point ai_started;
#endif
  if (options.textures) {
    textures = std::make_unique<emulation::psx::TextureTracker>(system->gpu().vram());
    if (options.texture_dump != nullptr) {
      textures->set_dump_folder(options.texture_dump);
      printf("textures       named, and each new one written into %s\n", options.texture_dump);
    } else {
      printf("textures       named\n");
    }
    if (options.texture_log > 0 || options.texture_trace[2] > 0)
      textures->set_log(stdout, options.texture_log);
    const int* t = options.texture_trace;
    textures->set_trace(t[0], t[1], t[2], t[3]);
    system->gpu().set_raster_observer(textures.get());
    if (options.texture_pack != nullptr || options.texture_ai != nullptr) {
      pack = std::make_unique<emulation::psx::TexturePack>(textures.get());
      pack->set_synchronous(true);
      pack->set_test_scale(options.texture_pack_scale);
      std::string error;
      const std::string folder = options.texture_pack != nullptr ? options.texture_pack : "";
      const std::string ai_folder = options.texture_ai_out != nullptr ? options.texture_ai_out : "";
      if (!pack->Open(folder, &error, options.texture_ai != nullptr ? ai_folder : "")) {
        fprintf(stderr, "--texture-pack: %s\n", error.c_str());
        return 2;
      }
      if (!options.hw_raster)
        fprintf(stderr, "--texture-pack: only the hardware rasteriser (--hw-raster) draws them\n");
      printf("texture pack   %llu replacements in %s\n",
             static_cast<unsigned long long>(pack->counts().files),
             folder.empty() ? ai_folder.c_str() : folder.c_str());
      system->gpu().set_texture_replacer(pack.get());
    }
#ifdef PSXEMU_HW_RASTER
    if (options.texture_ai != nullptr) {
      ai_textures = MakeAiTextures(options);
      if (!ai_textures)
        return 2;
      ai_textures->set_pack(pack.get());
      textures->set_upscale_queue(ai_textures.get());
      ai_started = std::chrono::steady_clock::now();
    }
#endif
  }

  // Before the machine executes a single instruction, so the whole run is one
  // way or the other and comparing it against the baselines means something.
  if (options.gpu_transfer_timing) {
    system->config().gpu_transfer_timing = true;
    printf("gpu            charging CPU-VRAM transfers\n");
  }
  if (options.icache_timing) {
    system->config().icache_timing = true;
    printf("cpu            instruction-cache timing\n");
  }
  if (options.exact_event_timing) {
    system->config().exact_event_timing = true;
    printf("timing         exact event timing\n");
  }
  if (options.dma_stops_cpu) {
    system->config().dma_stops_cpu = true;
    printf("dma            stops the cpu\n");
  }
  if (options.measured_bus_timing) {
    system->config().measured_bus_timing = true;
    printf("bus            measured timing\n");
  }
  if (options.write_queue_timing) {
    system->config().write_queue_timing = true;
    printf("cpu            write queue timing\n");
  }

  if (options.gpu_thread) {
    system->config().gpu_thread = true;
    printf("gpu            rasterising on a thread of its own\n");
  }

  if (options.recompiler) {
    system->EnableRecompiler(true);
    printf("cpu            recompiler (compiled blocks, interpreter fallback)\n");
  }
  if (options.gte_x64)
    system->config().gte_x64 = true;
  if (options.gte_cpp)
    system->config().gte_x64 = false;
  if (options.recompiler)
    printf("gte            %s\n", system->config().gte_x64 ? "commands emitted as x64"
                                                         : "commands run as C++ (--no-gte-x64)");

  // Before anything mounts a disc or restores a state, so the drive is under
  // the timing the run asked for from its very first command.
  if (options.cd_mechanical) {
    system->config().cdrom_mechanical_timing = true;
    printf("cd timing      mechanical (spin-up, seek distance, latency)\n");
  }
  if (options.cd_mednafen) {
    system->config().cdrom_mednafen_timing = true;
    printf("cd timing      Mednafen's (EmuConfig::cdrom_mednafen_timing)\n");
  }

  if (options.load_state != nullptr) {
    const std::string error = system->LoadState(options.load_state);
    if (!error.empty()) {
      fprintf(stderr, "failed to load state %s: %s\n", options.load_state,
              error.c_str());
      return 1;
    }
    printf("loaded state   %s\n", options.load_state);
  }

  // A loaded state already carries the CD-ROM, GPU, CPU - everything - so
  // none of --disc/--auto-boot/--boot-disc/--exe make sense on top of it;
  // running them would just be overwritten by the load.
  if (options.load_state == nullptr && options.disc != nullptr) {
    if (!system->LoadDisc(options.disc)) {
      fprintf(stderr, "failed to mount %s\n", options.disc);
      return 1;
    }
    printf("mounted        %s (%d track(s), %u sectors)\n", options.disc,
           system->cdrom().disc().track_count(),  // NOLINT
           system->cdrom().disc().total_sectors());
    // --movie: whether this is the disc it was recorded on, as far as the two can say.
    if (movie.playing()) {
      const emulation::psx::MovieDiscCheck check =
          emulation::psx::CheckMovieDisc(movie.movie(), system->cdrom().disc());
      printf("movie          %s\n", check.Describe(movie.movie()).c_str());
    }
  }

  if (options.watch_ram != 0)
    system->cpu().set_watch_address(options.watch_ram);

  if (options.volume >= 0.0f)
    system->config().audio_volume = options.volume;

  if (options.trace_spu != nullptr)
    system->spu().set_trace_voices(true);

  // --auto-boot with --exe defers the side-load to the same address rather
  // than doing both: a raw side-load right now would run over the reset
  // state --auto-boot exists to get past in the first place. None of this
  // runs at all with --load-state - see the comment above its guard.
  const bool defer_exe_to_auto_boot =
      options.auto_boot && options.exe != nullptr;

  if (options.load_state == nullptr) {
    if (options.auto_boot && !defer_exe_to_auto_boot) {
      // Let the BIOS run its intro, then take over when it reaches the
      // address it would hand a game control at.
      system->set_auto_boot(true);
      printf("auto-boot      armed at pc=80030000\n");
    } else if (defer_exe_to_auto_boot) {
      system->set_auto_boot_exe(true, options.exe);
      printf("auto-boot      armed at POST 7, will side-load %s\n",
             options.exe);
    }

    if (options.boot_disc) {
      System::DiscBootInfo info;
      const bool booted = system->BootDisc(&info);
      if (!info.volume_id.empty())
        printf("volume         %s\n", info.volume_id.c_str());
      if (!info.boot_path.empty())
        printf("boot line      %s\n", info.boot_path.c_str());
      if (!booted) {
        fprintf(stderr, "disc boot failed: %s\n",
                info.error ? info.error : "unknown reason");
        return 1;
      }
      printf("booted         %s (%u bytes) at pc=%08X\n",
             info.executable.c_str(), info.executable_size,
             system->cpu().context()->pc);
    }

    if (options.exe != nullptr && !defer_exe_to_auto_boot && system->hle_active()) {
      // The HLE BIOS loads it itself, once its kernel is built (HleBios::Reset).
      system->set_auto_boot_exe(true, options.exe);
      printf("side-load      %s, by the HLE BIOS\n", options.exe);
    } else if (options.exe != nullptr && !defer_exe_to_auto_boot) {
      if (!system->LoadPsExe(options.exe)) {
        fprintf(stderr, "failed to load %s\n", options.exe);
        return 1;
      }
      printf("side-loaded %s\n", options.exe);
    }
  }

  if (options.watch != nullptr) {
    // Which command wrote each pixel is the software rasteriser's own per-pixel accounting.
    if (system->gpu().hardware_raster()) {
      fprintf(stderr, "--watch-vram needs the software rasteriser; leave out --hw-raster\n");
      return 2;
    }
    unsigned x = 0, y = 0, w = 0, h = 0;
    if (sscanf(options.watch, "%u,%u,%u,%u", &x, &y, &w, &h) == 4) {
      system->WatchVram(x, y, w, h);
      printf("watching       VRAM %u,%u %ux%u\n", x, y, w, h);
    } else {
      fprintf(stderr, "--watch-vram wants x,y,w,h\n");
      return 2;
    }
  }

  // One StepInstruction at a time, on this thread, and nothing paced by the
  // wall clock - which is what makes the harness deterministic.
  std::vector<int16_t> audio;
  uint64_t instructions = 0;
  // --memory-state-every: the one state, its buffers reused, and what the round trips cost.
  System::MemoryState memory_state;
  uint64_t memory_states = 0;
  double memory_state_save_ms = 0.0;
  double memory_state_load_ms = 0.0;
  // --run-ahead: the state the real frame is kept in, and what the frames ahead cost.
  System::MemoryState ahead_state;
  // --rewind-test: the store, a state a frame, and the one n frames before the end kept aside.
  std::unique_ptr<emulation::psx::RewindBuffer> rewind;
  System::MemoryState rewind_state;
  std::vector<uint8_t> rewind_target;
  if (options.rewind_test > 0)
    rewind.reset(new emulation::psx::RewindBuffer(size_t(4) << 30));
  uint64_t ahead_frames = 0;
  uint64_t ahead_instructions = 0;
  double ahead_save_ms = 0.0;
  double ahead_load_ms = 0.0;
  if (options.run_ahead > 0 &&
      (movie.playing() || !options.breaks.empty() || !options.watches.empty() ||
       options.track_calls || options.shared_picture)) {
    // A movie is input by frame number, a halt lands in whichever frame - real or ahead - it
    // falls in, and a picture left on the card from a frame ahead is one nobody takes.
    fprintf(stderr, "--run-ahead does not go with --movie, the debugger's options or "
                    "--shared-picture\n");
    return 2;
  }
  FILE* shown_log = nullptr;
  if (options.shown_log != nullptr) {
    shown_log = fopen(options.shown_log, "w");
    if (shown_log == nullptr) {
      fprintf(stderr, "could not write %s\n", options.shown_log);
      return 2;
    }
  }
  uint64_t last_frame = system->gpu().frame_count();
  // Frames this run produced. gpu().frame_count() is not it: a loaded save state
  // restores the counter the state was made with, so dividing by it understates a
  // per-frame figure by however long the original session had been running.
  const uint64_t first_frame = last_frame;
  int frames = 0;
  uint16_t last_buttons = 0;
  // --shared-picture: how many frames' pictures were handed over on the card, and of those how
  // many with the plane beside them (--planes).
  uint64_t shared_pictures = 0;
  uint64_t shared_planes = 0;
  uint64_t last_shared_serial = 0;
  uint64_t last_pictures = 0;   // --motion-log

  std::unordered_map<uint32_t, uint64_t> pc_counts;

  // Wall clock, so the run can say how fast it actually went. Nothing in
  // this project measured that until now, which made every discussion of
  // optimisation an argument about guesses.
  const clock_t wall_start = clock();

  // A safety net against a genuine infinite loop, not a budget tuned to any
  // real boot - 4 billion turned out to be too tight for its own purpose: a
  // CPU-heavy scene (Silent Hill's first monster encounter, reached via
  // --frames past ~13300) crosses it on totally ordinary per-frame cost,
  // stopping the run short of the frame it asked for and printing a
  // checksum for wherever it happened to be instead - which looked exactly
  // like a hang until raising this uncovered that it was not one.
  const uint64_t kInstructionLimit = 20000000000ull;
  for (uint32_t address : options.breaks)
    system->debugger().AddBreakpoint(address);
  for (const Options::Watch& watch : options.watches)
    system->debugger().AddWatchpoint(watch.address, watch.length, watch.read, watch.write);
  if (options.track_calls)
    system->debugger().SetCallTracking(true);
  const bool debugging =
      !options.breaks.empty() || !options.watches.empty() || options.track_calls;
  uint64_t break_hits = 0;
  // --idle-range's.
  bool was_idle = false;
  uint64_t idle_entered = 0;
  uint64_t idle_left = 0;
  uint64_t idle_instruction = 0;
  int idle_frame = 0;
  uint32_t idle_line = 0;

  // --movie: the next frame's input, and whatever the movie does to the console before it. Once
  // before the first instruction, then on every frame boundary - where the front end sets its
  // pads too.
  auto movie_frame = [&]() {
    const bool was_playing = movie.playing();
    if (!was_playing)
      return;
    const emulation::psx::MoviePlayer::Step step = movie.Next(system->gpu().frame_count());
    if (step.reset) {
      if (!emulation::psx::ResetForMovie(*system, options.bios)) {
        fprintf(stderr, "movie          reset at movie frame %u FAILED\n", movie.frame());
        movie.Stop();
        return;
      }
      // The GPU's count started again with the machine.
      last_frame = system->gpu().frame_count();
      printf("movie          reset at movie frame %u\n", movie.frame());
    }
    if (step.eject) {
      system->EjectDisc();
      printf("movie          lid opened at movie frame %u\n", movie.frame());
    }
    if (step.insert > 0) {
      const char* disc = movie_disc(step.insert);
      if (disc != nullptr && system->LoadDisc(disc))
        printf("movie          disc %d in at movie frame %u: %s\n", step.insert, movie.frame(),
               disc);
      else
        printf("movie          disc %d wanted at movie frame %u and not given (--movie-disc)\n",
               step.insert, movie.frame());
    }
    if (step.finished)
      printf("movie          finished after %d frames of this run\n", frames);
    // Also on the frame it finishes: that lets go of everything it held.
    emulation::psx::ApplyMoviePads(*system, movie);
    fflush(stdout);
  };
  movie_frame();

  // --time-calls: every kernel call's arguments and how many cycles it took to come back, one CSV
  // line each, for the HLE BIOS's timing model (Docs/HLE-BIOS.md).
  struct OpenCall {
    uint32_t vector, number, ra, sp;
    uint32_t a[4];
    uint64_t start;
  };
  std::vector<OpenCall> open_calls;
  FILE* time_calls = nullptr;
  if (options.time_calls != nullptr) {
    time_calls = fopen(options.time_calls, "w");
    if (time_calls == nullptr) {
      fprintf(stderr, "--time-calls: cannot write %s\n", options.time_calls);
      return 2;
    }
    fprintf(time_calls, "vector,number,a0,a1,a2,a3,cycles\n");
    system->kernel().set_observer([&](uint32_t vector, uint32_t number) {
      const emulation::psx::CpuContext* c = system->cpu().context();
      if (open_calls.size() >= 64)
        open_calls.erase(open_calls.begin());
      open_calls.push_back(OpenCall{ vector, number, c->gp.ra, c->gp.sp,
                                     { c->gp.a0, c->gp.a1, c->gp.a2, c->gp.a3 }, c->cycles });
    });
  }
  while (frames < options.frames && instructions < kInstructionLimit) {
    if (options.hot > 0)
      ++pc_counts[system->cpu().context()->pc];
    // --trace-irq arms the trace the moment a hardware interrupt is taken,
    // which is the one event a fixed skip count cannot be aimed at.
    if (options.trace_irq != 0 &&
        system->interrupts_taken() >= options.trace_irq) {
      options.trace_skip = instructions;
      options.trace_irq = 0;
    }
    // --trace-at arms the trace the first time the pc reaches an address, by
    // converting it into an ordinary skip count.
    if (options.trace_at_set &&
        system->cpu().context()->pc == options.trace_at) {
      if (options.trace_at_skip > 0) {
        --options.trace_at_skip;
      } else {
        options.trace_skip = instructions;
        options.trace_at_set = false;
      }
    }
    if (options.trace > 0 && instructions >= options.trace_skip &&
        instructions < options.trace_skip + options.trace) {
      const auto* ctx = system->cpu().context();
      char text[96];
      emulation::psx::Disassemble(ctx->pc, FetchCode(system, ctx->pc), text,
                         sizeof(text));
      // Show the registers this instruction actually reads or writes rather
      // than a fixed set, which is what makes a trace readable.
      const uint32_t code = FetchCode(system, ctx->pc);
      const uint32_t rs = (code >> 21) & 0x1F;
      const uint32_t rt = (code >> 16) & 0x1F;
      const uint32_t rd = (code >> 11) & 0x1F;
      printf("%09llu %08X  %-30s %s=%08X %s=%08X %s=%08X\n",
             static_cast<unsigned long long>(instructions), ctx->pc, text,
             emulation::psx::RegisterName(rs), ctx->gp.reg[rs],
             emulation::psx::RegisterName(rt), ctx->gp.reg[rt],
             emulation::psx::RegisterName(rd), ctx->gp.reg[rd]);
    }
    // With no --break or --watchpoint nothing can arm the debugger, so it is not asked.
    const bool ran = debugging ? system->StepInstruction()
                               : (system->StepInstructionUnarmed(), true);
    if (!ran) {
      // Nothing ran: not an instruction, and not a visit to this address either.
      if (options.hot > 0)
        --pc_counts[system->cpu().context()->pc];
      if (break_hits < static_cast<uint64_t>(options.break_print))
        PrintBreak(system, break_hits + 1, instructions);
      ++break_hits;
      system->debugger().Resume();
      continue;
    }
    ++instructions;

    // --idle-range: the game's work for a frame ends when the pc settles into its idle loop,
    // and starts again when an interrupt's handler hands the processor to something else. A
    // visit of a few instructions is the handler passing through, so only a stay is printed -
    // with where the beam was at each end, and how long the work before it took.
    if (options.idle_hi != 0) {
      const uint32_t pc = system->cpu().context()->pc;
      const bool idle = (pc >= options.idle_lo && pc <= options.idle_hi) ||
                        (options.idle2_hi != 0 && pc >= options.idle2_lo && pc <= options.idle2_hi);
      const uint64_t cycle = system->cpu().context()->cycles;
      if (idle && !was_idle) {
        idle_entered = cycle;
        idle_instruction = instructions;
        idle_frame = frames;
        idle_line = system->gpu().scanline();
      } else if (!idle && was_idle) {
        // 33,868,800 / 59.94 / 263: about 2,148 cycles a scanline.
        if (cycle - idle_entered > 2000) {
          printf("idle           work %7.1f lines, then idle from frame %d line %3u for %7.1f "
                 "lines (flips %u, instruction %llu)\n",
                 (idle_entered - idle_left) / 2148.0, idle_frame, idle_line,
                 (cycle - idle_entered) / 2148.0, system->gpu().stats().gp1_commands[5],
                 static_cast<unsigned long long>(idle_instruction));
          idle_left = cycle;
        }
      }
      was_idle = idle;
    }

    // --time-calls: a call has returned when the pc is back at its return address with the stack
    // it was made with. One that never comes back that way (a longjmp, an exit from inside an
    // exception) is dropped when one made before it returns.
    if (time_calls != nullptr && !open_calls.empty()) {
      const emulation::psx::CpuContext* c = system->cpu().context();
      for (size_t i = open_calls.size(); i-- > 0;) {
        const OpenCall& call = open_calls[i];
        if (c->pc != call.ra || c->gp.sp != call.sp)
          continue;
        fprintf(time_calls, "%02X,%02X,%08X,%08X,%08X,%08X,%llu\n", call.vector, call.number,
                call.a[0], call.a[1], call.a[2], call.a[3],
                static_cast<unsigned long long>(c->cycles - call.start));
        open_calls.resize(i);
        break;
      }
    }

    const uint64_t now = system->gpu().frame_count();
    if (now != last_frame) {
      last_frame = now;
      ++frames;
      // --shared-picture: as a presenter that takes each picture the moment it comes and is
      // done with the one before, which is what frees a texture for the next.
      if (const emulation::psx::SharedPicture& shared = system->gpu().shared_picture()) {
        if (shared.serial != last_shared_serial) {
          ++shared_pictures;
          if (shared.planes != nullptr)
            ++shared_planes;
          last_shared_serial = shared.serial;
          shared.source->Release(shared.serial);
        }
      }
      // --motion-log: a line for each new picture.
      if (options.motion_log && system->gpu().stats().pictures != last_pictures) {
        last_pictures = system->gpu().stats().pictures;
        const auto& picture = system->gpu().last_picture_motion();
        printf("picture        frame %llu: %llu of %llu vertices found%s\n",
               static_cast<unsigned long long>(now),
               static_cast<unsigned long long>(picture.found),
               static_cast<unsigned long long>(picture.looked),
               picture.reset ? ", afresh" : "");
      }
      // Switching CPU under a running machine, which is what the front end's
      // menu does. The setting is all this touches: System::StepInstruction
      // acts on it between instructions, which is the only place it is safe.
      if (options.recompiler_toggle > 0 &&
          (frames % options.recompiler_toggle) == 0) {
        system->config().recompiler = !system->config().recompiler;
      }
      // The same for the GTE's two ways of running a command under the recompiler: a run that
      // switches them every n frames has to come out exactly as one that never does.
      if (options.gte_x64_toggle > 0 && (frames % options.gte_x64_toggle) == 0)
        system->config().gte_x64 = !system->config().gte_x64;
      // --memory-state-every: a state kept in memory, cards and all, saved and loaded straight
      // back - which must leave the run exactly the run without it
      // (Docs/Rewind-and-Run-Ahead-Plan.md, phase 1).
      if (options.memory_state_every > 0 && (frames % options.memory_state_every) == 0) {
        const auto start = std::chrono::steady_clock::now();
        system->SaveMemoryState(&memory_state, /*with_cards=*/true);
        const auto saved = std::chrono::steady_clock::now();
        const std::string error = system->LoadMemoryState(memory_state);
        const auto loaded = std::chrono::steady_clock::now();
        if (!error.empty()) {
          fprintf(stderr, "memory state at frame %d: %s\n", frames, error.c_str());
          return 1;
        }
        ++memory_states;
        memory_state_save_ms += std::chrono::duration<double, std::milli>(saved - start).count();
        memory_state_load_ms += std::chrono::duration<double, std::milli>(loaded - saved).count();
      }
      // Put the disc in while the machine runs, which is how someone reaches
      // the CD player: the BIOS boots the game on any disc that is already
      // there, so the tray has to start empty.
      if (options.insert != nullptr && frames == options.insert_at) {
        if (system->LoadDisc(options.insert))
          printf("inserted       %s at frame %d\n", options.insert, frames);
        else
          printf("inserted       FAILED to open %s\n", options.insert);
        fflush(stdout);
      }
      // A timeline of one run, which is far cheaper than bisecting with many.
      // The checksum going quiet while the counters keep moving is a game
      // waiting for input; everything going quiet is a hang.
      if (options.frame_log > 0 && (frames % options.frame_log) == 0) {
        int w = 0;
        int h = 0;
        const uint32_t* px = system->gpu().framebuffer(w, h);
        // "flips" is GP1(05h), the display area moved: a double-buffered game's own frames, so
        // its change over a stretch against the frames run is the rate the game is managing.
        // "pads" is each port's pad as the game has it, A in analog mode and d in digital: a
        // game that reads no sticks because it switched the pad looks like one out of step.
        printf("frame %-6d %016llx  %6d non-black  %dx%d  "
               "flips %u  mdec %llu  cd %llu  gp0 %llu  pads %c%c\n",
               frames,
               static_cast<unsigned long long>(Checksum(px, w * h)),
               NonBlackPixels(px, w * h), w, h, system->gpu().stats().gp1_commands[5],
               static_cast<unsigned long long>(
                   system->io().mdec.stats().macroblocks),
               static_cast<unsigned long long>(
                   system->cdrom().stats().sectors_read),
               static_cast<unsigned long long>(
                   system->gpu().stats().gp0_words),
               system->sio().analog_mode(0) ? 'A' : 'd',
               system->sio().analog_mode(1) ? 'A' : 'd');
        // Above 1x, the sharper picture's checksum too: what the identity check compares.
        if (options.scale > 1) {
          int sw = 0, sh = 0;
          const uint32_t* shown = system->gpu().picture(sw, sh);
          printf("           shown %016llx  %dx%d\n",
                 static_cast<unsigned long long>(Checksum(shown, sw * sh)), sw, sh);
        }
        fflush(stdout);
      }
      // --ppm-every: the native picture as it stands, named for the frame.
      if (options.ppm_every > 0 && options.ppm != nullptr &&
          (frames % options.ppm_every) == 0) {
        std::string name = options.ppm;
        const size_t dot = name.find_last_of('.');
        const size_t slash = name.find_last_of("/\\");
        const bool has_extension =
            dot != std::string::npos && (slash == std::string::npos || dot > slash);
        char number[16];
        snprintf(number, sizeof(number), "_%06d", frames);
        name.insert(has_extension ? dot : name.size(), number);
        int w = 0;
        int h = 0;
        const uint32_t* px = system->gpu().framebuffer(w, h);
        WriteFrame(name.c_str(), px, w, h);
      }
      // --rewind-test: a state a frame into rewind's store, each made the newest before the next
      // (WaitIdle), so none is replaced; the one n frames before the end also kept whole.
      if (rewind != nullptr) {
        system->SaveMemoryState(&rewind_state, /*with_cards=*/false);
        if (frames == options.frames - options.rewind_test)
          rewind_target = rewind_state.payload;
        rewind->Push(&rewind_state.payload);
        rewind->WaitIdle();
      }
      // --run-ahead: the frame just run is the real one. The machine is kept as it stands, n
      // more frames run on the input the real frame had, the last of their pictures is the one
      // shown, and the machine goes back - as the front end will. So it comes before the
      // buttons change below, and nothing else here sees the frames ahead: everything this run
      // reports is the real frames' (Docs/Rewind-and-Run-Ahead-Plan.md, phase 2).
      if (options.run_ahead > 0 || shown_log != nullptr) {
        if (options.run_ahead > 0) {
          // The real frame's sound first: the SPU's output is not in a state, and what the
          // frames ahead make is thrown away.
          if (options.wav != nullptr) {
            int16_t chunk[2048];
            int got = 0;
            while ((got = system->spu().ReadSamples(
                        chunk, static_cast<int>(sizeof(chunk) / 2 / 2))) > 0)
              audio.insert(audio.end(), chunk, chunk + got * 2);
          }
          const auto start = std::chrono::steady_clock::now();
          system->SaveMemoryState(&ahead_state, /*with_cards=*/true);
          if (textures)
            textures->Hold();   // as the front end's machine does
          const auto saved = std::chrono::steady_clock::now();
          for (int i = 0; i < options.run_ahead; ++i)
            ahead_instructions += bench::RunFrame(system);
          ahead_frames += options.run_ahead;
          ahead_save_ms += std::chrono::duration<double, std::milli>(saved - start).count();
        }
        int w = 0;
        int h = 0;
        const uint32_t* px = system->gpu().framebuffer(w, h);
        if (shown_log != nullptr)
          fprintf(shown_log, "%d %016llx %d\n", frames,
                  static_cast<unsigned long long>(Checksum(px, w * h)), NonBlackPixels(px, w * h));
        if (options.run_ahead > 0) {
          int16_t discard[2048];
          while (system->spu().ReadSamples(discard, static_cast<int>(sizeof(discard) / 2 / 2)) > 0) {
          }
          const auto start = std::chrono::steady_clock::now();
          const std::string error = system->LoadMemoryState(ahead_state, /*continuing=*/true);
          if (textures)
            textures->Release();
          ahead_load_ms += std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now() - start).count();
          if (!error.empty()) {
            fprintf(stderr, "run-ahead at frame %d: %s\n", frames, error.c_str());
            return 1;
          }
        }
      }
      // Buttons change on the frame boundary, which is where the front end
      // samples them too.
      if (!options.presses.empty()) {
        const uint16_t buttons = ButtonsOnFrame(options.presses, frames);
        if (buttons != last_buttons) {
          if (options.justifier) {
            using emulation::psx::Sio;
            system->sio().set_guncon(0, (buttons & Sio::kSquare) != 0,
                                     (buttons & Sio::kStart) != 0, (buttons & Sio::kCross) != 0,
                                     options.justifier_x, options.justifier_y);
          } else {
            system->sio().set_buttons(0, buttons);
          }
          last_buttons = buttons;
        }
      }
      movie_frame();
      // Drain the SPU once a frame, exactly as the front end does. Without
      // this the mixer's buffer simply fills and drops, and nothing about the
      // audio can be checked at all.
      if (options.wav != nullptr) {
        int16_t chunk[2048];
        int got = 0;
        while ((got = system->spu().ReadSamples(
                    chunk, static_cast<int>(sizeof(chunk) / 2 / 2))) > 0) {
          audio.insert(audio.end(), chunk, chunk + got * 2);
        }
      }
      if (!options.quiet && (frames % 100) == 0) {
        int width = 0, height = 0;
        const uint32_t* pixels = system->gpu().framebuffer(width, height);
        printf("frame %5d  %dx%d  checksum %016llx  lit %d\n",
               frames, width, height,
               static_cast<unsigned long long>(Checksum(pixels, width * height)),
               NonBlackPixels(pixels, width * height));
        fflush(stdout);
      }
    }
  }

  int width = 0, height = 0;
  const uint32_t* pixels = system->gpu().framebuffer(width, height);
  const int lit = NonBlackPixels(pixels, width * height);

  printf("\n");
  printf("instructions   %llu\n", static_cast<unsigned long long>(instructions));
  printf("frames         %d\n", frames);
  if (options.movie != nullptr) {
    printf("movie          %s at frame %u of %u\n", movie.playing() ? "playing" : "over",
           movie.frame(), movie.movie().frames());
  }
  if (!options.breaks.empty() || !options.watches.empty()) {
    printf("breaks         %llu halts, %llu printed\n",
           static_cast<unsigned long long>(break_hits),
           static_cast<unsigned long long>(
               std::min<uint64_t>(break_hits, static_cast<uint64_t>(options.break_print))));
  }
  if (options.track_calls) {
    const auto& stack = system->debugger().call_stack();
    printf("call stack     %zu deep at the end", stack.size());
    for (size_t i = stack.size(); i-- > 0 && stack.size() - i <= 4;)
      printf("%s%08X", i + 1 == stack.size() ? ": " : " < ", stack[i].target);
    printf("\n");
  }
  for (const auto& wp : system->debugger().watchpoints()) {
    printf("watchpoint     %08X+%u %s%s: %llu accesses\n", wp.address, wp.length,
           wp.read ? "r" : "", wp.write ? "w" : "", static_cast<unsigned long long>(wp.hits));
  }
  if (system->recompiler_enabled()) {
    const emulation::rec::Recompiler::Stats& rec = system->recompiler()->stats();
    printf("rec blocks     %llu compiled, %llu entries, %llu invalidated\n",
           static_cast<unsigned long long>(rec.blocks_compiled),
           static_cast<unsigned long long>(rec.blocks_executed),
           static_cast<unsigned long long>(rec.blocks_invalidated));
    printf("rec inst       %llu compiled, %llu interpreted, %llu faults\n",
           static_cast<unsigned long long>(rec.instructions_compiled),
           static_cast<unsigned long long>(rec.instructions_interpreted),
           static_cast<unsigned long long>(rec.faults));
    printf("rec links      %llu made, %llu broken, %llu chains carried on past one not made\n",
           static_cast<unsigned long long>(rec.links_made),
           static_cast<unsigned long long>(rec.links_broken),
           static_cast<unsigned long long>(rec.chains_carried_on));
    printf("rec ram        %llu loads read RAM directly\n",
           static_cast<unsigned long long>(rec.ram_reads_direct));
    if (const emulation::rec::GteCompiler* gte = system->recompiler()->gte_compiler()) {
      if (system->config().gte_x64)
        printf("rec gte x64    %zu command words emitted, %zu bytes\n", gte->functions(), gte->bytes());
    }
  }
  {
    const double seconds =
        static_cast<double>(clock() - wall_start) / CLOCKS_PER_SEC;
    // A PSX runs 33,868,800 cycles a second, so the emulated second count is
    // the cycles executed divided by that. Speed is emulated time over real
    // time: 1.00 is real speed, and under it is too slow to play.
    const double emulated =
        static_cast<double>(system->cpu().context()->cycles) / 33868800.0;
    printf("speed          %.2fs emulated in %.2fs wall = %.2fx real time,"
           " %.1f fps\n",
           emulated, seconds, seconds > 0.0 ? emulated / seconds : 0.0,
           seconds > 0.0 ? frames / seconds : 0.0);
  }
  if (memory_states > 0)
    printf("memory states  %llu saved and loaded back, %.3f ms a save, %.3f ms a load\n",
           static_cast<unsigned long long>(memory_states), memory_state_save_ms / memory_states,
           memory_state_load_ms / memory_states);
  if (options.run_ahead > 0 && frames > 0)
    printf("run-ahead      %d frames ahead of each: %llu frames and %llu steps run ahead, "
           "%.3f ms a save, %.3f ms a load\n",
           options.run_ahead, static_cast<unsigned long long>(ahead_frames),
           static_cast<unsigned long long>(ahead_instructions), ahead_save_ms / frames,
           ahead_load_ms / frames);
  if (shown_log != nullptr) {
    fclose(shown_log);
    printf("wrote          %s (the picture shown, frame by frame)\n", options.shown_log);
  }
  printf("resolution     %dx%d\n", width, height);
  printf("display        vram (%u,%u) %dx%d, %s\n",
         system->gpu().display_vram_x(), system->gpu().display_vram_y(),
         width, height,
         system->gpu().display_disabled() ? "DISABLED" : "enabled");
  printf("crtc           hdisp %u-%u gpu clocks, vdisp %u-%u lines, GPUSTAT=%08x\n",
         system->gpu().horizontal_display_start(),
         system->gpu().horizontal_display_end(),
         system->gpu().vertical_display_start(),
         system->gpu().vertical_display_end(),
         system->gpu().status_raw());
  printf("checksum       %016llx\n",
         static_cast<unsigned long long>(Checksum(pixels, width * height)));
  printf("non-black      %d of %d pixels\n", lit, width * height);
  printf("unimplemented  %llu paths hit,  %llu RFEs executed\n",
         static_cast<unsigned long long>(TrapCounter::count),
         static_cast<unsigned long long>(TrapCounter::rfe_count));

  const auto* ctx = system->cpu().context();
  if (emulation::psx::TrapCounter::site_count != 0) {
    printf("unimplemented paths\n");
    for (uint32_t i = 0; i < emulation::psx::TrapCounter::site_count; ++i) {
      const emulation::psx::TrapCounter::Site& site =
          emulation::psx::TrapCounter::sites[i];
      printf("  %s line %d, %llu hits", site.file, site.line,
             static_cast<unsigned long long>(site.hits));
      if (site.detail != 0)
        printf(", first %08X", site.detail);
      printf("\n");
    }
  }
  printf("interrupts     %llu taken, %llu blocked, IE on for %llu insns\n",
         static_cast<unsigned long long>(system->interrupts_taken()),
         static_cast<unsigned long long>(system->interrupts_blocked()),
         static_cast<unsigned long long>(system->instructions_with_ie()));
  if (options.justifier)
    printf("justifier      %llu IRQ10s fired as the beam passed its spot\n",
           static_cast<unsigned long long>(system->gpu().lightgun_irqs()));
  printf("               I_STAT=%08X I_MASK=%08X SR=%08X\n",
         system->io().io.interrupt_stat, system->io().io.interrupt_mask,
         ctx->ctrl.SR.raw);
  // Interrupts that landed on a GTE command, which the hardware issues before
  // it recognises the interrupt. Each one is a command the emulator used to
  // drop on the floor - see System::StepInstruction.
  printf("               %llu delivered behind a GTE command\n",
         static_cast<unsigned long long>(
             system->interrupts_after_gte_command()));

  const emulation::psx::Cdrom::Stats& cd_stats = system->cdrom().stats();
  printf("cdrom          %llu commands (last %02X), %llu sectors, "
         "%llu interrupts, %llu unknown\n",
         static_cast<unsigned long long>(cd_stats.commands),
         cd_stats.last_command,
         static_cast<unsigned long long>(cd_stats.sectors_read),
         static_cast<unsigned long long>(cd_stats.interrupts),
         static_cast<unsigned long long>(cd_stats.unknown_commands));
  if (cd_stats.cdda_sectors != 0 || cd_stats.cdda_failures != 0) {
    printf("cdda           %llu sectors played, %llu could not be read\n",
           static_cast<unsigned long long>(cd_stats.cdda_sectors),
           static_cast<unsigned long long>(cd_stats.cdda_failures));
  }
  if (cd_stats.xa_sectors != 0 || cd_stats.xa_filtered != 0) {
    printf("               %llu XA audio sectors decoded, %llu filtered out\n",
           static_cast<unsigned long long>(cd_stats.xa_sectors),
           static_cast<unsigned long long>(cd_stats.xa_filtered));
  }

  {
    static const char* kCdNames[256] = {
      0,"Getstat","Setloc","Play","Forward","Backward","ReadN","MotorOn",
      "Stop","Pause","Init","Mute","Demute","Setfilter","Setmode","Getparam",
      "GetlocL","GetlocP","SetSession","GetTN","GetTD","SeekL","SeekP",0,
      0,"Test","GetID","ReadS","Reset","GetQ","ReadTOC"
    };
    const emulation::psx::Cdrom::Stats& c = system->cdrom().stats();
    bool any = false;
    for (int i = 0; i < 256; ++i) {
      if (c.issued[i] == 0)
        continue;
      if (!any) { printf("\ncdrom commands issued\n"); any = true; }
      printf("  %02X  %8u  %s\n", i, c.issued[i],
             (i < 31 && kCdNames[i]) ? kCdNames[i] : "");
    }
    static const char* kIntNames[8] = {
      "none", "INT1 data ready", "INT2 complete", "INT3 acknowledge",
      "INT4 data end", "INT5 error", "", ""
    };
    for (int i = 0; i < 8; ++i) {
      if (c.delivered[i] == 0)
        continue;
      printf("      %8u  %s\n", c.delivered[i], kIntNames[i]);
    }
    if (any) printf("\n");

    if (c.event_count > 0) {
      printf("cdrom seeks and sectors (%u events%s)\n", c.event_count,
             c.event_count > emulation::psx::Cdrom::Stats::kEventCapacity
                 ? ", last 4000 shown" : "");

      const uint32_t cd_first =
          (c.event_count > emulation::psx::Cdrom::Stats::kEventCapacity)
              ? c.event_count - emulation::psx::Cdrom::Stats::kEventCapacity : 0;
      for (uint32_t i = cd_first; i < c.event_count; ++i) {
        const auto& e = c.events[i % emulation::psx::Cdrom::Stats::kEventCapacity];
        switch (e.kind) {
          case 0:
            printf("  %9.2f ms  setloc   lba %6u  (sector %6d)\n", e.cycle / 33868.8,
                   e.lba, static_cast<int>(e.lba) - 150);
            break;
          case 2:
            printf("  %9.2f ms  setmode  %02X  (%s, %s)\n", e.cycle / 33868.8, e.mode,
                   (e.mode & 0x80) ? "2x" : "1x",
                   (e.mode & 0x20) ? "whole sector 0x924" : "data only 0x800");
            break;
          case 3: {
            const char* name =
                e.mode == 0x06 ? "ReadN" : e.mode == 0x09 ? "Pause" :
                e.mode == 0x15 ? "SeekL" : e.mode == 0x1B ? "ReadS" :
                e.mode == 0x0A ? "Init"  : e.mode == 0x08 ? "Stop"  :
                e.mode == 0x03 ? "Play"  : e.mode == 0x16 ? "SeekP" : "?";
            printf("  %9.2f ms  %-8s at lba %6u  (previous sector %u/%u taken)\n",
                   e.cycle / 33868.8, name, e.lba, e.consumed, e.size);
            break;
          }
          default:
            // A sector whose predecessor was not fully taken is a dropped one.
            printf("  %9.2f ms  sector   lba %6u  (sector %6d)  mode %02X"
                   "  previous %u/%u%s\n",
                   e.cycle / 33868.8, e.lba, static_cast<int>(e.lba) - 150, e.mode, e.consumed,
                   e.size,
                   (e.size != 0 && e.consumed < e.size) ? "  DROPPED" : "");
            break;
        }
      }
      printf("\n");
    }
  }

  {
    const emulation::psx::Kernel::Stats& k = system->kernel().stats();
    printf("bios calls     %llu total\n",
           static_cast<unsigned long long>(k.total));
    struct Row { const char* table; const uint32_t* counts; };
    const Row rows[3] = { { "A0", k.a0 }, { "B0", k.b0 }, { "C0", k.c0 } };
    for (int r = 0; r < 3; ++r) {
      for (int i = 0; i < 256; ++i) {
        if (rows[r].counts[i] < (system->hle_active() ? 1u : 1000u))
          continue;   // only the ones called enough to matter
        printf("  %s(%02X)  %10u\n", rows[r].table, i, rows[r].counts[i]);
      }
    }
    if (k.tty_length != 0) {
      // What the BIOS printed to its serial console, verbatim. Control
      // characters other than newline are shown as dots so the layout of the
      // real message survives.
      printf("\nbios console   %u characters\n", k.tty_length);
      printf("  | ");
      for (uint32_t i = 0; i < k.tty_length; ++i) {
        const char c = k.tty[i];
        if (c == '\n')
          printf("\n  | ");
        else if (c >= 0x20 && c < 0x7F)
          putchar(c);
        else if (c != '\r')
          putchar('.');
      }
      printf("\n");
    }
  }

  if (system->hle_active() && !system->hle().messages().empty()) {
    printf("\nhle bios       %zu message(s)\n", system->hle().messages().size());
    for (const std::string& message : system->hle().messages())
      printf("  %s\n", message.c_str());
  }

  if (options.watch_ram != 0) {
    emulation::psx::Cpu& cpu = system->cpu();
    printf("\nwrites to %08X   %u total%s\n", options.watch_ram,
           cpu.watch_count_,
           cpu.watch_count_ > emulation::psx::Cpu::kWatchCapacity
               ? ", last 4000 shown" : "");
    const uint32_t first =
        (cpu.watch_count_ > emulation::psx::Cpu::kWatchCapacity)
            ? cpu.watch_count_ - emulation::psx::Cpu::kWatchCapacity : 0;
    for (uint32_t i = first; i < cpu.watch_count_; ++i) {
      const emulation::psx::Cpu::Watch& w =
          cpu.watch_[i % emulation::psx::Cpu::kWatchCapacity];
      printf("  pc %08X  %u-byte write of %08X to %08X\n", w.pc, w.size,
             w.value, w.address);
    }
  }
  {
    const emulation::psx::Dma::Stats& d = system->io().dma.stats();
    for (int ch = 0; ch < 7; ++ch) {
      if (d.counts[ch] == 0)
        continue;
      printf("\ndma channel %d   %u transfers%s\n", ch, d.counts[ch],
             d.counts[ch] > emulation::psx::Dma::kTransferCapacity
                 ? ", last 3000 shown" : "");
      // Oldest first within the ring, so the list still reads forwards.
      const uint32_t first_index =
          (d.counts[ch] > emulation::psx::Dma::kTransferCapacity)
              ? d.counts[ch] - emulation::psx::Dma::kTransferCapacity : 0;
      for (uint32_t i = first_index; i < d.counts[ch]; ++i) {
        const emulation::psx::Dma::Transfer& t =
            d.transfers[ch][i % emulation::psx::Dma::kTransferCapacity];
        printf("  chcr %08X  bcr %08X  madr %08X  %6u words -> %08X"
               "  lba %u  first %08X  pc %08X\n",
               t.chcr, t.bcr, t.madr, t.words, t.end, t.lba, t.first, t.pc);
      }
    }
  }


  {
    static const char* kNames[11] = {
      "vblank", "gpu", "cdrom", "dma", "timer0", "timer1",
      "timer2", "pad/card", "sio", "spu", "lightpen",
    };
    const uint64_t* by_source = system->interrupts_taken_by_source();
    printf("vblank irqs    %llu raised by the gpu\n",
           static_cast<unsigned long long>(system->gpu().frame_count()));
    printf("interrupts by source\n");
    for (int i = 0; i < 11; ++i) {
      if (by_source[i] != 0)
        printf("  %-9s %10llu\n", kNames[i],
               static_cast<unsigned long long>(by_source[i]));
    }
  }
  {
    // Root counter state. VSync measures its own timeout against counter 1, so
    // a counter running at the wrong rate makes the BIOS give up waiting for a
    // frame that was going to arrive on time.
    printf("\nroot counters\n");
    static const char* kSources[3][4] = {
      { "sysclock", "dotclock", "sysclock", "dotclock" },
      { "sysclock", "hblank",   "sysclock", "hblank"   },
      { "sysclock", "sysclock", "sysclock/8", "sysclock/8" },
    };
    for (int i = 0; i < 3; ++i) {
      emulation::psx::RootCounter& rc = system->io().rootcounter_[i];
      printf("  %d  mode %04X  count %5u  target %5u  src %-10s %s%s%s\n",
             i, rc.mode.raw & 0xFFFF, rc.counter, rc.target,
             kSources[i][rc.mode.clcsrc],
             rc.mode.en ? "" : "[disabled] ",
             rc.mode.irq_target ? "irq-on-target " : "",
             rc.mode.irq_0xffff ? "irq-on-wrap" : "");
    }
  }
  const emulation::psx::Spu::Stats& spu_stats = system->spu().stats();
  printf("spu            %llu frames, %llu key-ons, %llu blocks, peak %d/%d\n",
         static_cast<unsigned long long>(spu_stats.frames),
         static_cast<unsigned long long>(spu_stats.key_ons),
         static_cast<unsigned long long>(spu_stats.blocks_decoded),
         spu_stats.peak_left, spu_stats.peak_right);
  // What software asked for, next to what came out. A voice that measures in
  // tune says nothing about the register behind it, and a mix that is wrong
  // in the balance between its parts is invisible in any single number.
  if (spu_stats.key_ons != 0) {
    printf("spu requests   max pitch %04Xh (%.2fx), %llu writes over 3FFFh; "
           "volumes %llu sweeps / %llu levels\n",
           spu_stats.max_pitch, spu_stats.max_pitch / 4096.0,
           static_cast<unsigned long long>(spu_stats.pitch_writes_over_3fff),
           static_cast<unsigned long long>(spu_stats.volume_sweeps),
           static_cast<unsigned long long>(spu_stats.volume_levels));
    printf("spu modes      pmon %06X  noise %06X  reverb %06X  control %04X"
           " (reverb %s, unmute %s)\n",
           spu_stats.pitch_modulation_seen, spu_stats.noise_seen,
           spu_stats.reverb_seen, spu_stats.control_seen,
           (spu_stats.control_seen & 0x0080) ? "on" : "off",
           (spu_stats.control_seen & 0x4000) ? "on" : "off");
  }
  if (spu_stats.cd_samples_in != 0 || spu_stats.cd_samples_out != 0) {
    printf("cd audio       %llu CD-DA pairs in (peak %d), %llu mixed"
           " (peak %d after volume), %llu dropped, %llu muted\n",
           static_cast<unsigned long long>(spu_stats.cd_samples_in),
           spu_stats.cd_peak,
           static_cast<unsigned long long>(spu_stats.cd_samples_out),
           spu_stats.cd_out_peak,
           static_cast<unsigned long long>(spu_stats.cd_samples_dropped),
           static_cast<unsigned long long>(spu_stats.cd_frames_muted));
  }

  {
    const emulation::psx::Mdec::Stats& m = system->io().mdec.stats();
    if (m.commands != 0) {
      printf("mdec           %llu commands, %llu macroblocks, %llu words in, "
             "%llu out\n",
             static_cast<unsigned long long>(m.commands),
             static_cast<unsigned long long>(m.macroblocks),
             static_cast<unsigned long long>(m.words_in),
             static_cast<unsigned long long>(m.words_out));
      if (m.unknown_commands != 0 || m.short_blocks != 0 || m.overflows != 0) {
        printf("               %llu unknown, %llu short blocks, %llu overflows\n",
               static_cast<unsigned long long>(m.unknown_commands),
               static_cast<unsigned long long>(m.short_blocks),
               static_cast<unsigned long long>(m.overflows));
      }
    }
  }
  const emulation::psx::Gte::Stats& gte_stats = system->gte().stats();
  printf("gte            %llu commands, %llu unrecognised\n",
         static_cast<unsigned long long>(gte_stats.commands),
         static_cast<unsigned long long>(gte_stats.unknown_commands));

  const emulation::psx::Gpu::Stats& gpu_stats = system->gpu().stats();
  printf("gpu            %llu GP0 words, %llu GP1 words\n",
         static_cast<unsigned long long>(gpu_stats.gp0_words),
         static_cast<unsigned long long>(gpu_stats.gp1_words));
  printf("               %llu primitives, %llu pixels plotted\n",
         static_cast<unsigned long long>(gpu_stats.primitives),
         static_cast<unsigned long long>(gpu_stats.pixels));
  // What the rasteriser was charged, against what a frame actually holds: it
  // gets through two draw ticks a CPU cycle and a frame is 1/59.94 of a second,
  // so a little over 1,130,000 of them (bug 141; before that they were paid at
  // the 53.2 MHz video clock, 888,000 a frame). Over that, on average, and the
  // GPU cannot keep up with what the game is asking for and the game waits
  // (bugs 85, 86).
  const double kTicksPerFrame =
      33868800.0 * emulation::psx::Gpu::kDrawTicksPerCycle / 59.94;
  const uint64_t frames_drawn = system->gpu().frame_count() - first_frame;
  printf("               %llu draw ticks, %.0f a frame of %.0f (%.0f%% of the "
         "GPU's time)\n",
         static_cast<unsigned long long>(gpu_stats.draw_ticks),
         frames_drawn ? static_cast<double>(gpu_stats.draw_ticks) / frames_drawn : 0.0,
         kTicksPerFrame,
         frames_drawn ? 100.0 * static_cast<double>(gpu_stats.draw_ticks) /
                            (frames_drawn * kTicksPerFrame) : 0.0);
  if (gpu_stats.transfer_ticks > 0) {
    printf("               %llu of the draw ticks were transfers\n",
           static_cast<unsigned long long>(gpu_stats.transfer_ticks));
  }
  if (system->cpu().icache_hits() + system->cpu().icache_misses() +
      system->cpu().uncached_fetches() > 0) {
    printf("icache         %llu hits, %llu misses, %llu uncached fetches\n",
           static_cast<unsigned long long>(system->cpu().icache_hits()),
           static_cast<unsigned long long>(system->cpu().icache_misses()),
           static_cast<unsigned long long>(system->cpu().uncached_fetches()));
  }
  if (gpu_stats.raster_jobs > 0) {
    printf("               %llu jobs to the rasteriser, %llu barriers waited\n",
           static_cast<unsigned long long>(gpu_stats.raster_jobs),
           static_cast<unsigned long long>(gpu_stats.raster_waits));
    printf("               the rasteriser's thread drew for %.2f s\n",
           static_cast<double>(gpu_stats.raster_busy_ns) / 1e9);
  }
  printf("               GP0 queue peaked at %u words, %llu dropped\n",
         gpu_stats.queue_peak,
         static_cast<unsigned long long>(gpu_stats.queue_overflows));
  printf("               %llu clipped, %llu mask-rejected, %llu transparent, %llu field-skipped\n",
         static_cast<unsigned long long>(gpu_stats.clipped),
         static_cast<unsigned long long>(gpu_stats.mask_rejected),
         static_cast<unsigned long long>(gpu_stats.transparent_texels),
         static_cast<unsigned long long>(gpu_stats.field_skipped));
  printf("               texels by depth: %llu 4-bit, %llu 8-bit, %llu 15-bit\n",
         static_cast<unsigned long long>(gpu_stats.texels_by_depth[0]),
         static_cast<unsigned long long>(gpu_stats.texels_by_depth[1]),
         static_cast<unsigned long long>(gpu_stats.texels_by_depth[2]));
  if (system->gpu().hardware_raster())
    printf("               (the hardware rasteriser keeps no pixel, clip or texel counts)\n");
  if (options.pgxp)
    printf("pgxp           %llu of %llu polygon vertices drawn from the GTE's unrounded positions "
           "(%llu found by value in its cache)\n",
           static_cast<unsigned long long>(gpu_stats.precise_vertices),
           static_cast<unsigned long long>(gpu_stats.polygon_vertices),
           static_cast<unsigned long long>(gpu_stats.recalled_vertices));
  if (options.shared_picture)
    printf("shared         %llu of %d frames' pictures handed over on the card\n",
           static_cast<unsigned long long>(shared_pictures), frames);
  if (options.shared_picture && options.planes)
    printf("planes         %llu of them with the plane beside\n",
           static_cast<unsigned long long>(shared_planes));
  if (textures) {
    using emulation::psx::TextureTracker;
    system->gpu().vram();   // waits for the rasteriser: every job counted
    textures->Flush();      // every source not yet written, written
    const TextureTracker::Counts& t = textures->counts();
    auto share = [&](uint64_t part) {
      return t.textured_pixels == 0
                 ? 0.0
                 : 100.0 * static_cast<double>(part) / static_cast<double>(t.textured_pixels);
    };
    printf("textures       %llu uploads, %llu more joined to them, %llu patching them; records "
           "%llu drawn over, %llu uploaded over, %llu cut, %llu copied, %llu live; %llu textures "
           "named, %llu written\n",
           static_cast<unsigned long long>(t.uploads),
           static_cast<unsigned long long>(t.uploads_joined),
           static_cast<unsigned long long>(t.uploads_patched),
           static_cast<unsigned long long>(t.records_retired),
           static_cast<unsigned long long>(t.records_replaced),
           static_cast<unsigned long long>(t.records_cut),
           static_cast<unsigned long long>(t.records_copied),
           static_cast<unsigned long long>(t.records_live),
           static_cast<unsigned long long>(t.sources), static_cast<unsigned long long>(t.dumped));
    printf("               %llu textured pixels in %llu jobs (%llu named by several records):\n",
           static_cast<unsigned long long>(t.textured_pixels),
           static_cast<unsigned long long>(t.textured_jobs),
           static_cast<unsigned long long>(t.composite_jobs));
    for (int r = 0; r < TextureTracker::kReasons; ++r)
      printf("                 %5.1f%%  %-28s (%llu jobs)\n", share(t.pixels[r]),
             TextureTracker::ReasonName(static_cast<TextureTracker::Reason>(r)),
             static_cast<unsigned long long>(t.jobs[r]));
    if (t.dumped > 0)
      printf("               written: %.1f megatexels, %.1f%% of them read and filled in\n",
             static_cast<double>(t.dumped_texels) / 1e6,
             100.0 * static_cast<double>(t.dumped_used) / static_cast<double>(t.dumped_texels));
    if (t.dumped_merged > 0)
      printf("               %llu files already there had cells added to them\n",
             static_cast<unsigned long long>(t.dumped_merged));
    if (t.dump_failures > 0)
      printf("               %llu could not be written\n",
             static_cast<unsigned long long>(t.dump_failures));
#ifdef PSXEMU_HW_RASTER
    // The upscaler's queue finished: everything the run read upscaled.
    if (ai_textures) {
      ai_textures->Drain();
      ReportAi(*ai_textures, std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                                           ai_started).count());
    }
#endif
    if (pack) {
      const emulation::psx::TexturePack::Counts p = pack->counts();
      printf("replaced       %llu jobs drawn with a replacement (%llu the upscaler's, %llu images "
             "handed over); %llu named one the pack has not, %llu one it leaves alone; %llu "
             "images read, %llu unusable, %llu let go\n",
             static_cast<unsigned long long>(p.jobs_replaced),
             static_cast<unsigned long long>(p.jobs_ai),
             static_cast<unsigned long long>(p.provided),
             static_cast<unsigned long long>(p.jobs_unmatched),
             static_cast<unsigned long long>(p.jobs_never),
             static_cast<unsigned long long>(p.loaded), static_cast<unsigned long long>(p.bad),
             static_cast<unsigned long long>(p.dropped));
      for (const std::string& warning : pack->warnings())
        printf("               %s\n", warning.c_str());
    }
  }
  if (options.planes) {
    auto percent = [](uint64_t part, uint64_t whole) {
      return whole == 0 ? 0.0 : 100.0 * static_cast<double>(part) / static_cast<double>(whole);
    };
    printf("motion         %llu new pictures, %llu starting afresh; vertices found in the last "
           "picture %llu of %llu (%.1f%%, by %s), 2D primitives %llu of %llu (%.1f%%)\n",
           static_cast<unsigned long long>(gpu_stats.pictures),
           static_cast<unsigned long long>(gpu_stats.picture_resets),
           static_cast<unsigned long long>(gpu_stats.motion_vertices_found),
           static_cast<unsigned long long>(gpu_stats.motion_vertices),
           percent(gpu_stats.motion_vertices_found, gpu_stats.motion_vertices),
           options.motion_key == emulation::psx::Gte::MotionKey::kOrder     ? "order"
           : options.motion_key == emulation::psx::Gte::MotionKey::kAddress ? "address"
                                                                         : "model",
           static_cast<unsigned long long>(gpu_stats.motion_sprites_found),
           static_cast<unsigned long long>(gpu_stats.motion_sprites),
           percent(gpu_stats.motion_sprites_found, gpu_stats.motion_sprites));
    // 480 lines interlaced: how many new pictures were, how many of them were drawn a field at
    // a time, and how many of those were half the last field's - not filled (Gpu::MixesFields).
    if (gpu_stats.pictures_interlaced != 0)
      printf("interlaced     %llu of the new pictures 480 lines interlaced, %llu drawn a field "
             "at a time, %llu of those half the last field's\n",
             static_cast<unsigned long long>(gpu_stats.pictures_interlaced),
             static_cast<unsigned long long>(gpu_stats.pictures_one_field),
             static_cast<unsigned long long>(gpu_stats.pictures_mixed));
  }
  if (options.motion) {
    // Per pixel and channel, in 8-bit steps: how far each new picture was from the last one
    // moved by its motion, and left still. Below 1.0 the motion helps.
    const double samples = static_cast<double>(gpu_stats.warp_pixels) * 3.0;
    const double moved = samples > 0 ? gpu_stats.warp_error_moved / samples : 0.0;
    const double still = samples > 0 ? gpu_stats.warp_error_still / samples : 0.0;
    printf("warp           %llu pictures: moved by their motion %.3f from the next, left still "
           "%.3f, ratio %.3f; motion known for %.1f%% of pixels\n",
           static_cast<unsigned long long>(gpu_stats.warp_pictures), moved, still,
           still > 0.0 ? moved / still : 0.0,
           gpu_stats.warp_pixels == 0
               ? 0.0
               : 100.0 * static_cast<double>(gpu_stats.warp_moved_pixels) /
                     static_cast<double>(gpu_stats.warp_pixels));
  }

  if (gpu_stats.setup_count > 0) {
    printf("\nfirst textured primitives\n");
    static const char* kDepths[4] = { "4-bit CLUT", "8-bit CLUT", "15-bit direct",
                                      "15-bit (reserved)" };
    for (uint32_t i = 0; i < gpu_stats.setup_count; ++i) {
      const auto& s = gpu_stats.setups[i];
      printf("  %02X  page=%04X clut=%04X  texpage (%4u,%3u)  %-14s "
             "clut (%4u,%3u)  semi %u%s%s\n",
             s.command, s.raw_page, s.raw_clut, s.texpage_x, s.texpage_y,
             kDepths[s.colors & 3], s.clut_x, s.clut_y, s.semi_mode,
             (s.flags & 1) ? " raw" : "",
             (s.flags & 2) ? " semi-transparent" : "");
    }
  }

  printf("\ngp0 commands issued\n");
  for (int i = 0; i < 256; ++i) {
    if (gpu_stats.gp0_commands[i] == 0)
      continue;
    const char* what = "";
    if (i == 0x02) what = "fill rectangle";
    else if (i < 0x20) what = "nop / clear cache";
    else if (i < 0x40) what = "polygon";
    else if (i < 0x60) what = "line";
    else if (i < 0x80) what = "rectangle";
    else if (i < 0xA0) what = "vram to vram";
    else if (i < 0xC0) what = "cpu to vram";
    else if (i < 0xE0) what = "vram to cpu";
    else if (i == 0xE1) what = "draw mode";
    else if (i == 0xE2) what = "texture window";
    else if (i == 0xE3) what = "draw area top-left";
    else if (i == 0xE4) what = "draw area bottom-right";
    else if (i == 0xE5) what = "draw offset";
    else if (i == 0xE6) what = "mask setting";
    printf("  %02X  %8u  %s", i, gpu_stats.gp0_commands[i], what);
    if (i >= 0x20 && i < 0x80) {
      // Spell out the attribute bits, which is where a primitive that draws
      // nothing usually differs from one that draws.
      printf(" [");
      if (i < 0x40) {
        printf("%s %s", (i & 0x10) ? "gouraud" : "flat",
               (i & 0x08) ? "quad" : "tri");
        if (i & 0x04) printf(" textured");
      } else if (i < 0x60) {
        printf("%s", (i & 0x10) ? "gouraud" : "flat");
        if (i & 0x08) printf(" polyline");
      } else {
        static const char* kSizes[4] = { "variable", "1x1", "8x8", "16x16" };
        printf("%s", kSizes[(i >> 3) & 3]);
        if (i & 0x04) printf(" textured");
      }
      if (i & 0x02) printf(" semi-transparent");
      if ((i & 0x01) && (i >= 0x24)) printf(" raw");
      printf("]");
    }
    printf("\n");
  }

  if (gpu_stats.watch_writes > 0) {
    printf("\nwrites into the watched VRAM area (%llu total)\n",
           static_cast<unsigned long long>(gpu_stats.watch_writes));
    for (int i = 0; i < 256; ++i) {
      if (gpu_stats.watch_writers[i] == 0)
        continue;
      printf("  %02X  %8u\n", i, gpu_stats.watch_writers[i]);
    }
  }

  if (gpu_stats.transfer_log_count > 0) {
    printf("\ncpu-to-vram transfers\n");
    for (uint32_t i = 0; i < gpu_stats.transfer_log_count; ++i) {
      const auto& t = gpu_stats.transfers[i];
      const uint32_t expected = static_cast<uint32_t>(t.w) * t.h;
      printf("  to (%4u,%3u)  %3ux%-3u  %6u of %6u pixels%s\n", t.x, t.y,
             t.w, t.h, t.written, expected,
             (t.written == expected) ? "" : "   SHORT");
    }
  }

  printf("\ngp1 commands issued\n");
  for (int i = 0; i < 64; ++i) {
    if (gpu_stats.gp1_commands[i] == 0)
      continue;
    static const char* kNames[] = {
      "reset", "reset fifo", "ack irq", "display enable", "dma direction",
      "display area", "horizontal range", "vertical range", "display mode"
    };
    printf("  %02X  %8u  %s\n", i, gpu_stats.gp1_commands[i],
           (i < 9) ? kNames[i] : (i == 0x10 ? "get gpu info" : ""));
  }

  {
    // The Cop0 status history. With only a handful of exceptions in a whole
    // run, the order they happened in says more than any counter.
    using emulation::psx::ExceptionLog;
    const uint32_t total = ExceptionLog::written;
    const uint32_t shown =
        (total < ExceptionLog::kCapacity) ? total : ExceptionLog::kCapacity;
    printf("\ncop0 status history (%u events", total);
    if (total > shown)
      printf(", last %u shown", shown);
    printf(")\n");
    for (uint32_t i = total - shown; i < total; ++i) {
      const ExceptionLog::Entry& e =
          ExceptionLog::entries[i % ExceptionLog::kCapacity];
      const char* kind = (e.kind == ExceptionLog::kException) ? "exception"
                       : (e.kind == ExceptionLog::kReturn)    ? "rfe      "
                                                              : "mtc0 SR  ";
      printf("  %-9s pc=%08X epc=%08X cause=%08X sr %08X -> %08X",
             kind, e.pc, e.epc, e.cause, e.status_before, e.status_after);
      if (e.kind == ExceptionLog::kException)
        printf("  code=%u", (e.cause >> 2) & 0x1F);
      printf("\n");
    }
  }

  if (options.dis != nullptr) {
    const uint32_t start = strtoul(options.dis, nullptr, 16);
    const char* colon = strchr(options.dis, ':');
    const int count = (colon != nullptr) ? atoi(colon + 1) : 16;
    printf("\ndisassembly from %08X\n", start);
    for (int i = 0; i < count; ++i) {
      const uint32_t address = start + i * 4;
      const uint32_t code = FetchCode(system, address);
      char text[96];
      emulation::psx::Disassemble(address, code, text, sizeof(text));
      printf("  %08X  %08X  %s\n", address, code, text);
    }
  }

  {
    const emulation::psx::IOInterface::AccessLog& log =
        system->io().access_log;
    printf("\nhardware registers touched\n");
    bool any = false;
    for (int i = 0; i < 0x2000; ++i) {
      if (log.reads[i] == 0 && log.writes[i] == 0)
        continue;
      any = true;
      printf("  %08X  %8u reads  %8u writes\n", 0x1F801000u + i, log.reads[i],
             log.writes[i]);
    }
    if (!any)
      printf("  (none)\n");
  }

  if (options.hot > 0) {
    std::vector<std::pair<uint32_t, uint64_t> > sorted(pc_counts.begin(),
                                                      pc_counts.end());
    std::sort(sorted.begin(), sorted.end(),
              [](const std::pair<uint32_t, uint64_t>& a,
                 const std::pair<uint32_t, uint64_t>& b) {
                return a.second > b.second;
              });
    printf("\nhottest addresses (%zu distinct)\n", sorted.size());
    const int limit = std::min<int>(options.hot, (int)sorted.size());
    for (int i = 0; i < limit; ++i) {
      printf("  %08X  %llu  (%.1f%%)\n", sorted[i].first,
             static_cast<unsigned long long>(sorted[i].second),
             100.0 * sorted[i].second / instructions);
    }
  }

  if (options.vram != nullptr) {
    // The whole of VRAM, not just the visible window. When pixels are being
    // plotted but nothing shows, this is what tells you whether the drawing is
    // in the wrong place or simply is not happening.
    const uint16_t* vram = system->gpu().vram();
    const int w = emulation::psx::GpuCore::kVramWidth;
    const int h = emulation::psx::GpuCore::kVramHeight;
    std::vector<uint32_t> pixels(static_cast<size_t>(w) * h);
    for (int i = 0; i < w * h; ++i) {
      const uint16_t p = vram[i];
      const uint32_t r = ((p & 0x1F) << 3) | ((p & 0x1F) >> 2);
      const uint32_t g = (((p >> 5) & 0x1F) << 3) | (((p >> 5) & 0x1F) >> 2);
      const uint32_t b = (((p >> 10) & 0x1F) << 3) | (((p >> 10) & 0x1F) >> 2);
      pixels[i] = (r << 16) | (g << 8) | b;
    }
    if (WriteFrame(options.vram, pixels.data(), w, h))
      printf("wrote          %s (%dx%d, %d non-black)\n", options.vram, w, h,
             NonBlackPixels(pixels.data(), w * h));
  }

  if (options.ppm != nullptr) {
    // What would be shown: above 1x, the hardware rasteriser's own picture of the display
    // area. Everything measured above is the native one.
    int shown_width = 0, shown_height = 0;
    const uint32_t* shown = system->gpu().picture(shown_width, shown_height);
#ifdef PSXEMU_HW_RASTER
    // Left on the card, it is read back the way a screenshot is.
    std::vector<uint32_t> shared_pixels;
    if (const emulation::psx::SharedPicture& shared = system->gpu().shared_picture()) {
      if (psxemu::HardwareRaster::ReadSharedPicture(shared, &shared_pixels)) {
        shown = shared_pixels.data();
        shown_width = shared.width;
        shown_height = shared.height;
      } else {
        fprintf(stderr, "the shared picture could not be read back\n");
      }
    }
#endif
    if (WriteFrame(options.ppm, shown, shown_width, shown_height))
      printf("wrote          %s\n", options.ppm);
    else
      fprintf(stderr, "failed to write %s\n", options.ppm);
  }
  if (options.save_state != nullptr) {
    const std::string error = system->SaveState(options.save_state);
    if (error.empty())
      printf("saved state    %s\n", options.save_state);
    else
      fprintf(stderr, "failed to save state %s: %s\n", options.save_state,
              error.c_str());
  }
  if (options.wav != nullptr) {
    if (WriteWav(options.wav, audio)) {
      int32_t peak = 0;
      for (size_t i = 0; i < audio.size(); ++i) {
        const int32_t magnitude = (audio[i] < 0) ? -audio[i] : audio[i];
        if (magnitude > peak)
          peak = magnitude;
      }
      printf("wrote          %s (%.2f seconds, peak %d)\n", options.wav,
             audio.size() / 2.0 / emulation::psx::Spu::kSampleRate, peak);
    } else {
      fprintf(stderr, "could not write %s\n", options.wav);
    }
  }

  // Every key-on, with the registers as they stood when it fired. The pitch
  // and the start address together say what note the voice was asked for; the
  // volumes say whether the mix was a level or a sweep, which decides whether
  // an unimplemented sweep is flattening it.
  if (options.trace_spu != nullptr) {
    const emulation::psx::Spu& spu = system->spu();
    FILE* fp = fopen(options.trace_spu, "w");
    if (fp != nullptr) {
      fprintf(fp, "# time voice event pitch rate start repeat adsr vol\n");
      for (int i = 0; i < spu.voice_event_count(); ++i) {
        const emulation::psx::Spu::VoiceEvent& e = spu.voice_events()[i];
        const double time =
            e.frame / static_cast<double>(emulation::psx::Spu::kSampleRate);
        if (e.kind == emulation::psx::Spu::kEventRepeatWrite) {
          fprintf(fp, "%8.3f  v%02u  REPEAT= %04X  (start %04X)\n", time,
                  e.voice, e.repeat_address, e.start_address);
          continue;
        }
        fprintf(fp,
                "%8.3f  v%02u  KEYON  pitch %04X (%6.3fx)  start %04X  "
                "repeat-was %04X  adsr %04X %04X  vol %04X%s %04X%s\n",
                time, e.voice, e.pitch, e.pitch / 4096.0, e.start_address,
                e.repeat_address, e.adsr_low, e.adsr_high,
                e.volume_left, (e.volume_left & 0x8000) ? "s" : " ",
                e.volume_right, (e.volume_right & 0x8000) ? "s" : " ");
      }
      fclose(fp);
      printf("wrote          %s (%d key-ons%s)\n", options.trace_spu,
             spu.voice_event_count(),
             spu.voice_event_count() >= emulation::psx::Spu::kMaxVoiceEvents
                 ? ", TRUNCATED" : "");
    } else {
      fprintf(stderr, "could not write %s\n", options.trace_spu);
    }
  }

  // The 512 KB of sound RAM as it stands at the end of the run. The only way
  // to see what a voice was actually pointed at: a start address means
  // nothing without the ADPCM sitting behind it.
  if (options.spu_ram != nullptr) {
    FILE* fp = fopen(options.spu_ram, "wb");
    if (fp != nullptr) {
      fwrite(system->spu().ram(), 1, emulation::psx::Spu::kRamSize, fp);
      fclose(fp);
      printf("wrote          %s (%u bytes of sound RAM)\n", options.spu_ram,
             emulation::psx::Spu::kRamSize);
    } else {
      fprintf(stderr, "could not write %s\n", options.spu_ram);
    }
  }

  // --rewind-test: step back n states from the end - each the one a frame before - and check the
  // last is the frame kept aside, byte for byte; then load it and play n frames on, which must
  // come to the run's own end exactly, as a rewind released there would.
  if (rewind != nullptr) {
    const emulation::psx::RewindBuffer::Stats stats = rewind->stats();
    const size_t held = rewind->count();
    const size_t held_bytes = rewind->bytes();
    System::MemoryState end;
    system->SaveMemoryState(&end, false);
    std::vector<uint8_t> state;
    bool popped = true;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i <= options.rewind_test && popped; ++i)
      popped = rewind->Pop(&state);
    const double pop_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
            .count() / (options.rewind_test + 1);
    const bool same_frame = popped && state == rewind_target;
    bool same_end = false;
    if (same_frame) {
      System::MemoryState back;
      back.payload = state;
      if (system->LoadMemoryState(back).empty()) {
        for (int i = 0; i < options.rewind_test; ++i)
          bench::RunFrame(system);
        System::MemoryState again;
        system->SaveMemoryState(&again, false);
        same_end = again.payload == end.payload;
      }
    }
    printf("rewind         %zu states held in %.1f MB; a delta %.1f KB in %.2f ms on the worker, "
           "%.2f ms a step back\n",
           held, held_bytes / 1048576.0,
           stats.deltas ? stats.delta_bytes / 1024.0 / stats.deltas : 0.0,
           stats.deltas ? stats.delta_ms / stats.deltas : 0.0, pop_ms);
    printf("rewind         %d states back: %s; played on %d frames: %s\n", options.rewind_test,
           same_frame ? "the frame kept aside, byte for byte" : "NOT the frame kept aside",
           options.rewind_test,
           same_end ? "the run's own end, byte for byte" : "NOT the run's own end");
  }

  // Last, since it runs the machine on from where the run left it.
  if (options.bench_state > 0)
    bench::Run(system, options.bench_state);

  system->Deinitialize();
  delete system;

  // A run that drew nothing at all is a failure, not a pass with a boring
  // picture, so say so through the exit code.
  return lit > 0 ? 0 : 1;
}
