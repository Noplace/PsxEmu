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
using emulation::psx::Sio1;
using emulation::psx::System;
using emulation::psx::kInterruptSIO1;

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

// 0x46, 0x47 and 0x4C each take a question in the first byte the host sends
// and answer it in bytes 4-7 (bug 96). They used to reply zeros, and 0x4C
// answered 04h whatever it was asked. psx-spx's and DuckStation's values.
void TestCapabilityQueries(System* system) {
  printf("0x46, 0x47 and 0x4C answer the question they are asked\n");
  FreshPad(system, 0);
  PadHarness pad(system);
  const uint8_t enter[1] = { 0x01 };
  pad.Command(0, 0x43, enter, 1, nullptr, 0);

  const uint8_t query0[6] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
  const uint8_t query1[6] = { 0x01, 0x00, 0x00, 0x00, 0x00, 0x00 };
  const uint8_t query2[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x00 };
  uint8_t reply[16] = {};

  auto tail = [&](const uint8_t* want, const char* what) {
    const bool same = reply[4] == want[0] && reply[5] == want[1] &&
                      reply[6] == want[2] && reply[7] == want[3];
    if (!same)
      printf("    got %02X %02X %02X %02X\n", reply[4], reply[5], reply[6], reply[7]);
    Check(same, what);
  };

  pad.Command(0, 0x46, query0, 6, reply, sizeof(reply));
  { const uint8_t want[4] = { 0x01, 0x02, 0x00, 0x0A }; tail(want, "0x46 query 0 answers 01 02 00 0A"); }
  pad.Command(0, 0x46, query1, 6, reply, sizeof(reply));
  { const uint8_t want[4] = { 0x01, 0x01, 0x01, 0x14 }; tail(want, "0x46 query 1 answers 01 01 01 14"); }
  pad.Command(0, 0x46, query2, 6, reply, sizeof(reply));
  { const uint8_t want[4] = { 0x00, 0x00, 0x00, 0x00 }; tail(want, "0x46 any other query answers zeros"); }

  pad.Command(0, 0x47, query0, 6, reply, sizeof(reply));
  { const uint8_t want[4] = { 0x02, 0x00, 0x01, 0x00 }; tail(want, "0x47 query 0 answers 02 00 01 00"); }
  pad.Command(0, 0x47, query1, 6, reply, sizeof(reply));
  { const uint8_t want[4] = { 0x00, 0x00, 0x00, 0x00 }; tail(want, "0x47 any other query answers zeros"); }

  pad.Command(0, 0x4C, query0, 6, reply, sizeof(reply));
  { const uint8_t want[4] = { 0x00, 0x04, 0x00, 0x00 }; tail(want, "0x4C query 0 answers 04 in byte 5"); }
  pad.Command(0, 0x4C, query1, 6, reply, sizeof(reply));
  { const uint8_t want[4] = { 0x00, 0x07, 0x00, 0x00 }; tail(want, "0x4C query 1 answers 07 in byte 5"); }

  const uint8_t leave[1] = { 0x00 };
  pad.Command(0, 0x43, leave, 1, nullptr, 0);
}

// The ANALOG button (bug 97): the player switching the pad's mode, which the
// game can refuse by locking it.
void TestAnalogButton(System* system) {
  printf("the ANALOG button switches the mode unless the game has locked it\n");
  FreshPad(system, 0);
  PadHarness pad(system);
  uint8_t reply[16] = {};

  auto poll_id = [&]() -> int {
    const int n = pad.Command(0, 0x42, nullptr, 0, reply, sizeof(reply));
    return n > 0 ? reply[0] : -1;
  };

  CheckEqual(poll_id(), 0x41, "a fresh DualShock polls as a digital pad");
  system->sio().PressAnalogButton(0);
  CheckEqual(poll_id(), 0x73, "one press and it polls as analog");
  CheckEqual(pad.Command(0, 0x42, nullptr, 0, reply, sizeof(reply)), 8,
             "with the eight-byte analog reply");
  system->sio().PressAnalogButton(0);
  CheckEqual(poll_id(), 0x41, "a second press puts it back to digital");

  // The game locks it into analog: 0x44 with mode 1 and lock 3.
  const uint8_t enter[1] = { 0x01 };
  const uint8_t leave[1] = { 0x00 };
  const uint8_t analog_locked[2] = { 0x01, 0x03 };
  pad.Command(0, 0x43, enter, 1, nullptr, 0);
  pad.Command(0, 0x44, analog_locked, 2, nullptr, 0);
  pad.Command(0, 0x43, leave, 1, nullptr, 0);
  CheckEqual(poll_id(), 0x73, "the game switched it to analog and locked it");
  system->sio().PressAnalogButton(0);
  CheckEqual(poll_id(), 0x73, "and the button is refused while it is locked");

  // A press in the middle of a transfer waits for it to end, so the transfer
  // keeps the length it started with.
  FreshPad(system, 0);
  pad.Begin(0);
  pad.Exchange(0x01);
  CheckEqual(pad.Exchange(0x42), 0x41, "a poll starts as digital");
  system->sio().PressAnalogButton(0);
  pad.Exchange(0x00);   // status
  pad.Exchange(0x00);   // buttons low
  pad.Exchange(0x00);   // buttons high - a digital reply ends here
  CheckEqual(pad.Acknowledged() ? 1 : 0, 0,
             "the transfer the press arrived in still ends at four bytes");
  pad.End();
  CheckEqual(poll_id(), 0x73, "and the next one sees the new mode");

  // A plain digital pad has no ANALOG button to press.
  system->sio().set_controller_type(0, Sio::kDigital);
  FreshPad(system, 0);
  system->sio().PressAnalogButton(0);
  CheckEqual(poll_id(), 0x41, "a digital pad ignores it");
  system->sio().set_controller_type(0, Sio::kDualShock);   // leave it as found
  FreshPad(system, 0);
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

// Each of a multitap's players can be a different kind of pad (bug 98).
void TestMultitapPlayerTypes(System* system) {
  printf("multitap: each player can be a different kind of pad, or nothing\n");
  system->sio().set_controller_type(0, Sio::kMultitap);
  system->sio().set_multitap_player_type(0, 0, Sio::kDualShock);
  system->sio().set_multitap_player_type(0, 1, Sio::kDigital);
  system->sio().set_multitap_player_type(0, 2, Sio::kNone);
  system->sio().set_multitap_player_type(0, 3, Sio::kDualAnalog);
  for (int player = 0; player < 4; ++player)
    system->sio().set_connected(0, player != 2, player);
  PadHarness pad(system);
  uint8_t reply[16] = {};
  const uint8_t enter[1] = { 0x01 };
  const uint8_t leave[1] = { 0x00 };
  const uint8_t go_analog[2] = { 0x01, 0x02 };

  // Player A, a DualShock, takes the configuration handshake.
  int n = pad.Command(0, 0x43, enter, 1, reply, sizeof(reply), 0x01);
  Check(n > 1, "player A, a DualShock, takes 0x43");
  pad.Command(0, 0x43, leave, 1, nullptr, 0, 0x01);

  // Player B, a digital pad, has no configuration mode to enter.
  n = pad.Command(0, 0x43, enter, 1, reply, sizeof(reply), 0x02);
  CheckEqual(n, 1, "player B, a digital pad, refuses 0x43 at the command byte");
  n = pad.Command(0, 0x42, nullptr, 0, reply, sizeof(reply), 0x02);
  CheckEqual(reply[0], 0x41, "and polls as a digital pad");

  // Player C, set to nothing, does not answer at all.
  n = pad.Command(0, 0x42, nullptr, 0, reply, sizeof(reply), 0x03);
  CheckEqual(n, 0, "player C, set to nothing, gives no /ACK");

  // Player D, a Dual Analog, goes analog like a DualShock.
  pad.Command(0, 0x43, enter, 1, nullptr, 0, 0x04);
  pad.Command(0, 0x44, go_analog, 2, nullptr, 0, 0x04);
  pad.Command(0, 0x43, leave, 1, nullptr, 0, 0x04);
  n = pad.Command(0, 0x42, nullptr, 0, reply, sizeof(reply), 0x04);
  CheckEqual(reply[0], 0x73, "player D, a Dual Analog, can be put into analog mode");

  // Changing a player's type is a different pad in that socket.
  system->sio().set_multitap_player_type(0, 3, Sio::kDualShock);
  system->sio().set_connected(0, true, 3);
  n = pad.Command(0, 0x42, nullptr, 0, reply, sizeof(reply), 0x04);
  CheckEqual(reply[0], 0x41, "a new pad in D's socket starts digital again");

  for (int player = 0; player < 4; ++player)
    system->sio().set_multitap_player_type(0, player, Sio::kDualShock);
  system->sio().set_controller_type(0, Sio::kDualShock);
  FreshPad(system, 0);
}

// Reads one sector over the bus the way a game does: the card address, 52h,
// the sector number, then 128 bytes of data. Returns how many data bytes came
// back - zero if nothing answered the address.
int ReadCardSector(PadHarness& pad, int port, uint8_t address, uint16_t sector,
                   uint8_t* data) {
  pad.Begin(port);
  pad.Exchange(address);
  if (!pad.Acknowledged()) {
    pad.End();
    return 0;
  }
  pad.Exchange(0x52);                                   // read
  pad.Exchange(0x00);                                   // 5Ah
  pad.Exchange(0x00);                                   // 5Dh
  pad.Exchange(static_cast<uint8_t>(sector >> 8));
  pad.Exchange(static_cast<uint8_t>(sector));
  for (int i = 0; i < 4; ++i)                           // 5Ch 5Dh and the sector echoed
    pad.Exchange(0x00);
  int n = 0;
  for (; n < 128; ++n)
    data[n] = pad.Exchange(0x00);
  pad.End();
  return n;
}

// A multitap's four card slots (bug 99): 81h-84h reach slot A-D, slot A is the
// port's own card, and an empty slot does not answer.
void TestMultitapMemoryCards(System* system) {
  printf("multitap: 81h-84h reach four separate memory card slots\n");
  char dir[MAX_PATH] = {};
  GetTempPathA(MAX_PATH, dir);
  const char* names[3] = { "sio_test_card_a.mcr", "sio_test_card_b.mcr", "sio_test_card_c.mcr" };
  for (int slot = 0; slot < 3; ++slot) {
    const std::string path = std::string(dir) + names[slot];
    emulation::psx::MC& card = system->mc(0, slot);
    card.CreateFile(path.c_str());
    // A sector each card can be told apart by.
    uint8_t mark[128];
    memset(mark, 0xA0 + slot, sizeof(mark));
    card.WriteSector(0x100, mark);
  }
  system->mc(0, 3).Eject();   // slot D left empty

  system->sio().set_controller_type(0, Sio::kMultitap);
  system->sio().set_connected(0, true, 0);
  PadHarness pad(system);
  uint8_t data[128] = {};

  CheckEqual(ReadCardSector(pad, 0, 0x81, 0x100, data), 128, "81h reads slot A");
  CheckEqual(data[0], 0xA0, "and it is the port's own card");
  CheckEqual(ReadCardSector(pad, 0, 0x82, 0x100, data), 128, "82h reads slot B");
  CheckEqual(data[0], 0xA1, "a different card");
  CheckEqual(ReadCardSector(pad, 0, 0x83, 0x100, data), 128, "83h reads slot C");
  CheckEqual(data[0], 0xA2, "and another");
  CheckEqual(ReadCardSector(pad, 0, 0x84, 0x100, data), 0,
             "84h, an empty slot, does not answer");

  // Without a multitap there is one card slot, and only 81h reaches it.
  system->sio().set_controller_type(0, Sio::kDualShock);
  FreshPad(system, 0);
  CheckEqual(ReadCardSector(pad, 0, 0x81, 0x100, data), 128, "a plain port still reads its card");
  CheckEqual(data[0], 0xA0, "the same card as slot A");
  CheckEqual(ReadCardSector(pad, 0, 0x82, 0x100, data), 0,
             "and 82h reaches nothing without a multitap");

  for (int slot = 0; slot < 3; ++slot) {
    system->mc(0, slot).Eject();
    DeleteFileA((std::string(dir) + names[slot]).c_str());
  }
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

// ---------------------------------------------------------------------------
// SIO1, the serial port (sio1.h). A different device from everything above:
// the same chip family, but the asynchronous port on the back of the console
// with nothing plugged into it. These check the registers behave as a port
// with no cable does, since that is the whole of what is modelled.

const uint32_t kSio1Data = 0x1F801050;
const uint32_t kSio1Stat = 0x1F801054;
const uint32_t kSio1Mode = 0x1F801058;
const uint32_t kSio1Ctrl = 0x1F80105A;
const uint32_t kSio1Baud = 0x1F80105E;

// The reset strobe, which every test starts from.
void ResetSio1(System* system) {
  system->io().Write16(kSio1Ctrl, Sio1::kCtrlReset);
  system->io().io.interrupt_stat = 0;
}

void TestSio1ResetState(System* system) {
  printf("SIO1 comes out of reset idle, ready, and with nothing connected\n");
  ResetSio1(system);
  const uint32_t stat = system->io().Read32(kSio1Stat);
  Check((stat & Sio1::kStatTxReady) != 0, "the transmitter can take a byte");
  Check((stat & Sio1::kStatTxIdle) != 0, "and is idle");
  Check((stat & Sio1::kStatRxNotEmpty) == 0, "the receive FIFO is empty");
  Check((stat & Sio1::kStatInterrupt) == 0, "no interrupt is latched");
  Check((stat & Sio1::kStatDsrLevel) == 0, "no device is asserting /DSR");
  Check((stat & Sio1::kStatCtsLevel) == 0, "nor CTS");
  CheckEqual(system->io().Read16(kSio1Mode), 0, "mode is clear");
  CheckEqual(system->io().Read16(kSio1Ctrl), 0, "control is clear");
  CheckEqual(system->io().Read16(kSio1Baud), 0xDC, "baud is DCh");
}

void TestSio1EmptyReceiveFifoReadsOnes(System* system) {
  printf("reading SIO1's data register with nothing attached gives the idle line\n");
  ResetSio1(system);
  CheckEqual(system->io().Read08(kSio1Data), 0xFF, "a byte read is FFh");
  CheckEqual(system->io().Read16(kSio1Data), 0xFFFF, "a halfword read is FFFFh");
  CheckEqual(system->io().Read32(kSio1Data), 0xFFFFFFFFu, "and a word FFFFFFFFh");
}

void TestSio1RegistersRoundTrip(System* system) {
  printf("SIO1's mode, control and baud registers keep what is written\n");
  ResetSio1(system);
  system->io().Write16(kSio1Mode, 0x004D);
  system->io().Write16(kSio1Baud, 0x1234);
  system->io().Write16(kSio1Ctrl, Sio1::kCtrlTxEnable | Sio1::kCtrlRxEnable |
                                      Sio1::kCtrlDtrOutput);
  CheckEqual(system->io().Read16(kSio1Mode), 0x004D, "mode reads back");
  CheckEqual(system->io().Read16(kSio1Baud), 0x1234, "baud reads back");
  CheckEqual(system->io().Read16(kSio1Ctrl),
             Sio1::kCtrlTxEnable | Sio1::kCtrlRxEnable | Sio1::kCtrlDtrOutput,
             "control reads back");

  // The acknowledge bit is a strobe: it does something and does not stick.
  system->io().Write16(kSio1Ctrl, Sio1::kCtrlTxEnable | Sio1::kCtrlAcknowledge);
  CheckEqual(system->io().Read16(kSio1Ctrl), Sio1::kCtrlTxEnable,
             "the acknowledge strobe does not stay set");

  // A byte write reaches the same register as a halfword write.
  system->io().Write08(kSio1Baud, 0x56);
  CheckEqual(system->io().Read08(kSio1Baud), 0x56, "a byte write lands in baud");

  // A word access covers the pair of halfword registers sharing its word:
  // mode with control, and baud with the unused 105Ch below it.
  system->io().Write32(kSio1Mode, 0x00010021);
  CheckEqual(system->io().Read16(kSio1Mode), 0x0021,
             "a word write puts its low half in mode");
  CheckEqual(system->io().Read16(kSio1Ctrl), 0x0001,
             "and its high half in control");
  CheckEqual(system->io().Read32(kSio1Mode), 0x00010021,
             "a word read gives both back");
  system->io().Write32(kSio1Baud, 0x00990000);
  CheckEqual(system->io().Read16(kSio1Baud), 0x0099,
             "baud is the high half of its word");
}

void TestSio1StatusIsReadOnly(System* system) {
  printf("writing SIO1's status register changes nothing\n");
  ResetSio1(system);
  const uint32_t before = system->io().Read32(kSio1Stat) & 0x7FF;
  system->io().Write32(kSio1Stat, 0xFFFFFFFFu);
  CheckEqual(system->io().Read32(kSio1Stat) & 0x7FF, before,
             "status is what it was");
}

void TestSio1TransmitNeedsTheTransmitterEnabled(System* system) {
  printf("SIO1 transmits, and raises IRQ8, only when it is enabled to\n");
  ResetSio1(system);

  // Transmitter off: the byte goes nowhere and nothing is raised.
  system->io().Write16(kSio1Ctrl, Sio1::kCtrlTxInterrupt);
  system->io().Write08(kSio1Data, 'A');
  CheckEqual(system->io().io.interrupt_stat & kInterruptSIO1, 0,
             "a disabled transmitter raises no interrupt");

  // On, with the transmit interrupt armed: the byte leaves and IRQ8 follows.
  ResetSio1(system);
  system->io().Write16(kSio1Ctrl, Sio1::kCtrlTxEnable | Sio1::kCtrlTxInterrupt);
  system->io().Write08(kSio1Data, 'B');
  CheckEqual(system->io().io.interrupt_stat & kInterruptSIO1, kInterruptSIO1,
             "an enabled one raises IRQ8");
  Check((system->io().Read32(kSio1Stat) & Sio1::kStatInterrupt) != 0,
        "and latches the interrupt bit in status");
  Check((system->io().Read32(kSio1Stat) & Sio1::kStatTxReady) != 0,
        "the transmitter is ready for the next byte");

  // The acknowledge strobe clears the latch.
  system->io().Write16(kSio1Ctrl, Sio1::kCtrlTxEnable | Sio1::kCtrlAcknowledge);
  Check((system->io().Read32(kSio1Stat) & Sio1::kStatInterrupt) == 0,
        "acknowledging clears the latch");
}

void TestSio1ResetStrobeRestoresEverything(System* system) {
  printf("SIO1's reset strobe puts the port back as it started\n");
  ResetSio1(system);
  system->io().Write16(kSio1Mode, 0x00FF);
  system->io().Write16(kSio1Baud, 0x4321);
  system->io().Write16(kSio1Ctrl, Sio1::kCtrlTxEnable | Sio1::kCtrlRtsOutput);
  system->io().Write16(kSio1Ctrl, Sio1::kCtrlReset);
  CheckEqual(system->io().Read16(kSio1Mode), 0, "mode is clear again");
  CheckEqual(system->io().Read16(kSio1Baud), 0xDC, "baud is back to DCh");
  CheckEqual(system->io().Read16(kSio1Ctrl), 0, "control is clear again");
}

void TestSio1BaudTimerCountsDown(System* system) {
  printf("SIO1's baud-rate timer counts down and reloads\n");
  ResetSio1(system);
  system->io().Write16(kSio1Mode, 0);          // reload factor 1
  system->io().Write16(kSio1Baud, 0x0100);     // 256 cycles a period
  const uint32_t start =
      (system->io().Read32(kSio1Stat) >> Sio1::kStatTimerShift) & Sio1::kStatTimerMask;
  CheckEqual(start, 0x0100, "writing baud reloads the timer");

  system->io().sio1.Tick(100);
  const uint32_t after =
      (system->io().Read32(kSio1Stat) >> Sio1::kStatTimerShift) & Sio1::kStatTimerMask;
  CheckEqual(after, 0x0100 - 100, "100 cycles later it has counted down 100");

  // Past zero it reloads rather than stopping - 156 cycles finishes this
  // period, the next 256 the following one, leaving a whole period again.
  system->io().sio1.Tick(156);
  const uint32_t reloaded =
      (system->io().Read32(kSio1Stat) >> Sio1::kStatTimerShift) & Sio1::kStatTimerMask;
  CheckEqual(reloaded, 0x0100, "reaching zero reloads it");
}

void TestSio1ConsoleRedirect(System* system) {
  printf("SIO1 hands what it transmits to the console only when asked to\n");
  ResetSio1(system);
  std::string text;
  system->kernel().TakeConsoleText(&text);   // drain whatever was pending

  system->config().sio1_to_console = false;
  system->io().Write16(kSio1Ctrl, Sio1::kCtrlTxEnable);
  system->io().Write08(kSio1Data, 'n');
  system->kernel().TakeConsoleText(&text);
  Check(text.empty(), "off, the byte is gone");

  system->config().sio1_to_console = true;
  system->io().Write08(kSio1Data, 'h');
  system->io().Write08(kSio1Data, 'i');
  system->kernel().TakeConsoleText(&text);
  Check(text == "hi", "on, it reaches the console text");

  // Still only what the port actually transmits.
  system->io().Write16(kSio1Ctrl, 0);
  system->io().Write08(kSio1Data, 'x');
  system->kernel().TakeConsoleText(&text);
  Check(text.empty(), "with the transmitter disabled, nothing is sent");
  system->config().sio1_to_console = false;
}

void TestSio1SurvivesSaveState(System* system) {
  printf("SIO1's registers round-trip through a save state\n");
  ResetSio1(system);
  system->io().Write16(kSio1Mode, 0x0032);
  system->io().Write16(kSio1Baud, 0x0708);
  system->io().Write16(kSio1Ctrl, Sio1::kCtrlTxEnable | Sio1::kCtrlDtrOutput);

  const std::string path = "Temp\\tools\\sio1_state_test.sav";
  Check(system->SaveState(path).empty(), "save succeeds");

  system->io().Write16(kSio1Ctrl, Sio1::kCtrlReset);
  Check(system->LoadState(path).empty(), "load succeeds");

  CheckEqual(system->io().Read16(kSio1Mode), 0x0032, "mode came back");
  CheckEqual(system->io().Read16(kSio1Baud), 0x0708, "baud came back");
  CheckEqual(system->io().Read16(kSio1Ctrl),
             Sio1::kCtrlTxEnable | Sio1::kCtrlDtrOutput, "control came back");
}

}  // namespace

// A GunCon (5A63h): answers 42h with its ID, three buttons and where it is
// aimed, and nothing else.
void TestGunConReply(System* system) {
  using emulation::psx::Gpu;
  printf("guncon: id, buttons and position come back in the right shape\n");
  system->sio().set_controller_type(0, Sio::kGunCon);
  system->sio().set_connected(0, true);
  PadHarness pad(system);
  uint8_t reply[16] = {};

  // A 320-wide picture, the usual NTSC display range: beam on from dot 608 to
  // 3168 (320 dots of 8 clocks) and lines 16 to 256.
  Gpu& gpu = system->gpu();
  gpu.WriteStatus(0x08000001);                          // 320 wide, 240 lines
  gpu.WriteStatus(0x06000000 | (3168u << 12) | 608u);
  gpu.WriteStatus(0x07000000 | (256u << 10) | 16u);

  system->sio().set_guncon(0, false, false, false, 0.5f, 0.5f);
  int n = pad.Command(0, 0x42, nullptr, 0, reply, sizeof(reply));
  CheckEqual(n, 8, "a GunCon's reply is eight bytes");
  CheckEqual(reply[0], 0x63, "id low byte");
  CheckEqual(reply[1], 0x5A, "id high byte");
  CheckEqual(reply[2], 0xFF, "nothing held: buttons low byte all ones");
  CheckEqual(reply[3], 0xFF, "and the high byte");
  // The middle of the picture: dot 608 + 160 x 8 = 1888, in the gun's 8 MHz
  // counts 1888 / 6.6528 = 283; line 16 + 120 = 136.
  CheckEqual(reply[4] | (reply[5] << 8), 283, "X is the beam's dot in 8 MHz counts");
  CheckEqual(reply[6] | (reply[7] << 8), 136, "Y is the beam's line");

  system->sio().set_guncon(0, true, false, false, 0.0f, 0.0f);
  pad.Command(0, 0x42, nullptr, 0, reply, sizeof(reply));
  CheckEqual(reply[3], 0xDF, "the trigger is bit 13, active low");
  CheckEqual(reply[4] | (reply[5] << 8), 91, "the left edge is where the beam turns on (608 / 6.6528)");
  CheckEqual(reply[6] | (reply[7] << 8), 16, "the top edge is the first line shown");

  system->sio().set_guncon(0, false, true, true, 0.5f, 0.5f);
  pad.Command(0, 0x42, nullptr, 0, reply, sizeof(reply));
  CheckEqual(reply[2], 0xF7, "A is bit 3");
  CheckEqual(reply[3], 0xBF, "B is bit 14");

  // Aimed off the picture, the gun sees no beam and says so the way the real
  // one does - which a game takes as a shot off the screen, the reload.
  system->sio().set_guncon(0, true, false, false, 1.2f, 0.5f);
  pad.Command(0, 0x42, nullptr, 0, reply, sizeof(reply));
  CheckEqual(reply[4] | (reply[5] << 8), 0x0001, "off the screen: X is 0001h");
  CheckEqual(reply[6] | (reply[7] << 8), 0x000A, "and Y is 000Ah");

  // Both fields shown at once: 480 rows in the picture, still 240 lines.
  gpu.WriteStatus(0x08000025);
  system->sio().set_guncon(0, false, false, false, 0.5f, 0.5f);
  pad.Command(0, 0x42, nullptr, 0, reply, sizeof(reply));
  CheckEqual(reply[6] | (reply[7] << 8), 136, "interlaced, the middle is still line 136");
  gpu.WriteStatus(0x08000001);

  // It has no configuration mode, and drops out at any other command.
  const uint8_t enter[1] = { 0x01 };
  n = pad.Command(0, 0x43, enter, 1, reply, sizeof(reply));
  CheckEqual(n, 1, "43h: no /ACK after the command byte");
  CheckEqual(reply[0], 0xFF, "and no ID either");

  // A pad on the other port is untouched by any of it.
  FreshPad(system, 1);
  n = pad.Command(1, 0x42, nullptr, 0, reply, sizeof(reply));
  CheckEqual(reply[0], 0x41, "the other port's pad still answers as a pad");

  // Unplugged, nothing answers.
  system->sio().set_connected(0, false);
  CheckEqual(pad.Command(0, 0x42, nullptr, 0, reply, sizeof(reply)), 0,
             "an unplugged GunCon gives no /ACK");
  system->sio().set_connected(0, true);
}

// The gun's kind is in a state (as the port's controller type); what it is
// doing is not, and comes from the front end again after a load.
void TestGunConSurvivesSaveState(System* system) {
  printf("guncon: a state keeps the port a GunCon\n");
  system->sio().set_controller_type(0, Sio::kGunCon);
  system->sio().set_connected(0, true);
  const std::string path = "Temp\\tools\\guncon_state_test.sav";
  Check(system->SaveState(path).empty(), "save succeeds");
  system->sio().set_controller_type(0, Sio::kDualShock);
  Check(system->LoadState(path).empty(), "load succeeds");
  CheckEqual(system->sio().controller_type(0), Sio::kGunCon, "port 0 is a GunCon again");
  PadHarness pad(system);
  uint8_t reply[16] = {};
  system->sio().set_guncon(0, true, false, false, 0.5f, 0.5f);
  CheckEqual(pad.Command(0, 0x42, nullptr, 0, reply, sizeof(reply)), 8, "and answers as one");
  CheckEqual(reply[3], 0xDF, "with the trigger it is told about now");
  system->sio().set_controller_type(0, Sio::kDualShock);
  FreshPad(system, 0);
}

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
  TestCapabilityQueries(system);
  TestAnalogButton(system);
  TestMultitapPlayerTypes(system);
  TestMultitapMemoryCards(system);
  TestDualShockRefusesConfigCommandsOutsideConfigMode(system);
  TestGunConReply(system);
  TestGunConSurvivesSaveState(system);

  TestSio1ResetState(system);
  TestSio1EmptyReceiveFifoReadsOnes(system);
  TestSio1RegistersRoundTrip(system);
  TestSio1StatusIsReadOnly(system);
  TestSio1TransmitNeedsTheTransmitterEnabled(system);
  TestSio1ResetStrobeRestoresEverything(system);
  TestSio1BaudTimerCountsDown(system);
  TestSio1ConsoleRedirect(system);
  TestSio1SurvivesSaveState(system);

  printf("\n%d checks, %d failures\n", g_checks, g_failures);
  delete system;
  return g_failures == 0 ? 0 : 1;
}
