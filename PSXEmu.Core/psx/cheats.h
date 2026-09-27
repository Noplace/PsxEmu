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

// GameShark codes - the PlayStation cheat cartridge's, which is how cheats for these games are
// written down everywhere - read, kept per game, and applied once a frame.
//
// A code is one or more lines of "TTAAAAAA VVVV": a type byte, a 24-bit address in main RAM (the
// cartridge's addresses are KSEG0's, 80xxxxxx, less the 80), and a 16-bit value. The types are
// the original cartridge's:
//
//   30 write 8 bits          80 write 16 bits         1F write 16 bits to the scratchpad
//   10/11 add/subtract 16    20/21 add/subtract 8     (every frame, as the cartridge did)
//   D0-D3 if 16 bits ==, !=, <, >    E0-E3 the same, 8 bits    D4 if the buttons are exactly
//       - each applies to the next line that is not itself a condition, and skips it if false
//   C0 if 16 bits == ...    D5 if the buttons are ...    D6 if the buttons are not ...
//       - false skips the rest of the code, up to a "00000000 FFFF" line if there is one
//   C1 hold everything after it back until VVVV x 0.3 frames have passed since the game booted
//   C2 copy VVVV bytes from AAAAAA to the next line's address
//   50 repeat the next line's write: XXYY ZZZZ - XX times, the address up by YY and the value
//       by ZZZZ each time
//
// Buttons are the cartridge's own word: Select 0100, L3 0200, R3 0400, Start 0800, Up 1000,
// Right 2000, Down 4000, Left 8000, L2 0001, R2 0002, L1 0004, R1 0008, Triangle 0010,
// Circle 0020, Cross 0040, Square 0080 - the pad's own bits with their bytes swapped.
//
// A value that is already right is not written again, and a write that changes RAM is reported
// (CheatMemory::written), so code a cheat patches is recompiled rather than run stale.
//
// Nothing here knows about files on disk beyond their text, or about threads: the front end reads
// and writes the files and hands the engine its codes on the machine's thread.

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace emulation {
namespace psx {

  // One "TTAAAAAA VVVV" line.
  struct CheatLine {
    uint32_t first = 0;    // type << 24 | address
    uint16_t value = 0;
    uint8_t type() const { return static_cast<uint8_t>(first >> 24); }
    uint32_t address() const { return first & 0x00FFFFFFu; }
  };

  // One cheat as the person sees it: a name, on or off, and its code as they wrote it.
  struct Cheat {
    std::string name;
    bool enabled = false;
    std::string code;              // the lines, as text
    std::vector<CheatLine> lines;  // the same, parsed
  };

  // Whether the engine can run a line of this type.
  inline bool IsKnownCheatType(uint8_t type) {
    switch (type) {
      case 0x00: case 0x30: case 0x80: case 0x1F: case 0x10: case 0x11: case 0x20: case 0x21:
      case 0xD0: case 0xD1: case 0xD2: case 0xD3: case 0xE0: case 0xE1: case 0xE2: case 0xE3:
      case 0xD4: case 0xC0: case 0xD5: case 0xD6: case 0xC1: case 0xC2: case 0x50:
        return true;
      default:
        return false;
    }
  }

  // `code` - one line per "TTAAAAAA VVVV", as a person types it or a cheat site prints it - into
  // lines. Spaces, dashes and '+' may separate the two halves or the lines (RetroArch's files
  // join lines with '+'), and letters may be either case. False, with `error` naming the line and
  // what is wrong with it, for anything else: a line that is not twelve hex digits, a type the
  // engine does not know, or a 50, C2 left without the line it applies to.
  inline bool ParseCheatCode(const std::string& code, std::vector<CheatLine>* lines,
                             std::string* error) {
    lines->clear();
    // Hex digits only, twelve to a line; anything else between them is a separator, except
    // letters past F, which are a typo.
    std::string digits;
    int line_number = 0;
    auto flush = [&](bool end) -> bool {
      if (digits.empty())
        return true;
      ++line_number;
      if (digits.size() != 12) {
        if (error != nullptr) {
          char text[160];
          snprintf(text, sizeof(text),
                   "Line %d has %zu hex digits; a GameShark line has 12, like 80012345 0063.",
                   line_number, digits.size());
          *error = text;
        }
        return false;
      }
      CheatLine line;
      line.first = static_cast<uint32_t>(std::stoul(digits.substr(0, 8), nullptr, 16));
      line.value = static_cast<uint16_t>(std::stoul(digits.substr(8, 4), nullptr, 16));
      if (!IsKnownCheatType(line.type())) {
        if (error != nullptr) {
          char text[160];
          snprintf(text, sizeof(text), "Line %d starts %02X, which is not a code type known here.",
                   line_number, line.type());
          *error = text;
        }
        return false;
      }
      lines->push_back(line);
      digits.clear();
      (void)end;
      return true;
    };
    // A line ends at a newline, or at its twelfth digit. '+' is only a separator: RetroArch puts
    // one between a line's halves as well as between lines, so only the count can tell them apart.
    for (size_t i = 0; i <= code.size(); ++i) {
      const char c = i < code.size() ? code[i] : '\n';
      if (c == '\n' || c == '\r' || c == ';') {
        if (!flush(i == code.size()))
          return false;
        if (c == ';') {   // a comment to the end of the line
          while (i < code.size() && code[i] != '\n')
            ++i;
        }
        continue;
      }
      if (isxdigit(static_cast<unsigned char>(c))) {
        digits += static_cast<char>(toupper(static_cast<unsigned char>(c)));
        // Twelve digits make a line, so several on one row - as some sites print them - still
        // read as several lines.
        if (digits.size() == 12 && !flush(false))
          return false;
        continue;
      }
      if (c == ' ' || c == '\t' || c == '-' || c == ':' || c == '+')
        continue;
      if (error != nullptr) {
        char text[160];
        snprintf(text, sizeof(text), "Line %d has '%c', which is not a hex digit.",
                 line_number + 1, c);
        *error = text;
      }
      return false;
    }
    for (size_t i = 0; i < lines->size(); ++i) {
      const uint8_t type = (*lines)[i].type();
      if ((type == 0x50 || type == 0xC2) && i + 1 >= lines->size()) {
        if (error != nullptr)
          *error = std::string(type == 0x50 ? "A 50" : "A C2") +
                   " line needs the line it applies to after it.";
        return false;
      }
    }
    if (lines->empty()) {
      if (error != nullptr)
        *error = "There is no code - type lines like 80012345 0063.";
      return false;
    }
    return true;
  }

  // The machine's memory, as the engine sees it.
  struct CheatMemory {
    uint8_t* ram = nullptr;          // 2 MB, mirrored
    uint8_t* scratchpad = nullptr;   // 1 KB
    // RAM at `offset`, `bytes` long, was changed: drop whatever was compiled from it.
    void (*written)(void* context, uint32_t offset, uint32_t bytes) = nullptr;
    void* context = nullptr;
  };

  class CheatEngine {
   public:
    // The codes to run from now on: every enabled cheat's lines, in order.
    void Set(const std::vector<Cheat>& cheats) {
      codes_.clear();
      for (const Cheat& cheat : cheats) {
        if (cheat.enabled && !cheat.lines.empty())
          codes_.push_back(cheat.lines);
      }
    }
    bool empty() const { return codes_.empty(); }

    // A new game: C1's delay counts from here.
    void ResetFrames() { frames_ = 0; }

    // Once a frame, after it. `buttons` is every pad's held buttons OR'd together, in the
    // pad's own bits (Sio::Button: Select bit 0 ... Square bit 15).
    void Apply(const CheatMemory& memory, uint16_t buttons) {
      ++frames_;
      if (codes_.empty() || memory.ram == nullptr)
        return;
      memory_ = memory;
      // The cartridge's button word is the pad's with its two bytes swapped.
      buttons_ = static_cast<uint16_t>((buttons << 8) | (buttons >> 8));
      for (const std::vector<CheatLine>& code : codes_)
        Run(code);
    }

    uint64_t frames() const { return frames_; }

   private:
    static bool IsCondition(uint8_t type) {
      return (type >= 0xD0 && type <= 0xD4) || (type >= 0xE0 && type <= 0xE3);
    }

    uint8_t Read8(uint32_t address) const { return memory_.ram[address & 0x1FFFFF]; }
    uint16_t Read16(uint32_t address) const {
      return static_cast<uint16_t>(Read8(address) | (Read8(address + 1) << 8));
    }
    void Write8(uint32_t address, uint8_t value) {
      const uint32_t offset = address & 0x1FFFFF;
      if (memory_.ram[offset] == value)
        return;
      memory_.ram[offset] = value;
      if (memory_.written != nullptr)
        memory_.written(memory_.context, offset, 1);
    }
    void Write16(uint32_t address, uint16_t value) {
      Write8(address, static_cast<uint8_t>(value));
      Write8(address + 1, static_cast<uint8_t>(value >> 8));
    }

    // Past the next line that is not a condition - what a false condition skips.
    static size_t SkipGuarded(const std::vector<CheatLine>& code, size_t index) {
      while (index < code.size() && IsCondition(code[index].type()))
        ++index;
      return index + 1;
    }

    void Run(const std::vector<CheatLine>& code) {
      size_t i = 0;
      while (i < code.size()) {
        const CheatLine& line = code[i];
        const uint32_t a = line.address();
        const uint16_t v = line.value;
        bool pass = true;
        switch (line.type()) {
          case 0x30: Write8(a, static_cast<uint8_t>(v)); ++i; break;
          case 0x80: Write16(a, v); ++i; break;
          case 0x1F:
            if (memory_.scratchpad != nullptr) {
              memory_.scratchpad[a & 0x3FE] = static_cast<uint8_t>(v);
              memory_.scratchpad[(a & 0x3FE) + 1] = static_cast<uint8_t>(v >> 8);
            }
            ++i;
            break;
          case 0x10: Write16(a, static_cast<uint16_t>(Read16(a) + v)); ++i; break;
          case 0x11: Write16(a, static_cast<uint16_t>(Read16(a) - v)); ++i; break;
          case 0x20: Write8(a, static_cast<uint8_t>(Read8(a) + v)); ++i; break;
          case 0x21: Write8(a, static_cast<uint8_t>(Read8(a) - v)); ++i; break;

          // A condition guards the next line that is not one.
          case 0xD0: case 0xD1: case 0xD2: case 0xD3:
          case 0xE0: case 0xE1: case 0xE2: case 0xE3:
          case 0xD4: {
            const bool wide = line.type() < 0xE0;
            const uint16_t have = line.type() == 0xD4 ? buttons_
                                  : wide              ? Read16(a)
                                                      : Read8(a);
            const uint16_t want = wide ? v : static_cast<uint16_t>(v & 0xFF);
            switch (line.type() & 0x0F) {
              case 0: pass = have == want; break;
              case 1: pass = have != want; break;
              case 2: pass = have < want; break;
              case 3: pass = have > want; break;
              default: pass = have == want; break;   // D4: the buttons, exactly
            }
            i = pass ? i + 1 : SkipGuarded(code, i + 1);
            break;
          }

          // These guard everything after them, up to a separator.
          case 0xC0: case 0xD5: case 0xD6: {
            pass = line.type() == 0xC0   ? Read16(a) == v
                   : line.type() == 0xD5 ? buttons_ == v
                                         : buttons_ != v;
            ++i;
            if (!pass) {
              while (i < code.size() && !(code[i].first == 0 && code[i].value == 0xFFFF))
                ++i;
              ++i;   // and the separator itself
            }
            break;
          }

          case 0xC1:
            // 0.3 of a frame per unit: 4000 is about 20 seconds at 60 frames a second.
            if (frames_ * 10 / 3 < v)
              return;
            ++i;
            break;

          case 0xC2: {
            const uint32_t to = code[i + 1].address();
            for (uint32_t n = 0; n < v; ++n)
              Write8(to + n, Read8(a + n));
            i += 2;
            break;
          }

          case 0x50: {
            const uint32_t count = (line.first >> 8) & 0xFF;
            const uint32_t step = line.first & 0xFF;
            const CheatLine& write = code[i + 1];
            uint32_t address = write.address();
            uint16_t value = write.value;
            for (uint32_t n = 0; n < count; ++n) {
              if (write.type() == 0x30)
                Write8(address, static_cast<uint8_t>(value));
              else if (write.type() == 0x80)
                Write16(address, value);
              address += step;
              value = static_cast<uint16_t>(value + v);
            }
            i += 2;
            break;
          }

          default:   // 00: nothing - a separator, or a blank
            ++i;
            break;
        }
      }
    }

    std::vector<std::vector<CheatLine>> codes_;
    CheatMemory memory_;
    uint16_t buttons_ = 0;
    uint64_t frames_ = 0;
  };

  // ---- Files ------------------------------------------------------------------------------------

  // A game's cheats as text. The layout is DuckStation's, so its files load as they are:
  //
  //   [Infinite HP]
  //   Enabled = 1
  //   Type = Gameshark
  //   80012345 0063
  //
  // "Enabled" is this emulator's own; a file without it has every cheat off. A cheat marked
  // "Activation = Manual" - one to apply once, when asked - is left out. Any other
  // "key = value" line is kept out of the way. A cheat whose code cannot be parsed - or whose
  // Type is not Gameshark - is left out, and counted in `skipped`.
  //
  // RetroArch's .cht files load too: "cheats = N", then cheatN_desc, cheatN_code (lines joined
  // with '+') and cheatN_enable.
  inline std::vector<Cheat> ParseCheatFile(const std::string& text, int* skipped) {
    std::vector<Cheat> cheats;
    int dropped = 0;
    auto trim = [](std::string s) {
      const char* ws = " \t\r\n";
      const size_t b = s.find_first_not_of(ws);
      if (b == std::string::npos)
        return std::string();
      return s.substr(b, s.find_last_not_of(ws) - b + 1);
    };
    auto unquote = [&](std::string s) {
      s = trim(s);
      if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
        s = s.substr(1, s.size() - 2);
      return s;
    };
    std::vector<std::string> rows;
    {
      size_t start = 0;
      while (start <= text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos)
          end = text.size();
        rows.push_back(trim(text.substr(start, end - start)));
        start = end + 1;
      }
    }

    const bool retroarch = std::any_of(rows.begin(), rows.end(), [](const std::string& r) {
      return r.compare(0, 6, "cheats") == 0 && r.find('=') != std::string::npos &&
             r.compare(0, 7, "cheats_") != 0 && r.find("cheat0") == std::string::npos;
    });
    if (retroarch) {
      // cheatN_desc / cheatN_code / cheatN_enable, in any order.
      std::vector<Cheat> found;
      for (const std::string& row : rows) {
        if (row.compare(0, 5, "cheat") != 0 || !isdigit(static_cast<unsigned char>(row[5])))
          continue;
        const size_t under = row.find('_');
        const size_t eq = row.find('=');
        if (under == std::string::npos || eq == std::string::npos || under > eq)
          continue;
        const size_t index = static_cast<size_t>(std::stoul(row.substr(5, under - 5)));
        if (index > 4096)
          continue;
        if (found.size() <= index)
          found.resize(index + 1);
        const std::string key = trim(row.substr(under + 1, eq - under - 1));
        const std::string value = unquote(row.substr(eq + 1));
        if (key == "desc")
          found[index].name = value;
        else if (key == "code")
          found[index].code = value;
        else if (key == "enable")
          found[index].enabled = value == "true" || value == "1";
      }
      for (Cheat& cheat : found) {
        if (cheat.code.empty())
          continue;
        std::string ignored;
        if (!ParseCheatCode(cheat.code, &cheat.lines, &ignored)) {
          ++dropped;
          continue;
        }
        // One line per row, as the window shows a code.
        std::string tidy;
        for (const CheatLine& line : cheat.lines) {
          char row[16];
          snprintf(row, sizeof(row), "%08X %04X", line.first, line.value);
          tidy += (tidy.empty() ? "" : "\n") + std::string(row);
        }
        cheat.code = tidy;
        if (cheat.name.empty())
          cheat.name = "Cheat " + std::to_string(cheats.size() + 1);
        cheats.push_back(cheat);
      }
    } else {
      Cheat current;
      bool open = false;
      bool gameshark = true;
      auto finish = [&]() {
        if (!open)
          return;
        std::string ignored;
        if (gameshark && ParseCheatCode(current.code, &current.lines, &ignored))
          cheats.push_back(current);
        else
          ++dropped;
      };
      for (const std::string& row : rows) {
        if (row.empty() || row[0] == '#' || row[0] == ';')
          continue;
        if (row.front() == '[' && row.back() == ']') {
          finish();
          current = Cheat();
          current.name = trim(row.substr(1, row.size() - 2));
          open = true;
          gameshark = true;
          continue;
        }
        if (!open)
          continue;
        const size_t eq = row.find('=');
        if (eq != std::string::npos) {
          std::string key = trim(row.substr(0, eq));
          std::transform(key.begin(), key.end(), key.begin(),
                         [](unsigned char c) { return static_cast<char>(tolower(c)); });
          std::string value = trim(row.substr(eq + 1));
          std::transform(value.begin(), value.end(), value.begin(),
                         [](unsigned char c) { return static_cast<char>(tolower(c)); });
          if (key == "enabled")
            current.enabled = value == "1" || value == "true";
          else if (key == "type")
            gameshark = gameshark && value == "gameshark";
          else if (key == "activation")
            // A cheat meant to be applied once, when asked, would do something else entirely
            // applied every frame - so it is not taken.
            gameshark = gameshark && value != "manual";
          continue;
        }
        current.code += (current.code.empty() ? "" : "\n") + row;
      }
      finish();
    }
    if (skipped != nullptr)
      *skipped = dropped;
    return cheats;
  }

  // The same, back to text - this emulator's layout, which is DuckStation's plus "Enabled".
  inline std::string SerialiseCheatFile(const std::vector<Cheat>& cheats,
                                        const std::string& heading) {
    std::string out = "; PSXEmu cheats";
    if (!heading.empty())
      out += " - " + heading;
    out += "\n";
    for (const Cheat& cheat : cheats) {
      out += "\n[" + cheat.name + "]\n";
      out += std::string("Enabled = ") + (cheat.enabled ? "1" : "0") + "\n";
      out += "Type = Gameshark\n";
      out += cheat.code + "\n";
    }
    return out;
  }

}  // namespace psx
}  // namespace emulation
