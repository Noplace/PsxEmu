// boot_runner - headless boot harness for the PSX core.
//
//   boot_runner <bios.bin> [options]
//
//     --disc <path>      mount a disc: a .cue, an image file, or a drive letter
//     --exe <file.exe>   side-load a PS-EXE once the BIOS reaches the shell
//     --frames <n>       run for n frames and stop      (default 300)
//     --ppm <file>       write the final frame as a PPM
//     --vram <file>      write the whole of VRAM as a PPM
//     --trace <n>        print the first n instructions as they execute
//     --trace-skip <n>   start tracing only after n instructions
//     --trace-at <hex>   start tracing the first time the pc reaches an address
//     --trace-irq        start tracing when the first hardware interrupt is taken
//     --hot <n>          print the n most-executed addresses at the end
//     --dis <hex>:<n>    disassemble n instructions of RAM from an address
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

struct Options {
  const char* bios;
  const char* exe;
  const char* disc;
  const char* ppm;
  const char* vram;
  int frames;
  int trace;
  uint64_t trace_skip;
  uint32_t trace_at;
  bool trace_at_set;
  bool trace_irq;
  const char* dis;
  int hot;
  bool quiet;
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
  options->frames = 300;
  options->trace = 0;
  options->trace_skip = 0;
  options->trace_at = 0;
  options->trace_at_set = false;
  options->trace_irq = false;
  options->dis = nullptr;
  options->hot = 0;
  options->quiet = false;

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
    } else if (strcmp(arg, "--trace") == 0 && i + 1 < argc) {
      options->trace = atoi(argv[++i]);
    } else if (strcmp(arg, "--trace-skip") == 0 && i + 1 < argc) {
      options->trace_skip = strtoull(argv[++i], nullptr, 0);
    } else if (strcmp(arg, "--trace-at") == 0 && i + 1 < argc) {
      options->trace_at = strtoul(argv[++i], nullptr, 16);
      options->trace_at_set = true;
    } else if (strcmp(arg, "--dis") == 0 && i + 1 < argc) {
      options->dis = argv[++i];
    } else if (strcmp(arg, "--hot") == 0 && i + 1 < argc) {
      options->hot = atoi(argv[++i]);
    } else if (strcmp(arg, "--trace-irq") == 0) {
      options->trace_irq = true;
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
  if (options->trace_at_set || options->trace_irq)
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
           system->cdrom().disc().track_count(),
           system->cdrom().disc().total_sectors());
  }

  if (options.exe != nullptr) {
    if (!system->LoadPsExe(options.exe)) {
      fprintf(stderr, "failed to load %s\n", options.exe);
      return 1;
    }
    printf("side-loaded %s\n", options.exe);
  }

  // Running the core directly rather than through System::Run keeps the
  // harness single-threaded and deterministic.
  uint64_t instructions = 0;
  uint64_t last_frame = system->gpu().frame_count();
  int frames = 0;

  std::unordered_map<uint32_t, uint64_t> pc_counts;

  const uint64_t kInstructionLimit = 4000000000ull;
  while (frames < options.frames && instructions < kInstructionLimit) {
    if (options.hot > 0)
      ++pc_counts[system->cpu().context()->pc];
    // --trace-irq arms the trace the moment a hardware interrupt is taken,
    // which is the one event a fixed skip count cannot be aimed at.
    if (options.trace_irq && system->interrupts_taken() > 0) {
      options.trace_skip = instructions;
      options.trace_irq = false;
    }
    // --trace-irq arms the trace the moment a hardware interrupt is taken,
    // which is the one event a fixed skip count cannot be aimed at.
    if (options.trace_irq && system->interrupts_taken() > 0) {
      options.trace_skip = instructions;
      options.trace_irq = false;
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
  printf("checksum       %016llx\n",
         static_cast<unsigned long long>(Checksum(pixels, width * height)));
  printf("non-black      %d of %d pixels\n", lit, width * height);
  printf("unimplemented  %llu paths hit,  %llu RFEs executed\n",
         static_cast<unsigned long long>(TrapCounter::count),
         static_cast<unsigned long long>(TrapCounter::rfe_count));

  const auto* ctx = system->cpu().context();
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

  const emulation::psx::Gpu::Stats& gpu_stats = system->gpu().stats();
  printf("gpu            %llu GP0 words, %llu GP1 words\n",
         static_cast<unsigned long long>(gpu_stats.gp0_words),
         static_cast<unsigned long long>(gpu_stats.gp1_words));
  printf("               %llu primitives, %llu pixels plotted\n",
         static_cast<unsigned long long>(gpu_stats.primitives),
         static_cast<unsigned long long>(gpu_stats.pixels));

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

  system->Deinitialize();
  delete system;

  // A run that drew nothing at all is a failure, not a pass with a boring
  // picture, so say so through the exit code.
  return lit > 0 ? 0 : 1;
}
