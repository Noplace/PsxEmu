// bindings_test - which key or pad control presses which PSX button.
//
// Two layers. platform/input_bindings.h is the arithmetic: a gamepad's
// controls as bits, a stick pushed far enough counting as a press, a map
// turning what is held into buttons, one input per button, and a map as one
// line of text. The front end's controller_bindings.h puts those together:
// one map per port and device, their defaults, the settings file - including
// the older file that had only one set of keys - and the left stick doubling
// as the d-pad.
//
// Nothing here opens a device or a window. The front end's headers are
// included for their inline code only.

#include "platform/input_bindings.h"
#include "platform/sony_pad_reports.h"
#include "../../PSXEmu.Win32/input/controller_bindings.h"

#include <cstdio>
#include <initializer_list>
#include <string>
#include <vector>

using namespace utilities;
using emulation::psx::Sio;
using psxemu::ControllerBindings;
using psxemu::KeyMap;
using psxemu::kKeyboardDevice;
using psxemu::kPadButtons;

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool condition, const char* what) {
  ++g_checks;
  if (!condition) {
    ++g_failures;
    printf("  FAIL  %s\n", what);
  }
}

void CheckEqual(long long got, long long want, const char* what) {
  ++g_checks;
  if (got != want) {
    ++g_failures;
    printf("  FAIL  %s: got %lld want %lld\n", what, got, want);
  }
}

void CheckString(const std::string& got, const std::string& want, const char* what) {
  ++g_checks;
  if (got != want) {
    ++g_failures;
    printf("  FAIL  %s: got \"%s\" want \"%s\"\n", what, got.c_str(), want.c_str());
  }
}

// kKeyBindings positions, so a check reads as the button it is about.
enum Button {
  kUp, kDown, kLeft, kRight, kCross, kSquare, kCircle, kTriangle,
  kL1, kR1, kL2, kR2, kStart, kSelect, kAnalog, kL3, kR3
};

const int kGamepad1 = 1;
const int kPort1 = 0;
const int kPort2 = 1;

void TestPadInputs() {
  printf("a gamepad's controls\n");
  CheckEqual(kPadInputCount, 24, "twenty-four controls, sticks' directions included");
  int round_trips = 0;
  for (int code = 1; code <= kPadInputCount; ++code)
    round_trips += PadInputFromKey(PadInputKey(code)) == code;
  CheckEqual(round_trips, kPadInputCount, "every control's name reads back as that control");
  CheckEqual(PadInputFromKey("dpadup"), kPadDpadUp, "names are read ignoring case");
  CheckEqual(PadInputFromKey("Guide"), 0, "an unknown name is no control, not a guess");
  CheckEqual(PadInputFromKey(""), 0, "and so is an empty one");
  CheckEqual(PadInputBit(0), 0, "no control has no bit");
  CheckEqual(PadInputBit(kPadA), 1, "A is bit 0");
  CheckEqual(PadInputBit(kPadRStickRight), 1u << 23, "the last is bit 23");
  CheckString(PadInputKey(0), "", "no control is written as nothing");
}

void TestSticks() {
  printf("a stick pushed far enough is a press\n");
  CheckEqual(StickInputs(0, 0, kPadLStickUp), 0, "centred presses nothing");
  CheckEqual(StickInputs(0, kStickPress, kPadLStickUp), 0,
             "exactly at the threshold is still not a press");
  CheckEqual(StickInputs(0, kStickPress + 1, kPadLStickUp), PadInputBit(kPadLStickUp),
             "past it upward is Up (XInput's Y is positive upward)");
  CheckEqual(StickInputs(0, -32768, kPadLStickUp), PadInputBit(kPadLStickDown),
             "fully down is Down");
  CheckEqual(StickInputs(-32768, 0, kPadLStickUp), PadInputBit(kPadLStickLeft), "left is Left");
  CheckEqual(StickInputs(32767, 32767, kPadRStickUp),
             PadInputBit(kPadRStickUp) | PadInputBit(kPadRStickRight),
             "a diagonal is two directions, and the right stick has its own four");
}

void TestMapping() {
  printf("a map turns what is held into buttons\n");
  const int codes[3] = { 'X', 0, 'S' };
  const uint32_t buttons[3] = { 1u << 14, 1u << 3, 1u << 13 };
  uint32_t keys[8] = {};
  CheckEqual(MapKeys(codes, buttons, 3, keys), 0, "nothing held, nothing pressed");
  SetKeyHeld(keys, 'X');
  SetKeyHeld(keys, 'Q');
  CheckEqual(MapKeys(codes, buttons, 3, keys), 1u << 14, "a bound key presses its button");
  SetKeyHeld(keys, 'S');
  CheckEqual(MapKeys(codes, buttons, 3, keys), (1u << 14) | (1u << 13), "and two press two");
  SetKeyHeld(keys, 0);
  Check(!KeyHeld(keys, 0), "no key is never held, so an unbound button is never pressed");
  SetKeyHeld(keys, 300);
  Check(!KeyHeld(keys, 300), "and a code past 255 is ignored rather than written past the set");

  const int pad_codes[2] = { kPadA, kPadRT };
  CheckEqual(MapPad(pad_codes, buttons, 2, PadInputBit(kPadRT) | PadInputBit(kPadB)), 1u << 3,
             "a pad control presses its button and an unbound one presses nothing");
}

void TestOneInputPerButton() {
  printf("one input per button within a map\n");
  int codes[4] = { 'A', 'B', 'C', 'D' };
  CheckEqual(BindTakingFromOthers(codes, 4, 0, 'C'), 2, "taking C for the first says who had it");
  Check(codes[0] == 'C' && codes[2] == 0, "and the one that had it now has none");
  CheckEqual(BindTakingFromOthers(codes, 4, 1, 'E'), -1, "a free key takes from nobody");
  CheckEqual(BindTakingFromOthers(codes, 4, 3, 0), -1, "clearing takes from nobody");
  Check(codes[0] == 'C' && codes[1] == 'E' && codes[3] == 0,
        "and clears only the button it was asked to");
}

void TestText() {
  printf("a map as one settings line\n");
  const char* names[3] = { "up", "cross", "l3" };
  const int codes[3] = { kPadDpadUp, kPadA, 0 };
  const std::string line = SerialiseMap(codes, names, 3, [](int c) { return PadInputKey(c); });
  CheckString(line, "up=DpadUp cross=A l3=", "each button by name, an unbound one left empty");
  int back[3] = { 7, 7, 7 };
  ParseMap(line, names, 3, [](const std::string& n) { return PadInputFromKey(n); }, back);
  Check(back[0] == kPadDpadUp && back[1] == kPadA && back[2] == 0,
        "reads back exactly, the empty one as unbound");
  int partial[3] = { 7, 7, 7 };
  ParseMap("  cross=B   start=A ", names, 3,
           [](const std::string& n) { return PadInputFromKey(n); }, partial);
  Check(partial[0] == 7 && partial[1] == kPadB && partial[2] == 7,
        "a button the line leaves out keeps what it had, and one it does not know is skipped");
}

void TestDefaults() {
  printf("the defaults are the layout the front end had\n");
  const ControllerBindings bindings;
  const KeyMap& keys = bindings.map[kPort1][kKeyboardDevice];
  Check(keys[kCross] == 'X' && keys[kSquare] == 'Z' && keys[kCircle] == 'S' &&
            keys[kTriangle] == 'A' && keys[kStart] == VK_RETURN && keys[kAnalog] == 'E',
        "the keyboard's are the keys it always had");
  Check(keys[kL3] == 0 && keys[kR3] == 0, "and L3 and R3, new to the keyboard, start unbound");

  // What gamepad.h used to hard-wire, control by control: the same buttons now come out of
  // the default map.
  const KeyMap& pad = bindings.map[kPort1][kGamepad1];
  struct Old { int input; uint32_t button; };
  const Old old[] = {
      { kPadA, Sio::kCross },         { kPadB, Sio::kCircle },      { kPadX, Sio::kSquare },
      { kPadY, Sio::kTriangle },      { kPadStart, Sio::kStart },   { kPadBack, Sio::kSelect },
      { kPadLB, Sio::kL1 },           { kPadRB, Sio::kR1 },         { kPadLT, Sio::kL2 },
      { kPadRT, Sio::kR2 },           { kPadLS, Sio::kL3 },         { kPadRS, Sio::kR3 },
      { kPadDpadUp, Sio::kUp },       { kPadDpadDown, Sio::kDown }, { kPadDpadLeft, Sio::kLeft },
      { kPadDpadRight, Sio::kRight }, { kPadLStickUp, Sio::kUp },   { kPadLStickDown, Sio::kDown },
      { kPadLStickLeft, Sio::kLeft }, { kPadLStickRight, Sio::kRight },
  };
  int same = 0;
  for (const Old& o : old)
    same += psxemu::MapGamepad(pad, PadInputBit(o.input)) == o.button;
  CheckEqual(same, static_cast<int>(std::size(old)),
             "every control the pad had presses the button it pressed before, and only that");
  CheckEqual(psxemu::MapGamepad(pad, PadInputBit(kPadRStickUp)), 0,
             "the right stick's directions press nothing by default");
}

void TestStickAsDpad() {
  printf("the left stick doubles as the d-pad, except where it is bound\n");
  ControllerBindings bindings;
  KeyMap& pad = bindings.map[kPort1][kGamepad1];
  pad[kTriangle] = kPadLStickUp;
  CheckEqual(psxemu::MapGamepad(pad, PadInputBit(kPadLStickUp)), Sio::kTriangle,
             "a direction bound to a button presses that button, not Up as well");
  CheckEqual(psxemu::MapGamepad(pad, PadInputBit(kPadLStickDown)), Sio::kDown,
             "the others still work as the d-pad");
  pad[kUp] = 0;
  CheckEqual(psxemu::MapGamepad(pad, PadInputBit(kPadLStickDown) | PadInputBit(kPadDpadUp)),
             Sio::kDown, "and unbinding the d-pad's Up leaves the stick's Down alone");
}

void TestPerPort() {
  printf("each port has its own keys\n");
  ControllerBindings bindings;
  bindings.map[kPort2][kKeyboardDevice][kCross] = 'M';
  bindings.map[kPort2][kKeyboardDevice][kUp] = 'I';
  uint32_t keys[8] = {};
  SetKeyHeld(keys, 'M');
  CheckEqual(psxemu::MapKeyboard(bindings.map[kPort2][kKeyboardDevice], keys), Sio::kCross,
             "M is Port 2's Cross");
  CheckEqual(psxemu::MapKeyboard(bindings.map[kPort1][kKeyboardDevice], keys), 0,
             "and nothing to Port 1");
  const std::array<bool, 256> used = psxemu::KeysInUse(bindings);
  Check(used['M'] && used['I'] && used['X'], "the input thread is told to read both ports' keys");
  Check(!used['K'], "and not keys nothing uses");
}

void TestSettingsFile() {
  printf("the settings file\n");
  const std::string absent = "<absent>";

  // A file from before bindings were per port: one set of keys, which both ports read.
  emulation::psx::SettingsFile old_file;
  old_file.SetString("key_cross", "K");
  old_file.SetString("key_circle", "");
  ControllerBindings loaded;
  psxemu::LoadBindings(old_file, &loaded);
  Check(loaded.map[kPort1][kKeyboardDevice][kCross] == 'K' &&
            loaded.map[kPort1][kKeyboardDevice][kCircle] == 0,
        "Port 1 reads key_*, an empty one as deliberately unbound");
  Check(loaded.map[kPort2][kKeyboardDevice][kCross] == 'K' &&
            loaded.map[psxemu::kBindingSlotCount - 1][kKeyboardDevice][kCircle] == 0,
        "and every other keyboard slot starts from them, so Port 2 keeps what it had");
  Check(loaded.map[kPort2][kGamepad1] == ControllerBindings::Default(kGamepad1),
        "the pads start from their defaults");

  // Written back: key_* always, a bind_ line only for a map that is not its default.
  emulation::psx::SettingsFile written;
  psxemu::StoreBindings(&written, loaded);
  CheckString(written.GetString("key_cross", absent), "K", "Port 1 stays in key_*");
  CheckString(written.GetString("key_l3", absent), "", "including the new L3, unbound");
  Check(written.GetString("bind_port2_keyboard", absent) != absent,
        "the migrated Port 2 keys are written, since they are not the defaults");
  CheckString(written.GetString("bind_port2_gamepad1", absent), absent,
              "a map left at its defaults is not written at all");
  CheckEqual(written.GetInt("bindings_version", 0), psxemu::kBindingsVersion,
             "and the file says it holds per-port bindings now");

  ControllerBindings reloaded;
  psxemu::LoadBindings(written, &reloaded);
  bool same = true;
  for (int slot = 0; slot < psxemu::kBindingSlotCount; ++slot) {
    for (int device = 0; device < psxemu::kBindingDevices; ++device)
      same = same && reloaded.map[slot][device] == loaded.map[slot][device];
  }
  Check(same, "and every map reads back as it was written");

  // Once the file is per port, a slot with no line has the defaults - Port 1's keys no longer
  // leak into it.
  reloaded.map[kPort2][kKeyboardDevice] = ControllerBindings::Default(kKeyboardDevice);
  reloaded.map[kPort2][kGamepad1][kCross] = kPadRT;
  psxemu::StoreBindings(&written, reloaded);
  CheckString(written.GetString("bind_port2_keyboard", absent), absent,
              "a map put back to its defaults loses its line");
  ControllerBindings again;
  psxemu::LoadBindings(written, &again);
  Check(again.map[kPort2][kKeyboardDevice] == ControllerBindings::Default(kKeyboardDevice),
        "and reads back as the defaults, not as Port 1's keys");
  CheckEqual(again.map[kPort2][kGamepad1][kCross], kPadRT, "a pad's change survives");
  CheckEqual(again.map[kPort2][kGamepad1][kCircle], kPadB, "and so does the rest of that pad");
}

}  // namespace

// ---------------------------------------------------------------------------
// DualShock 4 and DualSense reports (platform/sony_pad_reports.h)
// ---------------------------------------------------------------------------

// A DualShock 4 USB report (or a short Bluetooth one): sticks, three button
// bytes, the triggers. `hat` is 0-7 clockwise from up, 8 for none.
std::vector<uint8_t> Ds4Report(uint8_t id, int hat, uint8_t face, uint8_t shoulders,
                               uint8_t lx = 0x80, uint8_t ly = 0x80, uint8_t rx = 0x80,
                               uint8_t ry = 0x80, uint8_t l2 = 0, uint8_t r2 = 0) {
  std::vector<uint8_t> r(id == 0x11 ? 78 : 64, 0);
  r[0] = id;
  uint8_t* d = r.data() + (id == 0x11 ? 3 : 1);
  d[0] = lx; d[1] = ly; d[2] = rx; d[3] = ry;
  d[4] = static_cast<uint8_t>((face & 0xF0) | (hat & 0x0F));
  d[5] = shoulders;
  d[7] = l2; d[8] = r2;
  return r;
}

// The DualSense's own layout, USB 0x01 or Bluetooth 0x31: sticks, triggers, a
// counter, then the same three button bytes.
std::vector<uint8_t> DualSenseReport(uint8_t id, int hat, uint8_t face, uint8_t shoulders,
                                     uint8_t l2 = 0, uint8_t r2 = 0, uint8_t lx = 0x80) {
  std::vector<uint8_t> r(id == 0x31 ? 78 : 64, 0);
  r[0] = id;
  uint8_t* d = r.data() + (id == 0x31 ? 2 : 1);
  d[0] = lx; d[1] = 0x80; d[2] = 0x80; d[3] = 0x80;
  d[4] = l2; d[5] = r2;
  d[6] = 0x5A;   // the counter, which must not be read as anything
  d[7] = static_cast<uint8_t>((face & 0xF0) | (hat & 0x0F));
  d[8] = shoulders;
  return r;
}

uint32_t Bits(std::initializer_list<int> codes) {
  uint32_t bits = 0;
  for (int code : codes)
    bits |= PadInputBit(code);
  return bits;
}

// A second CRC-32, table-driven, to check the bytewise one against.
uint32_t TableCrc32(const std::vector<uint8_t>& bytes) {
  uint32_t table[256];
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t c = i;
    for (int k = 0; k < 8; ++k)
      c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    table[i] = c;
  }
  uint32_t crc = 0xFFFFFFFFu;
  for (uint8_t b : bytes)
    crc = table[(crc ^ b) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

void TestSonyPads() {
  printf("DualShock 4 and DualSense\n");
  SonyPadState s;

  // A pad at rest reads as nothing held and both sticks centred.
  auto rest = Ds4Report(0x01, 8, 0, 0);
  Check(ParseSonyPadReport(false, false, rest.data(), rest.size(), &s), "DS4 USB report reads");
  CheckEqual(s.inputs, 0, "DS4 at rest holds nothing");
  CheckEqual(s.left_x, 0x80, "DS4 at rest: left stick centred");

  // Each face button, shoulder and middle button, onto the XInput control in its place.
  struct { uint8_t face, shoulders; int code; const char* what; } buttons[] = {
      { 0x10, 0, kPadX, "Square is X" },        { 0x20, 0, kPadA, "Cross is A" },
      { 0x40, 0, kPadB, "Circle is B" },        { 0x80, 0, kPadY, "Triangle is Y" },
      { 0, 0x01, kPadLB, "L1 is LB" },          { 0, 0x02, kPadRB, "R1 is RB" },
      { 0, 0x10, kPadBack, "Share is Back" },   { 0, 0x20, kPadStart, "Options is Start" },
      { 0, 0x40, kPadLS, "L3 is LS" },          { 0, 0x80, kPadRS, "R3 is RS" },
  };
  for (const auto& b : buttons) {
    auto r = Ds4Report(0x01, 8, b.face, b.shoulders);
    ParseSonyPadReport(false, false, r.data(), r.size(), &s);
    CheckEqual(s.inputs, PadInputBit(b.code), b.what);
    // The same over Bluetooth, from where the long report puts it.
    auto bt = Ds4Report(0x11, 8, b.face, b.shoulders);
    SonyPadState t;
    Check(ParseSonyPadReport(false, true, bt.data(), bt.size(), &t) &&
              t.inputs == PadInputBit(b.code), b.what);
  }
  // L2 and R2's own digital bits are not what counts - how far the trigger is.
  auto digital_only = Ds4Report(0x01, 8, 0, 0x0C);
  ParseSonyPadReport(false, false, digital_only.data(), digital_only.size(), &s);
  CheckEqual(s.inputs, 0, "L2/R2 bits alone press nothing");
  auto triggers = Ds4Report(0x01, 8, 0, 0, 0x80, 0x80, 0x80, 0x80, 31, 30);
  ParseSonyPadReport(false, false, triggers.data(), triggers.size(), &s);
  CheckEqual(s.inputs, PadInputBit(kPadLT), "L2 past XInput's threshold, R2 on it");

  // The hat, all nine positions.
  const uint32_t hats[9] = {
      Bits({ kPadDpadUp }),   Bits({ kPadDpadUp, kPadDpadRight }),
      Bits({ kPadDpadRight }), Bits({ kPadDpadRight, kPadDpadDown }),
      Bits({ kPadDpadDown }), Bits({ kPadDpadDown, kPadDpadLeft }),
      Bits({ kPadDpadLeft }), Bits({ kPadDpadLeft, kPadDpadUp }), 0 };
  for (int hat = 0; hat <= 8; ++hat) {
    auto r = Ds4Report(0x01, hat, 0, 0);
    ParseSonyPadReport(false, false, r.data(), r.size(), &s);
    char what[48];
    snprintf(what, sizeof(what), "hat %d", hat);
    CheckEqual(s.inputs, hats[hat], what);
  }

  // Sticks: the PSX convention as they are, a small deadzone, and the four
  // directions a button can be bound to.
  auto sticks = Ds4Report(0x01, 8, 0, 0, 0x00, 0x8B, 0xFF, 0x75);
  ParseSonyPadReport(false, false, sticks.data(), sticks.size(), &s);
  CheckEqual(s.left_x, 0x00, "left stick hard left stays 0x00");
  CheckEqual(s.left_y, 0x80, "left stick 11 below centre is inside the deadzone");
  CheckEqual(s.right_x, 0xFF, "right stick hard right stays 0xFF");
  CheckEqual(s.right_y, 0x80, "right stick 11 above centre is inside the deadzone");
  CheckEqual(s.inputs, Bits({ kPadLStickLeft, kPadRStickRight }),
             "sticks pushed all the way read as their directions");
  auto up = Ds4Report(0x01, 8, 0, 0, 0x80, 0x00);
  ParseSonyPadReport(false, false, up.data(), up.size(), &s);
  CheckEqual(s.inputs, PadInputBit(kPadLStickUp), "0x00 on a y axis is up");

  // The DualSense's own layout, over USB and Bluetooth.
  auto ds = DualSenseReport(0x01, 4, 0x80, 0x02, 0, 200, 0x00);
  Check(ParseSonyPadReport(true, false, ds.data(), ds.size(), &s), "DualSense USB report reads");
  CheckEqual(s.inputs, Bits({ kPadY, kPadRB, kPadDpadDown, kPadRT, kPadLStickLeft }),
             "DualSense USB: Triangle, R1, down, R2 and the stick, not the counter");
  auto ds_bt = DualSenseReport(0x31, 6, 0x20, 0x10, 255, 0);
  Check(ParseSonyPadReport(true, true, ds_bt.data(), ds_bt.size(), &s),
        "DualSense Bluetooth 0x31 reads");
  CheckEqual(s.inputs, Bits({ kPadA, kPadBack, kPadDpadLeft, kPadLT }),
             "DualSense Bluetooth: Cross, Create, left and L2");
  // Before it is sent anything over Bluetooth, a DualSense sends the DualShock 4's short report.
  auto ds_short = Ds4Report(0x01, 0, 0x40, 0x20);
  ds_short.resize(10);
  Check(ParseSonyPadReport(true, true, ds_short.data(), ds_short.size(), &s),
        "DualSense short Bluetooth report reads");
  CheckEqual(s.inputs, Bits({ kPadB, kPadStart, kPadDpadUp }),
             "DualSense short report: Circle, Options and up, in the DS4 layout");

  // What is not a pad reading is left alone.
  SonyPadState untouched;
  untouched.inputs = 0x12345;
  const uint8_t other[16] = { 0x05 };
  Check(!ParseSonyPadReport(false, false, other, sizeof(other), &untouched) &&
            untouched.inputs == 0x12345, "another report id is ignored");
  Check(!ParseSonyPadReport(false, false, rest.data(), 9, &untouched), "a short report is ignored");

  // CRC-32 against the standard check value, and against a table-driven one.
  const uint8_t check[] = { '1', '2', '3', '4', '5', '6', '7', '8', '9' };
  const uint32_t check_crc = ~Crc32Update(0xFFFFFFFFu, check, sizeof(check));
  CheckEqual(check_crc, 0xCBF43926u, "CRC-32 of \"123456789\"");

  // The motor reports.
  uint8_t out[78];
  Check(!BuildSonyRumbleReport(false, false, 0, 1, 2, out, 31), "too short a buffer is refused");
  Check(BuildSonyRumbleReport(false, false, 0, 0xFF, 0x40, out, 32), "DS4 USB motor report");
  Check(out[0] == 0x05 && out[1] == 0x01 && out[4] == 0xFF && out[5] == 0x40 && out[6] == 0,
        "DS4 USB: report 5, motors only, small then large, light bar untouched");
  Check(BuildSonyRumbleReport(true, false, 0, 0xFF, 0x40, out, 48), "DualSense USB motor report");
  Check(out[0] == 0x02 && out[1] == 0x03 && out[2] == 0 && out[3] == 0xFF && out[4] == 0x40,
        "DualSense USB: report 2, compatible rumble, small then large");

  for (int dualsense = 0; dualsense < 2; ++dualsense) {
    Check(BuildSonyRumbleReport(dualsense != 0, true, 0x13, 0xFF, 0x40, out, sizeof(out)),
          "Bluetooth motor report");
    std::vector<uint8_t> covered = { 0xA2 };
    covered.insert(covered.end(), out, out + 74);
    const uint32_t crc = TableCrc32(covered);
    const uint32_t stored = out[74] | (out[75] << 8) | (out[76] << 16) |
                            (static_cast<uint32_t>(out[77]) << 24);
    CheckEqual(stored, crc, dualsense ? "DualSense Bluetooth CRC" : "DS4 Bluetooth CRC");
  }
  BuildSonyRumbleReport(false, true, 0, 0xFF, 0x40, out, sizeof(out));
  Check(out[0] == 0x11 && out[1] == 0xC0 && out[3] == 0x01 && out[6] == 0xFF && out[7] == 0x40,
        "DS4 Bluetooth: report 0x11, HID+CRC, motors only");
  BuildSonyRumbleReport(true, true, 0x13, 0xFF, 0x40, out, sizeof(out));
  Check(out[0] == 0x31 && out[1] == 0x30 && out[2] == 0x10 && out[3] == 0x03 && out[5] == 0xFF &&
            out[6] == 0x40,
        "DualSense Bluetooth: report 0x31, the sequence's low bits, tag 0x10, then as USB");

  // The bindings window's names for a PlayStation pad's controls.
  CheckString(PadInputLabel(kPadA, true), "Cross", "A is Cross on a PlayStation pad");
  CheckString(PadInputLabel(kPadLT, true), "L2", "LT is L2");
  CheckString(PadInputLabel(kPadBack, true), "Share / Create", "Back is Share or Create");
  CheckString(PadInputLabel(kPadA), "A", "and still A on an XInput pad");
}

int main() {
  printf("bindings_test - keys and pad controls onto PSX buttons\n\n");

  TestPadInputs();
  TestSticks();
  TestMapping();
  TestOneInputPerButton();
  TestText();
  TestDefaults();
  TestStickAsDpad();
  TestPerPort();
  TestSettingsFile();
  TestSonyPads();

  printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
