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
#include "psx/psx.h"

#include <cstring>

namespace emulation {
namespace psx {

namespace {

// How long after a byte is exchanged the acknowledge interrupt arrives.
const int32_t kAcknowledgeCycles = 500;

// How long the device holds /ACK low before releasing it back to high on its
// own - psx-spx: "the LOW duration is circa 100 clock cycles" (also given as
// "at least 2 us", and matching the Sony Mouse note that a normal pad or
// memory card "set /ACK=LOW only for around 100 clk cycles"). Real hardware
// never lets software clear this level directly - it just has to wait.
const int32_t kAckPulseCycles = 100;

// Status register bits.
const uint16_t kStatusTxReady      = 0x0001;
const uint16_t kStatusRxNotEmpty   = 0x0002;
const uint16_t kStatusTxDone       = 0x0004;
const uint16_t kStatusAcknowledge  = 0x0080;
const uint16_t kStatusInterrupt    = 0x0200;

// Whether a pad acknowledges a command byte at all - see ExchangeController.
// A plain digital pad (SCPH-1080) understands a poll and nothing else; this
// is DuckStation's and Mednafen's digital pad too. The DualShock line
// understands a poll and 0x43 (enter or leave configuration mode) always, and
// the rest of the configuration set only while configuration mode is on.
bool PadUnderstands(Sio::ControllerType type, bool config_mode,
                    uint8_t command) {
  if (command == 0x42)
    return true;
  if (type == Sio::kDigital)
    return false;
  if (command == 0x43)
    return true;
  const bool config_command = command == 0x44 || command == 0x45 ||
                              command == 0x46 || command == 0x47 ||
                              command == 0x4C || command == 0x4D;
  return config_command && config_mode;
}

}  // namespace

Sio::Sio() {
}

Sio::~Sio() {
}

int Sio::Initialize() {
  // A fresh Pad gives every field its at-rest default - centred sticks,
  // digital mode, an unmapped rumble table - which a plain memset would
  // not, since "every byte zero" is the wrong default for a byte
  // convention where 0x80 means centred and 0xFF means unmapped.
  pad_[0] = std::make_unique<Pad>();
  pad_[1] = std::make_unique<Pad>();
  mouse_[0] = Mouse();
  mouse_[1] = Mouse();
  // Slot 1 has a digital pad in it, slot 2 is empty. A front end overrides
  // this as soon as it knows better.
  pad_[0]->connected = true;
  pad_[1]->connected = false;

  // The front end re-asserts its configured choice every frame - the same
  // way it already does for `connected` - so resetting to the power-on
  // default here rather than trying to preserve whatever was set before is
  // enough; see PSXEmu.Win32/main.cpp's per-frame input block.
  controller_type_[0] = kDualShock;
  controller_type_[1] = kDualShock;

  control_ = 0;
  mode_ = 0;
  baud_ = 0;
  status_ = kStatusTxReady | kStatusTxDone;

  target_ = kTargetNone;
  transfer_step_ = 0;
  receive_ = 0xFF;
  receive_full_ = false;
  acknowledge_ = false;
  interrupt_timer_ = 0;
  interrupt_pending_ = false;
  ack_pulse_timer_ = 0;
  pad_command_ = 0;
  exchange_scratch_ = 0;
  return S_OK;
}

int Sio::Deinitialize() {
  return S_OK;
}

// A Pad with a vtable is no longer trivially copyable, so it can no longer
// ride along as one memset-shaped blob the way io.Plain(pad_) used to
// serialise it - each field is written out explicitly instead, in a fixed
// order both directions agree on.
void Sio::Pad::Serialise(StateIO& io) {
  io.Plain(connected);
  io.Plain(buttons);
  io.Plain(left_x);
  io.Plain(left_y);
  io.Plain(right_x);
  io.Plain(right_y);
  io.Plain(analog_mode);
  io.Plain(analog_locked);
  io.Plain(config_mode);
  io.Plain(dualshock_enabled);
  io.Plain(rumble_map);
  io.Plain(motor_small);
  io.Plain(motor_large);
}

void Sio::Multitap::Serialise(StateIO& io) {
  // The inherited Pad fields are never actually used by a Multitap itself
  // (see the class comment - the interesting state is players[]), but
  // round-tripping them anyway keeps every concrete Pad type serialising
  // itself the same way, rather than Multitap being a special case that
  // skips its own base.
  Pad::Serialise(io);
  for (Pad& player : players)
    player.Serialise(io);
  io.Plain(pending_long_response);
  io.Plain(invalid_long_response);
  io.Plain(selected_player);
}

void Sio::Serialise(StateIO& io) {
  // Read (or write) before pad_ itself, on purpose: which concrete type
  // each port's Pad actually is has to be known before Serialise can run
  // on it at all - a Multitap and a plain Pad write different amounts of
  // data, so on load the right one has to already be sitting there first.
  io.Plain(controller_type_);
  if (!io.saving()) {
    for (int port = 0; port < 2; ++port) {
      if (controller_type_[port] == kMultitap)
        pad_[port] = std::make_unique<Multitap>();
      else
        pad_[port] = std::make_unique<Pad>();
    }
  }
  pad_[0]->Serialise(io);
  pad_[1]->Serialise(io);
  io.Plain(mouse_);
  io.Plain(control_);
  io.Plain(mode_);
  io.Plain(baud_);
  io.Plain(status_);
  io.Plain(target_);
  io.Plain(transfer_step_);
  io.Plain(receive_);
  io.Plain(receive_full_);
  io.Plain(acknowledge_);
  io.Plain(interrupt_timer_);
  io.Plain(interrupt_pending_);
  io.Plain(ack_pulse_timer_);
  io.Plain(pad_command_);
  io.Plain(exchange_scratch_);
  io.Plain(mc_command_);
  io.Plain(mc_sector_);
  io.Plain(mc_checksum_);
  io.Plain(mc_buffer_);
  io.Plain(mc_previous_tx_);
}

void Sio::set_connected(int port, bool connected, int player) {
  if (port < 0 || port >= 2)
    return;
  // kNone is a standing choice, not something the front end has to
  // remember to keep unplugging every frame - see the class comment on
  // ControllerType - so it overrides whatever a caller still asks for here.
  if (controller_type_[port] == kNone)
    connected = false;
  if (controller_type_[port] == kMouse) {
    mouse_[port].connected = connected;
    return;
  }
  Pad* pad = ResolvePad(port, player);
  if (pad == nullptr)
    return;
  if (connected && !pad->connected) {
    // A freshly connected pad has negotiated nothing yet - a real DualShock
    // that has just been plugged in does not remember being in analog mode
    // on some other console, and neither should this one. What it was doing
    // before this moment (buttons, axes) does not matter and is overwritten
    // by the next poll regardless. Reset in place, not by replacing the
    // object - `pad` may be one of a Multitap's four players, not
    // something this call owns to replace wholesale.
    *pad = Pad();
    pad->connected = true;
  }
  pad->connected = connected;
}

void Sio::set_controller_type(int port, ControllerType type) {
  if (port < 0 || port >= 2 || controller_type_[port] == type)
    return;
  // Changing the physical controller is a fresh connection as far as the
  // protocol is concerned - a real console cannot tell a DualShock swapped
  // for a plain digital pad from an unplug/replug, and neither should this
  // one: whatever the old one had negotiated (analog mode, rumble mapping)
  // must not carry over to a controller of a different kind. old_type is
  // read before either slot is touched, since it says which of Pad/Mouse
  // actually held the connection being carried forward - a Multitap's own
  // `connected` (forced true below, the moment one exists) falls out of
  // the same pad_[port]->connected read as an ordinary pad's.
  const ControllerType old_type = controller_type_[port];
  const bool was_connected = (old_type == kMouse) ? mouse_[port].connected
                            : (old_type == kNone)  ? false
                                                    : pad_[port]->connected;
  controller_type_[port] = type;
  mouse_[port] = Mouse();
  if (type == kMultitap) {
    // The adaptor itself has no "unplugged" state of its own once chosen -
    // the interesting connected-ness lives per player (players[i].connected),
    // not here.
    pad_[port] = std::make_unique<Multitap>();
    pad_[port]->connected = true;
  } else {
    pad_[port] = std::make_unique<Pad>();
    if (type == kMouse)
      mouse_[port].connected = was_connected;
    else if (type != kNone)
      pad_[port]->connected = was_connected;
  }
}

void Sio::Tick(uint32_t cycles) {
  if (ack_pulse_timer_ > 0) {
    ack_pulse_timer_ -= static_cast<int32_t>(cycles);
    if (ack_pulse_timer_ <= 0) {
      ack_pulse_timer_ = 0;
      status_ &= ~kStatusAcknowledge;
    }
  }

  if (!interrupt_pending_)
    return;

  interrupt_timer_ -= static_cast<int32_t>(cycles);
  if (interrupt_timer_ > 0)
    return;

  interrupt_pending_ = false;
  if (acknowledge_) {
    status_ |= kStatusInterrupt;
    system().io().SetInterrupt(kInterruptSIO0);
  }
}

// One byte in, one byte out. The device decides whether to acknowledge, and a
// device that is not there never does - which ends the exchange and is how
// software discovers an empty slot.
uint8_t Sio::Exchange(uint8_t data) {
  const int port = selected_slot();

  if (transfer_step_ == 0) {
    // First byte selects the device: 0x01 is a controller - whichever kind
    // is actually plugged into this slot, see controller_type_ - 0x81 a
    // memory card. A slot set to kNone never has anything answer 0x01,
    // exactly like an unplugged pad; that is already true of pad->connected
    // by the time this runs (set_controller_type/set_connected enforce it),
    // but the type is checked directly too so a mouse-holding slot answers
    // as a mouse rather than whatever pad->connected happens to say.
    //
    // 0x02-0x04 only mean anything when a Multitap is actually plugged in -
    // psx-spx: they select its Players B/C/D the same way 0x01 selects A -
    // so an ordinary pad's port behaves byte-for-byte as it always has.
    target_ = kTargetNone;
    if (data == 0x01) {
      if (controller_type_[port] == kMouse) {
        if (mouse_[port].connected)
          target_ = kTargetMouse;
      } else if (pad_[port]->connected) {
        if (pad_[port]->is_multitap()) {
          target_ = kTargetMultitap;
          static_cast<Multitap&>(*pad_[port]).selected_player = 0;
        } else {
          target_ = kTargetPad;
        }
      }
    } else if (data >= 0x02 && data <= 0x04 && pad_[port]->is_multitap() &&
               pad_[port]->connected) {
      target_ = kTargetMultitap;
      static_cast<Multitap&>(*pad_[port]).selected_player = data - 0x01;
    } else if (data == 0x81 && system().mc(port).connected()) {
      target_ = kTargetMemoryCard;
    }

    acknowledge_ = (target_ != kTargetNone);
    ++transfer_step_;
    return 0xFF;
  }

  if (target_ == kTargetMemoryCard) {
    return ExchangeMemoryCard(data, system().mc(port));
  }

  if (target_ == kTargetMouse) {
    return ExchangeMouse(data, port);
  }

  if (target_ == kTargetMultitap || target_ == kTargetMultitapAll) {
    return ExchangeMultitap(data, port);
  }

  if (target_ != kTargetPad) {
    acknowledge_ = false;
    return 0xFF;
  }

  return ExchangeController(data, *pad_[port], controller_type_[port]);
}

// The ID a pad's reply starts with. It depends only on what the pad
// currently is, never on what is being asked of it - on real hardware the
// pad has already committed to this byte before it has seen enough of the
// command to know what it is.
//
// The high nibble says which of the three shapes a reply is: 4 for a plain
// digital pad, 7 for one in analog mode, F for one that is inside
// configuration mode (which stays true regardless of analog/digital, since
// entering configuration mode is itself a DualShock-only thing to be able to
// do at all). The low nibble is fixed at 1 or 3 by the same analog/digital
// split for a normal-mode reply, and doubles as how long the reply is: one
// halfword of data beyond the ID and status for a digital pad, three for an
// analog one. Configuration mode is not a third point on that same split,
// though - psx-spx is explicit that "while in config mode, the ID bytes are
// always F3h 5Ah" regardless of what the pad's analog/digital state
// underneath it is, so the low nibble there is fixed at 3, not derived.
uint8_t Sio::PadIdByte(const Pad& pad) const {
  if (pad.config_mode)
    return 0xF3;
  const uint8_t high = pad.analog_mode ? 0x7 : 0x4;
  const uint8_t low = pad.analog_mode ? 0x3 : 0x1;
  return static_cast<uint8_t>((high << 4) | low);
}

// One byte of a command-0x42 poll reply, at `payload_index` counting from
// the first byte after the ID and status - 0 and 1 are the button bytes,
// which exist regardless of mode, and 2 through 5 are the four analog axes,
// which only exist - and are only ever asked for - once the pad is in
// analog mode.
//
// The same bytes are simultaneously carrying whatever the host is asking the
// motors to do, exactly as every other exchange on this bus is full duplex:
// once a game has mapped them with command 0x4D, `incoming` at a mapped
// position becomes that motor's new speed. Before any game has ever done
// that, the pad falls back to the pattern every original one answered to -
// a fixed two-byte code that only ever turns the small motor fully on or
// fully off.
uint8_t Sio::PollPayloadByte(Pad& pad, int payload_index, uint8_t incoming,
                             bool rumble_capable) {
  uint8_t out = 0x00;
  const uint16_t buttons = static_cast<uint16_t>(~pad.buttons);
  switch (payload_index) {
    case 0: out = static_cast<uint8_t>(buttons); break;
    case 1: out = static_cast<uint8_t>(buttons >> 8); break;
    case 2: out = pad.right_x; break;
    case 3: out = pad.right_y; break;
    case 4: out = pad.left_x; break;
    case 5: out = pad.left_y; break;
    default: break;
  }

  // A Dual Analog controller (kDualAnalog) reaches this too - it has the
  // same config/analog handshake as a DualShock - but it predates the
  // DualShock's motors entirely, so neither rumble scheme below ever does
  // anything on one.
  if (!rumble_capable)
    return out;

  if (pad.dualshock_enabled) {
    if (payload_index >= 0 && payload_index < 5) {
      const uint8_t motor = pad.rumble_map[payload_index];
      if (motor == kSmallMotor)
        pad.motor_small = incoming;
      else if (motor == kLargeMotor)
        pad.motor_large = incoming;
    }
  } else if (payload_index == 0) {
    exchange_scratch_ = incoming;
  } else if (payload_index == 1) {
    const bool on =
        (exchange_scratch_ & 0xC0) == 0x40 && (incoming & 0x01) != 0;
    pad.motor_small = on ? 255 : 0;
  }
  return out;
}

// The controller side of an exchange, from the second byte on - the first
// was already consumed by Exchange() (or, for a Multitap's Method 2,
// ExchangeMultitap) to pick the device. Takes the Pad and its type
// directly rather than a port to look them up from, since a call from
// ExchangeMultitap is not talking to pad_[port] at all - it is talking to
// one of that port's Multitap's four players.
uint8_t Sio::ExchangeController(uint8_t data, Pad& pad, ControllerType type) {
  const int step = transfer_step_;

  // Step 1 is the command byte itself (0x42 to poll, 0x43 to enter or leave
  // configuration mode, and so on), remembered for the rest of the exchange.
  // The ID goes out while that byte is still coming in, so it is the pad's
  // current ID whatever the command turns out to be - the pad does not know
  // yet either. What it does know once the byte has arrived is whether it
  // understands it, and a command it does not understand gets no /ACK: the
  // transfer ends right here, the way an empty slot's ends one byte earlier.
  //
  // Not a reply-shaped run of zeros, which is what this used to send.
  // Buttons are active low, so a zero payload is every button held at once,
  // and a driver that takes its button state from whatever reply comes back
  // - Bomberman Party Edition's, which keeps sending 0x43 and 0x45 to a
  // digital pad because it never enters configuration mode - saw the whole
  // pad pressed on two frames out of three.
  if (step == 1) {
    pad_command_ = data;
    acknowledge_ = PadUnderstands(type, pad.config_mode, data);
    ++transfer_step_;
    return PadIdByte(pad);
  }

  // Step 2 is the status byte, which is always this one value everywhere
  // else on this bus already uses for the same purpose.
  if (step == 2) {
    acknowledge_ = true;
    ++transfer_step_;
    return 0x5A;
  }

  // A poll's length follows the pad's mode, since that is genuinely how much
  // there is to say, and so does 0x43's when it arrives in normal mode -
  // psx-spx: there the reply to 43h is the same joypad data 42h returns.
  // Every other command is a fixed eight bytes, deliberately not recomputed
  // from the mode again after this point - 0x44 can change analog_mode
  // partway through its own exchange, and a length that could change under
  // it mid-transaction is not a length a real host could keep up with. It is
  // also why 0x43 does not switch configuration mode until its last byte.
  //
  // Configuration mode forces the long shape on 0x42 too, regardless of
  // analog_mode - psx-spx: "Config Mode - Command 42h ... Same as command
  // 42h in normal mode, but with forced analog response (ie. analog inputs
  // ... are returned even in Digital Mode with LED=Off)". A driver that
  // stays in config mode between reads (psx-spx's own documented way to
  // dodge the config-mode watchdog reset) and gets the short four-byte
  // reply instead sees a transfer that ended early, not a normal poll.
  const bool poll_shaped =
      pad_command_ == 0x42 || (pad_command_ == 0x43 && !pad.config_mode);
  const int total_length =
      poll_shaped ? ((pad.analog_mode || pad.config_mode) ? 8 : 4) : 8;

  if (step > total_length) {
    acknowledge_ = false;
    return 0xFF;
  }

  const int payload_index = step - 3;
  uint8_t out = 0x00;

  // Only commands this pad acknowledged at step 1 get this far.
  switch (pad_command_) {
    case 0x42:
      out = PollPayloadByte(pad, payload_index, data, type == kDualShock);
      break;

    case 0x43:
      // Outside configuration mode the reply is the pad's own buttons (and
      // sticks, in analog mode) - read only, so none of what the host sends
      // alongside them drives a motor the way a poll's bytes can. Inside it
      // the reply is zeros.
      if (!pad.config_mode)
        out = PollPayloadByte(pad, payload_index, data,
                              /*rumble_capable=*/false);

      // The only byte that matters is the first: 1 to enter, anything else
      // to leave. It is held until the last byte and applied there - the
      // mode decides this very transfer's length, and DuckStation applies it
      // at the same point. Entering marks the pad as a DualShock for good -
      // real hardware does not forget that just because the game later takes
      // it back out of configuration mode.
      //
      // A plain digital pad (kDigital) never gets here: it does not
      // understand this command - a real one never had a configuration mode
      // to enter - and that alone is what keeps its ID at 5A41h for ever:
      // config_mode and analog_mode can only ever be set from inside this
      // switch.
      if (payload_index == 0)
        exchange_scratch_ = data;
      if (step == total_length) {
        pad.config_mode = (exchange_scratch_ == 1);
        if (pad.config_mode)
          pad.dualshock_enabled = true;
      }
      break;

    case 0x44:
      // Byte 0 is the mode to switch to, byte 1 whether to lock it there.
      // Values outside the two each byte actually uses are left alone
      // rather than guessed at.
      if (payload_index == 0 && (data == 0x00 || data == 0x01))
        pad.analog_mode = (data == 0x01);
      else if (payload_index == 1 && (data == 0x02 || data == 0x03))
        pad.analog_locked = (data == 0x03);
      break;

    case 0x45:
      // A fixed status block bar one byte: whether the pad is currently in
      // analog mode.
      if (payload_index == 0) out = 0x01;
      else if (payload_index == 1) out = 0x02;
      else if (payload_index == 2) out = pad.analog_mode ? 0x01 : 0x00;
      else if (payload_index == 3) out = 0x02;
      else if (payload_index == 4) out = 0x01;
      break;

    case 0x46:
    case 0x47:
      // Capability queries close to nothing exercises. Acknowledged with
      // the right shape so a game that tries them does not stall waiting
      // for a reply that never comes; the exact bytes have not been
      // checked against real hardware and default to zero rather than a
      // guess.
      out = 0x00;
      break;

    case 0x4C:
      // Which kind of DualShock this is - 0x04 here, since pressure-
      // sensitive buttons (which would make it 0x07, a DualShock 2) are
      // not implemented.
      out = (payload_index == 3) ? 0x04 : 0x00;
      break;

    case 0x4D:
      // Read-modify-write: the reply carries the mapping this byte held
      // before, and what the host sends becomes the new one, in the same
      // exchange - the same as every other byte on this bus. The last
      // byte is not part of the mapping; it is where a motor nothing maps
      // to any more gets switched off rather than left running.
      if (payload_index < 5) {
        out = pad.rumble_map[payload_index];
        pad.rumble_map[payload_index] = data;
      } else if (payload_index == 5) {
        bool has_small = false;
        bool has_large = false;
        for (uint8_t motor : pad.rumble_map) {
          has_small = has_small || (motor == kSmallMotor);
          has_large = has_large || (motor == kLargeMotor);
        }
        if (!has_small)
          pad.motor_small = 0;
        if (!has_large)
          pad.motor_large = 0;
      }
      break;

    default:
      break;
  }

  ++transfer_step_;
  acknowledge_ = (transfer_step_ <= total_length);
  return out;
}

// The multitap side of an exchange, from the second byte on - the first
// was already consumed by Exchange() to pick the device and, via
// Multitap::selected_player, which of its four players 0x01-0x04 asked
// for (see the class comment there for why it is always reset to A the
// moment a transfer becomes the long response, regardless of what picked
// it). Two methods, both from psx-spx:
//
// Method 2 ("normal reads") is everything below once the id+status pair
// is done: a pure passthrough to ExchangeController for whichever player
// was selected - identical to an ordinary single pad, just pointed at one
// of these four instead of pad_[port] directly. Every multitap player
// behaves as a full DualShock for now (see the class comment on
// Multitap) - hence kDualShock passed to ExchangeController everywhere
// below rather than a per-player type that does not exist yet.
//
// Method 1 ("read all four", psx-spx: "the more commonly used one") is
// triggered by bit 0 of the third byte of any transfer - psx-spx: setting
// it "does NOT affect the current response. Instead, it does request
// that the NEXT command shall return special data" - so it is latched
// into Multitap::pending_long_response for the transfer after this one,
// regardless of whether this one is itself short or long. When a
// transfer that WAS so queued actually arrives, and its own command byte
// is 0x42 (psx-spx/DuckStation: anything else aborts the escalation), the
// id+status pair becomes 5A80h (the multitap's own id) instead of
// whatever the selected player would have answered, and everything after
// that is 4 players x 8 bytes (4 halfwords each), 0xFF-padded past
// whatever a shorter reply (a plain digital pad, say) actually has.
uint8_t Sio::ExchangeMultitap(uint8_t data, int port) {
  Multitap& tap = static_cast<Multitap&>(*pad_[port]);
  const int step = transfer_step_;

  if (step == 1) {
    if (tap.pending_long_response) {
      tap.invalid_long_response = (data != 0x42);
      target_ = kTargetMultitapAll;
      acknowledge_ = true;
      ++transfer_step_;
      return 0x80;   // ID low - 5A80h says "multitap".
    }
    return ExchangeController(data, tap.players[tap.selected_player], kDualShock);
  }

  if (step == 2) {
    // Whatever this transfer turns out to be, bit 0 of this byte always
    // latches what the NEXT one should be - independent of whether this
    // access is itself short or long, and read before anything below can
    // end the transfer early.
    const bool next_wants_long = (data & 0x01) != 0;

    if (target_ == kTargetMultitapAll) {
      tap.pending_long_response = next_wants_long;
      tap.selected_player = 0;   // the long response always starts at A
      acknowledge_ = !tap.invalid_long_response;
      ++transfer_step_;
      return 0x5A;   // ID high, same byte every device on this bus uses.
    }
    const uint8_t out =
        ExchangeController(data, tap.players[tap.selected_player], kDualShock);
    tap.pending_long_response = next_wants_long;
    return out;
  }

  if (target_ == kTargetMultitapAll) {
    const int overall_index = step - 3;   // 0..31
    const int player = overall_index / 8;
    const int local_index = overall_index % 8;
    Pad& p = tap.players[player];
    uint8_t out = 0xFF;
    if (p.connected) {
      if (local_index == 0) {
        out = PadIdByte(p);
      } else if (local_index == 1) {
        out = 0x5A;
      } else {
        // Payload bytes only run as long as this player's own poll reply
        // actually would - psx-spx: "padded with FFFFh values for devices
        // like Digital Joypads... which do use less than 4 halfwords".
        const int payload_index = local_index - 2;
        const int payload_length = (p.analog_mode || p.config_mode) ? 6 : 2;
        if (payload_index < payload_length)
          out = PollPayloadByte(p, payload_index, data, /*rumble_capable=*/true);
      }
    }
    ++transfer_step_;
    acknowledge_ = (overall_index + 1 < 32);
    return out;
  }

  // Method 2, continuing: still a pure passthrough to whichever player was
  // selected.
  return ExchangeController(data, tap.players[tap.selected_player], kDualShock);
}

// The byte psx-spx calls the mouse's "switches": bits 8-9 of the halfword
// this belongs to are always 0, bit 10 is the right button and bit 11 the
// left, both active low, and the rest are always 1 - 0xFC is that byte at
// rest, with the two button bits cleared as they are held.
uint8_t Sio::MouseSwitchesByte(const Mouse& mouse) const {
  uint8_t out = 0xFC;
  if (mouse.left) out &= ~0x08;
  if (mouse.right) out &= ~0x04;
  return out;
}

uint8_t Sio::DrainMouseAxis(int32_t& accumulator) {
  int32_t sent = accumulator;
  if (sent > 127) sent = 127;
  if (sent < -128) sent = -128;
  accumulator -= sent;
  return static_cast<uint8_t>(static_cast<int8_t>(sent));
}

// The mouse side of an exchange, from the second byte on - the first was
// already consumed by Exchange() to pick the device. Unlike a pad, there is
// only one reply a mouse ever gives: it has no configuration mode, no
// analog mode and nothing else to negotiate, so `data` (the command byte
// and everything after it) is never inspected - every command the host
// sends here gets the same six-byte reply, psx-spx documenting no other
// command a mouse answers to. The shape, per psx-spx: ID low (5A12h's
// 0x12), ID high (0x5A), a fixed 0xFF filler byte, the switches, then the
// two motion bytes.
uint8_t Sio::ExchangeMouse(uint8_t data, int slot) {
  Mouse& mouse = mouse_[slot];
  const int step = transfer_step_;

  if (step == 1) {
    acknowledge_ = true;
    ++transfer_step_;
    return 0x12;   // ID low - 5A12h says "mouse" the way 5A41h says pad.
  }
  if (step == 2) {
    acknowledge_ = true;
    ++transfer_step_;
    return 0x5A;   // ID high, the same byte every device on this bus uses.
  }

  const int payload_index = step - 3;
  const int kTotalLength = 6;
  if (step > kTotalLength) {
    acknowledge_ = false;
    return 0xFF;
  }

  uint8_t out = 0x00;
  switch (payload_index) {
    case 0:
      // psx-spx: "bits 0-7 not used, all bits always 1" - the buttons are
      // the next byte, not this one.
      out = 0xFF;
      break;
    case 1:
      out = MouseSwitchesByte(mouse);
      break;
    case 2:
      out = DrainMouseAxis(mouse.accum_dx);
      break;
    case 3:
      out = DrainMouseAxis(mouse.accum_dy);
      break;
    default:
      break;
  }

  ++transfer_step_;
  acknowledge_ = (transfer_step_ <= kTotalLength);
  return out;
}

uint8_t Sio::Read08(uint32_t address) {
  if ((address & 0xF) == 0x0) {
    const uint8_t value = receive_;
    receive_ = 0xFF;
    receive_full_ = false;
    status_ &= ~kStatusRxNotEmpty;
    return value;
  }
  return static_cast<uint8_t>(Read16(address & ~1u));
}

uint16_t Sio::Read16(uint32_t address) {
  switch (address & 0xF) {
    case 0x0: {
      const uint8_t value = receive_;
      receive_ = 0xFF;
      receive_full_ = false;
      status_ &= ~kStatusRxNotEmpty;
      return value;
    }
    case 0x4: return status_;
    case 0x8: return mode_;
    case 0xA: return control_;
    case 0xE: return baud_;
    default:  return 0;
  }
}

uint32_t Sio::Read32(uint32_t address) {
  if ((address & 0xF) == 0x4)
    return status_;
  return Read16(address);
}

void Sio::Write08(uint32_t address, uint8_t data) {
  if ((address & 0xF) == 0x0) {
    receive_ = Exchange(data);
    receive_full_ = true;
    status_ |= kStatusRxNotEmpty | kStatusTxReady | kStatusTxDone;
    if (acknowledge_) {
      status_ |= kStatusAcknowledge;
      ack_pulse_timer_ = kAckPulseCycles;
      interrupt_pending_ = true;
      interrupt_timer_ = kAcknowledgeCycles;
    } else {
      status_ &= ~kStatusAcknowledge;
      ack_pulse_timer_ = 0;
      // Nothing answered, so the exchange is over and the next byte starts a
      // new one.
      transfer_step_ = 0;
      target_ = kTargetNone;
    }
    return;
  }
  Write16(address & ~1u, data);
}

void Sio::Write16(uint32_t address, uint16_t data) {
  switch (address & 0xF) {
    case 0x0:
      Write08(address, static_cast<uint8_t>(data));
      return;
    case 0x8:
      mode_ = data;
      return;
    case 0xA:
      control_ = data;
      if (data & 0x0040) {          // reset
        // This resets the SIO peripheral interface, not the controllers on
        // the other end of it - a real DualShock does not forget it is in
        // analog mode just because the console reset the port, and neither
        // should this one. Pad state is untouched here.
        status_ = kStatusTxReady | kStatusTxDone;
        control_ = 0;
        mode_ = 0;
        transfer_step_ = 0;
        target_ = kTargetNone;
        receive_ = 0xFF;
        receive_full_ = false;
        interrupt_pending_ = false;
        ack_pulse_timer_ = 0;
      }
      if (data & 0x0010) {          // acknowledge
        // Only the latched interrupt-request flag is software's to clear.
        // kStatusAcknowledge tracks the live /ACK line - it releases on its
        // own once the device's pulse ends (psx-spx: software "must first
        // wait until SIO0_STAT.7=0" before this write even takes effect on
        // bit 9, on real hardware).
        status_ &= ~kStatusInterrupt;
      }
      if ((data & 0x0002) == 0) {
        // Chip select dropped: the device is deselected and the next byte
        // starts a fresh exchange.
        transfer_step_ = 0;
        target_ = kTargetNone;
      }
      return;
    case 0xE:
      baud_ = data;
      return;
    default:
      return;
  }
}

void Sio::Write32(uint32_t address, uint32_t data) {
  Write16(address, static_cast<uint16_t>(data));
}

uint8_t Sio::ExchangeMemoryCard(uint8_t data, class MC& mc) {
  uint8_t result = 0xFF;
  acknowledge_ = true;

  if (transfer_step_ == 1) {
    mc_command_ = data;
    result = mc.flag();
  } else if (transfer_step_ == 2) {
    result = 0x5A;
  } else if (transfer_step_ == 3) {
    result = 0x5D;
  } else {
    // Process command specific states
    if (mc_command_ == 0x52) { // Read
      if (transfer_step_ == 4) { mc_sector_ = data << 8; result = 0x00; }
      else if (transfer_step_ == 5) { mc_sector_ |= data; result = mc_previous_tx_; mc.ReadSector(mc_sector_, mc_buffer_); }
      else if (transfer_step_ == 6) { result = 0x5C; }
      else if (transfer_step_ == 7) { result = 0x5D; }
      else if (transfer_step_ == 8) { result = (mc_sector_ >> 8) & 0xFF; }
      else if (transfer_step_ == 9) { result = mc_sector_ & 0xFF; mc_checksum_ = (mc_sector_ >> 8) ^ (mc_sector_ & 0xFF); }
      else if (transfer_step_ >= 10 && transfer_step_ <= 137) {
        int idx = transfer_step_ - 10;
        result = mc_buffer_[idx];
        mc_checksum_ ^= result;
      }
      else if (transfer_step_ == 138) { result = mc_checksum_; }
      else if (transfer_step_ == 139) { result = 0x47; acknowledge_ = false; }
    } else if (mc_command_ == 0x57) { // Write
      if (transfer_step_ == 4) { mc_sector_ = data << 8; result = 0x00; }
      else if (transfer_step_ == 5) { mc_sector_ |= data; result = mc_previous_tx_; mc_checksum_ = (mc_sector_ >> 8) ^ (mc_sector_ & 0xFF); }
      else if (transfer_step_ >= 6 && transfer_step_ <= 133) {
        int idx = transfer_step_ - 6;
        mc_buffer_[idx] = data;
        mc_checksum_ ^= data;
        result = mc_previous_tx_;
      }
      else if (transfer_step_ == 134) {
        result = mc_previous_tx_;
        // We write upon receiving checksum
        if (data == mc_checksum_) {
          mc.WriteSector(mc_sector_, mc_buffer_);
        }
      }
      else if (transfer_step_ == 135) { result = 0x5C; }
      else if (transfer_step_ == 136) { result = 0x5D; }
      else if (transfer_step_ == 137) { result = 0x47; acknowledge_ = false; }
    } else if (mc_command_ == 0x53) { // Get ID
      if (transfer_step_ == 4) { result = 0x5C; }
      else if (transfer_step_ == 5) { result = 0x5D; }
      else if (transfer_step_ == 6) { result = 0x04; }
      else if (transfer_step_ == 7) { result = 0x00; }
      else if (transfer_step_ == 8) { result = 0x00; }
      else if (transfer_step_ == 9) { result = 0x80; acknowledge_ = false; }
    } else {
      acknowledge_ = false;
    }
  }

  mc_previous_tx_ = data;
  ++transfer_step_;
  return result;
}

}
}
