/*****************************************************************************************************************
* Copyright (c) 2014 Khalid Ali Al-Kooheji                                                                       *
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

// The DualShock 4's and DualSense's HID reports - reading what the pad sends, and writing the one
// that sets its motors. No I/O here: the Win32 front end's SonyPads moves the bytes, and
// bindings_test checks these against reports laid out by hand.
//
// Input reports, by where the controls start:
//
//   DualShock 4  USB 0x01 at byte 1, Bluetooth 0x01 at byte 1 (the short report a pad sends
//                until it is sent an output report) or 0x11 at byte 3:
//                  left x, left y, right x, right y, three button bytes, L2, R2
//   DualSense    USB 0x01 at byte 1, Bluetooth 0x31 at byte 2:
//                  left x, left y, right x, right y, L2, R2, a counter, three button bytes
//                and over Bluetooth its short 0x01 is the DualShock 4's layout.
//
// The three button bytes are the same on both: the d-pad as a hat in the low nibble of the
// first (0 up, then clockwise in eighths, 8 for none) and Square, Cross, Circle, Triangle above
// it; then L1, R1, L2, R2, Share or Create, Options, L3, R3; then the PS button and the touchpad.
//
// Output reports set the motors only (and leave the light bar, the player lights and the
// DualSense's adaptive triggers alone). Over Bluetooth they end in a CRC-32 taken over the
// byte 0xA2 - Bluetooth HID's header for an output report - and then the report.

#include "platform/input_bindings.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace utilities {

// A pad's controls as PadInput bits, and its sticks in the PSX pad's own convention - 0x00 left
// or up, 0xFF right or down - which is the PlayStation pads' convention too.
struct SonyPadState {
  uint32_t inputs = 0;
  uint8_t left_x = 0x80, left_y = 0x80, right_x = 0x80, right_y = 0x80;
};

// How far off centre a stick byte has to be to count at all: the same small deadzone Gamepad
// gives an XInput stick, so a stick resting a little off true still reads centred.
inline uint8_t SonyStickDeadzone(uint8_t v) {
  return (v > 0x80 - 12 && v < 0x80 + 12) ? 0x80 : v;
}

// One input report into `out`. False, and `out` untouched, for a report this does not read -
// the pads send others too.
inline bool ParseSonyPadReport(bool dualsense, bool bluetooth, const uint8_t* report,
                               size_t length, SonyPadState* out) {
  if (length == 0)
    return false;
  const uint8_t* sticks = nullptr;
  const uint8_t* buttons = nullptr;
  uint8_t l2 = 0, r2 = 0;
  const uint8_t id = report[0];
  if (dualsense && ((id == 0x01 && !bluetooth && length >= 11) || (id == 0x31 && length >= 12))) {
    const uint8_t* d = report + (id == 0x31 ? 2 : 1);
    sticks = d;
    l2 = d[4];
    r2 = d[5];
    buttons = d + 7;
  } else if ((id == 0x01 && length >= 10) || (id == 0x11 && length >= 12)) {
    const uint8_t* d = report + (id == 0x11 ? 3 : 1);
    sticks = d;
    buttons = d + 4;
    l2 = d[7];
    r2 = d[8];
  } else {
    return false;
  }

  uint32_t inputs = 0;
  auto set = [&inputs](bool held, int code) {
    if (held)
      inputs |= PadInputBit(code);
  };
  const int hat = buttons[0] & 0x0F;
  set(hat == 7 || hat == 0 || hat == 1, kPadDpadUp);
  set(hat >= 1 && hat <= 3, kPadDpadRight);
  set(hat >= 3 && hat <= 5, kPadDpadDown);
  set(hat >= 5 && hat <= 7, kPadDpadLeft);
  set((buttons[0] & 0x10) != 0, kPadX);       // Square
  set((buttons[0] & 0x20) != 0, kPadA);       // Cross
  set((buttons[0] & 0x40) != 0, kPadB);       // Circle
  set((buttons[0] & 0x80) != 0, kPadY);       // Triangle
  set((buttons[1] & 0x01) != 0, kPadLB);      // L1
  set((buttons[1] & 0x02) != 0, kPadRB);      // R1
  set((buttons[1] & 0x10) != 0, kPadBack);    // Share, or Create
  set((buttons[1] & 0x20) != 0, kPadStart);   // Options
  set((buttons[1] & 0x40) != 0, kPadLS);      // L3
  set((buttons[1] & 0x80) != 0, kPadRS);      // R3
  // The triggers by how far they are pulled, with XInput's own threshold for a press.
  set(l2 > 30, kPadLT);
  set(r2 > 30, kPadRT);
  // The stick directions a button can be bound to, from the stick as an XInput axis would read.
  auto axis_x = [](uint8_t v) { return (static_cast<int>(v) - 128) * 256; };
  auto axis_y = [](uint8_t v) { return (128 - static_cast<int>(v)) * 256; };
  inputs |= StickInputs(axis_x(sticks[0]), axis_y(sticks[1]), kPadLStickUp);
  inputs |= StickInputs(axis_x(sticks[2]), axis_y(sticks[3]), kPadRStickUp);

  out->inputs = inputs;
  out->left_x = SonyStickDeadzone(sticks[0]);
  out->left_y = SonyStickDeadzone(sticks[1]);
  out->right_x = SonyStickDeadzone(sticks[2]);
  out->right_y = SonyStickDeadzone(sticks[3]);
  return true;
}

// CRC-32, the zlib one (reflected, polynomial 0xEDB88320), continued from `crc` - start with
// 0xFFFFFFFF and invert the end.
inline uint32_t Crc32Update(uint32_t crc, const uint8_t* data, size_t length) {
  for (size_t i = 0; i < length; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return crc;
}

// How long the motor report is, before Windows pads it to the pad's longest output report.
inline size_t SonyRumbleReportLength(bool dualsense, bool bluetooth) {
  return bluetooth ? 78 : (dualsense ? 48 : 32);
}

// The report that sets the two motors, into `out`, which is `size` bytes - at least
// SonyRumbleReportLength - and zeroed beyond it. `sequence` counts a DualSense's Bluetooth
// reports; it keeps the low four bits. The small motor is the right-hand one on both pads, as on
// a DualShock. False if `size` is too short.
inline bool BuildSonyRumbleReport(bool dualsense, bool bluetooth, uint8_t sequence,
                                  uint8_t small_motor, uint8_t large_motor, uint8_t* out,
                                  size_t size) {
  if (size < SonyRumbleReportLength(dualsense, bluetooth))
    return false;
  memset(out, 0, size);
  if (!dualsense) {
    if (bluetooth) {
      out[0] = 0x11;
      out[1] = 0xC0;   // a HID report, with a CRC
      out[3] = 0x01;   // set the motors only
      out[6] = small_motor;
      out[7] = large_motor;
    } else {
      out[0] = 0x05;
      out[1] = 0x01;   // set the motors only
      out[4] = small_motor;
      out[5] = large_motor;
    }
  } else {
    uint8_t* common = out + 1;
    if (bluetooth) {
      out[0] = 0x31;
      out[1] = static_cast<uint8_t>((sequence & 0x0F) << 4);
      out[2] = 0x10;
      common = out + 3;
    } else {
      out[0] = 0x02;
    }
    // 0x01 is the rumble the DualSense emulates a DualShock's with, and 0x02 says the motors
    // rather than audio drive its haptics.
    common[0] = 0x03;
    common[2] = small_motor;
    common[3] = large_motor;
  }
  if (bluetooth) {
    const uint8_t header = 0xA2;
    const uint32_t crc = ~Crc32Update(Crc32Update(0xFFFFFFFFu, &header, 1), out, 74);
    out[74] = static_cast<uint8_t>(crc);
    out[75] = static_cast<uint8_t>(crc >> 8);
    out[76] = static_cast<uint8_t>(crc >> 16);
    out[77] = static_cast<uint8_t>(crc >> 24);
  }
  return true;
}

}  // namespace utilities
