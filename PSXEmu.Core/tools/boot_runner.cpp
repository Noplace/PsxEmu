// boot_runner - headless boot harness for the PSX core.
//
//   boot_runner <bios.bin> [options]
//
//     --disc <path>      mount a disc: a .cue, an image file, or a drive letter
//     --boot-disc        read SYSTEM.CNF from the disc and start its executable
//     --auto-boot        let the BIOS run, then take over at pc=80030000
//     --exe <file.exe>   side-load a PS-EXE once the BIOS reaches the shell
//     --frames <n>       run for n frames and stop      (default 300)
//     --ppm <file>       write the final frame as a PPM
//     --vram <file>      write the whole of VRAM as a PPM
//     --trace <n>        print the first n instructions as they execute
//     --trace-skip <n>   start tracing only after n instructions
//     --trace-at <hex>   start tracing the first time the pc reaches an address
//     --trace-irq [n]    start tracing at the nth hardware interrupt (default 1)
//     --hot <n>          print the n most-executed addresses at the end
//     --dis <hex>:<n>    disassemble n instructions of RAM from an address
//     --watch-vram x,y,w,h   report which GP0 command wrote into a VRAM area
//     --press b@f[+h]    press a button at frame f, holding h frames
//                        e.g. --press start@1800 --press down+cross@2000+8
//     --quiet            suppress the per-100-frame progress lines
//
// Takes no window, no input and no audio device, so it can be run from a shell
// and diffed. Prints a framebuffer checksum, which is the cheap regression
// signal: a change to the CPU, timing or the renderer that moves the checksum
// for a fixed frame number needs explaining.

#include "psx/psx.h"
#include "tools/disasm.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <algorithm>
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
  int frames;
  int trace;
  uint64_t trace_skip;
  uint32_t trace_at;
  bool trace_at_set;
  uint64_t trace_irq;   // which interrupt to trace from; 0 means none
  const char* dis;
  const char* watch;
  uint32_t watch_ram;
  int hot;
  bool boot_disc;
  bool auto_boot;
  bool quiet;
  int frame_log;
  float volume;
  std::vector<Press> presses;
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

bool ParseOptions(int argc, char** argv, Options* options) {
  options->bios = nullptr;
  options->exe = nullptr;
  options->disc = nullptr;
  options->ppm = nullptr;
  options->vram = nullptr;
  options->wav = nullptr;
  options->frames = 300;
  options->trace = 0;
  options->trace_skip = 0;
  options->trace_at = 0;
  options->trace_at_set = false;
  options->trace_irq = 0;
  options->dis = nullptr;
  options->watch = nullptr;
  options->watch_ram = 0;
  options->hot = 0;
  options->boot_disc = false;
  options->auto_boot = false;
  options->quiet = false;
  options->frame_log = 0;
  options->volume = -1.0f;

  for (int i = 1; i < argc; ++i) {
    const char* arg = argv[i];
    if (strcmp(arg, "--exe") == 0 && i + 1 < argc) {
      options->exe = argv[++i];
    } else if (strcmp(arg, "--disc") == 0 && i + 1 < argc) {
      options->disc = argv[++i];
    } else if (strcmp(arg, "--frames") == 0 && i + 1 < argc) {
      options->frames = atoi(argv[++i]);
    } else if (strcmp(arg, "--ppm") == 0 && i + 1 < argc) {
      options->ppm = argv[++i];
    } else if (strcmp(arg, "--vram") == 0 && i + 1 < argc) {
      options->vram = argv[++i];
    } else if (strcmp(arg, "--wav") == 0 && i + 1 < argc) {
      options->wav = argv[++i];
    } else if (strcmp(arg, "--trace") == 0 && i + 1 < argc) {
      options->trace = atoi(argv[++i]);
    } else if (strcmp(arg, "--trace-skip") == 0 && i + 1 < argc) {
      options->trace_skip = strtoull(argv[++i], nullptr, 0);
    } else if (strcmp(arg, "--trace-at") == 0 && i + 1 < argc) {
      options->trace_at = strtoul(argv[++i], nullptr, 16);
      options->trace_at_set = true;
    } else if (strcmp(arg, "--dis") == 0 && i + 1 < argc) {
      options->dis = argv[++i];
    } else if (strcmp(arg, "--watch-vram") == 0 && i + 1 < argc) {
      options->watch = argv[++i];
    } else if (strcmp(arg, "--volume") == 0 && i + 1 < argc) {
      options->volume = static_cast<float>(atof(argv[++i]));
    } else if (strcmp(arg, "--frame-log") == 0 && i + 1 < argc) {
      options->frame_log = atoi(argv[++i]);
    } else if (strcmp(arg, "--press") == 0 && i + 1 < argc) {
      Press press;
      if (!ParsePress(argv[++i], &press)) {
        fprintf(stderr, "bad --press: %s\n", argv[i]);
        fprintf(stderr, "  expected <button>[+<button>]@<frame>[+<hold>], "
                        "e.g. start@1800 or start@1800+6\n");
        return false;
      }
      options->presses.push_back(press);
    } else if (strcmp(arg, "--watch-ram") == 0 && i + 1 < argc) {
      options->watch_ram = strtoul(argv[++i], nullptr, 16);
    } else if (strcmp(arg, "--hot") == 0 && i + 1 < argc) {
      options->hot = atoi(argv[++i]);
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
    } else if (strcmp(arg, "--quiet") == 0) {
      options->quiet = true;
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
  return options->bios != nullptr;
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!ParseOptions(argc, argv, &options)) {
    fprintf(stderr,
            "usage: boot_runner <bios.bin> [--exe f] [--frames n] [--ppm f]\n"
            "                              [--trace n] [--quiet]\n");
    return 2;
  }

  System* system = new System();
  if (system->Initialize(options.bios) != 0) {
    fprintf(stderr, "failed to initialise the core (bios: %s)\n", options.bios);
    return 1;
  }

  if (options.disc != nullptr) {
    if (!system->LoadDisc(options.disc)) {
      fprintf(stderr, "failed to mount %s\n", options.disc);
      return 1;
    }
    printf("mounted        %s (%d track(s), %u sectors)\n", options.disc,
           system->cdrom().disc().track_count(),  // NOLINT
           system->cdrom().disc().total_sectors());
  }

  if (options.watch_ram != 0)
    system->cpu().set_watch_address(options.watch_ram);

  if (options.volume >= 0.0f)
    system->config().audio_volume = options.volume;

  if (options.auto_boot) {
    // Let the BIOS run its intro, then take over when it reaches the address
    // it would hand a game control at.
    system->set_auto_boot(true);
    printf("auto-boot      armed at pc=80030000\n");
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

  if (options.exe != nullptr) {
    if (!system->LoadPsExe(options.exe)) {
      fprintf(stderr, "failed to load %s\n", options.exe);
      return 1;
    }
    printf("side-loaded %s\n", options.exe);
  }

  if (options.watch != nullptr) {
    unsigned x = 0, y = 0, w = 0, h = 0;
    if (sscanf(options.watch, "%u,%u,%u,%u", &x, &y, &w, &h) == 4) {
      system->WatchVram(x, y, w, h);
      printf("watching       VRAM %u,%u %ux%u\n", x, y, w, h);
    } else {
      fprintf(stderr, "--watch-vram wants x,y,w,h\n");
      return 2;
    }
  }

  // Running the core directly rather than through System::Run keeps the
  // harness single-threaded and deterministic.
  std::vector<int16_t> audio;
  uint64_t instructions = 0;
  uint64_t last_frame = system->gpu().frame_count();
  int frames = 0;
  uint16_t last_buttons = 0;

  std::unordered_map<uint32_t, uint64_t> pc_counts;

  const uint64_t kInstructionLimit = 4000000000ull;
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
      options.trace_skip = instructions;
      options.trace_at_set = false;
    }
    if (options.trace > 0 && instructions >= options.trace_skip &&
        instructions < options.trace_skip + options.trace) {
      const auto* ctx = system->cpu().context();
      char text[96];
      tools::Disassemble(ctx->pc, FetchCode(system, ctx->pc), text,
                         sizeof(text));
      // Show the registers this instruction actually reads or writes rather
      // than a fixed set, which is what makes a trace readable.
      const uint32_t code = FetchCode(system, ctx->pc);
      const uint32_t rs = (code >> 21) & 0x1F;
      const uint32_t rt = (code >> 16) & 0x1F;
      const uint32_t rd = (code >> 11) & 0x1F;
      printf("%09llu %08X  %-30s %s=%08X %s=%08X %s=%08X\n",
             static_cast<unsigned long long>(instructions), ctx->pc, text,
             tools::RegisterName(rs), ctx->gp.reg[rs],
             tools::RegisterName(rt), ctx->gp.reg[rt],
             tools::RegisterName(rd), ctx->gp.reg[rd]);
    }
    system->StepInstruction();
    ++instructions;

    const uint64_t now = system->gpu().frame_count();
    if (now != last_frame) {
      last_frame = now;
      ++frames;
      // A timeline of one run, which is far cheaper than bisecting with many.
      // The checksum going quiet while the counters keep moving is a game
      // waiting for input; everything going quiet is a hang.
      if (options.frame_log > 0 && (frames % options.frame_log) == 0) {
        int w = 0;
        int h = 0;
        const uint32_t* px = system->gpu().framebuffer(w, h);
        printf("frame %-6d %016llx  %6d non-black  %dx%d  "
               "mdec %llu  cd %llu  gp0 %llu\n",
               frames,
               static_cast<unsigned long long>(Checksum(px, w * h)),
               NonBlackPixels(px, w * h), w, h,
               static_cast<unsigned long long>(
                   system->io().mdec.stats().macroblocks),
               static_cast<unsigned long long>(
                   system->cdrom().stats().sectors_read),
               static_cast<unsigned long long>(
                   system->gpu().stats().gp0_words));
        fflush(stdout);
      }
      // Buttons change on the frame boundary, which is where the front end
      // samples them too.
      if (!options.presses.empty()) {
        const uint16_t buttons = ButtonsOnFrame(options.presses, frames);
        if (buttons != last_buttons) {
          system->sio().set_buttons(0, buttons);
          last_buttons = buttons;
        }
      }
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
  printf("resolution     %dx%d\n", width, height);
  printf("display        vram (%u,%u) %dx%d, %s\n",
         system->gpu().display_vram_x(), system->gpu().display_vram_y(),
         width, height,
         system->gpu().display_disabled() ? "DISABLED" : "enabled");
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
  printf("               I_STAT=%08X I_MASK=%08X SR=%08X\n",
         system->io().io.interrupt_stat, system->io().io.interrupt_mask,
         ctx->ctrl.SR.raw);

  const emulation::psx::Cdrom::Stats& cd_stats = system->cdrom().stats();
  printf("cdrom          %llu commands (last %02X), %llu sectors, "
         "%llu interrupts, %llu unknown\n",
         static_cast<unsigned long long>(cd_stats.commands),
         cd_stats.last_command,
         static_cast<unsigned long long>(cd_stats.sectors_read),
         static_cast<unsigned long long>(cd_stats.interrupts),
         static_cast<unsigned long long>(cd_stats.unknown_commands));
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
            printf("  setloc   lba %6u  (sector %6d)\n", e.lba,
                   static_cast<int>(e.lba) - 150);
            break;
          case 2:
            printf("  setmode  %02X  (%s, %s)\n", e.mode,
                   (e.mode & 0x80) ? "2x" : "1x",
                   (e.mode & 0x20) ? "whole sector 0x924" : "data only 0x800");
            break;
          case 3: {
            const char* name =
                e.mode == 0x06 ? "ReadN" : e.mode == 0x09 ? "Pause" :
                e.mode == 0x15 ? "SeekL" : e.mode == 0x1B ? "ReadS" :
                e.mode == 0x0A ? "Init"  : e.mode == 0x08 ? "Stop"  : "?";
            printf("  %-8s at lba %6u  (previous sector %u/%u taken)\n", name,
                   e.lba, e.consumed, e.size);
            break;
          }
          default:
            // A sector whose predecessor was not fully taken is a dropped one.
            printf("  sector   lba %6u  (sector %6d)  mode %02X"
                   "  previous %u/%u%s\n",
                   e.lba, static_cast<int>(e.lba) - 150, e.mode, e.consumed,
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
        if (rows[r].counts[i] < 1000)
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
  printf("               %llu clipped, %llu mask-rejected, %llu transparent\n",
         static_cast<unsigned long long>(gpu_stats.clipped),
         static_cast<unsigned long long>(gpu_stats.mask_rejected),
         static_cast<unsigned long long>(gpu_stats.transparent_texels));
  printf("               texels by depth: %llu 4-bit, %llu 8-bit, %llu 15-bit\n",
         static_cast<unsigned long long>(gpu_stats.texels_by_depth[0]),
         static_cast<unsigned long long>(gpu_stats.texels_by_depth[1]),
         static_cast<unsigned long long>(gpu_stats.texels_by_depth[2]));

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
      tools::Disassemble(address, code, text, sizeof(text));
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
    if (WritePpm(options.vram, pixels.data(), w, h))
      printf("wrote          %s (%dx%d, %d non-black)\n", options.vram, w, h,
             NonBlackPixels(pixels.data(), w * h));
  }

  if (options.ppm != nullptr) {
    if (WritePpm(options.ppm, pixels, width, height))
      printf("wrote          %s\n", options.ppm);
    else
      fprintf(stderr, "failed to write %s\n", options.ppm);
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


  system->Deinitialize();
  delete system;

  // A run that drew nothing at all is a failure, not a pass with a boring
  // picture, so say so through the exit code.
  return lit > 0 ? 0 : 1;
}
