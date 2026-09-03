// sio_test - checks the controller port against things that must be true.
//
// The digital pad was already exercised indirectly by every other harness
// here - a game that cannot read its own buttons does not boot - so this is
// about the part that was never exercised at all: the DualShock handshake
// (0x43/0x44/0x45), the rumble configuration (0x4D) and its pre-DualShock
// fallback, and the axis bytes, none of which any existing test ever sent a
// byte to.

#include "psx/psx.h"

#include <cstdio>
#include <cstring>

using emulation::psx::Sio;
using emulation::psx::System;

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

void CheckEqual(uint32_t got, uint32_t want, const char* what) {
  ++g_checks;
  if (got != want) {
    ++g_failures;
    printf("  FAIL  %s: got %02X want %02X\n", what, got, want);
  }
}

// Every test wants to start from a pad that has negotiated nothing, and
// set_connected(slot, true) is a no-op when the slot is already connected -
// which it usually is, since these tests share one System run one after
// another. Disconnecting first forces the reset a genuinely fresh plug-in
// gets, regardless of what an earlier test left behind.
void FreshPad(System* system, int slot) {
  system->sio().set_connected(slot, false);
  system->sio().set_connected(slot, true);
}

// Drives the port the way real software does: assert chip select and the
// slot-select bit, exchange bytes one at a time through the data register,
// then drop chip select to end the transaction. The bits used here are
// exactly the ones Sio::Write16 actually inspects - bit 1 for chip select,
// bit 13 for which slot - so this is not standing in for the register
// interface, it is the register interface.
class PadHarness {
 public:
  explicit PadHarness(System* system) : sio_(&system->sio()) {}

  void Begin(int slot) {
    const uint16_t select = (slot == 1) ? 0x2000 : 0x0000;
    sio_->Write16(0x1F80104A, static_cast<uint16_t>(0x0002 | select));
  }

  void End() {
    sio_->Write16(0x1F80104A, 0x0000);
  }

  uint8_t Exchange(uint8_t data) {
    sio_->Write08(0x1F801040, data);
    return sio_->Read08(0x1F801040);
  }

  // Whether the last exchanged byte was acknowledged - status register bit
  // 7. A transaction that has run past its real length stops setting this,
  // which is how a BIOS driver knows to stop reading.
  bool Acknowledged() const {
    return (sio_->Read16(0x1F801044) & 0x0080) != 0;
  }

  // One full command: select the slot, send the command byte and however
  // many follow-up bytes the caller wants, capture every reply byte from
  // the ID onward, and end the transaction. Returns how many were captured.
  int Command(int slot, uint8_t command, const uint8_t* payload,
             int payload_count, uint8_t* out, int capacity) {
    Begin(slot);
    Exchange(0x01);   // device select: this is a controller
    int n = 0;
    if (!Acknowledged()) {
      End();
      return 0;
    }

    // Exchange() has to run for every byte regardless of whether the caller
    // wants it captured - `out`/`capacity` control what is *remembered*, not
    // what is actually sent, and a command whose reply nobody wants still has
    // to reach the pad for its side effects to happen at all.
    const uint8_t id_reply = Exchange(command);
    if (n < capacity)
      out[n++] = id_reply;

    if (!Acknowledged()) {
      End();
      return n;
    }
    // The status byte has its own wire position - nothing the host sends
    // here is meaningful, exactly as it is not for the memory card's own
    // fixed id/status bytes. The caller's payload starts on the byte after
    // it, not this one.
    const uint8_t status_reply = Exchange(0x00);
    if (n < capacity)
      out[n++] = status_reply;

    for (int i = 0; i < payload_count; ++i) {
      if (!Acknowledged())
        break;
      const uint8_t reply = Exchange(payload[i]);
      if (n < capacity)
        out[n++] = reply;
    }
    // Keep going past the caller's own payload with zero bytes for as long
    // as the pad keeps acknowledging, so the reply is captured in full even
    // when the caller only cares about supplying the first few bytes.
    while (Acknowledged()) {
      const uint8_t reply = Exchange(0x00);
      if (n < capacity)
        out[n++] = reply;
    }
    End();
    return n;
  }

 private:
  Sio* sio_;
};

// ---------------------------------------------------------------------------

void TestDigitalPadUnaffected(System* system) {
  printf("digital pad, unaffected by any of this\n");
  FreshPad(system, 0);
  PadHarness pad(system);
  system->sio().set_buttons(0, Sio::kCross | Sio::kUp);

  uint8_t reply[16] = {};
  const int n = pad.Command(0, 0x42, nullptr, 0, reply, sizeof(reply));

  CheckEqual(n, 4, "a digital pad's poll reply is four bytes");
  CheckEqual(reply[0], 0x41, "digital pad id low byte");
  CheckEqual(reply[1], 0x5A, "digital pad id high byte");
  const uint16_t buttons =
      static_cast<uint16_t>(reply[2] | (reply[3] << 8));
  CheckEqual(static_cast<uint16_t>(~buttons) & 0xFFFF,
             Sio::kCross | Sio::kUp,
             "the buttons that were pressed come back pressed");
}

void TestEmptySlotNeverAcknowledges(System* system) {
  printf("an empty slot never acknowledges\n");
  system->sio().set_connected(1, false);

  PadHarness pad(system);
  uint8_t reply[16] = {};
  const int n = pad.Command(1, 0x42, nullptr, 0, reply, sizeof(reply));
  CheckEqual(n, 0, "nothing comes back from a slot with nothing in it");
}

void TestConfigModeGatesTheSpecialCommands(System* system) {
  printf("0x44 does nothing outside configuration mode\n");
  FreshPad(system, 0);

  PadHarness pad(system);
  const uint8_t enable_analog[2] = { 0x01, 0x03 };
  pad.Command(0, 0x44, enable_analog, 2, nullptr, 0);

  // Read back what mode the pad is actually in via an ordinary poll, since
  // that is what a game would see - a four-byte reply means still digital.
  uint8_t reply[16] = {};
  const int n = pad.Command(0, 0x42, nullptr, 0, reply, sizeof(reply));
  CheckEqual(n, 4, "0x44 sent cold left the pad digital");
  CheckEqual(reply[0], 0x41, "and still answering as one");
}

void TestEnteringAnalogMode(System* system) {
  printf("the config-mode handshake actually switches the pad\n");
  FreshPad(system, 0);

  PadHarness pad(system);
  const uint8_t enter[1] = { 0x01 };
  pad.Command(0, 0x43, enter, 1, nullptr, 0);

  const uint8_t go_analog[2] = { 0x01, 0x03 };   // analog, locked
  pad.Command(0, 0x44, go_analog, 2, nullptr, 0);

  const uint8_t leave[1] = { 0x00 };
  pad.Command(0, 0x43, leave, 1, nullptr, 0);

  uint8_t reply[16] = {};
  const int n = pad.Command(0, 0x42, nullptr, 0, reply, sizeof(reply));
  CheckEqual(n, 8, "an analog pad's poll reply is eight bytes");
  CheckEqual(reply[0], 0x73, "analog pad id low byte");
  CheckEqual(reply[1], 0x5A, "analog pad id high byte");
}

void TestStatusQueryReportsTheMode(System* system) {
  printf("0x45 reports whether the pad is in analog mode\n");
  FreshPad(system, 0);
  PadHarness pad(system);

  auto enter_config = [&] {
    const uint8_t enter[1] = { 0x01 };
    pad.Command(0, 0x43, enter, 1, nullptr, 0);
  };
  auto leave_config = [&] {
    const uint8_t leave[1] = { 0x00 };
    pad.Command(0, 0x43, leave, 1, nullptr, 0);
  };

  enter_config();
  uint8_t reply[16] = {};
  pad.Command(0, 0x45, nullptr, 0, reply, sizeof(reply));
  CheckEqual(reply[4], 0x00, "0x45 reports digital before any mode switch");

  const uint8_t go_analog[2] = { 0x01, 0x02 };
  pad.Command(0, 0x44, go_analog, 2, nullptr, 0);
  pad.Command(0, 0x45, nullptr, 0, reply, sizeof(reply));
  CheckEqual(reply[4], 0x01, "and analog once 0x44 has switched it");

  leave_config();
}

void TestAxesRoundTrip(System* system) {
  printf("analog axes\n");
  FreshPad(system, 0);
  PadHarness pad(system);

  const uint8_t enter[1] = { 0x01 };
  pad.Command(0, 0x43, enter, 1, nullptr, 0);
  const uint8_t go_analog[2] = { 0x01, 0x02 };
  pad.Command(0, 0x44, go_analog, 2, nullptr, 0);
  const uint8_t leave[1] = { 0x00 };
  pad.Command(0, 0x43, leave, 1, nullptr, 0);

  // Four different values, so a byte landing in the wrong position cannot
  // hide behind two axes that happen to agree.
  system->sio().set_axes(0, /*left_x=*/0x10, /*left_y=*/0x20,
                         /*right_x=*/0x30, /*right_y=*/0x40);

  uint8_t reply[16] = {};
  const int n = pad.Command(0, 0x42, nullptr, 0, reply, sizeof(reply));
  CheckEqual(n, 8, "an analog poll is eight bytes");
  // The wire order is right-x, right-y, left-x, left-y, starting right after
  // the two button bytes.
  CheckEqual(reply[4], 0x30, "right-x is the first axis byte");
  CheckEqual(reply[5], 0x40, "right-y is the second");
  CheckEqual(reply[6], 0x10, "left-x is the third");
  CheckEqual(reply[7], 0x20, "left-y is the fourth");
}

void TestLegacyRumble(System* system) {
  printf("the pre-DualShock two-byte rumble pattern\n");
  FreshPad(system, 0);
  // A pad that has never been through configuration mode is not a DualShock
  // yet as far as rumble is concerned, which is the case this is checking -
  // do not enter config mode here.

  PadHarness pad(system);
  uint8_t small = 0xFF, large = 0xFF;

  const uint8_t on_pattern[2] = { 0x40, 0x01 };
  pad.Command(0, 0x42, on_pattern, 2, nullptr, 0);
  system->sio().motor_state(0, &small, &large);
  CheckEqual(small, 255, "the magic pattern turns the small motor fully on");

  const uint8_t off_pattern[2] = { 0x00, 0x00 };
  pad.Command(0, 0x42, off_pattern, 2, nullptr, 0);
  system->sio().motor_state(0, &small, &large);
  CheckEqual(small, 0, "and an ordinary poll turns it off again");

  const uint8_t near_miss[2] = { 0x40, 0x00 };   // byte 1 missing its low bit
  pad.Command(0, 0x42, on_pattern, 2, nullptr, 0);
  pad.Command(0, 0x42, near_miss, 2, nullptr, 0);
  system->sio().motor_state(0, &small, &large);
  CheckEqual(small, 0, "a near miss on the pattern does not trigger it");
}

void TestRumbleMapping(System* system) {
  printf("command 0x4D maps poll bytes to motors\n");
  FreshPad(system, 0);
  PadHarness pad(system);

  const uint8_t enter[1] = { 0x01 };
  pad.Command(0, 0x43, enter, 1, nullptr, 0);

  // Map poll byte 0 (the would-be right-x position) to the small motor and
  // poll byte 1 to the large one - deliberately not the positions the
  // legacy scheme used, so this is genuinely exercising the configurable
  // path and not coincidentally passing through the fallback.
  const uint8_t mapping[6] = { 0x00, 0x01, 0xFF, 0xFF, 0xFF, 0xFF };
  pad.Command(0, 0x4D, mapping, 6, nullptr, 0);

  const uint8_t leave[1] = { 0x00 };
  pad.Command(0, 0x43, leave, 1, nullptr, 0);

  // Still in digital mode - the mapping applies before an ordinary poll even
  // has axis bytes to send, which is the point: rumble does not need analog
  // mode, only a DualShock that has been configured at all.
  const uint8_t speeds[2] = { 0x80, 0xFF };
  pad.Command(0, 0x42, speeds, 2, nullptr, 0);

  uint8_t small = 0, large = 0;
  system->sio().motor_state(0, &small, &large);
  CheckEqual(small, 0x80, "the byte mapped to the small motor set its speed");
  CheckEqual(large, 0xFF, "and the byte mapped to the large motor set its");
}

void TestUnconfiguredDualShockRumblesAtNothing(System* system) {
  printf("a DualShock that has never called 0x4D drives no motor\n");
  FreshPad(system, 0);
  PadHarness pad(system);

  // Enter and leave configuration mode without ever touching 0x4D. The pad
  // is a DualShock now (it has seen 0x43 once) so the legacy pattern no
  // longer applies to it, but nothing has mapped a motor either.
  const uint8_t enter[1] = { 0x01 };
  pad.Command(0, 0x43, enter, 1, nullptr, 0);
  const uint8_t leave[1] = { 0x00 };
  pad.Command(0, 0x43, leave, 1, nullptr, 0);

  const uint8_t on_pattern[2] = { 0x40, 0x01 };   // the legacy magic bytes
  pad.Command(0, 0x42, on_pattern, 2, nullptr, 0);

  uint8_t small = 0xFF, large = 0xFF;
  system->sio().motor_state(0, &small, &large);
  CheckEqual(small, 0, "the legacy pattern is ignored once it is a DualShock");
  CheckEqual(large, 0, "and nothing else has mapped the large motor either");
}

void TestReconnectForgetsNegotiation(System* system) {
  printf("a freshly connected pad has negotiated nothing\n");
  FreshPad(system, 0);
  PadHarness pad(system);

  const uint8_t enter[1] = { 0x01 };
  pad.Command(0, 0x43, enter, 1, nullptr, 0);
  const uint8_t go_analog[2] = { 0x01, 0x02 };
  pad.Command(0, 0x44, go_analog, 2, nullptr, 0);
  const uint8_t leave[1] = { 0x00 };
  pad.Command(0, 0x43, leave, 1, nullptr, 0);

  uint8_t reply[16] = {};
  pad.Command(0, 0x42, nullptr, 0, reply, sizeof(reply));
  CheckEqual(reply[0], 0x73, "it really is in analog mode before the check");

  system->sio().set_connected(0, false);
  FreshPad(system, 0);

  pad.Command(0, 0x42, nullptr, 0, reply, sizeof(reply));
  CheckEqual(reply[0], 0x41,
             "and back to a plain digital pad once reconnected");
}

void TestTwoSlotsAreIndependent(System* system) {
  printf("the two ports do not share state\n");
  FreshPad(system, 0);
  FreshPad(system, 1);
  PadHarness pad(system);

  const uint8_t enter[1] = { 0x01 };
  const uint8_t go_analog[2] = { 0x01, 0x02 };
  const uint8_t leave[1] = { 0x00 };
  pad.Command(0, 0x43, enter, 1, nullptr, 0);
  pad.Command(0, 0x44, go_analog, 2, nullptr, 0);
  pad.Command(0, 0x43, leave, 1, nullptr, 0);

  uint8_t reply0[16] = {};
  uint8_t reply1[16] = {};
  pad.Command(0, 0x42, nullptr, 0, reply0, sizeof(reply0));
  pad.Command(1, 0x42, nullptr, 0, reply1, sizeof(reply1));

  CheckEqual(reply0[0], 0x73, "port 1 is the one that was switched to analog");
  CheckEqual(reply1[0], 0x41, "port 2 was never touched and is still digital");
}

}  // namespace

int main() {
  System* system = new System();
  system->InitializeWithoutBios();

  TestDigitalPadUnaffected(system);
  TestEmptySlotNeverAcknowledges(system);
  TestConfigModeGatesTheSpecialCommands(system);
  TestEnteringAnalogMode(system);
  TestStatusQueryReportsTheMode(system);
  TestAxesRoundTrip(system);
  TestLegacyRumble(system);
  TestRumbleMapping(system);
  TestUnconfiguredDualShockRumblesAtNothing(system);
  TestReconnectForgetsNegotiation(system);
  TestTwoSlotsAreIndependent(system);

  printf("\n%d checks, %d failures\n", g_checks, g_failures);
  delete system;
  return g_failures == 0 ? 0 : 1;
}
