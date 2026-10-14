/*****************************************************************************************************************
* Copyright (c) 2026 Khalid Ali Al-Kooheji                                                                       *
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
#include "psx/psx.h"
#include "media/mc_directory.h"
#include "emu/movie.h"

#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

namespace emulation {
namespace psx {

namespace {

// ---------------------------------------------------------------------------------------------
// The archive
// ---------------------------------------------------------------------------------------------

uint16_t Read16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t Read32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

std::string Lower(const std::string& text) {
  std::string out = text;
  for (char& c : out)
    c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
  return out;
}

// A text file in a movie is a few megabytes an hour. Anything past this is not one.
const uint32_t kMaxEntrySize = 512u * 1024u * 1024u;

// One file out of a zip, by name, whatever its case. Sizes and offsets come from the central
// directory: .NET's writer, which is BizHawk's, streams its entries, and a streamed entry's own
// header holds zeroes where the sizes go.
//
// 0 not there, 1 read, -1 there and unreadable (`error` says why).
int ReadZipEntry(const uint8_t* data, size_t size, const char* name, std::string* out,
                 std::string* error) {
  const uint32_t kEndSignature = 0x06054B50;
  const uint32_t kCentralSignature = 0x02014B50;
  const uint32_t kLocalSignature = 0x04034B50;
  if (size < 22) {
    *error = "not a zip archive";
    return -1;
  }
  // The end record is the last thing in the file but for a comment of up to 64 KB.
  size_t end = size - 22;
  const size_t stop = size > 22 + 0xFFFF ? size - 22 - 0xFFFF : 0;
  for (;; --end) {
    if (Read32(data + end) == kEndSignature)
      break;
    if (end == stop) {
      *error = "not a zip archive";
      return -1;
    }
  }
  const uint32_t entries = Read16(data + end + 10);
  const uint32_t directory_size = Read32(data + end + 12);
  const uint32_t directory = Read32(data + end + 16);
  if (directory > size || directory_size > size - directory) {
    *error = "the archive's directory is damaged";
    return -1;
  }

  const std::string wanted = Lower(name);
  size_t at = directory;
  for (uint32_t i = 0; i < entries; ++i) {
    if (at + 46 > size || Read32(data + at) != kCentralSignature) {
      *error = "the archive's directory is damaged";
      return -1;
    }
    const uint16_t method = Read16(data + at + 10);
    const uint32_t packed = Read32(data + at + 20);
    const uint32_t unpacked = Read32(data + at + 24);
    const uint16_t name_length = Read16(data + at + 28);
    const uint16_t extra_length = Read16(data + at + 30);
    const uint16_t comment_length = Read16(data + at + 32);
    const uint32_t local = Read32(data + at + 42);
    if (at + 46 + name_length > size) {
      *error = "the archive's directory is damaged";
      return -1;
    }
    const std::string entry(reinterpret_cast<const char*>(data + at + 46), name_length);
    at += 46u + name_length + extra_length + comment_length;
    if (Lower(entry) != wanted)
      continue;

    if (local > size || size - local < 30 || Read32(data + local) != kLocalSignature) {
      *error = std::string(name) + " is damaged in the archive";
      return -1;
    }
    const size_t start = static_cast<size_t>(local) + 30u + Read16(data + local + 26) +
                         Read16(data + local + 28);
    if (start > size || packed > size - start || unpacked > kMaxEntrySize) {
      *error = std::string(name) + " is damaged in the archive";
      return -1;
    }
    if (method == 0) {
      out->assign(reinterpret_cast<const char*>(data + start), packed);
      return 1;
    }
    if (method != 8) {
      *error = std::string(name) + " is compressed in a way this cannot read";
      return -1;
    }
    out->assign(unpacked, '\0');
    z_stream stream = {};
    // Negative window bits: a zip holds the deflate stream bare, with no zlib header round it.
    if (inflateInit2(&stream, -MAX_WBITS) != Z_OK) {
      *error = "could not start decompressing the archive";
      return -1;
    }
    stream.next_in = const_cast<Bytef*>(data + start);
    stream.avail_in = packed;
    stream.next_out = reinterpret_cast<Bytef*>(&(*out)[0]);
    stream.avail_out = unpacked;
    const int result = unpacked == 0 ? Z_STREAM_END : inflate(&stream, Z_FINISH);
    const bool whole = result == Z_STREAM_END && stream.total_out == unpacked;
    inflateEnd(&stream);
    if (!whole) {
      *error = std::string(name) + " is damaged in the archive";
      return -1;
    }
    return 1;
  }
  return 0;
}

// ---------------------------------------------------------------------------------------------
// Header.txt and SyncSettings.json
// ---------------------------------------------------------------------------------------------

std::string Trim(const std::string& text) {
  size_t first = 0;
  size_t last = text.size();
  while (first < last && isspace(static_cast<unsigned char>(text[first])))
    ++first;
  while (last > first && isspace(static_cast<unsigned char>(text[last - 1])))
    --last;
  return text.substr(first, last - first);
}

// The file's lines, without their ends - which are CR LF from BizHawk on Windows.
std::vector<std::string> Lines(const std::string& text) {
  std::vector<std::string> lines;
  size_t at = 0;
  // A byte order mark, which a text file from .NET may start with.
  if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
      static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF)
    at = 3;
  while (at < text.size()) {
    size_t end = text.find('\n', at);
    if (end == std::string::npos)
      end = text.size();
    size_t stop = end;
    if (stop > at && text[stop - 1] == '\r')
      --stop;
    lines.push_back(text.substr(at, stop - at));
    at = end + 1;
  }
  return lines;
}

// "Key value": the key is everything up to the first space. Keys in lower case.
std::map<std::string, std::string> ParseHeader(const std::string& text) {
  std::map<std::string, std::string> header;
  for (const std::string& line : Lines(text)) {
    const size_t space = line.find(' ');
    if (space == std::string::npos) {
      if (!line.empty())
        header[Lower(line)] = "";
      continue;
    }
    header[Lower(line.substr(0, space))] = Trim(line.substr(space + 1));
  }
  return header;
}

bool Truth(const std::string& value) {
  const std::string v = Lower(value);
  return v == "true" || v == "1";
}

// The value that follows "key": in a JSON text, as it is written - an array or an object with
// its brackets, a string with its quotes, a number or a word bare. Empty if the key is not
// there. Enough for the two things looked for in SyncSettings.json; this is no JSON parser.
std::string JsonValue(const std::string& json, const std::string& key) {
  const std::string quoted = "\"" + key + "\"";
  size_t at = 0;
  for (;;) {
    at = json.find(quoted, at);
    if (at == std::string::npos)
      return std::string();
    at += quoted.size();
    size_t i = at;
    while (i < json.size() && isspace(static_cast<unsigned char>(json[i])))
      ++i;
    if (i >= json.size() || json[i] != ':')
      continue;   // the name appeared as a value, not as a key
    ++i;
    while (i < json.size() && isspace(static_cast<unsigned char>(json[i])))
      ++i;
    if (i >= json.size())
      return std::string();
    const size_t start = i;
    if (json[i] == '"') {
      for (++i; i < json.size() && json[i] != '"'; ++i) {
        if (json[i] == '\\')
          ++i;
      }
      return json.substr(start, i + 1 - start);
    }
    if (json[i] == '[' || json[i] == '{') {
      int depth = 0;
      bool in_string = false;
      for (; i < json.size(); ++i) {
        const char c = json[i];
        if (in_string) {
          if (c == '\\')
            ++i;
          else if (c == '"')
            in_string = false;
        } else if (c == '"') {
          in_string = true;
        } else if (c == '[' || c == '{') {
          ++depth;
        } else if (c == ']' || c == '}') {
          if (--depth == 0)
            return json.substr(start, i + 1 - start);
        }
      }
      return std::string();
    }
    while (i < json.size() && json[i] != ',' && json[i] != '}' && json[i] != ']' &&
           !isspace(static_cast<unsigned char>(json[i])))
      ++i;
    return json.substr(start, i - start);
  }
}

std::string Unquote(const std::string& text) {
  const std::string t = Trim(text);
  if (t.size() >= 2 && t.front() == '"' && t.back() == '"')
    return t.substr(1, t.size() - 2);
  return t;
}

// The members of a flat array - "[1, 0, true]" - as their text, strings without their quotes.
std::vector<std::string> JsonArray(const std::string& text) {
  std::vector<std::string> out;
  if (text.size() < 2 || text.front() != '[')
    return out;
  std::string item;
  bool in_string = false;
  for (size_t i = 1; i + 1 < text.size(); ++i) {
    const char c = text[i];
    if (c == '"')
      in_string = !in_string;
    if (c == ',' && !in_string) {
      out.push_back(Unquote(item));
      item.clear();
    } else {
      item += c;
    }
  }
  if (!Trim(item).empty())
    out.push_back(Unquote(item));
  return out;
}

// What the two cores call each device. Octoshock writes its own enumeration, as a number or by
// name; Nymashock writes Mednafen's names.
MovieDevice DeviceFromName(const std::string& text) {
  const std::string name = Lower(text);
  if (name == "0" || name == "none")
    return MovieDevice::kNone;
  if (name == "1" || name == "pad" || name == "gamepad")
    return MovieDevice::kDigital;
  if (name == "2" || name == "dualshock")
    return MovieDevice::kDualShock;
  if (name == "3" || name == "dualanalog")
    return MovieDevice::kDualAnalog;
  return MovieDevice::kUnsupported;
}

// ---------------------------------------------------------------------------------------------
// Input Log.txt
// ---------------------------------------------------------------------------------------------

struct Column {
  enum Kind { kIgnored, kPadButton, kAnalogButton, kLeftX, kLeftY, kRightX, kRightY,
              kConsoleButton, kDisc };
  std::string name;
  int player = 0;        // the n of "P<n> ", 0 for the console's own
  Kind kind = kIgnored;
  uint16_t mask = 0;     // the Sio button, or the MovieConsole one
  int disc_base = 0;     // what the column calls the first disc
  bool axis = false;
  bool sniff = false;    // a name not known: what the first line holds says which it is
};

// The words of a name, in lower case: letters and digits, with anything else between them.
std::vector<std::string> Words(const std::string& text) {
  std::vector<std::string> words;
  std::string word;
  for (char c : text) {
    if (isalnum(static_cast<unsigned char>(c))) {
      word += static_cast<char>(tolower(static_cast<unsigned char>(c)));
    } else if (!word.empty()) {
      words.push_back(word);
      word.clear();
    }
  }
  if (!word.empty())
    words.push_back(word);
  return words;
}

bool Has(const std::vector<std::string>& words, const char* word) {
  return std::find(words.begin(), words.end(), word) != words.end();
}

bool Contains(const std::string& text, const char* part) {
  return text.find(part) != std::string::npos;
}

// One of a pad's controls, from the name either core gives it. Mednafen's names carry a
// description after the symbol - "○ (right)", "L1 (front left shoulder)" - so the order here
// matters: the sticks, the symbols and the shoulders are all told apart before anything is
// taken for a direction because it has "left" in it.
void ClassifyPadControl(const std::string& control, Column* column) {
  const std::string lower = Lower(control);
  const std::vector<std::string> words = Words(control);
  auto button = [column](uint16_t mask) {
    column->kind = Column::kPadButton;
    column->mask = mask;
  };

  if (Contains(lower, "stick")) {
    const bool left = Contains(lower, "lstick") || Contains(lower, "left stick");
    const bool vertical = Contains(lower, "up / down") || (!words.empty() && words.back() == "y");
    const bool horizontal =
        Contains(lower, "left / right") || (!words.empty() && words.back() == "x");
    if (vertical || horizontal) {
      column->kind = left ? (vertical ? Column::kLeftY : Column::kLeftX)
                          : (vertical ? Column::kRightY : Column::kRightX);
      column->axis = true;
      return;
    }
    button(left ? Sio::kL3 : Sio::kR3);   // "Left Stick, Button"
    return;
  }
  if (Has(words, "l3")) return button(Sio::kL3);
  if (Has(words, "r3")) return button(Sio::kR3);
  // The symbols, as UTF-8: a triangle, a circle and a square. Cross is a plain x.
  if (Has(words, "triangle") || Contains(control, "\xE2\x96\xB3")) return button(Sio::kTriangle);
  if (Has(words, "circle") || Contains(control, "\xE2\x97\x8B")) return button(Sio::kCircle);
  if (Has(words, "square") || Contains(control, "\xE2\x96\xA1")) return button(Sio::kSquare);
  if (Has(words, "cross") || Has(words, "x") || Contains(control, "\xC3\x97"))
    return button(Sio::kCross);
  if (Has(words, "l1")) return button(Sio::kL1);
  if (Has(words, "r1")) return button(Sio::kR1);
  if (Has(words, "l2")) return button(Sio::kL2);
  if (Has(words, "r2")) return button(Sio::kR2);
  if (Has(words, "select")) return button(Sio::kSelect);
  if (Has(words, "start")) return button(Sio::kStart);
  if (Has(words, "mode") || Has(words, "analog")) {
    column->kind = Column::kAnalogButton;
    return;
  }
  if (Has(words, "up")) return button(Sio::kUp);
  if (Has(words, "down")) return button(Sio::kDown);
  if (Has(words, "left")) return button(Sio::kLeft);
  if (Has(words, "right")) return button(Sio::kRight);
  column->sniff = true;
}

void ClassifyConsoleControl(const std::string& control, Column* column) {
  const std::vector<std::string> words = Words(control);
  auto button = [column](uint8_t mask) {
    column->kind = Column::kConsoleButton;
    column->mask = mask;
  };
  // "Disc Select" counts from one with nought for none; "Disk Index" from nought with -1.
  if ((Has(words, "disc") || Has(words, "disk")) && (Has(words, "select") || Has(words, "index"))) {
    column->kind = Column::kDisc;
    column->axis = true;
    column->disc_base = Has(words, "index") ? 0 : 1;
    return;
  }
  if (Has(words, "open")) return button(MovieConsole::kOpen);
  if (Has(words, "close")) return button(MovieConsole::kClose);
  if (Has(words, "reset")) return button(MovieConsole::kReset);
  if (Has(words, "power")) return button(MovieConsole::kPower);
  column->sniff = true;
}

// "LogKey:#Disc Select|Open|#P1 Up|P1 Down|" as its columns, in the order a line holds them.
std::vector<Column> ParseLogKey(const std::string& key) {
  std::vector<Column> columns;
  size_t at = key.find('#');
  while (at != std::string::npos && at < key.size()) {
    const size_t next = key.find_first_of("#|", at + 1);
    const std::string name =
        key.substr(at + 1, (next == std::string::npos ? key.size() : next) - at - 1);
    at = next;
    if (name.empty())
      continue;
    Column column;
    column.name = name;
    // "P12 Something": a player's. Anything else is the console's.
    size_t digits = 1;
    while (digits < name.size() && isdigit(static_cast<unsigned char>(name[digits])))
      ++digits;
    if ((name[0] == 'P' || name[0] == 'p') && digits > 1 && digits < name.size() &&
        name[digits] == ' ') {
      column.player = atoi(name.substr(1, digits - 1).c_str());
      ClassifyPadControl(name.substr(digits + 1), &column);
    } else {
      ClassifyConsoleControl(name, &column);
    }
    columns.push_back(column);
  }
  return columns;
}

// Whether what starts at `at` is an axis's number: spaces, digits, a comma.
bool LooksLikeAxis(const std::string& line, size_t at) {
  while (at < line.size() && line[at] == ' ')
    ++at;
  if (at < line.size() && line[at] == '-')
    ++at;
  const size_t digits = at;
  while (at < line.size() && isdigit(static_cast<unsigned char>(line[at])))
    ++at;
  return at > digits && at < line.size() && line[at] == ',';
}

uint8_t StickByte(int value) {
  return static_cast<uint8_t>(value < 0 ? 0 : value > 255 ? 255 : value);
}

// One frame's line into the pads and the console. `slot_of_column` says which of the frame's
// pads each column is for. False if the line ends in the middle of a column.
bool ParseFrame(const std::string& line, std::vector<Column>& columns,
                const std::vector<int>& slot_of_column, bool first_line, MoviePad* pads,
                MovieConsole* console) {
  size_t at = 0;
  for (size_t c = 0; c < columns.size(); ++c) {
    Column& column = columns[c];
    while (at < line.size() && line[at] == '|')
      ++at;
    if (at >= line.size())
      return false;
    if (column.sniff && first_line)
      column.axis = LooksLikeAxis(line, at);

    if (column.axis) {
      const size_t comma = line.find(',', at);
      if (comma == std::string::npos)
        return false;
      const int value = atoi(line.substr(at, comma - at).c_str());
      at = comma + 1;
      MoviePad* pad = slot_of_column[c] >= 0 ? &pads[slot_of_column[c]] : nullptr;
      switch (column.kind) {
        case Column::kLeftX: pad->left_x = StickByte(value); break;
        case Column::kLeftY: pad->left_y = StickByte(value); break;
        case Column::kRightX: pad->right_x = StickByte(value); break;
        case Column::kRightY: pad->right_y = StickByte(value); break;
        case Column::kDisc: {
          const int disc = value < 0 ? (column.disc_base == 0 ? 0 : -1)
                                     : value - column.disc_base + 1;
          console->disc = static_cast<int8_t>(disc > 127 ? 127 : disc);
          break;
        }
        default: break;
      }
      continue;
    }

    const bool down = line[at] != '.';
    ++at;
    if (!down)
      continue;
    MoviePad* pad = slot_of_column[c] >= 0 ? &pads[slot_of_column[c]] : nullptr;
    switch (column.kind) {
      case Column::kPadButton: pad->buttons |= column.mask; break;
      case Column::kAnalogButton: pad->analog = true; break;
      case Column::kConsoleButton: console->buttons |= static_cast<uint8_t>(column.mask); break;
      default: break;
    }
  }
  return true;
}

// ---------------------------------------------------------------------------------------------
// Who plays where
// ---------------------------------------------------------------------------------------------

// What a player's columns alone say it is: a pad with sticks, a pad without, or neither.
MovieDevice DeviceFromColumns(const std::vector<Column>& columns, int player) {
  bool cross = false;
  bool sticks = false;
  for (const Column& column : columns) {
    if (column.player != player)
      continue;
    if (column.kind == Column::kPadButton && column.mask == Sio::kCross)
      cross = true;
    if (column.kind == Column::kLeftX || column.kind == Column::kRightX)
      sticks = true;
  }
  if (!cross)
    return MovieDevice::kUnsupported;
  return sticks ? MovieDevice::kDualShock : MovieDevice::kDigital;
}

// Octoshock numbers its players across the sockets that hold something, port 1's first: with a
// pad in port 2 alone, that pad is P1.
bool PlayersFromOctoshock(const std::string& sync, Movie* movie) {
  const std::vector<std::string> devices = JsonArray(JsonValue(sync, "Devices8"));
  if (devices.size() != 8)
    return false;
  const std::vector<std::string> multitaps = JsonArray(JsonValue(sync, "Multitaps"));
  const std::vector<std::string> cards = JsonArray(JsonValue(sync, "Memcards"));
  for (int port = 0; port < Movie::kPorts; ++port) {
    movie->multitap[port] = multitaps.size() == 2 && Truth(multitaps[port]);
    if (cards.size() == 2)
      movie->memory_card[port] = Truth(cards[port]);
  }
  int number = 1;
  for (int port = 0; port < Movie::kPorts; ++port) {
    const int sockets = movie->multitap[port] ? Movie::kSockets : 1;
    for (int socket = 0; socket < sockets; ++socket) {
      const std::string& name = devices[port * 4 + socket];
      const MovieDevice device = DeviceFromName(name);
      if (device == MovieDevice::kNone)
        continue;
      Movie::Player player;
      player.number = number++;
      player.port = port;
      player.socket = socket;
      player.device = device;
      player.device_name = name == "4" ? "negcon" : name;
      movie->players.push_back(player);
    }
  }
  return true;
}

// Nymashock's P<n> is Mednafen's virtual port n. A multitap on a console port takes four of
// them, in order: with one on port 1, virtual ports 1-4 are its sockets and 5 is port 2.
bool PlayersFromNymashock(const std::string& sync, const std::vector<int>& numbers,
                          Movie* movie) {
  const std::string devices = JsonValue(sync, "PortDevices");
  if (devices.empty())
    return false;
  const std::string values = JsonValue(sync, "MednafenValues");
  for (int port = 0; port < Movie::kPorts; ++port) {
    const std::string n = std::to_string(port + 1);
    movie->multitap[port] = Truth(Unquote(JsonValue(values, "psx.input.pport" + n + ".multitap")));
    // Mednafen's own default has a card in every port; BizHawk's has one in the first alone.
    const std::string card = Unquote(JsonValue(values, "psx.input.port" + n + ".memcard"));
    movie->memory_card[port] = card.empty() ? port == 0 : Truth(card);
  }
  for (int number : numbers) {
    int virtual_port = number - 1;
    Movie::Player player;
    player.number = number;
    bool placed = false;
    for (int port = 0; port < Movie::kPorts && !placed; ++port) {
      const int sockets = movie->multitap[port] ? Movie::kSockets : 1;
      if (virtual_port < sockets) {
        player.port = port;
        player.socket = virtual_port;
        placed = true;
      }
      virtual_port -= sockets;
    }
    if (!placed)
      continue;   // a virtual port with no socket behind it: nothing reads it
    // Port 1 holds a DualShock unless the movie says otherwise, and the rest nothing.
    std::string name = Unquote(JsonValue(devices, std::to_string(number - 1)));
    if (name.empty())
      name = number == 1 ? "dualshock" : "none";
    player.device = DeviceFromName(name);
    player.device_name = name;
    if (player.device != MovieDevice::kNone)
      movie->players.push_back(player);
  }
  return true;
}

// With nothing to go on but the log: P1 and P2 in the two ports, and past two players a multitap
// in port 1 for the first four.
void PlayersFromColumns(const std::vector<Column>& columns, const std::vector<int>& numbers,
                        Movie* movie) {
  const bool tap = numbers.size() > 2;
  movie->multitap[0] = tap;
  movie->multitap[1] = numbers.size() > 5;
  for (size_t i = 0; i < numbers.size(); ++i) {
    Movie::Player player;
    player.number = numbers[i];
    if (!tap) {
      player.port = static_cast<int>(i);
    } else if (i < 4) {
      player.socket = static_cast<int>(i);
    } else {
      player.port = 1;
      player.socket = static_cast<int>(i) - 4;
      if (player.socket >= Movie::kSockets)
        break;
    }
    player.device = DeviceFromColumns(columns, player.number);
    player.device_name = "controller";
    movie->players.push_back(player);
  }
}

Sio::ControllerType SioType(MovieDevice device) {
  switch (device) {
    case MovieDevice::kDigital: return Sio::kDigital;
    case MovieDevice::kDualAnalog: return Sio::kDualAnalog;
    case MovieDevice::kDualShock: return Sio::kDualShock;
    default: return Sio::kNone;
  }
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------------------------

std::string Movie::Summary() const {
  std::string text = game.empty() ? std::string("Movie") : game;
  if (!author.empty())
    text += " by " + author;
  // The count with its thousands apart, which is how a run's length is read.
  std::string count = std::to_string(frames());
  for (int at = static_cast<int>(count.size()) - 3; at > 0; at -= 3)
    count.insert(static_cast<size_t>(at), ",");
  text += ", " + count + " frames";
  if (!core.empty())
    text += " (" + core + ")";
  return text;
}

// SHA-1, as FIPS 180-1 gives it. Only to name a BIOS image by, which is what BizHawk's header
// does; nothing here leans on it for more.
std::string Sha1Hex(const uint8_t* data, size_t size) {
  uint32_t h[5] = { 0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u };
  auto rotate = [](uint32_t value, int bits) { return (value << bits) | (value >> (32 - bits)); };
  auto block = [&h, &rotate](const uint8_t* p) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i)
      w[i] = (static_cast<uint32_t>(p[i * 4]) << 24) | (static_cast<uint32_t>(p[i * 4 + 1]) << 16) |
             (static_cast<uint32_t>(p[i * 4 + 2]) << 8) | static_cast<uint32_t>(p[i * 4 + 3]);
    for (int i = 16; i < 80; ++i)
      w[i] = rotate(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; ++i) {
      uint32_t f, k;
      if (i < 20) {
        f = (b & c) | (~b & d);
        k = 0x5A827999u;
      } else if (i < 40) {
        f = b ^ c ^ d;
        k = 0x6ED9EBA1u;
      } else if (i < 60) {
        f = (b & c) | (b & d) | (c & d);
        k = 0x8F1BBCDCu;
      } else {
        f = b ^ c ^ d;
        k = 0xCA62C1D6u;
      }
      const uint32_t t = rotate(a, 5) + f + e + k + w[i];
      e = d;
      d = c;
      c = rotate(b, 30);
      b = a;
      a = t;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
  };
  size_t at = 0;
  for (; at + 64 <= size; at += 64)
    block(data + at);
  // The tail: what is left, a one bit, zeroes, and the length in bits.
  uint8_t tail[128] = {};
  const size_t left = size - at;
  if (left > 0)
    memcpy(tail, data + at, left);
  tail[left] = 0x80;
  const size_t padded = left < 56 ? 64 : 128;
  const uint64_t bits = static_cast<uint64_t>(size) * 8;
  for (int i = 0; i < 8; ++i)
    tail[padded - 1 - static_cast<size_t>(i)] = static_cast<uint8_t>(bits >> (8 * i));
  block(tail);
  if (padded == 128)
    block(tail + 64);
  char text[41];
  snprintf(text, sizeof(text), "%08x%08x%08x%08x%08x", h[0], h[1], h[2], h[3], h[4]);
  return text;
}

uint32_t BizHawkDiscId(const Disc& disc) {
  uLong crc = crc32(0, nullptr, 0);
  // Every number goes in as four bytes, low byte first - a C# int through BitConverter.
  auto add = [&crc](int32_t value) {
    const uint8_t bytes[4] = { static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8),
                               static_cast<uint8_t>(value >> 16),
                               static_cast<uint8_t>(value >> 24) };
    crc = crc32(crc, bytes, 4);
  };
  // The session's format - 20h, CD-ROM XA, which is every PlayStation disc - then the first and
  // last track numbers.
  add(0x20);
  add(1);
  add(disc.track_count());
  // A hundred entries: the tracks, and in the hundredth the lead-out. Each is its control (4 for
  // data, 0 for music and for the lead-out), whether it is there, and where it starts, counted
  // from the first sector of track 1. One that is not there is three zeroes.
  const int32_t lead_in = static_cast<int32_t>(Disc::kLeadInSectors);
  for (int entry = 1; entry <= 100; ++entry) {
    if (entry <= disc.track_count() && entry < 100) {
      const Disc::Track& track = disc.track(entry - 1);
      add(track.type == Disc::kTrackData ? 4 : 0);
      add(1);
      add(static_cast<int32_t>(track.start_lba) - lead_in);
    } else if (entry == 100) {
      add(0);
      add(1);
      add(static_cast<int32_t>(disc.total_sectors()) - lead_in);
    } else {
      add(0);
      add(0);
      add(0);
    }
  }
  // The first 26 sectors, all 2,352 bytes of each: 25 was not enough to tell two pressings of
  // Arc the Lad II apart, BizHawk's own note says.
  uint8_t sector[2352];
  for (uint32_t i = 0; i < 26; ++i) {
    memset(sector, 0, sizeof(sector));
    disc.ReadSector(Disc::kLeadInSectors + i, sector);
    crc = crc32(crc, sector, sizeof(sector));
  }
  return static_cast<uint32_t>(crc);
}

namespace {

// "SLPM-86247" from the disc's SYSTEM.CNF, whose BOOT line names the executable SLPM_862.47.
// Empty for a disc without one.
std::string SerialOfDisc(Disc& disc) {
  Iso9660 iso;
  if (!iso.Open(&disc))
    return std::string();
  Iso9660::File file;
  std::vector<uint8_t> contents;
  std::string boot;
  if (!iso.Find("SYSTEM.CNF", &file) || !iso.Read(file, &contents) ||
      !System::ParseSystemCnf(contents, &boot))
    return std::string();
  const size_t start = boot.find_last_of(":\\/");
  std::string name = boot.substr(start == std::string::npos ? 0 : start + 1);
  const size_t version = name.find(';');
  if (version != std::string::npos)
    name.erase(version);
  std::string serial;
  for (char c : name) {
    if (c == '_' || c == '-')
      serial += '-';
    else if (isalnum(static_cast<unsigned char>(c)))
      serial += static_cast<char>(toupper(static_cast<unsigned char>(c)));
  }
  return serial;
}

}  // namespace

MovieDiscCheck CheckMovieDisc(const Movie& movie, Disc& disc) {
  MovieDiscCheck check;
  char id[16];
  snprintf(id, sizeof(id), "%08x", BizHawkDiscId(disc));
  check.disc_id = id;
  // Only an eight-digit hash is BizHawk's disc id; anything longer is of something this has no
  // way to work out again, and says nothing either way.
  if (movie.disc_hash.size() == 8)
    check.by_hash = movie.disc_hash == check.disc_id ? MovieDiscCheck::kSame
                                                    : MovieDiscCheck::kDifferent;

  check.disc_serial = SerialOfDisc(disc);
  for (int port = 0; port < Movie::kPorts; ++port) {
    if (movie.card[port].size() != Movie::kCardSize)
      continue;
    // "BISLPM-86247G:E0pBAA": B, the region's letter, then the ten characters of the serial.
    for (const mcdir::Save& save : mcdir::List(movie.card[port].data(), false)) {
      if (save.filename.size() < 12)
        continue;
      std::string serial = save.filename.substr(2, 10);
      for (char& c : serial)
        c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
      if (std::find(check.card_serials.begin(), check.card_serials.end(), serial) ==
          check.card_serials.end())
        check.card_serials.push_back(serial);
    }
  }
  if (!check.card_serials.empty() && !check.disc_serial.empty()) {
    const bool found = std::find(check.card_serials.begin(), check.card_serials.end(),
                                 check.disc_serial) != check.card_serials.end();
    check.by_serial = found ? MovieDiscCheck::kSame : MovieDiscCheck::kDifferent;
  }
  return check;
}

std::string MovieDiscCheck::Describe(const Movie& movie) const {
  std::string text;
  if (by_hash == kSame) {
    text = "the movie names its disc " + movie.disc_hash + ", and this is it";
  } else if (by_hash == kDifferent) {
    text = "the movie was made on disc " + movie.disc_hash + " and this one is " + disc_id +
           ": NOT the same disc";
  } else if (!movie.disc_hash.empty()) {
    text = "the movie names its disc by a hash that cannot be worked out here (" +
           movie.disc_hash + "); BizHawk's id for this disc is " + disc_id;
  } else {
    text = "the movie does not say which disc it was made on; BizHawk's id for this one is " +
           disc_id;
  }
  if (by_serial != kCannotTell) {
    std::string serials;
    for (const std::string& serial : card_serials)
      serials += (serials.empty() ? "" : ", ") + serial;
    text += by_serial == kSame
                ? ". The saves on its memory card are " + serials + "'s, and so is this disc"
                : ". The saves on its memory card are " + serials + "'s and this disc is " +
                      disc_serial + ": NOT the same game";
  }
  return text;
}

bool ParseBk2Text(const std::string& header_text, const std::string& input_log,
                  const std::string& sync_settings, Movie* movie, std::string* error,
                  const std::vector<uint8_t>* save_ram) {
  *movie = Movie();
  const std::map<std::string, std::string> header = ParseHeader(header_text);
  auto value = [&header](const char* key) {
    const auto found = header.find(key);
    return found == header.end() ? std::string() : found->second;
  };
  movie->platform = value("platform");
  movie->core = value("core");
  movie->game = value("gamename");
  movie->author = value("author");
  movie->emulator = value("emuversion");
  movie->rerecords = strtoull(value("rerecordcount").c_str(), nullptr, 10);
  movie->pal = Truth(value("pal"));
  movie->from_savestate = Truth(value("startsfromsavestate"));
  movie->from_saveram = Truth(value("startsfromsaveram"));
  // The disc, under whichever of the hash keys the BizHawk that wrote it used; some write the
  // kind in front, "SHA1:...".
  for (const char* key : { "sha1", "sha256", "md5", "crc32" }) {
    std::string hash = Lower(value(key));
    const size_t colon = hash.find_last_of(':');
    if (colon != std::string::npos)
      hash.erase(0, colon + 1);
    if (!hash.empty() && movie->disc_hash.empty())
      movie->disc_hash = hash;
  }
  // "PSX_Firmware_J 3438...": the BIOS of whichever region the game was, by its SHA-1.
  for (const auto& entry : header) {
    if (entry.first.compare(0, 13, "psx_firmware_") == 0 && entry.second.size() == 40)
      movie->bios_sha1 = Lower(entry.second);
  }

  if (!movie->platform.empty() && Lower(movie->platform) != "psx") {
    *error = "this movie is for another machine (" + movie->platform + "), not the PlayStation";
    return false;
  }
  if (movie->from_savestate) {
    *error = "this movie starts from a BizHawk save state, which only BizHawk can load - "
             "only movies that start from power-on can be played";
    return false;
  }
  // The log: its key, then a line a frame.
  const std::vector<std::string> lines = Lines(input_log);
  std::vector<Column> columns;
  size_t first_frame = lines.size();
  for (size_t i = 0; i < lines.size(); ++i) {
    if (lines[i].compare(0, 7, "LogKey:") == 0) {
      columns = ParseLogKey(lines[i]);
    } else if (!lines[i].empty() && lines[i][0] == '|') {
      first_frame = i;
      break;
    }
  }
  if (columns.empty()) {
    *error = "the movie's input log has no LogKey line naming its columns - it is from a "
             "BizHawk too old to read";
    return false;
  }

  // The players the log has, in the order of their numbers.
  std::vector<int> numbers;
  for (const Column& column : columns) {
    if (column.player > 0 &&
        std::find(numbers.begin(), numbers.end(), column.player) == numbers.end())
      numbers.push_back(column.player);
  }
  std::sort(numbers.begin(), numbers.end());

  const std::string core = Lower(movie->core);
  bool placed = false;
  if (!sync_settings.empty()) {
    if (core.find("nyma") != std::string::npos)
      placed = PlayersFromNymashock(sync_settings, numbers, movie);
    else
      placed = PlayersFromOctoshock(sync_settings, movie);
    if (!placed)   // whichever the header named, try the other's layout
      placed = PlayersFromOctoshock(sync_settings, movie) ||
               PlayersFromNymashock(sync_settings, numbers, movie);
  }
  // Settings that name players the log has no columns for - or the other way about - are not
  // this log's: the log is what there is to play.
  if (placed) {
    for (const Movie::Player& player : movie->players) {
      if (std::find(numbers.begin(), numbers.end(), player.number) == numbers.end())
        placed = false;
    }
  }
  if (!placed) {
    movie->players.clear();
    movie->multitap[0] = movie->multitap[1] = false;
    PlayersFromColumns(columns, numbers, movie);
  }

  const bool nymashock = core.find("nyma") != std::string::npos;
  movie->new_cards_formatted = nymashock;
  for (Movie::Player& player : movie->players)
    player.always_analog = player.device == MovieDevice::kDualAnalog;

  // The cards a movie starts from: 128 KB for each slot that has one, in the slots' order.
  if (movie->from_saveram) {
    const size_t slots = (movie->memory_card[0] ? 1u : 0u) + (movie->memory_card[1] ? 1u : 0u);
    if (save_ram == nullptr || save_ram->empty()) {
      *error = "this movie starts from memory cards saved inside it, and they are not in the "
               "file (no MovieSaveRam.bin)";
      return false;
    }
    if (slots == 0 || save_ram->size() != slots * Movie::kCardSize) {
      *error = "this movie starts from memory cards saved inside it, laid out in a way this "
               "does not know: " + std::to_string(save_ram->size()) + " bytes for " +
               std::to_string(slots) + " card" + (slots == 1 ? "" : "s");
      return false;
    }
    size_t at = 0;
    for (int port = 0; port < Movie::kPorts; ++port) {
      if (!movie->memory_card[port])
        continue;
      movie->card[port].assign(save_ram->begin() + static_cast<ptrdiff_t>(at),
                               save_ram->begin() + static_cast<ptrdiff_t>(at + Movie::kCardSize));
      at += Movie::kCardSize;
    }
  }

  for (const Movie::Player& player : movie->players) {
    if (player.device == MovieDevice::kUnsupported) {
      *error = "this movie has a " + player.device_name + " in port " +
               std::to_string(player.port + 1) +
               ", and only digital pads, Dual Analog pads and DualShocks can be played back";
      return false;
    }
  }

  // Which of a frame's pads each column fills: -1 for the console's, and for a player with
  // nothing plugged in.
  std::vector<int> slot_of_column(columns.size(), -1);
  for (size_t c = 0; c < columns.size(); ++c) {
    Column& column = columns[c];
    if (column.player == 0)
      continue;
    for (size_t p = 0; p < movie->players.size(); ++p) {
      if (movie->players[p].number == column.player)
        slot_of_column[c] = static_cast<int>(p);
    }
    if (slot_of_column[c] < 0) {
      // Read past, like a name not known - but its kind still says whether it is an axis.
      column.kind = Column::kIgnored;
    }
  }
  for (const Column& column : columns) {
    if (column.sniff)
      movie->ignored.push_back(column.name);
  }

  const size_t players = movie->players.size();
  size_t frames = 0;
  for (size_t i = first_frame; i < lines.size(); ++i) {
    if (!lines[i].empty() && lines[i][0] == '|')
      ++frames;
  }
  movie->console.reserve(frames);
  movie->pads.reserve(frames * players);
  std::vector<MoviePad> pads(players);
  bool first_line = true;
  for (size_t i = first_frame; i < lines.size(); ++i) {
    const std::string& line = lines[i];
    if (line.empty() || line[0] != '|')
      continue;   // "[/Input]", or a blank line at the end
    std::fill(pads.begin(), pads.end(), MoviePad());
    MovieConsole console;
    // Without a disc column there is one disc and it stays in.
    console.disc = -1;
    if (!ParseFrame(line, columns, slot_of_column, first_line, pads.data(), &console)) {
      *error = "frame " + std::to_string(movie->console.size()) +
               " of the movie's input log does not match its LogKey";
      return false;
    }
    first_line = false;
    movie->console.push_back(console);
    movie->pads.insert(movie->pads.end(), pads.begin(), pads.end());
  }
  if (movie->console.empty()) {
    *error = "the movie has no frames";
    return false;
  }
  return true;
}

bool ParseBk2(const uint8_t* data, size_t size, Movie* movie, std::string* error) {
  std::string header;
  std::string log;
  std::string sync;
  if (ReadZipEntry(data, size, "Header.txt", &header, error) < 0)
    return false;
  const int found = ReadZipEntry(data, size, "Input Log.txt", &log, error);
  if (found < 0)
    return false;
  if (found == 0) {
    *error = "there is no Input Log.txt in the archive - this is not a .bk2 movie";
    return false;
  }
  // Without its settings a movie still plays: the log's own columns say who there is.
  std::string ignored;
  if (ReadZipEntry(data, size, "SyncSettings.json", &sync, &ignored) < 0)
    sync.clear();
  // The cards a movie starts from, if it starts from any. Newer BizHawks compress the entry
  // with zstd and name it so, which there is nothing here to read.
  std::string ram;
  if (ReadZipEntry(data, size, "MovieSaveRam.bin", &ram, error) < 0)
    return false;
  if (ram.empty() && ReadZipEntry(data, size, "MovieSaveRam.bin.zst", &ram, &ignored) != 0) {
    *error = "this movie starts from memory cards saved inside it, compressed with zstd, which "
             "this cannot read";
    return false;
  }
  const std::vector<uint8_t> save_ram(ram.begin(), ram.end());
  return ParseBk2Text(header, log, sync, movie, error, &save_ram);
}

bool LoadBk2(const std::string& path, Movie* movie, std::string* error) {
  FILE* file = fopen(path.c_str(), "rb");
  if (file == nullptr) {
    *error = "the movie file could not be opened";
    return false;
  }
  std::vector<uint8_t> data;
  fseek(file, 0, SEEK_END);
  const long size = ftell(file);
  fseek(file, 0, SEEK_SET);
  if (size > 0) {
    data.resize(static_cast<size_t>(size));
    if (fread(data.data(), 1, data.size(), file) != data.size())
      data.clear();
  }
  fclose(file);
  if (data.empty()) {
    *error = "the movie file could not be read";
    return false;
  }
  return ParseBk2(data.data(), data.size(), movie, error);
}

// ---------------------------------------------------------------------------------------------
// Playing
// ---------------------------------------------------------------------------------------------

void MoviePlayer::Start(Movie movie) {
  movie_ = std::move(movie);
  playing_ = movie_.frames() > 0;
  offset_ = 0;
  position_ = 0;
  frame_ = 0;
  have_last_ = false;
  const size_t players = movie_.players.size();
  current_.assign(players, MoviePad());
  analog_held_.assign(players, false);
  analog_pressed_.assign(players, false);
  // The first frame's lid is the state the machine boots in (initial_disc), not an event.
  selected_disc_ = initial_disc();
  lid_open_ = selected_disc_ == 0;
  if (lid_open_ && movie_.frames() > 0 && movie_.console[0].disc > 0)
    selected_disc_ = movie_.console[0].disc;
}

void MoviePlayer::Stop() {
  playing_ = false;
  current_.assign(current_.size(), MoviePad());
  analog_pressed_.assign(analog_pressed_.size(), false);
}

int MoviePlayer::initial_disc() const {
  if (movie_.frames() == 0)
    return 1;
  const MovieConsole& first = movie_.console[0];
  // Open held and Close not: the movie starts with the lid up.
  if ((first.buttons & MovieConsole::kOpen) != 0 && (first.buttons & MovieConsole::kClose) == 0)
    return 0;
  return first.disc < 0 ? 1 : first.disc;
}

MoviePlayer::Step MoviePlayer::Next(uint64_t frame_count) {
  Step step;
  if (!playing_)
    return step;
  if (have_last_ && frame_count == last_frame_count_) {
    // The same frame again: its pads stand, but a press of ANALOG is not made twice.
    analog_pressed_.assign(analog_pressed_.size(), false);
    return step;
  }
  have_last_ = true;
  last_frame_count_ = frame_count;

  const int64_t index = position_++ - offset_;
  current_.assign(current_.size(), MoviePad());
  analog_pressed_.assign(analog_pressed_.size(), false);
  if (index < 0)
    return step;   // waiting out the offset, with nothing held
  if (index >= static_cast<int64_t>(movie_.frames())) {
    frame_ = movie_.frames();
    playing_ = false;
    step.finished = true;
    return step;
  }
  frame_ = static_cast<uint32_t>(index);

  const MovieConsole& console = movie_.console[frame_];
  if ((console.buttons & (MovieConsole::kReset | MovieConsole::kPower)) != 0) {
    step.reset = true;
    // The GPU's count starts again with the machine, so it says nothing about the next frame.
    have_last_ = false;
  }
  if (frame_ > 0) {
    if (!lid_open_ && (console.buttons & MovieConsole::kOpen) != 0) {
      lid_open_ = true;
      step.eject = true;
    }
    if (lid_open_ && console.disc >= 0)
      selected_disc_ = console.disc;
    if (lid_open_ && (console.buttons & MovieConsole::kClose) != 0) {
      lid_open_ = false;
      step.insert = selected_disc_;
    }
  }

  for (size_t p = 0; p < current_.size(); ++p) {
    current_[p] = movie_.pad(frame_, p);
    analog_pressed_[p] = current_[p].analog && !analog_held_[p];
    analog_held_[p] = current_[p].analog;
  }
  return step;
}

// ---------------------------------------------------------------------------------------------
// The machine
// ---------------------------------------------------------------------------------------------

void ApplyMovieTiming(EmuConfig& config) {
  config.cdrom_mednafen_timing = true;
  config.cdrom_mechanical_timing = false;
  config.recompiler = false;
  config.icache_timing = false;
  config.gpu_transfer_timing = false;
  config.exact_event_timing = false;
  config.dma_stops_cpu = false;
  config.measured_bus_timing = false;
  config.write_queue_timing = false;
  // The real BIOS, from its first instruction: the recording went through its intro.
  config.hle_bios = false;
  config.skip_bios_intro = false;
}

uint16_t ApplyMoviePads(System& system, const MoviePlayer& player) {
  Sio& sio = system.sio();
  const Movie& movie = player.movie();
  auto player_at = [&movie](int port, int socket) -> const Movie::Player* {
    for (const Movie::Player& p : movie.players) {
      if (p.port == port && p.socket == socket)
        return &p;
    }
    return nullptr;
  };

  // What is plugged in, every frame, as a front end sets it from its settings: Sio takes a type
  // it already has as no change.
  for (int port = 0; port < Movie::kPorts; ++port) {
    if (movie.multitap[port]) {
      sio.set_controller_type(port, Sio::kMultitap);
      for (int socket = 0; socket < Movie::kSockets; ++socket) {
        const Movie::Player* p = player_at(port, socket);
        const Sio::ControllerType type = p != nullptr ? SioType(p->device) : Sio::kNone;
        sio.set_multitap_player_type(port, socket, type);
        sio.set_connected(port, type != Sio::kNone, socket);
      }
      continue;
    }
    const Movie::Player* p = player_at(port, 0);
    const Sio::ControllerType type = p != nullptr ? SioType(p->device) : Sio::kNone;
    sio.set_controller_type(port, type);
    sio.set_connected(port, type != Sio::kNone);
  }

  uint16_t held = 0;
  const std::vector<MoviePad>& pads = player.pads();
  const std::vector<bool>& analog = player.analog_pressed();
  for (size_t i = 0; i < movie.players.size() && i < pads.size(); ++i) {
    const Movie::Player& p = movie.players[i];
    const MoviePad& pad = pads[i];
    sio.set_buttons(p.port, pad.buttons, p.socket);
    sio.set_axes(p.port, pad.left_x, pad.left_y, pad.right_x, pad.right_y, p.socket);
    if (p.always_analog) {
      // Mednafen's Dual Analog: analog whatever happens, and its MODE button does nothing.
      if (!sio.analog_mode(p.port, p.socket))
        sio.PressAnalogButton(p.port, p.socket);
    } else if (analog[i]) {
      sio.PressAnalogButton(p.port, p.socket);
    }
    held |= pad.buttons;
  }
  return held;
}

bool ResetForMovie(System& system, const char* bios_path) {
  // Deinitialize closes the disc and takes the cards out, saving them; both go back as they were.
  const std::string disc = system.cdrom().disc_loaded() ? system.cdrom().disc().path()
                                                        : std::string();
  std::string cards[Movie::kPorts][System::kCardSlotsPerPort];
  for (int port = 0; port < Movie::kPorts; ++port)
    for (int slot = 0; slot < System::kCardSlotsPerPort; ++slot)
      cards[port][slot] = system.mc(port, slot).filename();

  system.Deinitialize();
  if (system.Initialize(bios_path) != 0)
    return false;
  system.set_auto_boot(false);
  if (!disc.empty())
    system.LoadDisc(disc.c_str());
  for (int port = 0; port < Movie::kPorts; ++port) {
    for (int slot = 0; slot < System::kCardSlotsPerPort; ++slot) {
      if (!cards[port][slot].empty())
        system.mc(port, slot).LoadFile(cards[port][slot].c_str());
    }
  }
  return true;
}

bool InsertMovieCards(System& system, const Movie& movie, const std::string& directory,
                      std::string* error) {
  for (int port = 0; port < Movie::kPorts; ++port) {
    // The sockets a multitap adds start empty, as they do on both of BizHawk's cores.
    for (int slot = 1; slot < System::kCardSlotsPerPort; ++slot)
      system.mc(port, slot).Eject();
    MC& card = system.mc(port, 0);
    card.Eject();
    if (!movie.memory_card[port])
      continue;
    const std::string path =
        directory + "\\movie_card" + std::to_string(port + 1) + ".mcr";
    // CreateFile writes a formatted card over whatever the last movie left.
    if (card.CreateFile(path.c_str()) != S_OK) {
      *error = "could not make a new memory card for the movie at " + path;
      return false;
    }
    // ...which is then the movie's own, or blank the way the recording's core left a new one.
    const std::vector<uint8_t>& saved = movie.card[port];
    if (saved.size() == Movie::kCardSize)
      memcpy(card.data(), saved.data(), Movie::kCardSize);
    else if (!movie.new_cards_formatted)
      memset(card.data(), 0, Movie::kCardSize);
    else
      continue;
    card.Modified();
    card.Flush();
  }
  return true;
}

}  // namespace psx
}  // namespace emulation
