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
#include "../../PSXEmu.Win32/input/controller_bindings.h"

#include <cstdio>
#include <string>

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

  printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
