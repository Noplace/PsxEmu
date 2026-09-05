// wav_pitch - reads a WAV and prints what note is in it, window by window.
//
//   wav_pitch <file.wav> [options]
//
//     --from <sec>     start of the range to analyse   (default 0)
//     --to <sec>       end of the range                (default end of file)
//     --window <n>     analysis window in samples      (default 2048)
//     --hop <n>        step between windows in samples (default 1024)
//     --min-rms <n>    skip windows quieter than this  (default 300)
//     --min-hz <n>     lowest pitch to look for        (default 60)
//     --max-hz <n>     highest pitch to look for       (default 5000)
//     --summary        one line per note, not per window
//
// The counterpart to boot_runner's --wav. A capture either has the right
// notes in it or it does not, and "listen to it" is not a measurement that
// can be diffed, put in a bug report, or checked again after a change.
//
// Pitch by YIN's cumulative mean normalised difference rather than plain
// autocorrelation, which is not a detail: autocorrelation's characteristic
// failure is reporting a note an octave out, and an octave is exactly what
// this tool is being pointed at. A detector that guesses octaves cannot
// diagnose one.
//
// Depends on nothing - not even the core - so it builds anywhere.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <vector>

namespace {

struct Options {
  const char* path;
  double from, to;
  int window, hop;
  double min_rms, min_hz, max_hz;
  bool summary;
};

// A 16-bit PCM WAV, walked chunk by chunk rather than assuming the 44-byte
// header: anything that writes a LIST or fact chunk would break the fixed
// offset, and finding that out from wrong numbers rather than an error is
// exactly the kind of afternoon this tool exists to avoid.
bool ReadWav(const char* path, std::vector<float>* mono, int* rate) {
  FILE* fp = fopen(path, "rb");
  if (fp == nullptr) {
    fprintf(stderr, "could not open %s\n", path);
    return false;
  }
  char riff[12];
  if (fread(riff, 1, 12, fp) != 12 || memcmp(riff, "RIFF", 4) != 0 ||
      memcmp(riff + 8, "WAVE", 4) != 0) {
    fprintf(stderr, "%s is not a RIFF/WAVE file\n", path);
    fclose(fp);
    return false;
  }

  int channels = 0, bits = 0;
  *rate = 0;
  for (;;) {
    char id[4];
    uint32_t size = 0;
    if (fread(id, 1, 4, fp) != 4) break;
    if (fread(&size, 4, 1, fp) != 1) break;

    if (memcmp(id, "fmt ", 4) == 0) {
      unsigned char fmt[16];
      const uint32_t want = size < 16 ? size : 16;
      if (fread(fmt, 1, want, fp) != want) break;
      channels = fmt[2] | (fmt[3] << 8);
      *rate = fmt[4] | (fmt[5] << 8) | (fmt[6] << 16) | (fmt[7] << 24);
      bits = fmt[14] | (fmt[15] << 8);
      if (size > want) fseek(fp, size - want, SEEK_CUR);
    } else if (memcmp(id, "data", 4) == 0) {
      if (bits != 16 || channels < 1) {
        fprintf(stderr, "%s: expected 16-bit PCM, got %d-bit / %d channels\n",
                path, bits, channels);
        fclose(fp);
        return false;
      }
      const uint32_t frames = size / (2 * channels);
      mono->resize(frames);
      std::vector<int16_t> row(channels);
      for (uint32_t i = 0; i < frames; ++i) {
        if (fread(&row[0], 2, channels, fp) != static_cast<size_t>(channels)) {
          mono->resize(i);
          break;
        }
        int32_t sum = 0;
        for (int c = 0; c < channels; ++c) sum += row[c];
        (*mono)[i] = static_cast<float>(sum) / channels;
      }
      fclose(fp);
      return true;
    } else {
      fseek(fp, size + (size & 1), SEEK_CUR);
    }
  }
  fprintf(stderr, "%s: no data chunk\n", path);
  fclose(fp);
  return false;
}

const char* kNoteNames[12] = { "C", "C#", "D", "D#", "E", "F", "F#",
                               "G", "G#", "A", "A#", "B" };

// The nearest equal-tempered note to a frequency, and how far off it is. The
// cents matter: a note 20 cents flat is a tuning question, one 1200 cents flat
// is an octave, and those are different bugs.
void NameOf(double hz, char* out, int size, double* cents) {
  const double midi = 69.0 + 12.0 * log2(hz / 440.0);
  const int nearest = static_cast<int>(floor(midi + 0.5));
  *cents = (midi - nearest) * 100.0;
  const int octave = nearest / 12 - 1;
  snprintf(out, size, "%s%d", kNoteNames[((nearest % 12) + 12) % 12], octave);
}

// YIN, cut down to what is needed here. Returns 0 if nothing periodic was
// found in the search range.
double Detect(const float* x, int window, int max_lag, double threshold) {
  std::vector<double> d(max_lag + 2, 0.0);
  for (int lag = 1; lag <= max_lag; ++lag) {
    double sum = 0.0;
    for (int i = 0; i < window; ++i) {
      const double delta = x[i] - x[i + lag];
      sum += delta * delta;
    }
    d[lag] = sum;
  }

  // Cumulative mean normalisation: this is what stops a half-frequency lag,
  // which always correlates at least as well, from winning.
  std::vector<double> dn(max_lag + 2, 1.0);
  double running = 0.0;
  for (int lag = 1; lag <= max_lag; ++lag) {
    running += d[lag];
    dn[lag] = (running == 0.0) ? 1.0 : d[lag] * lag / running;
  }

  int best = -1;
  for (int lag = 2; lag < max_lag; ++lag) {
    if (dn[lag] < threshold && dn[lag] <= dn[lag + 1]) {
      best = lag;
      break;
    }
  }
  if (best < 0) {
    double lowest = 1e30;
    for (int lag = 2; lag <= max_lag; ++lag) {
      if (dn[lag] < lowest) {
        lowest = dn[lag];
        best = lag;
      }
    }
    if (best < 0 || lowest > 0.6) return 0.0;
  }

  // Parabolic interpolation around the minimum, so a note is not quantised to
  // whole samples of period - at 2 kHz that would be a 5% error on its own.
  double refined = best;
  if (best > 1 && best < max_lag) {
    const double a = dn[best - 1], b = dn[best], c = dn[best + 1];
    const double denominator = 2.0 * (2.0 * b - a - c);
    if (denominator != 0.0) refined = best + (c - a) / denominator;
  }
  return refined;
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  options.path = nullptr;
  options.from = 0.0;
  options.to = 1e30;
  options.window = 2048;
  options.hop = 1024;
  options.min_rms = 300.0;
  options.min_hz = 60.0;
  options.max_hz = 5000.0;
  options.summary = false;

  for (int i = 1; i < argc; ++i) {
    const char* arg = argv[i];
    if (strcmp(arg, "--from") == 0 && i + 1 < argc) {
      options.from = atof(argv[++i]);
    } else if (strcmp(arg, "--to") == 0 && i + 1 < argc) {
      options.to = atof(argv[++i]);
    } else if (strcmp(arg, "--window") == 0 && i + 1 < argc) {
      options.window = atoi(argv[++i]);
    } else if (strcmp(arg, "--hop") == 0 && i + 1 < argc) {
      options.hop = atoi(argv[++i]);
    } else if (strcmp(arg, "--min-rms") == 0 && i + 1 < argc) {
      options.min_rms = atof(argv[++i]);
    } else if (strcmp(arg, "--min-hz") == 0 && i + 1 < argc) {
      options.min_hz = atof(argv[++i]);
    } else if (strcmp(arg, "--max-hz") == 0 && i + 1 < argc) {
      options.max_hz = atof(argv[++i]);
    } else if (strcmp(arg, "--summary") == 0) {
      options.summary = true;
    } else if (arg[0] != '-' && options.path == nullptr) {
      options.path = arg;
    } else {
      fprintf(stderr, "unknown option: %s\n", arg);
      return 1;
    }
  }
  if (options.path == nullptr) {
    fprintf(stderr, "usage: wav_pitch <file.wav> [--from s] [--to s] "
                    "[--window n] [--hop n] [--min-rms n] [--summary]\n");
    return 1;
  }

  std::vector<float> mono;
  int rate = 0;
  if (!ReadWav(options.path, &mono, &rate)) return 1;

  printf("%s  %d Hz  %.2f seconds  %u samples\n", options.path, rate,
         mono.size() / static_cast<double>(rate),
         static_cast<unsigned>(mono.size()));

  const int max_lag = static_cast<int>(rate / options.min_hz);
  const int min_lag = static_cast<int>(rate / options.max_hz);
  const size_t first = static_cast<size_t>(options.from * rate);
  size_t last = static_cast<size_t>(options.to >= 1e29
                                        ? static_cast<double>(mono.size())
                                        : options.to * rate);
  if (last > mono.size()) last = mono.size();

  if (!options.summary)
    printf("\n    time      rms     peak        hz   note   cents\n");

  char previous_note[16] = "";
  double note_start = 0.0, note_hz = 0.0;
  int note_windows = 0;

  for (size_t start = first;
       start + options.window + max_lag <= last;
       start += options.hop) {
    const float* x = &mono[start];
    double energy = 0.0, peak = 0.0;
    for (int i = 0; i < options.window; ++i) {
      energy += static_cast<double>(x[i]) * x[i];
      const double magnitude = x[i] < 0 ? -x[i] : x[i];
      if (magnitude > peak) peak = magnitude;
    }
    const double rms = sqrt(energy / options.window);
    const double time = start / static_cast<double>(rate);

    double hz = 0.0;
    if (rms >= options.min_rms) {
      const double lag = Detect(x, options.window, max_lag, 0.15);
      if (lag >= min_lag && lag > 0.0) hz = rate / lag;
    }

    if (options.summary) {
      char name[16] = "";
      double cents = 0.0;
      if (hz > 0.0) NameOf(hz, name, sizeof(name), &cents);
      if (strcmp(name, previous_note) != 0) {
        if (previous_note[0] != 0 && note_windows > 0) {
          printf("  %7.3f  %8.2f Hz  %-5s  %6.3f s\n", note_start, note_hz,
                 previous_note, time - note_start);
        }
        snprintf(previous_note, sizeof(previous_note), "%s", name);
        note_start = time;
        note_hz = hz;
        note_windows = 1;
      } else {
        ++note_windows;
      }
      continue;
    }

    if (hz > 0.0) {
      char name[16];
      double cents;
      NameOf(hz, name, sizeof(name), &cents);
      printf("%8.3f  %7.0f  %7.0f  %8.2f  %-5s  %+6.1f\n", time, rms, peak, hz,
             name, cents);
    } else {
      printf("%8.3f  %7.0f  %7.0f         -      -       -\n", time, rms, peak);
    }
  }

  if (options.summary && previous_note[0] != 0 && note_windows > 0)
    printf("  %7.3f  %8.2f Hz  %-5s\n", note_start, note_hz, previous_note);
  return 0;
}
