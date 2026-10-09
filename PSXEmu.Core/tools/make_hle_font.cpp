// make_hle_font - the HLE BIOS's character sets, from the Shinonome fonts.
//
// Writes PSXEmu.Core/psx/hle_font_data.cpp: the ROM's charset 2 (524 16x15 symbols, kana and
// alphabets, Shift-JIS 8140h-84BEh), charset 3 (the 2,965 level-1 kanji, 889Fh-9872h) and charset 4
// (8x15 ASCII, 21h-7Fh), in the format and order the SCPH-1001's ROM keeps them (psx-spx "BIOS
// Character Sets"; the order measured by calling its Krom2Offset for every code - see
// Docs/HLE-BIOS.md). The glyphs are Shinonome's - shnmk16.bdf and shnm8x16r.bdf, from
// shinonome-0.9.11p1 (http://openlab.jp/efont/shinonome/), which are in the public domain. The
// 16-line glyphs lose whichever of their top and bottom lines is used less.
//
//   make_hle_font <shnmk16.bdf> <shnm8x16r.bdf> <out.cpp>
//
// Built and run by hand, once; the output is committed.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

// A BDF font's glyphs by ENCODING: 16 rows, each a 16-bit row with bit 15 the leftmost pixel.
std::map<int, std::vector<uint16_t>> ReadBdf(const char* path) {
  std::map<int, std::vector<uint16_t>> glyphs;
  FILE* fp = fopen(path, "r");
  if (fp == nullptr)
    return glyphs;
  char line[512];
  int encoding = -1, width = 0, height = 0, x = 0, y = 0, ascent = 14, descent = 2;
  bool bitmap = false;
  std::vector<uint16_t> rows;
  int row = 0;
  while (fgets(line, sizeof(line), fp)) {
    if (strncmp(line, "FONT_ASCENT ", 12) == 0) ascent = atoi(line + 12);
    else if (strncmp(line, "FONT_DESCENT ", 13) == 0) descent = atoi(line + 13);
    else if (strncmp(line, "ENCODING ", 9) == 0) encoding = atoi(line + 9);
    else if (strncmp(line, "BBX ", 4) == 0) sscanf(line + 4, "%d %d %d %d", &width, &height, &x, &y);
    else if (strncmp(line, "BITMAP", 6) == 0) {
      bitmap = true;
      rows.assign(ascent + descent, 0);
      row = ascent - (y + height);   // the glyph's top line, counted from the cell's
    } else if (strncmp(line, "ENDCHAR", 7) == 0) {
      bitmap = false;
      if (encoding >= 0)
        glyphs[encoding] = rows;
    } else if (bitmap) {
      const unsigned long bits = strtoul(line, nullptr, 16);
      const int digits = static_cast<int>(strspn(line, "0123456789abcdefABCDEF"));
      uint32_t value = static_cast<uint32_t>(bits) << (32 - digits * 4);   // left-aligned
      value >>= x;
      if (row >= 0 && row < static_cast<int>(rows.size()))
        rows[row] = static_cast<uint16_t>(value >> 16);
      ++row;
    }
  }
  fclose(fp);
  return glyphs;
}

// Shift-JIS to JIS X 0208.
int SjisToJis(int sjis) {
  int s1 = sjis >> 8, s2 = sjis & 0xFF;
  int j1 = (s1 - (s1 >= 0xE0 ? 0xB0 : 0x70)) << 1;
  int j2;
  if (s2 >= 0x9F) {
    j2 = s2 - 0x7E;
  } else {
    j1 -= 1;
    j2 = s2 - (s2 >= 0x80 ? 0x20 : 0x1F);
  }
  return (j1 << 8) | j2;
}

// Charset 2's codes in ROM order: the runs of assigned cells, as the SCPH-1001's Krom2Offset
// numbers them.
const int kCharset2Runs[][2] = {
  {0x8140, 0x817E}, {0x8180, 0x81AC}, {0x81B8, 0x81BF}, {0x81C8, 0x81CE}, {0x81DA, 0x81E8},
  {0x81F0, 0x81F7}, {0x81FC, 0x81FC}, {0x824F, 0x8258}, {0x8260, 0x8279}, {0x8281, 0x829A},
  {0x829F, 0x82F1}, {0x8340, 0x837E}, {0x8380, 0x8396}, {0x839F, 0x83B6}, {0x83BF, 0x83D6},
  {0x8440, 0x8460}, {0x8470, 0x847E}, {0x8480, 0x8491}, {0x849F, 0x84BE},
};

void Emit(FILE* out, const char* name, const std::vector<uint8_t>& bytes) {
  fprintf(out, "extern const uint8_t %s[%zu] = {\n", name, bytes.size());
  for (size_t i = 0; i < bytes.size(); ++i)
    fprintf(out, "%s0x%02X,%s", i % 16 == 0 ? "  " : "", bytes[i], i % 16 == 15 ? "\n" : "");
  fprintf(out, "\n};\n\n");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 4) {
    fprintf(stderr, "usage: make_hle_font <shnmk16.bdf> <shnm8x16r.bdf> <out.cpp>\n");
    return 2;
  }
  const auto kanji = ReadBdf(argv[1]);
  const auto ascii = ReadBdf(argv[2]);
  if (kanji.empty() || ascii.empty()) {
    fprintf(stderr, "could not read the fonts\n");
    return 1;
  }

  std::vector<int> codes2, codes3;
  for (const auto& run : kCharset2Runs)
    for (int c = run[0]; c <= run[1]; ++c)
      if ((c & 0xFF) != 0x7F)
        codes2.push_back(c);
  for (int lead = 0x88; lead <= 0x98; ++lead)
    for (int low = 0x40; low <= 0xFC; ++low) {
      const int c = (lead << 8) | low;
      if (low == 0x7F || c < 0x889F || c > 0x9872)
        continue;
      codes3.push_back(c);
    }
  printf("charset 2: %zu codes, charset 3: %zu codes\n", codes2.size(), codes3.size());

  // Which line to drop: the one with fewer pixels across every glyph used.
  auto dropped_line = [](const std::map<int, std::vector<uint16_t>>& font, const std::vector<int>& jis) {
    long top = 0, bottom = 0;
    for (int j : jis) {
      auto it = font.find(j);
      if (it == font.end()) continue;
      for (int b = 0; b < 16; ++b) {
        top += (it->second.front() >> b) & 1;
        bottom += (it->second.back() >> b) & 1;
      }
    }
    return top <= bottom ? 0 : 15;
  };
  std::vector<int> jis2, jis3;
  for (int c : codes2) jis2.push_back(SjisToJis(c));
  for (int c : codes3) jis3.push_back(SjisToJis(c));
  std::vector<int> all = jis2;
  all.insert(all.end(), jis3.begin(), jis3.end());
  const int drop = dropped_line(kanji, all);

  int missing = 0;
  auto sixteen = [&](const std::vector<int>& jis) {
    std::vector<uint8_t> bytes;
    for (int j : jis) {
      auto it = kanji.find(j);
      for (int r = 0; r < 16; ++r) {
        if (r == drop) continue;
        const uint16_t bits = it != kanji.end() && r < static_cast<int>(it->second.size()) ? it->second[r] : 0;
        bytes.push_back(static_cast<uint8_t>(bits >> 8));
        bytes.push_back(static_cast<uint8_t>(bits));
      }
      if (it == kanji.end()) ++missing;
    }
    return bytes;
  };
  const std::vector<uint8_t> charset2 = sixteen(jis2);
  const std::vector<uint8_t> charset3 = sixteen(jis3);

  std::vector<int> ascii_codes;
  for (int c = 0x21; c <= 0x7F; ++c) ascii_codes.push_back(c);
  const int drop8 = dropped_line(ascii, ascii_codes);
  std::vector<uint8_t> charset4;
  for (int c : ascii_codes) {
    auto it = ascii.find(c);
    for (int r = 0; r < 16; ++r) {
      if (r == drop8) continue;
      charset4.push_back(it != ascii.end() && r < static_cast<int>(it->second.size())
                             ? static_cast<uint8_t>(it->second[r] >> 8) : 0);
    }
  }
  printf("dropped line %d of the 16x16 glyphs, %d of the 8x16; %d codes with no glyph\n", drop,
         drop8, missing);

  FILE* out = fopen(argv[3], "w");
  if (out == nullptr)
    return 1;
  fprintf(out,
          "// Generated by PSXEmu.Core/tools/make_hle_font.cpp - do not edit.\n"
          "//\n"
          "// The HLE BIOS's character sets (psx/hle_bios.h, Krom2RawAdd), from the Shinonome fonts\n"
          "// shnmk16.bdf and shnm8x16r.bdf (shinonome-0.9.11p1, The Electronic Font Open Laboratory,\n"
          "// http://openlab.jp/efont/shinonome/), which are in the public domain.\n\n"
          "#include <cstdint>\n\n"
          "namespace emulation {\nnamespace psx {\n\n");
  Emit(out, "kHleCharset2", charset2);
  Emit(out, "kHleCharset3", charset3);
  Emit(out, "kHleCharset4", charset4);
  fprintf(out, "}  // namespace psx\n}  // namespace emulation\n");
  fclose(out);
  printf("wrote %zu + %zu + %zu bytes\n", charset2.size(), charset3.size(), charset4.size());
  return 0;
}
