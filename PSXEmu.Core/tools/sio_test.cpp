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
  // `select_byte` defaults to 0x01 (an ordinary pad, or a Multitap's
  // Player A); 0x02-0x04 reach a Multitap's Players B-D instead.
  int Command(int slot, uint8_t command, const uint8_t* payload,
             int payload_count, uint8_t* out, int capacity,
             uint8_t select_byte = 0x01) {
    Begin(slot);
    Exchange(select_byte);
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

// psx-spx: the device "has to pull /ACK low for at least 2 us" and real
// hardware normally holds it there for "circa 100 clock cycles" before
// releasing it back to high on its own - software cannot clear this bit
// directly (bug 46: a driver that polls STAT.7 for that release, rather than
// only handling the interrupt, never saw one when this was modelled as a
// software-latched flag instead of a live level).
void TestAckPulseSelfReleases(System* system) {
  printf("the acknowledge bit is a pulse, not a latch\n");
  FreshPad(system, 0);
  PadHarness pad(system);

  pad.Begin(0);
  pad.Exchange(0x01);   // device select: acknowledged, no software ack yet
  Check(pad.Acknowledged(), "set the instant the device answers");

  system->sio().Tick(90);
  Check(pad.Acknowledged(), "still held 90 cycles in - short of the pulse");

  system->sio().Tick(20);   // total 110, past the ~100-cycle pulse width
  Check(!pad.Acknowledged(),
        "released on its own past the pulse width - nothing wrote the "
        "software acknowledge bit");

  pad.End();
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

void TestDigitalControllerTypeNeverGoesAnalog(System* system) {
  printf("a digital-only controller type ignores the whole handshake\n");
  FreshPad(system, 0);
  system->sio().set_controller_type(0, Sio::kDigital);
  PadHarness pad(system);

  const uint8_t enter[1] = { 0x01 };
  pad.Command(0, 0x43, enter, 1, nullptr, 0);
  const uint8_t go_analog[2] = { 0x01, 0x03 };
  pad.Command(0, 0x44, go_analog, 2, nullptr, 0);

  uint8_t reply[16] = {};
  const int n = pad.Command(0, 0x42, nullptr, 0, reply, sizeof(reply));
  CheckEqual(n, 4, "still a four-byte digital reply after 0x43 and 0x44");
  CheckEqual(reply[0], 0x41, "id never leaves 5A41h - 0x43 was never honoured");
}

void TestDualAnalogControllerTypeHasNoRumble(System* system) {
  printf("a Dual Analog controller type goes analog but never rumbles\n");
  FreshPad(system, 0);
  system->sio().set_controller_type(0, Sio::kDualAnalog);
  PadHarness pad(system);

  const uint8_t enter[1] = { 0x01 };
  const uint8_t leave[1] = { 0x00 };
  pad.Command(0, 0x43, enter, 1, nullptr, 0);
  const uint8_t go_analog[2] = { 0x01, 0x02 };
  pad.Command(0, 0x44, go_analog, 2, nullptr, 0);
  pad.Command(0, 0x43, leave, 1, nullptr, 0);

  // Leaving configuration mode first, exactly as TestEnteringAnalogMode does
  // - the ID reply is F3h5Ah for as long as config_mode stays set, regardless
  // of analog_mode, so checking it mid-config would prove nothing.
  uint8_t reply[16] = {};
  const int n = pad.Command(0, 0x42, nullptr, 0, reply, sizeof(reply));
  CheckEqual(n, 8, "the handshake works exactly like a DualShock's");
  CheckEqual(reply[0], 0x73, "and it does reach analog mode");

  // Map poll byte 1 to the large motor, same as TestRumbleMapping does for a
  // DualShock, then try to drive it. 0x4D needs configuration mode active
  // again, the same as TestRumbleMapping re-enters it.
  pad.Command(0, 0x43, enter, 1, nullptr, 0);
  const uint8_t mapping[6] = { 0xFF, 0x01, 0xFF, 0xFF, 0xFF, 0xFF };
  pad.Command(0, 0x4D, mapping, 6, nullptr, 0);
  pad.Command(0, 0x43, leave, 1, nullptr, 0);
  const uint8_t speeds[2] = { 0x00, 0xFF };
  pad.Command(0, 0x42, speeds, 2, nullptr, 0);

  uint8_t small = 0xFF, large = 0xFF;
  system->sio().motor_state(0, &small, &large);
  CheckEqual(small, 0, "no small motor exists to drive");
  CheckEqual(large, 0,
             "no large motor either, despite being mapped and sent a speed");
}

void TestMouseReportsExpectedShape(System* system) {
  printf("mouse: id, switches and axes come back in the right shape\n");
  system->sio().set_controller_type(0, Sio::kMouse);
  system->sio().set_connected(0, true);
  PadHarness pad(system);

  system->sio().set_mouse_buttons(0, /*left=*/true, /*right=*/false);
  system->sio().add_mouse_motion(0, /*dx=*/5, /*dy=*/-3);

  uint8_t reply[16] = {};
  const int n = pad.Command(0, 0x42, nullptr, 0, reply, sizeof(reply));

  CheckEqual(n, 6, "a mouse's poll reply is six bytes");
  CheckEqual(reply[0], 0x12, "mouse id low byte");
  CheckEqual(reply[1], 0x5A, "mouse id high byte");
  CheckEqual(reply[2], 0xFF, "the byte before the switches is always 0xFF");
  CheckEqual(reply[3], 0xF4,
             "left button held clears bit 3, right stays released (bit 2 set)");
  CheckEqual(reply[4], 0x05, "dx comes back as sent");
  CheckEqual(reply[5], 0xFD, "dy comes back as its two's-complement byte (-3)");

  system->sio().set_controller_type(0, Sio::kDualShock);   // leave it as found
}

void TestMouseMotionAccumulatesAcrossPolls(System* system) {
  printf("mouse: a big movement drains across as many polls as it takes\n");
  system->sio().set_controller_type(1, Sio::kMouse);
  system->sio().set_connected(1, true);
  PadHarness pad(system);

  system->sio().add_mouse_motion(1, /*dx=*/300, /*dy=*/0);   // past one byte

  uint8_t reply[16] = {};
  pad.Command(1, 0x42, nullptr, 0, reply, sizeof(reply));
  CheckEqual(reply[4], 0x7F, "the first poll sends as much as a byte can (127)");

  pad.Command(1, 0x42, nullptr, 0, reply, sizeof(reply));
  CheckEqual(reply[4], 0x7F, "and another 127 of the 173 that were left");

  pad.Command(1, 0x42, nullptr, 0, reply, sizeof(reply));
  CheckEqual(reply[4], 0x2E, "and the remaining 46 on the third poll");

  pad.Command(1, 0x42, nullptr, 0, reply, sizeof(reply));
  CheckEqual(reply[4], 0x00, "nothing left to send on the fourth");

  system->sio().set_controller_type(1, Sio::kDualShock);
}

void TestNoneControllerNeverAcknowledges(System* system) {
  printf("kNone: never answers, even if set_connected is told otherwise\n");
  system->sio().set_controller_type(0, Sio::kNone);
  system->sio().set_connected(0, true);   // a stale/mistaken call must not matter

  PadHarness pad(system);
  uint8_t reply[16] = {};
  const int n = pad.Command(0, 0x42, nullptr, 0, reply, sizeof(reply));
  CheckEqual(n, 0, "nothing comes back from a port set to no controller");

  system->sio().set_controller_type(0, Sio::kDualShock);
  FreshPad(system, 0);
}

void TestSwitchingToMouseStopsThePadAnswering(System* system) {
  printf("switching a port to mouse retires whatever pad was negotiated\n");
  FreshPad(system, 0);
  PadHarness pad(system);
  // Leave the old pad in analog mode before the switch, so this also proves
  // the switch is a fresh identity rather than the mouse somehow inheriting
  // the old pad's negotiated state.
  const uint8_t enter[1] = { 0x01 };
  pad.Command(0, 0x43, enter, 1, nullptr, 0);
  const uint8_t go_analog[2] = { 0x01, 0x02 };
  pad.Command(0, 0x44, go_analog, 2, nullptr, 0);

  system->sio().set_controller_type(0, Sio::kMouse);
  system->sio().set_connected(0, true);

  uint8_t reply[16] = {};
  const int n = pad.Command(0, 0x42, nullptr, 0, reply, sizeof(reply));
  CheckEqual(n, 6, "it now answers with the mouse's own reply shape");
  CheckEqual(reply[0], 0x12, "and the mouse's own id, not the pad's");

  system->sio().set_controller_type(0, Sio::kDualShock);
  FreshPad(system, 0);
}

void TestMultitapMethod2IndependentPlayers(System* system) {
  printf("multitap: method 2 addresses four independent players\n");
  system->sio().set_controller_type(0, Sio::kMultitap);
  for (int player = 0; player < 4; ++player)
    system->sio().set_connected(0, true, player);
  PadHarness pad(system);

  system->sio().set_buttons(0, Sio::kCross, /*player=*/0);
  system->sio().set_buttons(0, Sio::kSquare, /*player=*/1);
  system->sio().set_buttons(0, Sio::kTriangle, /*player=*/2);
  system->sio().set_buttons(0, Sio::kCircle, /*player=*/3);

  uint8_t reply[16] = {};
  int n = pad.Command(0, 0x42, nullptr, 0, reply, sizeof(reply), 0x01);
  CheckEqual(n, 4, "player A's poll reply is an ordinary four bytes");
  CheckEqual(reply[0], 0x41, "player A id");
  uint16_t buttons = static_cast<uint16_t>(reply[2] | (reply[3] << 8));
  CheckEqual(static_cast<uint16_t>(~buttons) & 0xFFFF, Sio::kCross,
             "player A sees its own buttons");

  n = pad.Command(0, 0x42, nullptr, 0, reply, sizeof(reply), 0x03);   // player C
  CheckEqual(reply[0], 0x41, "player C id");
  buttons = static_cast<uint16_t>(reply[2] | (reply[3] << 8));
  CheckEqual(static_cast<uint16_t>(~buttons) & 0xFFFF, Sio::kTriangle,
             "player C sees its own buttons, not player A's");

  system->sio().set_controller_type(0, Sio::kDualShock);
}

void TestNonMultitapPortIgnoresExtraSelectBytes(System* system) {
  printf("multitap: 0x02-0x04 never ack on an ordinary (non-multitap) port\n");
  FreshPad(system, 1);   // port 2, plain DualShock - never a multitap
  PadHarness pad(system);
  uint8_t reply[16] = {};
  const int n = pad.Command(1, 0x42, nullptr, 0, reply, sizeof(reply), 0x02);
  CheckEqual(n, 0, "an ordinary pad's port does not answer to 0x02");
}

void TestMultitapEscalationLatchesForNextTransferOnly(System* system) {
  printf("multitap: the long-response request affects the NEXT transfer, not this one\n");
  system->sio().set_controller_type(0, Sio::kMultitap);
  system->sio().set_connected(0, true, 0);
  PadHarness pad(system);

  // Manually drive the third byte, since PadHarness::Command always sends
  // 0x00 there itself - the caller's own payload only starts on the byte
  // after it, which is one position too late for this.
  pad.Begin(0);
  pad.Exchange(0x01);                          // select: player A
  const uint8_t id0 = pad.Exchange(0x42);      // command byte
  const uint8_t status0 = pad.Exchange(0x01);  // third byte: request the long response
  pad.End();
  CheckEqual(id0, 0x41, "the transfer that itself sets the request still answers as player A");
  CheckEqual(status0, 0x5A, "and its own status byte is the ordinary one, unaffected yet");

  uint8_t reply[40] = {};
  const int n = pad.Command(0, 0x42, nullptr, 0, reply, sizeof(reply), 0x01);
  CheckEqual(n, 34, "only the transfer AFTER the request becomes the long response");
  CheckEqual(reply[0], 0x80, "and it answers with the multitap's own id");

  system->sio().set_controller_type(0, Sio::kDualShock);
}

void TestMultitapEscalationAbortsOnWrongCommand(System* system) {
  printf("multitap: escalation only completes if the queued transfer's command is 0x42\n");
  system->sio().set_controller_type(0, Sio::kMultitap);
  system->sio().set_connected(0, true, 0);
  PadHarness pad(system);

  pad.Begin(0);
  pad.Exchange(0x01);
  pad.Exchange(0x42);
  pad.Exchange(0x01);   // queue the long response
  pad.End();

  // The queued transfer arrives, but asks 0x45 instead of 0x42.
  uint8_t reply[16] = {};
  const int n = pad.Command(0, 0x45, nullptr, 0, reply, sizeof(reply), 0x01);
  CheckEqual(n, 2, "the escalation aborts right after the id pair, once the command is wrong");
  CheckEqual(reply[0], 0x80, "the multitap id is still what committed to answering");

  system->sio().set_controller_type(0, Sio::kDualShock);
}

// Method 1's player blocks, each a poll. PadHarness::Command sends zeros
// there, and a zero is not a command any player understands.
const uint8_t kPollAll[32] = {
  0x42, 0, 0, 0, 0, 0, 0, 0,   0x42, 0, 0, 0, 0, 0, 0, 0,
  0x42, 0, 0, 0, 0, 0, 0, 0,   0x42, 0, 0, 0, 0, 0, 0, 0,
};

// Queues the long response for the next transfer, from an ordinary one.
void QueueLongResponse(PadHarness& pad) {
  pad.Begin(0);
  pad.Exchange(0x01);
  pad.Exchange(0x42);
  pad.Exchange(0x01);
  pad.End();
}

// A method 1 transfer driven byte by byte: the address, 0x42, the request
// bit for the transfer after this one, then 32 bytes - four blocks of eight,
// each one player's command and parameters. Returns how many bytes came
// back from the id on; `out` must hold 34.
int LongTransfer(PadHarness& pad, const uint8_t* blocks, bool queue_next,
                 uint8_t* out) {
  pad.Begin(0);
  pad.Exchange(0x01);
  int n = 0;
  out[n++] = pad.Exchange(0x42);
  if (pad.Acknowledged()) {
    out[n++] = pad.Exchange(queue_next ? 0x01 : 0x00);
    for (int i = 0; i < 32 && pad.Acknowledged(); ++i)
      out[n++] = pad.Exchange(blocks[i]);
  }
  pad.End();
  return n;
}

void TestMultitapMethod1LongResponseShape(System* system) {
  printf("multitap: method 1's long response is 34 bytes, ids and padding per player\n");
  system->sio().set_controller_type(0, Sio::kMultitap);
  system->sio().set_connected(0, true, 0);
  system->sio().set_connected(0, true, 1);
  // Players C and D are left disconnected on purpose - proves an empty
  // multitap slot pads its whole 8 bytes with 0xFF, same as psx-spx
  // documents for it.
  PadHarness pad(system);

  system->sio().set_buttons(0, Sio::kCross, /*player=*/0);
  system->sio().set_buttons(0, Sio::kSquare, /*player=*/1);

  QueueLongResponse(pad);
  uint8_t reply[34] = {};
  int n = LongTransfer(pad, kPollAll, /*queue_next=*/true, reply);
  CheckEqual(n, 34, "the long response is 34 bytes total");
  CheckEqual(reply[0], 0x80, "multitap id low byte");
  CheckEqual(reply[1], 0x5A, "multitap id high byte");
  // Method 1 answers one transfer behind, and nothing has been asked yet.
  CheckEqual(reply[2], 0xFF, "the first long response has nothing to answer with yet");
  CheckEqual(reply[33], 0xFF, "...anywhere in it");

  n = LongTransfer(pad, kPollAll, /*queue_next=*/false, reply);
  CheckEqual(n, 34, "and the next is 34 bytes too");
  CheckEqual(reply[2], 0x41, "player A (connected) id low");
  CheckEqual(reply[3], 0x5A, "player A id high");
  uint16_t buttons = static_cast<uint16_t>(reply[4] | (reply[5] << 8));
  CheckEqual(static_cast<uint16_t>(~buttons) & 0xFFFF, Sio::kCross,
             "player A's own buttons");
  CheckEqual(reply[6], 0xFF, "player A pads its unused 3rd halfword (digital)");
  CheckEqual(reply[9], 0xFF, "...through its 4th");

  CheckEqual(reply[10], 0x41, "player B (connected) id low");
  buttons = static_cast<uint16_t>(reply[12] | (reply[13] << 8));
  CheckEqual(static_cast<uint16_t>(~buttons) & 0xFFFF, Sio::kSquare,
             "player B's own buttons, not player A's");

  CheckEqual(reply[18], 0xFF, "player C (never connected) pads its id low too");
  CheckEqual(reply[19], 0xFF, "...and its id high");
  CheckEqual(reply[26], 0xFF, "player D (never connected) likewise");

  system->sio().set_controller_type(0, Sio::kDualShock);
}

// Each block of a long transfer is a whole command exchange with its own
// player, and its answer comes back in the next long transfer. Bomberman
// Party Edition takes every player through the DualShock handshake this
// way; a multitap that answered every block as a plain poll never let one
// into configuration mode, and the game went on asking for as long as it
// was plugged in (bug 53).
void TestMultitapMethod1ForwardsEachBlock(System* system) {
  printf("multitap: method 1 hands each block to its player, answered a transfer later\n");
  system->sio().set_controller_type(0, Sio::kMultitap);
  for (int player = 0; player < 4; ++player)
    system->sio().set_connected(0, true, player);
  PadHarness pad(system);

  QueueLongResponse(pad);
  uint8_t blocks[32];
  memcpy(blocks, kPollAll, sizeof(blocks));
  blocks[8] = 0x43;    // player B: enter or leave configuration mode...
  blocks[10] = 0x01;   // ...enter
  uint8_t reply[34] = {};
  LongTransfer(pad, blocks, /*queue_next=*/true, reply);

  LongTransfer(pad, kPollAll, /*queue_next=*/true, reply);
  CheckEqual(reply[2], 0x41, "player A's poll is answered a transfer later");
  CheckEqual(reply[10], 0x41,
             "so is player B's 0x43, under its normal-mode id");
  CheckEqual(reply[14], 0xFF, "a digital-mode 0x43 is a poll's length, then padding");

  LongTransfer(pad, kPollAll, /*queue_next=*/false, reply);
  CheckEqual(reply[10], 0xF3,
             "and the poll after it finds player B in configuration mode");
  CheckEqual(reply[2], 0x41, "while player A never went");

  uint8_t direct[16] = {};
  pad.Command(0, 0x42, nullptr, 0, direct, sizeof(direct), 0x02);
  CheckEqual(direct[0], 0xF3,
             "addressed on its own, player B answers from configuration mode too");

  system->sio().set_controller_type(0, Sio::kDualShock);
}

void TestMultitapSurvivesSaveState(System* system) {
  printf("multitap state (and which player is which) round-trips through a save state\n");
  system->sio().set_controller_type(0, Sio::kMultitap);
  system->sio().set_connected(0, true, 0);
  system->sio().set_connected(0, true, 2);
  system->sio().set_buttons(0, Sio::kCross, /*player=*/0);
  system->sio().set_buttons(0, Sio::kSquare, /*player=*/2);

  const std::string path = "Temp\\tools\\multitap_state_test.sav";
  const std::string save_error = system->SaveState(path);
  Check(save_error.empty(), "save succeeds");

  // Disturb the live state so loading actually has to restore something,
  // not just leave what was already there untouched.
  system->sio().set_controller_type(0, Sio::kDualShock);

  const std::string load_error = system->LoadState(path);
  Check(load_error.empty(), "load succeeds");
  CheckEqual(system->sio().controller_type(0), Sio::kMultitap,
             "port 0 is a multitap again after loading");

  PadHarness pad(system);
  uint8_t reply[16] = {};
  pad.Command(0, 0x42, nullptr, 0, reply, sizeof(reply), 0x01);   // player A
  uint16_t buttons = static_cast<uint16_t>(reply[2] | (reply[3] << 8));
  CheckEqual(static_cast<uint16_t>(~buttons) & 0xFFFF, Sio::kCross,
             "player A's buttons survived the round trip");

  pad.Command(0, 0x42, nullptr, 0, reply, sizeof(reply), 0x03);   // player C
  buttons = static_cast<uint16_t>(reply[2] | (reply[3] << 8));
  CheckEqual(static_cast<uint16_t>(~buttons) & 0xFFFF, Sio::kSquare,
             "and so did player C's, at the right player rather than swapped");

  system->sio().set_controller_type(0, Sio::kDualShock);
}

void TestDualShockControllerTypeDefaultStillRumbles(System* system) {
  printf("the default DualShock type is unaffected by the new gating\n");
  FreshPad(system, 0);
  system->sio().set_controller_type(0, Sio::kDualShock);
  PadHarness pad(system);

  const uint8_t enter[1] = { 0x01 };
  pad.Command(0, 0x43, enter, 1, nullptr, 0);
  const uint8_t go_analog[2] = { 0x01, 0x02 };
  pad.Command(0, 0x44, go_analog, 2, nullptr, 0);

  const uint8_t mapping[6] = { 0xFF, 0x01, 0xFF, 0xFF, 0xFF, 0xFF };
  pad.Command(0, 0x4D, mapping, 6, nullptr, 0);
  const uint8_t speeds[2] = { 0x00, 0xFF };
  pad.Command(0, 0x42, speeds, 2, nullptr, 0);

  uint8_t small = 0, large = 0;
  system->sio().motor_state(0, &small, &large);
  CheckEqual(large, 0xFF,
             "a DualShock still rumbles - the gating only excludes the "
             "other two types");
}

// A command a pad does not understand ends the transfer at the command byte:
// the id has already gone out with it, but no /ACK follows. These used to be
// answered in the shape of a poll full of zeros - and with buttons active low,
// zero is every button held (bug 52).
void TestDigitalPadRefusesConfigCommands(System* system) {
  printf("a digital pad does not acknowledge what it does not understand\n");
  FreshPad(system, 0);
  system->sio().set_controller_type(0, Sio::kDigital);
  PadHarness pad(system);

  uint8_t reply[16] = {};
  const uint8_t enter[1] = { 0x01 };
  int n = pad.Command(0, 0x43, enter, 1, reply, sizeof(reply));
  CheckEqual(n, 1, "0x43 ends the transfer at the command byte");
  CheckEqual(reply[0], 0x41, "with the id already sent");

  n = pad.Command(0, 0x45, nullptr, 0, reply, sizeof(reply));
  CheckEqual(n, 1, "0x45 likewise");

  n = pad.Command(0, 0x42, nullptr, 0, reply, sizeof(reply));
  CheckEqual(n, 4, "and a poll still answers in full");

  system->sio().set_controller_type(0, Sio::kDualShock);
}

// psx-spx: sent in normal mode, 43h answers with the same joypad data as 42h.
// It used to answer zeros, so a driver entering configuration mode read the
// whole pad as held on that frame.
void TestEnterConfigReplyCarriesTheButtons(System* system) {
  printf("0x43 outside configuration mode answers with the buttons\n");
  FreshPad(system, 0);
  system->sio().set_controller_type(0, Sio::kDualShock);
  PadHarness pad(system);
  system->sio().set_buttons(0, Sio::kCircle);

  uint8_t reply[16] = {};
  const uint8_t enter[1] = { 0x01 };
  const int n = pad.Command(0, 0x43, enter, 1, reply, sizeof(reply));
  CheckEqual(n, 4, "a digital-mode pad answers 0x43 in a poll's four bytes");
  CheckEqual(reply[0], 0x41, "under its normal-mode id");
  const uint16_t buttons =
      static_cast<uint16_t>(reply[2] | (reply[3] << 8));
  CheckEqual(static_cast<uint16_t>(~buttons) & 0xFFFF, Sio::kCircle,
             "carrying the button actually held, not every button");

  const int m = pad.Command(0, 0x42, nullptr, 0, reply, sizeof(reply));
  CheckEqual(reply[0], 0xF3, "and it is in configuration mode afterwards");
  CheckEqual(m, 8, "where a poll takes the forced long shape");

  const uint8_t leave[1] = { 0x00 };
  pad.Command(0, 0x43, leave, 1, nullptr, 0);
  CheckEqual(pad.Command(0, 0x42, nullptr, 0, reply, sizeof(reply)), 4,
             "and leaving it restores the four-byte poll");
  system->sio().set_buttons(0, 0);
}

// The configuration commands are only understood inside configuration mode.
// Outside it a DualShock does not acknowledge them.
void TestDualShockRefusesConfigCommandsOutsideConfigMode(System* system) {
  printf("a DualShock outside configuration mode refuses 0x44-0x4D\n");
  FreshPad(system, 0);
  system->sio().set_controller_type(0, Sio::kDualShock);
  PadHarness pad(system);

  uint8_t reply[16] = {};
  CheckEqual(pad.Command(0, 0x45, nullptr, 0, reply, sizeof(reply)), 1,
             "0x45 ends the transfer at the command byte");
  const uint8_t go_analog[2] = { 0x01, 0x03 };
  CheckEqual(pad.Command(0, 0x44, go_analog, 2, reply, sizeof(reply)), 1,
             "0x44 too");
  CheckEqual(pad.Command(0, 0x42, nullptr, 0, reply, sizeof(reply)), 4,
             "and it changed nothing: still a four-byte digital poll");
  CheckEqual(reply[0], 0x41, "id still 5A41h");
}

}  // namespace

int main() {
  System* system = new System();
  system->InitializeWithoutBios();

  TestDigitalPadUnaffected(system);
  TestEmptySlotNeverAcknowledges(system);
  TestAckPulseSelfReleases(system);
  TestConfigModeGatesTheSpecialCommands(system);
  TestEnteringAnalogMode(system);
  TestStatusQueryReportsTheMode(system);
  TestAxesRoundTrip(system);
  TestLegacyRumble(system);
  TestRumbleMapping(system);
  TestUnconfiguredDualShockRumblesAtNothing(system);
  TestReconnectForgetsNegotiation(system);
  TestTwoSlotsAreIndependent(system);
  TestDigitalControllerTypeNeverGoesAnalog(system);
  TestDualAnalogControllerTypeHasNoRumble(system);
  TestDualShockControllerTypeDefaultStillRumbles(system);
  TestMouseReportsExpectedShape(system);
  TestMouseMotionAccumulatesAcrossPolls(system);
  TestNoneControllerNeverAcknowledges(system);
  TestSwitchingToMouseStopsThePadAnswering(system);
  TestMultitapMethod2IndependentPlayers(system);
  TestNonMultitapPortIgnoresExtraSelectBytes(system);
  TestMultitapEscalationLatchesForNextTransferOnly(system);
  TestMultitapEscalationAbortsOnWrongCommand(system);
  TestMultitapMethod1LongResponseShape(system);
  TestMultitapMethod1ForwardsEachBlock(system);
  TestMultitapSurvivesSaveState(system);
  TestDigitalPadRefusesConfigCommands(system);
  TestEnterConfigReplyCarriesTheButtons(system);
  TestDualShockRefusesConfigCommandsOutsideConfigMode(system);

  printf("\n%d checks, %d failures\n", g_checks, g_failures);
  delete system;
  return g_failures == 0 ? 0 : 1;
}
