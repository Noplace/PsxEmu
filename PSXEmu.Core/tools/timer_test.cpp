// timer_test - checks the three root counters against things that must be
// true.
//
// The counters were the last part of the machine still running on a sketch:
// sync modes decoded and ignored, a target of zero silently never matching,
// the dot clock divided out of CPU cycles by a hardcoded 10, and a counter
// that could only wrap once however long the step. None of that announces
// itself - a game just runs at the wrong speed - so every one of those is a
// check here.
//
// Most of this drives RootCounter directly, because that is where the
// semantics live and a register-level test would only be able to observe the
// same thing through more indirection. The last group goes through the real
// register interface, to confirm the counters are actually reachable at their
// addresses and that a read is not a batch stale.

#include "psx/psx.h"

#include <cstdio>
#include <cstring>

using emulation::psx::RootCounter;
using emulation::psx::System;

namespace {

int g_checks = 0;
int g_failures = 0;
const char* g_group = "";

void Group(const char* name) {
  g_group = name;
  printf("%s\n", name);
}

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
    printf("  FAIL  %s: got %u want %u\n", what, got, want);
  }
}

// Mode register bits, spelled out so a test reads like the hardware document
// rather than like a magic number.
enum ModeBits {
  kSyncEnable   = 1 << 0,
  kSyncMode0    = 0 << 1,
  kSyncMode1    = 1 << 1,
  kSyncMode2    = 2 << 1,
  kSyncMode3    = 3 << 1,
  kResetAtTarget= 1 << 3,
  kIrqAtTarget  = 1 << 4,
  kIrqAtOverflow= 1 << 5,
  kIrqRepeat    = 1 << 6,
  kIrqToggle    = 1 << 7,
};

RootCounter MakeCounter(int index, uint32_t mode, uint32_t target) {
  RootCounter c;
  c.Initialize(index);
  c.WriteTarget(target);
  c.WriteMode(mode);
  return c;
}

// ---------------------------------------------------------------------------

void TestCountsAndWraps() {
  Group("a counter counts, and wraps where it is told to");

  RootCounter c = MakeCounter(0, 0, 0);
  c.Tick(100);
  CheckEqual(c.ReadCounter(), 100, "it counts what it is given");

  // Without reset-at-target the counter runs all the way to FFFFh and wraps
  // there, whatever the target says.
  RootCounter d = MakeCounter(0, kIrqAtTarget, 10);
  d.Tick(500);
  CheckEqual(d.ReadCounter(), 500, "the target alone does not stop it");

  // With reset-at-target it wraps at the target instead.
  RootCounter e = MakeCounter(0, kResetAtTarget, 100);
  e.Tick(250);
  CheckEqual(e.ReadCounter(), 50, "reset-at-target wraps there");

  // And the wrap has to survive a step long enough to cross it many times -
  // the old single subtraction left the counter far out of range.
  RootCounter f = MakeCounter(0, kResetAtTarget, 10);
  f.Tick(95);
  CheckEqual(f.ReadCounter(), 5, "many wraps in one step still land right");
  Check(f.ReadCounter() < 10, "and never above the target it wraps at");

  RootCounter g = MakeCounter(0, 0, 0);
  g.Tick(0x10000 + 7);
  CheckEqual(g.ReadCounter(), 7, "the overflow wrap is at 10000h, not FFFFh");
}

void TestTargetZero() {
  Group("a target of zero is a target, not the absence of one");

  // This is the one the old code got wrong by guarding on target > 0: a
  // counter asked to interrupt every count was simply silent.
  RootCounter c = MakeCounter(0, kIrqAtTarget | kIrqRepeat, 0);
  Check(c.Tick(1), "a target of zero matches on the very first count");
  Check(c.Tick(1), "and on the next one");
  Check(c.Tick(1), "and keeps matching");

  // Reset-at-target with a zero target must not try to divide by it.
  RootCounter d = MakeCounter(0, kIrqAtTarget | kResetAtTarget | kIrqRepeat, 0);
  d.Tick(50);
  Check(d.ReadCounter() <= 0xFFFF, "and does not wrap the counter to nonsense");
}

void TestTargetInterrupt() {
  Group("the target interrupt");

  RootCounter c = MakeCounter(0, kIrqAtTarget | kIrqRepeat, 100);
  Check(!c.Tick(50), "nothing before the target");
  Check(c.Tick(50), "and an interrupt on reaching it");
  CheckEqual(c.mode.reached_target, 1, "the reached-target flag is set");

  // Reading the mode register is what clears the flag.
  c.ReadMode();
  CheckEqual(c.mode.reached_target, 0, "and reading the mode clears it");

  // An interrupt the mode did not ask for does not happen, but the flag
  // still records that the target was reached.
  RootCounter d = MakeCounter(0, kIrqRepeat, 100);
  Check(!d.Tick(150), "no interrupt when the target one is not enabled");
  CheckEqual(d.mode.reached_target, 1, "though the flag still records it");
}

void TestOverflowInterrupt() {
  Group("the overflow interrupt");

  RootCounter c = MakeCounter(0, kIrqAtOverflow | kIrqRepeat, 0xFFFF);
  Check(!c.Tick(0xFFFF), "nothing yet at FFFFh");
  Check(c.Tick(1), "and an interrupt one count later, at the wrap");
  CheckEqual(c.ReadCounter(), 0, "which leaves the counter at zero");
  CheckEqual(c.mode.reached_0xffff, 1, "the reached-overflow flag is set");

  // A target of FFFFh with reset-at-target must still reach the overflow -
  // wrapping exactly at FFFFh would mean it never did.
  RootCounter d = MakeCounter(
      0, kIrqAtOverflow | kResetAtTarget | kIrqRepeat, 0xFFFF);
  Check(d.Tick(0x10000), "a target of FFFFh does not swallow the overflow");
}

void TestOneShotAndRepeat() {
  Group("one-shot fires once, repeat fires every time");

  RootCounter c = MakeCounter(0, kIrqAtTarget, 10);   // no repeat bit
  Check(c.Tick(10), "the first match interrupts");
  Check(!c.Tick(0x10000), "and nothing afterwards, however long it runs");

  // Writing the mode register is what re-arms it.
  c.WriteMode(kIrqAtTarget);
  Check(c.Tick(10), "until the mode register is written again");

  RootCounter d = MakeCounter(0, kIrqAtTarget | kIrqRepeat | kResetAtTarget, 10);
  Check(d.Tick(10), "in repeat mode the first match interrupts");
  Check(d.Tick(10), "and so does the second");
  Check(d.Tick(10), "and the third");
}

void TestPulseAndToggle() {
  Group("bit 10 - pulse against toggle");

  // Pulse mode: bit 10 dips and comes back, so a read almost never catches
  // it low.
  RootCounter c = MakeCounter(0, kIrqAtTarget | kIrqRepeat | kResetAtTarget, 10);
  CheckEqual(c.mode.irqpulse, 0, "this is pulse mode");
  c.Tick(10);
  CheckEqual(c.mode.intreq, 1, "pulse leaves bit 10 back at 1");

  // Toggle mode: bit 10 flips on every match, and the line is asserted only
  // on the flips that take it low.
  RootCounter d = MakeCounter(
      0, kIrqAtTarget | kIrqToggle | kIrqRepeat | kResetAtTarget, 10);
  CheckEqual(d.mode.intreq, 1, "it starts high");
  Check(d.Tick(10), "the first match takes bit 10 low and asserts");
  CheckEqual(d.mode.intreq, 0, "bit 10 is now low");
  Check(!d.Tick(10), "the second match takes it high and does not assert");
  CheckEqual(d.mode.intreq, 1, "bit 10 is high again");
  Check(d.Tick(10), "the third asserts again");

  // Toggling has to happen once per match even when a single step crosses
  // the target several times, which is why the counter steps rather than
  // adding in one go.
  RootCounter e = MakeCounter(
      0, kIrqAtTarget | kIrqToggle | kIrqRepeat | kResetAtTarget, 10);
  e.Tick(20);   // two matches: low then high
  CheckEqual(e.mode.intreq, 1, "two matches in one step toggle twice");
}

void TestModeWrite() {
  Group("writing the mode register");

  RootCounter c = MakeCounter(0, kIrqAtTarget | kIrqRepeat, 1000);
  c.Tick(500);
  CheckEqual(c.ReadCounter(), 500, "the counter has run");
  c.WriteMode(kIrqAtTarget | kIrqRepeat);
  CheckEqual(c.ReadCounter(), 0, "a mode write restarts the counter");
  CheckEqual(c.mode.intreq, 1, "and puts bit 10 back to 1");

  // Bits 10-12 belong to the hardware; software cannot write them.
  RootCounter d = MakeCounter(0, 0, 0);
  d.WriteMode(0x1C00);   // bits 10, 11, 12 all set
  CheckEqual(d.mode.reached_target, 0, "software cannot set reached-target");
  CheckEqual(d.mode.reached_0xffff, 0, "nor reached-overflow");
}

void TestSyncModesCounter0() {
  Group("counter 0 - the hblank gate");

  // Sync disabled is free-run whatever the sync mode bits say.
  RootCounter free = MakeCounter(0, kSyncMode0, 0);
  free.SetGate(true);
  Check(free.counting_enabled(), "sync disabled means free-run, gate or not");

  // Mode 0: pause while the gate is high.
  RootCounter c = MakeCounter(0, kSyncEnable | kSyncMode0, 0);
  Check(c.counting_enabled(), "mode 0 counts outside the blank");
  c.SetGate(true);
  Check(!c.counting_enabled(), "and pauses inside it");
  c.Tick(100);
  CheckEqual(c.ReadCounter(), 0, "a paused counter really does not count");
  c.SetGate(false);
  Check(c.counting_enabled(), "and resumes when the blank ends");
  c.Tick(100);
  CheckEqual(c.ReadCounter(), 100, "counting again");

  // Mode 1: free-run, but restart at the end of every blank.
  RootCounter d = MakeCounter(0, kSyncEnable | kSyncMode1, 0);
  d.Tick(100);
  d.SetGate(true);
  Check(d.counting_enabled(), "mode 1 keeps counting during the blank");
  CheckEqual(d.ReadCounter(), 100, "and does not reset at its start");
  d.SetGate(false);
  CheckEqual(d.ReadCounter(), 0, "but does at its end");

  // Mode 2: only count during the blank, restarting each time it begins.
  RootCounter e = MakeCounter(0, kSyncEnable | kSyncMode2, 0);
  Check(!e.counting_enabled(), "mode 2 does not count outside the blank");
  e.SetGate(true);
  Check(e.counting_enabled(), "only inside it");
  e.Tick(50);
  e.SetGate(false);
  e.SetGate(true);
  CheckEqual(e.ReadCounter(), 0, "and restarts every time it begins");

  // Mode 3: wait for one blank, then free-run for good.
  RootCounter f = MakeCounter(0, kSyncEnable | kSyncMode3, 0);
  Check(!f.counting_enabled(), "mode 3 waits for the first blank");
  f.SetGate(true);
  Check(f.counting_enabled(), "which starts it");
  f.SetGate(false);
  Check(f.counting_enabled(), "and after that it free-runs");
  CheckEqual(f.mode.en, 0, "with the sync bit cleared, as the hardware does");
  f.SetGate(true);
  Check(f.counting_enabled(), "so a later blank no longer stops it");
}

void TestSyncModesCounter2() {
  Group("counter 2 - no gate to wait for");

  // Counter 2 has no gate at all, so two of the four sync modes are simply a
  // way of stopping it, and the other two are free-run.
  RootCounter a = MakeCounter(2, kSyncEnable | kSyncMode0, 0);
  Check(!a.counting_enabled(), "sync mode 0 stops counter 2 dead");
  a.Tick(1000);
  CheckEqual(a.ReadCounter(), 0, "really stopped");

  RootCounter b = MakeCounter(2, kSyncEnable | kSyncMode3, 0);
  Check(!b.counting_enabled(), "and so does sync mode 3");

  RootCounter c = MakeCounter(2, kSyncEnable | kSyncMode1, 0);
  Check(c.counting_enabled(), "sync mode 1 is free-run");
  RootCounter d = MakeCounter(2, kSyncEnable | kSyncMode2, 0);
  Check(d.counting_enabled(), "and so is sync mode 2");

  RootCounter e = MakeCounter(2, 0, 0);
  Check(e.counting_enabled(), "as is sync disabled");
}

// ---------------------------------------------------------------------------
// Through the real register interface.

class TimerHarness {
 public:
  explicit TimerHarness(System* system) : system_(system) {}

  uint32_t ReadCounter(int n) {
    return system_->io().Read32(0x1F801100 + n * 0x10);
  }
  void WriteMode(int n, uint32_t value) {
    system_->io().Write32(0x1F801104 + n * 0x10, value);
  }
  void WriteTarget(int n, uint32_t value) {
    system_->io().Write32(0x1F801108 + n * 0x10, value);
  }
  uint32_t ReadTarget(int n) {
    return system_->io().Read32(0x1F801108 + n * 0x10);
  }
  void Run(uint32_t cycles) {
    for (uint32_t i = 0; i < cycles; ++i)
      system_->io().Tick(1);
  }

 private:
  System* system_;
};

void TestThroughTheRegisters(System* system) {
  Group("through the registers, where software finds them");

  TimerHarness t(system);
  t.WriteMode(2, 0);              // free-run, system clock
  t.WriteTarget(2, 0x1234);
  CheckEqual(t.ReadTarget(2), 0x1234, "the target reads back");

  t.WriteMode(2, 0);              // restarts the counter
  CheckEqual(t.ReadCounter(2), 0, "a mode write zeroes it");

  // The point of flushing on read: 10 cycles is well inside the batch the
  // machine would otherwise be waiting to fill, and the counter still has to
  // show them. Before, a read like this returned 0 until 32 had gone by.
  t.Run(10);
  CheckEqual(t.ReadCounter(2), 10, "ten cycles are visible immediately");
  t.Run(1);
  CheckEqual(t.ReadCounter(2), 11, "and so is the eleventh");

  // Counter 2 on system clock / 8.
  t.WriteMode(2, 2 << 8);
  t.Run(80);
  CheckEqual(t.ReadCounter(2), 10, "clock source 2 divides the clock by 8");

  // Counter 0 on the dot clock. What it should be is the GPU clock - 11/7 of
  // the CPU clock - divided by the resolution divider, which is 10 at the
  // 256-wide default. Not the CPU clock divided by 10, which is what it used
  // to be: that runs at 7/11 of the rate it should, and the measured gap
  // below (700 against 1100) is exactly that ratio.
  t.WriteMode(0, 1 << 8);
  t.Run(7000);
  const uint32_t dots = t.ReadCounter(0);
  const uint32_t expect = 7000u * 11u / 7u / 10u;    // 1100
  Check(dots > expect - 4 && dots < expect + 4,
        "counter 0 counts real dot clocks");
  if (dots <= expect - 4 || dots >= expect + 4)
    printf("        (got %u, expected about %u)\n", dots, expect);
}

}  // namespace

int main() {
  TestCountsAndWraps();
  TestTargetZero();
  TestTargetInterrupt();
  TestOverflowInterrupt();
  TestOneShotAndRepeat();
  TestPulseAndToggle();
  TestModeWrite();
  TestSyncModesCounter0();
  TestSyncModesCounter2();

  System* system = new System();
  system->InitializeWithoutBios();
  TestThroughTheRegisters(system);
  delete system;

  printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
