/*****************************************************************************************************************
* Copyright (c) 2012 Khalid Ali Al-Kooheji                                                                       *
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

namespace emulation {
namespace psx {

/*
  SIO0 - the controller and memory card port.

  A synchronous serial link: software writes a byte, the device on the other
  end shifts one back, and an interrupt says the exchange finished. Both slots
  are polled the same way, and a slot with nothing in it simply never
  acknowledges - which is how software knows it is empty.

  Each pad speaks the DualShock protocol: it boots as a plain digital pad
  (ID 5A41h), and a game that wants analog input drives it through the same
  handshake a real one goes through - 0x43 to enter configuration mode, 0x44
  to switch into analog (5A73h) and optionally lock the mode so the player
  cannot switch it back, 0x45 to ask what mode it is in, 0x4D to say which
  bytes of the poll reply the two vibration motors should be controlled by.
  None of this is optional plumbing bolted on afterwards - it is what a real
  pad does, and a game that never asks for analog input never sees anything
  different from the plain digital pad this always was.

  A port can also hold a mouse instead of a pad (ID 5A12h) - a different
  device entirely, with two buttons and no modes to negotiate, reporting
  relative movement rather than an absolute stick position - or nothing at
  all, which is the one case a real console cannot tell apart from a slot
  nobody has ever asked about: it never acknowledges either way. Which of
  these five is in a slot is chosen once, by the front end, the same way the
  three pad kinds already were - see ControllerType.
*/
class Sio : public Component {
 public:
  // Buttons, in the order the digital pad reports them. Active low on the
  // wire; this interface takes them the right way round and inverts on the way
  // out. kL3/kR3 - the stick clicks - only mean anything once a pad is in
  // analog mode, but they are reported the same way regardless.
  enum Button {
    kSelect   = 1 << 0,
    kL3       = 1 << 1,
    kR3       = 1 << 2,
    kStart    = 1 << 3,
    kUp       = 1 << 4,
    kRight    = 1 << 5,
    kDown     = 1 << 6,
    kLeft     = 1 << 7,
    kL2       = 1 << 8,
    kR2       = 1 << 9,
    kL1       = 1 << 10,
    kR1       = 1 << 11,
    kTriangle = 1 << 12,
    kCircle   = 1 << 13,
    kCross    = 1 << 14,
    kSquare   = 1 << 15,
  };

  // Which motor a byte of the poll reply has been told to drive, once a game
  // has configured that with command 0x4D. Matches the values real DualShock
  // software writes to ask for the mapping: 0 for the small on/off motor, 1
  // for the large variable-speed one.
  enum Motor { kSmallMotor = 0, kLargeMotor = 1 };

  // Which of five things is plugged into a port, chosen by the front end
  // rather than negotiated - a game cannot ask a port to be a different
  // physical device, only for the pad that is there to change mode.
  //   kDigital    - the original pad (SCPH-1080): never leaves digital mode,
  //                 ID 5A41h forever. Command 0x43 (enter configuration
  //                 mode) is meaningless to it and does nothing, which is
  //                 what stops 0x44/0x45/... from ever being reachable too.
  //   kDualAnalog - the pre-DualShock Analog Joystick (SCPH-1110): goes
  //                 analog exactly like a DualShock (5A41h/5A73h through the
  //                 same 0x43/0x44/0x45 handshake) but has no vibration
  //                 motors at all, so nothing it is ever told to do to them
  //                 has any effect.
  //   kDualShock  - today's full behaviour: analog plus both rumble motors.
  //   kMouse      - the SCPH-1030 mouse (ID 5A12h): two buttons, no modes to
  //                 negotiate, relative movement instead of an absolute
  //                 stick - see struct Mouse and ExchangeMouse.
  //   kNone       - no device at all. The same as unplugging a pad
  //                 (set_connected(slot, false)), just made a standing
  //                 choice instead of something the front end has to
  //                 remember to keep asserting every frame - see
  //                 set_connected and set_controller_type.
  enum ControllerType { kDigital, kDualAnalog, kDualShock, kMouse, kNone };

  Sio();
  ~Sio();

  int Initialize();
  int Deinitialize();
  void Tick(uint32_t cycles);

  uint8_t Read08(uint32_t address);
  uint16_t Read16(uint32_t address);
  uint32_t Read32(uint32_t address);
  void Write08(uint32_t address, uint8_t data);
  void Write16(uint32_t address, uint16_t data);
  void Write32(uint32_t address, uint32_t data);

  // Set by a front end. Bit set means pressed.
  void set_buttons(int slot, uint16_t buttons) {
    if (slot >= 0 && slot < 2) pad_[slot].buttons = buttons;
  }

  // The analog sticks, in the pad's own byte convention: 0x00 is left/up,
  // 0xFF is right/down, 0x80 is centred. Harmless to set even for a pad that
  // never goes into analog mode - the bytes simply never get sent.
  void set_axes(int slot, uint8_t left_x, uint8_t left_y, uint8_t right_x,
               uint8_t right_y) {
    if (slot < 0 || slot >= 2) return;
    pad_[slot].left_x = left_x;
    pad_[slot].left_y = left_y;
    pad_[slot].right_x = right_x;
    pad_[slot].right_y = right_y;
  }

  // A mouse's own two buttons - what the wire calls "switches" rather than
  // buttons, but the same active-low convention every button on this bus
  // already uses. Meaningless, and harmless, on a slot not currently holding
  // a mouse; see controller_type.
  void set_mouse_buttons(int slot, bool left, bool right) {
    if (slot < 0 || slot >= 2) return;
    mouse_[slot].left = left;
    mouse_[slot].right = right;
  }

  // Adds to the movement a mouse has not yet been asked for - see
  // Mouse::accum_dx/accum_dy for why this accumulates rather than replaces.
  // `dx`/`dy` are screen-space, right/down positive, which is also
  // psx-spx's own convention for this device, so neither axis is inverted
  // here the way a gamepad's Y axis has to be.
  void add_mouse_motion(int slot, int32_t dx, int32_t dy) {
    if (slot < 0 || slot >= 2) return;
    mouse_[slot].accum_dx += dx;
    mouse_[slot].accum_dy += dy;
  }

  // A pad that is freshly connected forgets whatever a previous one had
  // negotiated - defined out of line because that is more than a field
  // assignment now.
  void set_connected(int slot, bool connected);

  // Which controller is plugged into a port. Independent of Pad's negotiated
  // state and of set_connected's reset - unlike buttons or analog mode, this
  // is the front end's own standing choice, and a reconnect must not
  // silently forget it. Changing it, though, *is* treated as unplugging one
  // physical controller for a different one: see the .cpp for why.
  void set_controller_type(int slot, ControllerType type);
  ControllerType controller_type(int slot) const {
    return (slot >= 0 && slot < 2) ? controller_type_[slot] : kDualShock;
  }

  // What the two motors are currently being asked to do: 0 or 255 for the
  // small one, 0-255 for the large one. A front end reads this once a frame
  // and feeds it to whatever actually vibrates.
  void motor_state(int slot, uint8_t* small, uint8_t* large) const {
    if (slot < 0 || slot >= 2) {
      if (small != nullptr) *small = 0;
      if (large != nullptr) *large = 0;
      return;
    }
    if (small != nullptr) *small = pad_[slot].motor_small;
    if (large != nullptr) *large = pad_[slot].motor_large;
  }

  void Serialise(StateIO& io);

 private:
  struct Pad {
    bool connected = false;
    uint16_t buttons = 0;

    // 0x80 is centred, matching a stick at rest - not 0, which the byte
    // convention below reads as "pushed fully left and up".
    uint8_t left_x = 0x80, left_y = 0x80, right_x = 0x80, right_y = 0x80;

    // What the pad has been told to be, by the game rather than the player -
    // there is no physical ANALOG button here to toggle it any other way.
    bool analog_mode = false;
    // On real hardware this stops the player overriding the mode with the
    // pad's own ANALOG button. There is no such button emulated here, so
    // this is tracked - a game can read it back - but nothing yet refuses a
    // later 0x44 because of it, which matches real hardware anyway: locking
    // only ever blocked the button, never another command from the game.
    bool analog_locked = false;
    bool config_mode = false;
    // Set the first time this pad enters configuration mode, and never
    // cleared until it is disconnected. Before that, command 0x42 still
    // honours the older two-byte on/off rumble scheme every original pad
    // answered to; after it, only the bytes command 0x4D has actually mapped
    // drive a motor.
    bool dualshock_enabled = false;

    // Which motor (if any) each of the five configurable poll-reply bytes
    // drives, set by command 0x4D. 0xFF - not kSmallMotor or kLargeMotor -
    // means the byte controls nothing, which is the state every byte starts
    // in until a game asks for something else.
    uint8_t rumble_map[5] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

    uint8_t motor_small = 0;
    uint8_t motor_large = 0;
  };

  // A mouse's own protocol state. Deliberately not folded into Pad: a mouse
  // shares nothing with the DualShock lineage beyond both living on this
  // bus - no modes, no rumble, and its two motion fields are a relative
  // delta rather than an absolute stick position.
  struct Mouse {
    bool connected = false;
    // Active low on the wire, same as every pad button - see
    // MouseSwitchesByte for the inversion.
    bool left = false;
    bool right = false;

    // Movement not yet sent to the game, in the pad's signed byte range
    // (psx-spx: "-80h..+7Fh"). Wider than a byte so it can accumulate
    // rather than replace: the front end adds to this every emulated frame
    // (add_mouse_motion), and a poll only ever drains as much of it as one
    // signed byte can carry (see ExchangeMouse/DrainMouseAxis), leaving the
    // rest for the next one. A game polling once a frame, same as this
    // bus's buttons already are, never notices the difference; one polling
    // less often than that does not lose movement to it either.
    int32_t accum_dx = 0;
    int32_t accum_dy = 0;
  };

  // Which device the current exchange is talking to, and how far in it is.
  enum Target { kTargetNone, kTargetPad, kTargetMemoryCard, kTargetMouse };

  Pad pad_[2];
  Mouse mouse_[2];
  ControllerType controller_type_[2] = { kDualShock, kDualShock };

  uint16_t control_;
  uint16_t mode_;
  uint16_t baud_;
  uint16_t status_;

  Target target_;
  int transfer_step_;
  uint8_t receive_;
  bool receive_full_;
  bool acknowledge_;
  int32_t interrupt_timer_;
  bool interrupt_pending_;

  // /ACK is a physical line the device pulls low for a short pulse, not a
  // flag software can clear - it releases back to high on its own. This is
  // the countdown to that release, armed whenever kStatusAcknowledge is set.
  int32_t ack_pulse_timer_;

  int selected_slot() const { return (control_ & 0x2000) ? 1 : 0; }

  uint8_t Exchange(uint8_t data);
  uint8_t ExchangeMemoryCard(uint8_t data, class MC& mc);

  // The controller side of Exchange(). Split out because it is a real state
  // machine in its own right now, not the four-byte reply it used to be.
  uint8_t ExchangeController(uint8_t data, int slot);
  uint8_t PadIdByte(const Pad& pad) const;
  uint8_t PollPayloadByte(Pad& pad, int payload_index, uint8_t incoming,
                          bool rumble_capable);

  // The mouse side of Exchange() - its own state machine because a mouse's
  // reply shares no structure with a pad's: one fixed length, no command
  // byte ever changes it, see ExchangeMouse.
  uint8_t ExchangeMouse(uint8_t data, int slot);
  uint8_t MouseSwitchesByte(const Mouse& mouse) const;
  // Sends as much of an accumulated motion value as one signed byte can
  // carry and keeps whatever does not fit for the next poll, rather than
  // clipping it away - see Mouse::accum_dx/accum_dy. Static: it has nothing
  // to do with any other Sio state, only the accumulator it is handed.
  static uint8_t DrainMouseAxis(int32_t& accumulator);

  // Which controller command (0x42, 0x43, ...) the current exchange is
  // carrying out - decided by the byte the host sends right after selecting
  // the device, and needed for every byte after that.
  uint8_t pad_command_;

  // Scratch for the pre-DualShock two-byte rumble scheme, which needs both
  // of its bytes at once to decide anything and only gets them one at a
  // time. Shared rather than per-pad because only one pad is ever mid-
  // exchange at a time - the bus has one selected device.
  uint8_t legacy_rumble_byte2_;

  uint8_t mc_command_;
  uint16_t mc_sector_;
  uint8_t mc_checksum_;
  uint8_t mc_buffer_[128];
  uint8_t mc_previous_tx_;
};

}
}
