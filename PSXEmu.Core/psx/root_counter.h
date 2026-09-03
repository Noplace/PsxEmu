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

namespace emulation {
namespace psx {

/*
  A root counter - one of the three 16-bit timers at 1F801100h/1110h/1120h.

  Each counts something (the system clock, the dot clock, hblanks, or the
  system clock divided by 8), raises an interrupt when it reaches its target
  or wraps at FFFFh, and can be gated by the display: counter 0 by hblank and
  counter 1 by vblank. Counter 2 has no gate at all, which is why asking it to
  synchronise with one simply stops it.

  Three details are worth stating because they are all easy to get subtly
  wrong and none of them announces itself when it is:

   - A target of zero is not "no target". The counter matches it on every
     count, and software uses exactly that to ask for an interrupt every time.
   - The counter does not stop at its target unless reset-at-target is set.
     Otherwise it carries on to FFFFh and wraps there, and both interrupts can
     be armed at once.
   - Bit 10 is not a plain "an interrupt happened" flag. In pulse mode it dips
     low and comes straight back, and one-shot means one interrupt until the
     mode register is written again; in toggle mode it flips on every match
     and the line is asserted only on the flips that take it low.
*/
class RootCounter {
 public:
  // The counter wraps here rather than at FFFFh - the match is on the way
  // past 10000h, one count after the last value it actually holds.
  static const uint32_t kOverflow = 0x10000;

  // Bits 10-12 are status the hardware owns; a mode write leaves them alone.
  static const uint32_t kModeWriteMask = 0xE3FF;

  uint32_t counter;
  uint32_t target;
  union {
    struct {
      uint32_t en:1;              // 0     synchronise with the gate at all
      uint32_t syncmode:2;        // 1-2   what synchronising means
      uint32_t resetmode:1;       // 3     0=wrap at FFFFh, 1=wrap at target
      uint32_t irq_target:1;      // 4
      uint32_t irq_0xffff:1;      // 5
      uint32_t irqrepeat:1;       // 6     0=one-shot, 1=every time
      uint32_t irqpulse:1;        // 7     0=pulse bit 10, 1=toggle it
      uint32_t clcsrc:2;          // 8-9
      uint32_t intreq:1;          // 10    0=interrupt requested, 1=not
      uint32_t reached_target:1;  // 11    cleared by reading the register
      uint32_t reached_0xffff:1;  // 12    cleared by reading the register
      uint32_t _unknown:3;
      uint32_t _garbage:16;
    };
    uint32_t raw;
  }mode;

  // Which of the three this is. Sync modes and clock sources mean different
  // things per counter, so it cannot work this out for itself.
  void Initialize(int index) {
    index_ = index;
    counter = 0;
    target = 0;
    mode.raw = 0;
    mode.intreq = 1;      // 1 = no interrupt requested
    gate_ = false;
    irq_done_ = false;
    UpdateCountingEnabled();
  }

  uint32_t ReadCounter() const {
    return counter;
  }

  // Reading the mode register clears the two reached flags, which is how
  // software tells a target match from an overflow after the fact.
  uint32_t ReadMode() {
    const uint32_t result = mode.raw;
    mode.reached_0xffff = 0;
    mode.reached_target = 0;
    return result;
  }

  uint32_t ReadTarget() const {
    return target;
  }

  void WriteCounter(uint32_t value) {
    counter = value & 0xFFFF;
  }

  // A mode write restarts the counter and re-arms a one-shot interrupt. It
  // does not clear the reached flags - those belong to the read side.
  void WriteMode(uint32_t value) {
    mode.raw = (value & kModeWriteMask) | (mode.raw & ~kModeWriteMask);
    mode.intreq = 1;
    counter = 0;
    irq_done_ = false;
    UpdateCountingEnabled();
  }

  void WriteTarget(uint32_t value) {
    target = value & 0xFFFF;
  }

  // Hblank for counter 0, vblank for counter 1. Counter 2 has no gate and is
  // never handed one.
  void SetGate(bool state) {
    if (gate_ == state)
      return;
    gate_ = state;
    if (mode.en) {
      switch (mode.syncmode) {
        case 0:                                   // pause while gated
          break;
        case 1:                                   // restart when it ends
          if (!state) counter = 0;
          break;
        case 2:                                   // restart when it starts
          if (state) counter = 0;
          break;
        case 3:                                   // free-run after the first
          if (!state) mode.en = 0;
          break;
      }
    }
    UpdateCountingEnabled();
  }

  bool counting_enabled() const { return counting_enabled_; }

  // Advances the counter and says whether the interrupt line should be
  // pulled. Steps rather than adding in one go, so a batch long enough to
  // wrap the counter more than once still lands on the right value - and in
  // toggle mode still toggles bit 10 once per match rather than once per
  // batch.
  bool Tick(uint32_t cycles) {
    if (!counting_enabled_ || cycles == 0)
      return false;

    bool generate_irq = false;
    uint32_t remaining = cycles;
    while (remaining > 0) {
      // How far it can run before something has to be decided: the target, if
      // it wraps there and has not already passed it, otherwise the overflow.
      const uint32_t wrap_at =
          (mode.resetmode && counter < target) ? target : kOverflow;
      uint32_t step = wrap_at - counter;
      if (step > remaining)
        step = remaining;
      if (step == 0)
        step = 1;    // unreachable while counter < wrap_at; belt and braces

      const uint32_t old_counter = counter;
      counter += step;
      remaining -= step;
      generate_irq |= CheckForIrq(old_counter);
    }
    return generate_irq;
  }

 private:
  // Whether counting happens at all right now. Counters 0 and 1 follow their
  // gate; counter 2, having no gate, is simply stopped by the two sync modes
  // that would otherwise have waited for one.
  void UpdateCountingEnabled() {
    if (index_ != 2) {
      if (!mode.en) {
        counting_enabled_ = true;
        return;
      }
      switch (mode.syncmode) {
        case 0:  counting_enabled_ = !gate_; break;
        case 1:  counting_enabled_ = true;   break;
        default: counting_enabled_ = gate_;  break;   // 2 and 3
      }
    } else {
      counting_enabled_ = !mode.en || mode.syncmode == 1 || mode.syncmode == 2;
    }
  }

  // Decides what a step from old_counter to counter means. Called with the
  // counter possibly sitting exactly on the overflow, which is the one value
  // it must never be left at.
  bool CheckForIrq(uint32_t old_counter) {
    bool wrapped_overflow = false;
    if (counter >= kOverflow) {
      // Reaching the overflow only counts when the counter was not going to
      // wrap at its target first.
      wrapped_overflow = (!mode.resetmode || old_counter >= target);
      old_counter = 0;
    }

    bool interrupt = false;
    if (counter >= target && (old_counter < target || target == 0)) {
      if (mode.irq_target) interrupt = true;
      mode.reached_target = 1;
      // A target of FFFFh is left to the overflow path, one count later,
      // which would otherwise never be reached.
      if (mode.resetmode && target > 0 && target != 0xFFFF)
        counter %= target;
    }
    if (counter >= kOverflow || wrapped_overflow) {
      if (mode.irq_0xffff) interrupt = true;
      mode.reached_0xffff = 1;
      counter &= 0xFFFF;
    }

    if (!interrupt)
      return false;

    if (!mode.irqpulse) {
      // Pulse: bit 10 dips and returns, so software almost never catches it
      // low. One-shot means one interrupt until the mode register is written.
      const bool fire = (!irq_done_ || mode.irqrepeat != 0);
      irq_done_ = true;
      mode.intreq = 1;
      return fire;
    }
    // Toggle: bit 10 flips on every match, and the line is asserted only on
    // the flips that take it low.
    mode.intreq ^= 1;
    return mode.intreq == 0;
  }

  int index_ = 0;
  bool gate_ = false;
  bool counting_enabled_ = true;
  bool irq_done_ = false;
};

}
}
