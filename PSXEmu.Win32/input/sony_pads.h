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

// PlayStation 4 and 5 pads - the DualShock 4 and the DualSense (and DualSense Edge) - read
// straight from HID, over USB or Bluetooth, with their motors driven the same way.
//
// Windows has no XInput driver for them, so a pad plugged in on its own (nothing like Steam or
// DS4Windows presenting it as an Xbox pad) was simply not seen. This opens the pads, reads their
// input reports and writes their motor reports - platform/sony_pad_reports.h has the layouts - and
// hands them on as the same controls an XInput pad has (utilities::PadInput): Cross is A,
// L1 is LB, L2 is LT, Share or Create is Back, Options is Start. So a binding means the same
// button on either kind, and the defaults work unchanged. The PS button, the touchpad and the
// motion sensors are not read.
//
// Each pad takes the lowest of the four Gamepad slots no XInput pad is in, so an Xbox pad keeps
// the number it always had and a PlayStation pad on its own is Gamepad 1. The slot is kept while
// the pad stays connected, unless an XInput pad arrives in it - then the PlayStation pad moves.
//
// The input thread's alone: every call is from it.

#include "app/framework.h"
#include "host/input_exchange.h"

#include <array>
#include <memory>
#include <vector>

namespace psxemu {

    class SonyPads {
     public:
        SonyPads();
        ~SonyPads();
        SonyPads(const SonyPads&) = delete;
        SonyPads& operator=(const SonyPads&) = delete;

        // A HID device arrived: look for new pads on the next Poll. A pad that goes away is
        // noticed by its read failing, so there is nothing to call for that.
        void Rescan() { rescan_ = true; }

        // Reads every open pad and fills in the slots they hold - never one in `taken`, which is
        // where XInput pads are.
        void Poll(const std::array<bool, emulation::host::HostInput::kPads>& taken,
                  emulation::host::PadReading (&pads)[emulation::host::HostInput::kPads]);

        // What the machine asked `slot`'s motors for; nothing if no PlayStation pad is there.
        void SetRumble(int slot, uint8_t small_motor, uint8_t large_motor);

     private:
        struct Pad;
        void Scan();
        void Open(const std::wstring& path);
        void Close(Pad& pad);
        void Read(Pad& pad);
        void Parse(Pad& pad, const uint8_t* report, size_t length);
        void WriteRumble(Pad& pad);

        std::vector<std::unique_ptr<Pad>> pads_;
        bool rescan_ = true;
    };

}   // namespace psxemu
