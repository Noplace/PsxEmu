// The host's input devices, from the input thread to the machine's thread, and
// the pads' motors the other way.
//
// The input thread polls the pads, the keyboard and the mouse at a kilohertz
// and publishes what it read; the machine takes the latest reading once a
// frame, just before it runs one, and maps it onto the ports the way the
// settings say. Latest-wins for everything that is a level - a button held, a
// stick's position - because a level half a millisecond old is the right
// answer. Mouse motion is the exception: it is a quantity, and every count of
// it has to reach the game or the cursor drifts, so it accumulates here until
// the machine takes it.
//
// Nothing here knows what XInput or a keyboard is - the readings are already in
// the pad's own vocabulary (Sio::k* buttons, 0x00-0xFF axes centred at 0x80) -
// so the machine side is testable with no devices at all.
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>

namespace emulation {
namespace host {

// Raw rather than mapped: which pad button a control presses depends on the
// port it is playing, and one pad can play more than one - so the machine's
// thread maps it, through the bindings for that port (the front end's
// ApplyInput).
// What kind of pad fills a slot - which only changes what its controls are
// called, since every kind reports the same controls in the same bits.
enum class PadKind : uint8_t { kXInput, kDualShock4, kDualSense };

struct PadReading {
  bool connected = false;
  PadKind kind = PadKind::kXInput;
  uint32_t inputs = 0;    // the controls held: bit n is utilities::PadInput n+1
  uint8_t left_x = 0x80;
  uint8_t left_y = 0x80;
  uint8_t right_x = 0x80;
  uint8_t right_y = 0x80;
};

struct HostInput {
  // "Gamepad 1".."Gamepad 4": XInput's four slots, with any PlayStation pad
  // in whichever of them no XInput pad holds (the front end's SonyPads).
  static const int kPads = 4;

  // The keys held, one bit per virtual-key code (utilities::KeyHeld). Only
  // the keys some binding uses are read; the rest stay clear.
  uint32_t keys[8] = {};
  PadReading pads[kPads];
  bool mouse_left = false;
  bool mouse_right = false;
  bool mouse_middle = false;
  bool mouse_back = false;      // the side button nearer the wrist, XBUTTON1
  // Where the Windows cursor is over the picture, as fractions of its width
  // and height - (0,0) its top left corner, anything outside 0-1 off it, over
  // the letterbox bars or outside the window. For a light gun.
  float pointer_x = -1.0f;
  float pointer_y = -1.0f;
  int32_t mouse_dx = 0;         // motion not yet handed to the machine
  int32_t mouse_dy = 0;
  bool focused = false;         // whether the emulator's window has focus
};

class InputExchange {
 public:
  // The input thread. Everything replaces the last reading except mouse
  // motion, which adds to whatever the machine has not taken yet.
  void Publish(const HostInput& reading) {
    std::lock_guard<std::mutex> lock(mutex_);
    const int32_t dx = latest_.mouse_dx + reading.mouse_dx;
    const int32_t dy = latest_.mouse_dy + reading.mouse_dy;
    latest_ = reading;
    latest_.mouse_dx = dx;
    latest_.mouse_dy = dy;
    ++published_;
  }

  // The machine's thread. The latest reading, with the motion since the last
  // Take - which is then zeroed, so no count is ever delivered twice.
  HostInput Take() {
    std::lock_guard<std::mutex> lock(mutex_);
    HostInput reading = latest_;
    latest_.mouse_dx = 0;
    latest_.mouse_dy = 0;
    return reading;
  }

  // Any thread: the latest reading, leaving it as it is - for the bindings
  // window, which waits for a pad control to be pressed.
  HostInput Peek() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return latest_;
  }

  // How many readings the input thread has published - for "is it running".
  uint64_t published() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return published_;
  }

  // ---- Rumble: the machine sets, the input thread applies ----------------

  void SetRumble(int pad, uint8_t small_motor, uint8_t large_motor) {
    if (pad < 0 || pad >= HostInput::kPads)
      return;
    rumble_[pad].store(static_cast<uint16_t>((small_motor << 8) | large_motor),
                       std::memory_order_relaxed);
  }

  void GetRumble(int pad, uint8_t* small_motor, uint8_t* large_motor) const {
    const uint16_t both = (pad >= 0 && pad < HostInput::kPads)
                              ? rumble_[pad].load(std::memory_order_relaxed)
                              : 0;
    *small_motor = static_cast<uint8_t>(both >> 8);
    *large_motor = static_cast<uint8_t>(both & 0xFF);
  }

 private:
  mutable std::mutex mutex_;
  HostInput latest_;
  uint64_t published_ = 0;
  // Both motors of one pad in one atomic, so the input thread can never see a
  // new small motor with an old large one.
  std::atomic<uint16_t> rumble_[HostInput::kPads] = {};
};

}  // namespace host
}  // namespace emulation
