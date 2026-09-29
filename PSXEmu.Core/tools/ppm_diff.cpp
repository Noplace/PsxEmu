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

// ppm_diff: how far apart two of boot_runner's pictures are.
//
//   ppm_diff <a.ppm> <b.ppm> [--out side.ppm]
//
// Prints one line: the size, how many pixels differ and what share of the picture that is,
// how many of those differ by more than one step of a 5-bit channel (8 or 9 in the 8-bit values
// boot_runner writes), and the largest channel difference. With --out, writes a, b and a map
// of the differences side by side - the map black where they agree, and brighter the further
// apart they are.
//
// Exits 0 when the pictures are identical, 1 when they differ, 2 when they cannot be compared.
// Written for the hardware rasteriser's comparisons against the software one
// (Docs/Hardware-Renderer-Plan.md), where the software picture is the reference.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

  struct Image {
    int width = 0, height = 0;
    std::vector<unsigned char> rgb;
  };

  // Reads the binary P6 form boot_runner writes, comments and all.
  bool ReadPpm(const char* path, Image* image) {
    FILE* fp = fopen(path, "rb");
    if (fp == nullptr)
      return false;
    char magic[3] = {};
    int values[3] = {};
    bool ok = fread(magic, 1, 2, fp) == 2 && magic[0] == 'P' && magic[1] == '6';
    for (int i = 0; ok && i < 3; ++i) {
      int c = fgetc(fp);
      while (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '#') {
        if (c == '#')
          while (c != '\n' && c != EOF)
            c = fgetc(fp);
        c = fgetc(fp);
      }
      ungetc(c, fp);
      ok = fscanf(fp, "%d", &values[i]) == 1;
    }
    ok = ok && values[2] == 255 && values[0] > 0 && values[1] > 0;
    if (ok) {
      fgetc(fp);   // the one whitespace byte before the pixels
      image->width = values[0];
      image->height = values[1];
      image->rgb.resize(static_cast<size_t>(image->width) * image->height * 3);
      ok = fread(image->rgb.data(), 1, image->rgb.size(), fp) == image->rgb.size();
    }
    fclose(fp);
    return ok;
  }

}  // namespace

int main(int argc, char** argv) {
  const char* out = nullptr;
  const char* paths[2] = {};
  int count = 0;
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--out") == 0 && i + 1 < argc)
      out = argv[++i];
    else if (count < 2)
      paths[count++] = argv[i];
  }
  if (count != 2) {
    fprintf(stderr, "usage: ppm_diff <a.ppm> <b.ppm> [--out side.ppm]\n");
    return 2;
  }

  Image a, b;
  if (!ReadPpm(paths[0], &a) || !ReadPpm(paths[1], &b)) {
    fprintf(stderr, "ppm_diff: could not read %s\n", ReadPpm(paths[0], &a) ? paths[1] : paths[0]);
    return 2;
  }
  if (a.width != b.width || a.height != b.height) {
    printf("size differs: %dx%d against %dx%d\n", a.width, a.height, b.width, b.height);
    return 1;
  }

  const size_t pixels = static_cast<size_t>(a.width) * a.height;
  size_t differing = 0, beyond_one_step = 0;
  int largest = 0;
  std::vector<unsigned char> map(pixels * 3, 0);
  for (size_t p = 0; p < pixels; ++p) {
    int worst = 0;
    for (int c = 0; c < 3; ++c)
      worst = (std::max)(worst, std::abs(a.rgb[p * 3 + c] - b.rgb[p * 3 + c]));
    if (worst == 0)
      continue;
    ++differing;
    // A 5-bit channel widened to 8 bits (c << 3 | c >> 2) moves 8 or 9 a step.
    if (worst > 9)
      ++beyond_one_step;
    largest = (std::max)(largest, worst);
    const int shade = (std::min)(255, 64 + worst * 2);
    map[p * 3] = static_cast<unsigned char>(shade);
    map[p * 3 + 1] = static_cast<unsigned char>(shade / 4);
    map[p * 3 + 2] = static_cast<unsigned char>(shade / 4);
  }

  printf("%dx%d  %zu of %zu pixels differ (%.3f%%), %zu by more than one 5-bit step, "
         "largest %d\n",
         a.width, a.height, differing, pixels, 100.0 * differing / pixels, beyond_one_step,
         largest);

  if (out != nullptr) {
    FILE* fp = fopen(out, "wb");
    if (fp == nullptr) {
      fprintf(stderr, "ppm_diff: could not write %s\n", out);
      return 2;
    }
    fprintf(fp, "P6\n%d %d\n255\n", a.width * 3, a.height);
    for (int y = 0; y < a.height; ++y) {
      const size_t row = static_cast<size_t>(y) * a.width * 3;
      fwrite(a.rgb.data() + row, 1, a.width * 3, fp);
      fwrite(b.rgb.data() + row, 1, a.width * 3, fp);
      fwrite(map.data() + row, 1, a.width * 3, fp);
    }
    fclose(fp);
  }
  return differing == 0 ? 0 : 1;
}
