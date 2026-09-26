// Standards section 1.
#pragma once

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace utilities {

// What a controller binding is made of, without anything that needs Windows.
//
// A binding map is one code per pad button, in the front end's button order
// (kKeyBindings), 0 for none. For the keyboard a code is a virtual-key code;
// for a gamepad it is a PadInput below. The front end keeps one map per port
// and per device - "Port 2 on the keyboard" and "Port 1 on the keyboard" can
// differ, which is what lets two players share one keyboard - and the machine
// thread turns what the host is holding into the pad's buttons through them.

// ---------------------------------------------------------------------------
// A gamepad's controls
// ---------------------------------------------------------------------------

// Everything on a pad that can be pressed or pushed, by position - named as
// on an XInput pad, with a DualShock 4's or DualSense's in the same places. A
// code is the bit index plus one, so 0 stays "none". A stick pushed past
// kStickPress counts as pressed in that direction, which is what lets a
// button be bound to one. The order is load-bearing: it is the bit layout of
// PadReading::inputs, and the settings file stores the keys, not the numbers.
enum PadInput : int {
  kPadNone = 0,
  kPadA, kPadB, kPadX, kPadY,
  kPadLB, kPadRB, kPadLT, kPadRT,
  kPadBack, kPadStart, kPadLS, kPadRS,
  kPadDpadUp, kPadDpadDown, kPadDpadLeft, kPadDpadRight,
  kPadLStickUp, kPadLStickDown, kPadLStickLeft, kPadLStickRight,
  kPadRStickUp, kPadRStickDown, kPadRStickLeft, kPadRStickRight,
  kPadInputEnd
};
inline constexpr int kPadInputCount = kPadInputEnd - 1;

struct PadInputName {
  const char* key;           // what psxemu.ini stores
  const char* label;         // what the bindings window shows
  const char* playstation;   // the same, for a DualShock 4 or DualSense
};

// clang-format off
inline constexpr PadInputName kPadInputNames[kPadInputCount] = {
    { "A",           "A",                 "Cross" },
    { "B",           "B",                 "Circle" },
    { "X",           "X",                 "Square" },
    { "Y",           "Y",                 "Triangle" },
    { "LB",          "LB",                "L1" },
    { "RB",          "RB",                "R1" },
    { "LT",          "LT",                "L2" },
    { "RT",          "RT",                "R2" },
    { "Back",        "Back",              "Share / Create" },
    { "Start",       "Start",             "Options" },
    { "LS",          "Left Stick Press",  "L3" },
    { "RS",          "Right Stick Press", "R3" },
    { "DpadUp",      "D-Pad Up",          "D-Pad Up" },
    { "DpadDown",    "D-Pad Down",        "D-Pad Down" },
    { "DpadLeft",    "D-Pad Left",        "D-Pad Left" },
    { "DpadRight",   "D-Pad Right",       "D-Pad Right" },
    { "LStickUp",    "Left Stick Up",     "Left Stick Up" },
    { "LStickDown",  "Left Stick Down",   "Left Stick Down" },
    { "LStickLeft",  "Left Stick Left",   "Left Stick Left" },
    { "LStickRight", "Left Stick Right",  "Left Stick Right" },
    { "RStickUp",    "Right Stick Up",    "Right Stick Up" },
    { "RStickDown",  "Right Stick Down",  "Right Stick Down" },
    { "RStickLeft",  "Right Stick Left",  "Right Stick Left" },
    { "RStickRight", "Right Stick Right", "Right Stick Right" },
};
// clang-format on

inline constexpr uint32_t PadInputBit(int code) {
  return (code > 0 && code <= kPadInputCount) ? 1u << (code - 1) : 0u;
}

// The settings-file key for a code, "" for none or anything out of range.
inline std::string PadInputKey(int code) {
  if (code <= 0 || code > kPadInputCount)
    return std::string();
  return kPadInputNames[code - 1].key;
}

// `playstation` for a DualShock 4 or DualSense, whose face buttons are shapes
// and whose shoulders are numbered: the same control, called what it says on
// the pad in the person's hands.
inline std::string PadInputLabel(int code, bool playstation = false) {
  if (code <= 0 || code > kPadInputCount)
    return std::string();
  return playstation ? kPadInputNames[code - 1].playstation : kPadInputNames[code - 1].label;
}

// The reverse, ignoring case. 0 for an empty or unknown key, so a hand-edited
// typo leaves the button unbound rather than bound to something surprising.
inline int PadInputFromKey(const std::string& key) {
  for (int i = 0; i < kPadInputCount; ++i) {
    const char* name = kPadInputNames[i].key;
    if (key.size() != strlen(name))
      continue;
    bool same = true;
    for (size_t c = 0; c < key.size() && same; ++c) {
      same = tolower(static_cast<unsigned char>(key[c])) ==
             tolower(static_cast<unsigned char>(name[c]));
    }
    if (same)
      return i + 1;
  }
  return 0;
}

// How far a stick has to be pushed to count as a press in that direction:
// XInput's own left-stick deadzone plus a margin, so resting a thumb on it
// does not press anything. Axes are XInput's, -32768..32767, positive up and
// right.
inline constexpr int kStickPress = 7849 + 2000;

// The four direction bits of one stick, given which PadInput is its "up".
// The other three follow it in the order Up, Down, Left, Right.
inline uint32_t StickInputs(int x, int y, int up_code) {
  uint32_t bits = 0;
  if (y > kStickPress)
    bits |= PadInputBit(up_code);
  if (y < -kStickPress)
    bits |= PadInputBit(up_code + 1);
  if (x < -kStickPress)
    bits |= PadInputBit(up_code + 2);
  if (x > kStickPress)
    bits |= PadInputBit(up_code + 3);
  return bits;
}

// ---------------------------------------------------------------------------
// The keys held
// ---------------------------------------------------------------------------

// 256 bits, one per virtual-key code - what the input thread publishes.
inline bool KeyHeld(const uint32_t keys[8], int code) {
  return code > 0 && code < 256 && (keys[code >> 5] & (1u << (code & 31))) != 0;
}

inline void SetKeyHeld(uint32_t keys[8], int code) {
  if (code > 0 && code < 256)
    keys[code >> 5] |= 1u << (code & 31);
}

// ---------------------------------------------------------------------------
// Mapping
// ---------------------------------------------------------------------------

// The pad buttons a map presses, as the OR of `buttons[i]` for every button
// whose code is held. `buttons` is the front end's per-button bit (a Sio::k*
// value, or its ANALOG bit above them).
inline uint32_t MapKeys(const int* codes, const uint32_t* buttons, size_t count,
                        const uint32_t keys[8]) {
  uint32_t out = 0;
  for (size_t i = 0; i < count; ++i) {
    if (KeyHeld(keys, codes[i]))
      out |= buttons[i];
  }
  return out;
}

inline uint32_t MapPad(const int* codes, const uint32_t* buttons, size_t count,
                       uint32_t inputs) {
  uint32_t out = 0;
  for (size_t i = 0; i < count; ++i) {
    if ((inputs & PadInputBit(codes[i])) != 0)
      out |= buttons[i];
  }
  return out;
}

// One input per button within a map: binding `code` to `button` takes it from
// whichever other button had it, which is what the person means - leaving it
// on two would press both. Returns the button it was taken from, or -1.
inline int BindTakingFromOthers(int* codes, size_t count, size_t button, int code) {
  int taken_from = -1;
  if (code != 0) {
    for (size_t i = 0; i < count; ++i) {
      if (i != button && codes[i] == code) {
        codes[i] = 0;
        taken_from = static_cast<int>(i);
      }
    }
  }
  codes[button] = code;
  return taken_from;
}

// ---------------------------------------------------------------------------
// As text
// ---------------------------------------------------------------------------

// A whole map on one settings line: "up=Up down=Down cross=X l3=", each button
// by its own name and each code by the device's. An empty right-hand side is a
// button deliberately left unbound. Names are the caller's, since the keyboard
// and a pad name their codes differently.
template <typename CodeName>
std::string SerialiseMap(const int* codes, const char* const* button_names, size_t count,
                         CodeName code_name) {
  std::string out;
  for (size_t i = 0; i < count; ++i) {
    if (!out.empty())
      out += ' ';
    out += button_names[i];
    out += '=';
    out += code_name(codes[i]);
  }
  return out;
}

// The reverse, onto `codes`: a button the line names takes that code, and one
// it does not name keeps what it had - so a line written before a button
// existed leaves the newer button at its default. Unknown buttons are skipped.
template <typename CodeFromName>
void ParseMap(const std::string& line, const char* const* button_names, size_t count,
              CodeFromName code_from_name, int* codes) {
  size_t at = 0;
  while (at < line.size()) {
    while (at < line.size() && line[at] == ' ')
      ++at;
    size_t end = line.find(' ', at);
    if (end == std::string::npos)
      end = line.size();
    const std::string pair = line.substr(at, end - at);
    at = end;
    const size_t eq = pair.find('=');
    if (eq == std::string::npos)
      continue;
    const std::string button = pair.substr(0, eq);
    for (size_t i = 0; i < count; ++i) {
      if (button == button_names[i]) {
        codes[i] = code_from_name(pair.substr(eq + 1));
        break;
      }
    }
  }
}

}  // namespace utilities
