// host_test - checks the channels in PSXEmu.Core/host/, the one part of Core
// that knows about threads (Docs/Threading-Plan.md).
//
// Every other harness here is single-threaded by construction, and a checksum
// cannot see a race: a lost sample, a frame read while it was being written, a
// request run out of order. So each channel is checked the way it will be
// used - one thread on each side, millions of items with a sequence number in
// them - and the checks look for exactly those things: nothing lost, nothing
// duplicated, nothing out of order, nothing torn, and the edge counters
// counting what went over the edge.

#include "host/audio_output.h"
#include "host/doorbell.h"
#include "host/frame_mailbox.h"
#include "host/input_exchange.h"
#include "host/machine.h"
#include "host/request_queue.h"
#include "host/sample_ring.h"
#include "host/video_output.h"
#include "psx/psx.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

using emulation::host::AudioOutput;
using emulation::host::Doorbell;
using emulation::host::FrameMailbox;
using emulation::host::HostInput;
using emulation::host::InputExchange;
using emulation::host::Machine;
using emulation::host::MachineReport;
using emulation::host::Presenter;
using emulation::host::RequestQueue;
using emulation::host::SampleRing;
using emulation::host::VideoFrame;
using emulation::host::VideoOutput;
using emulation::psx::System;

namespace {

int failures = 0;
int checks = 0;

void Check(bool ok, const char* what) {
  ++checks;
  printf("  %-72s %s\n", what, ok ? "ok" : "FAILED");
  if (!ok)
    ++failures;
}

typedef std::chrono::steady_clock Clock;

double MsSince(Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

// ---------------------------------------------------------------------------

void DoorbellChecks() {
  printf("doorbell\n");

  {
    Doorbell bell;
    bell.Ring();
    const Clock::time_point start = Clock::now();
    const bool rung = bell.Wait(std::chrono::seconds(1));
    Check(rung && MsSince(start) < 50.0,
          "a ring that came first is not lost: the wait returns at once");
  }

  {
    Doorbell bell;
    const Clock::time_point start = Clock::now();
    const bool rung = bell.Wait(std::chrono::milliseconds(30));
    const double waited = MsSince(start);
    printf("    unrung 30 ms wait took %.1f ms\n", waited);
    Check(!rung && waited >= 20.0 && waited < 250.0,
          "an unrung wait times out, and says so");
  }

  {
    Doorbell bell;
    std::atomic<double> woke_after_ring_ms{-1.0};
    std::atomic<bool> rang{false};
    Clock::time_point rung_at;
    std::thread waiter([&] {
      const bool rung = bell.Wait(std::chrono::seconds(5));
      if (rung && rang.load())
        woke_after_ring_ms = MsSince(rung_at);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    rung_at = Clock::now();
    rang = true;
    bell.Ring();
    waiter.join();
    printf("    woke %.2f ms after the ring\n", woke_after_ring_ms.load());
    Check(woke_after_ring_ms >= 0.0 && woke_after_ring_ms < 50.0,
          "another thread's ring wakes a sleeping owner at once, not at the timeout");
  }
}

// ---------------------------------------------------------------------------

// What the requests act on in these checks: a record of who asked for what,
// in the order the owner saw it.
struct Ledger {
  static const int kProducers = 4;
  int last[kProducers] = {-1, -1, -1, -1};
  int out_of_order = 0;
  int total = 0;
  void Record(int producer, int sequence) {
    if (sequence <= last[producer])
      ++out_of_order;
    last[producer] = sequence;
    ++total;
  }
};

void RequestQueueChecks() {
  printf("request queue\n");

  {
    const int kPerProducer = 50000;
    Doorbell bell;
    RequestQueue<Ledger> queue(&bell);
    Ledger ledger;
    std::vector<std::thread> producers;
    for (int p = 0; p < Ledger::kProducers; ++p) {
      producers.emplace_back([&queue, p] {
        for (int i = 0; i < kPerProducer; ++i)
          queue.Post([p, i](Ledger& l) { l.Record(p, i); });
      });
    }
    const Clock::time_point start = Clock::now();
    while (ledger.total < Ledger::kProducers * kPerProducer && MsSince(start) < 20000.0) {
      if (queue.Drain(ledger) == 0)
        bell.Wait(std::chrono::milliseconds(5));
    }
    for (std::thread& t : producers)
      t.join();
    queue.Drain(ledger);
    printf("    %d requests from %d threads, drained in %.0f ms\n", ledger.total,
           Ledger::kProducers, MsSince(start));
    Check(ledger.total == Ledger::kProducers * kPerProducer,
          "every request posted from four threads ran, exactly once");
    Check(ledger.out_of_order == 0,
          "and each thread's requests ran in the order that thread posted them");
  }

  {
    RequestQueue<int> queue;
    queue.Post([&queue](int& n) {
      ++n;
      queue.Post([](int& m) { m += 10; });
    });
    int value = 0;
    const int first = queue.Drain(value);
    const bool waited = (first == 1 && value == 1);
    const int second = queue.Drain(value);
    Check(waited && second == 1 && value == 11,
          "a request posted by a running request waits for the next drain");
  }

  {
    Doorbell bell;
    RequestQueue<int> queue(&bell);
    queue.Post([](int&) {});
    const Clock::time_point start = Clock::now();
    Check(bell.Wait(std::chrono::seconds(1)) && MsSince(start) < 50.0,
          "posting rings the owner's doorbell");
  }
}

// ---------------------------------------------------------------------------

void SampleRingChecks() {
  printf("sample ring\n");

  Check(SampleRing(1000).capacity() == 1024, "capacity rounds up to a power of two");

  // Two threads, sequence-numbered frames, chunks of every size: the left
  // channel carries the low 16 bits of the frame's number and the right its
  // complement, so a lost, repeated or reordered frame - or a frame torn across
  // the two channels - shows up as a mismatch.
  {
    const int kFrames = 2000000;
    SampleRing ring(4096);
    std::atomic<int> mismatches{0};
    std::thread producer([&] {
      std::mt19937 random(1);
      std::vector<int16_t> chunk;
      int next = 0;
      while (next < kFrames) {
        const int want = std::min(1 + static_cast<int>(random() % 900), kFrames - next);
        chunk.resize(static_cast<size_t>(want) * 2);
        for (int i = 0; i < want; ++i) {
          chunk[i * 2] = static_cast<int16_t>(next + i);
          chunk[i * 2 + 1] = static_cast<int16_t>(~(next + i));
        }
        // Waits for room rather than overflowing, so continuity is testable.
        int done = 0;
        while (done < want) {
          const int room = ring.capacity() - ring.Available();
          if (room == 0) {
            std::this_thread::yield();
            continue;
          }
          const int n = std::min(room, want - done);
          done += ring.Write(chunk.data() + static_cast<size_t>(done) * 2, n);
        }
        next += want;
      }
    });
    std::mt19937 random(2);
    std::vector<int16_t> chunk(1000);
    int expected = 0;
    const Clock::time_point start = Clock::now();
    while (expected < kFrames && MsSince(start) < 20000.0) {
      const int want = 1 + static_cast<int>(random() % 500);
      const int got = ring.Read(chunk.data(), want);
      for (int i = 0; i < got; ++i) {
        if (chunk[i * 2] != static_cast<int16_t>(expected) ||
            chunk[i * 2 + 1] != static_cast<int16_t>(~expected))
          ++mismatches;
        ++expected;
      }
      if (got == 0)
        std::this_thread::yield();
    }
    producer.join();
    printf("    %d frames through a %d-frame ring in %.0f ms\n", expected, ring.capacity(),
           MsSince(start));
    Check(expected == kFrames && mismatches == 0,
          "two million frames arrive in order, none lost, repeated or torn");
    Check(ring.dropped_frames() == 0 && ring.Available() == 0,
          "and none were dropped while there was room");
  }

  {
    SampleRing ring(256);
    std::vector<int16_t> frames(300 * 2, 7);
    const int written = ring.Write(frames.data(), 300);
    Check(written == 256 && ring.dropped_frames() == 44 && ring.Available() == 256,
          "a full ring takes what fits and counts the rest as dropped");

    std::vector<int16_t> out(256 * 2);
    ring.Read(out.data(), 256);
    const bool oldest_kept = (out[0] == 7 && out[511] == 7);
    ring.WriteSilence(10);
    std::vector<int16_t> quiet(10 * 2, 99);
    const int got = ring.Read(quiet.data(), 10);
    bool silent = (got == 10);
    for (int16_t s : quiet)
      silent = silent && (s == 0);
    Check(oldest_kept && silent, "what was kept is the oldest, and silence reads as silence");
  }

  // The consumer's way out of a ring that has run far past its target: throw
  // the oldest away rather than play it all, late. See AudioOutput.
  {
    SampleRing ring(4096);
    ring.set_target_frames(200);
    std::vector<int16_t> frames(3000 * 2, 5);
    ring.Write(frames.data(), 3000);
    const int skipped = ring.Skip(ring.Available() - ring.target_frames() * 2);
    Check(skipped == 2600 && ring.Available() == 400 && ring.skipped_frames() == 2600,
          "skipping drops the oldest frames and leaves the ring where it was asked to");
    Check(ring.Skip(10000) == 400 && ring.Available() == 0,
          "and skipping more than there is empties it rather than running past");
  }
}

// ---------------------------------------------------------------------------

void FrameMailboxChecks() {
  printf("frame mailbox\n");

  {
    FrameMailbox mailbox;
    const bool empty = (mailbox.TakeNew() == nullptr && mailbox.current() == nullptr);
    mailbox.back().number = 1;
    mailbox.Publish();
    const VideoFrame* first = mailbox.TakeNew();
    const bool got_first = (first != nullptr && first->number == 1);
    const bool no_repeat = (mailbox.TakeNew() == nullptr);
    const bool still_current = (mailbox.current() != nullptr && mailbox.current()->number == 1);
    Check(empty && got_first && no_repeat && still_current,
          "a frame is taken once, and stays current until the next");

    mailbox.back().number = 2;
    mailbox.Publish();
    mailbox.back().number = 3;
    mailbox.Publish();
    const VideoFrame* newest = mailbox.TakeNew();
    Check(newest != nullptr && newest->number == 3 && mailbox.dropped() == 1,
          "the consumer gets the newest; the one it missed is counted as dropped");
  }

  // Every pixel of a frame carries the frame's number, so a frame read while
  // the producer was still writing it shows up as mixed numbers.
  {
    const int kFrames = 20000;
    const int kSide = 64;
    Doorbell bell;
    FrameMailbox mailbox(&bell);
    std::atomic<bool> done{false};
    std::thread producer([&] {
      for (int n = 1; n <= kFrames; ++n) {
        VideoFrame& frame = mailbox.back();
        frame.width = kSide;
        frame.height = kSide;
        frame.number = static_cast<uint64_t>(n);
        frame.pixels.assign(static_cast<size_t>(kSide) * kSide, static_cast<uint32_t>(n));
        mailbox.Publish();
      }
      done = true;
    });
    int torn = 0;
    int backwards = 0;
    uint64_t last = 0;
    const Clock::time_point start = Clock::now();
    while (MsSince(start) < 20000.0) {
      const VideoFrame* frame = mailbox.TakeNew();
      if (frame == nullptr) {
        if (done.load() && mailbox.TakeNew() == nullptr)
          break;
        bell.Wait(std::chrono::milliseconds(1));
        continue;
      }
      for (uint32_t pixel : frame->pixels) {
        if (pixel != static_cast<uint32_t>(frame->number)) {
          ++torn;
          break;
        }
      }
      if (frame->number <= last)
        ++backwards;
      last = frame->number;
    }
    producer.join();
    printf("    %llu published, %llu taken, %llu dropped\n",
           static_cast<unsigned long long>(mailbox.published()),
           static_cast<unsigned long long>(mailbox.taken()),
           static_cast<unsigned long long>(mailbox.dropped()));
    Check(torn == 0, "no frame was read while it was being written");
    Check(backwards == 0 && last == static_cast<uint64_t>(kFrames),
          "frames only move forwards, and the last one arrives");
    Check(mailbox.taken() + mailbox.dropped() == mailbox.published(),
          "every published frame was either taken or counted as dropped");
  }
}

// ---------------------------------------------------------------------------

void InputExchangeChecks() {
  printf("input exchange\n");

  // Every reading carries one count of motion each way. The machine's side
  // takes whenever it likes; however the two interleave, the counts it sees
  // must add up to exactly what was published.
  {
    const int kReadings = 100000;
    InputExchange exchange;
    std::atomic<bool> done{false};
    std::thread input([&] {
      for (int i = 1; i <= kReadings; ++i) {
        HostInput reading;
        reading.keyboard = static_cast<uint16_t>(i);
        reading.mouse_dx = 1;
        reading.mouse_dy = -1;
        reading.focused = true;
        exchange.Publish(reading);
      }
      done = true;
    });
    long long dx = 0;
    long long dy = 0;
    while (!done.load()) {
      const HostInput reading = exchange.Take();
      dx += reading.mouse_dx;
      dy += reading.mouse_dy;
    }
    input.join();
    const HostInput last = exchange.Take();
    dx += last.mouse_dx;
    dy += last.mouse_dy;
    printf("    %lld counts of motion across %d readings\n", dx, kReadings);
    Check(dx == kReadings && dy == -kReadings,
          "mouse motion adds up exactly: no count lost, none delivered twice");
    Check(last.keyboard == static_cast<uint16_t>(kReadings) && last.focused,
          "and levels are the latest reading");
  }

  {
    InputExchange exchange;
    exchange.SetRumble(2, 0xFF, 0x40);
    exchange.SetRumble(9, 0x11, 0x22);   // no such pad; ignored
    uint8_t small_motor = 0, large_motor = 0;
    exchange.GetRumble(2, &small_motor, &large_motor);
    uint8_t other_small = 1, other_large = 1;
    exchange.GetRumble(0, &other_small, &other_large);
    Check(small_motor == 0xFF && large_motor == 0x40 && other_small == 0 && other_large == 0,
          "rumble reaches the pad it was set for, and no other");
  }
}

// ---------------------------------------------------------------------------
// The threads themselves.

// boot_runner's checksum: FNV-1a over the visible framebuffer, alpha masked.
uint64_t Checksum(const uint32_t* pixels, int count) {
  uint64_t hash = 1469598103934665603ull;
  for (int i = 0; i < count; ++i) {
    const uint32_t pixel = pixels[i] & 0x00FFFFFF;
    for (int byte = 0; byte < 4; ++byte) {
      hash ^= (pixel >> (byte * 8)) & 0xFF;
      hash *= 1099511628211ull;
    }
  }
  return hash;
}

uint64_t FramebufferChecksum(System& system) {
  int width = 0;
  int height = 0;
  const uint32_t* pixels = system.gpu().framebuffer(width, height);
  return Checksum(pixels, width * height);
}

// A sound device that plays at exactly 44,100 frames a second of real time,
// out of a 50 ms buffer, and counts every time it found the buffer empty. It
// is the audio thread's side of the ring that is under test, not a driver.
class ClockedDevice : public IAudioEngine {
 public:
  bool Initialize(int sample_rate, int) override {
    rate_ = sample_rate;
    return true;
  }
  void Shutdown() override {}
  // Primed full of silence, as both real engines are: WASAPI's Play fills its
  // buffer before Start, and DirectSound starts writing past a cursor it has
  // just zeroed behind.
  void Play() override {
    start_ = Clock::now();
    written_ = kCapacity;
    playing_ = true;
  }
  void Pause() override { playing_ = false; }
  void WaitForRoom(int timeout_ms) override {
    std::this_thread::sleep_for(std::chrono::milliseconds(std::min(timeout_ms, 5)));
  }
  int WritableFrames() override { return kCapacity - Buffered(); }
  void WriteFrames(const int16_t*, int frames) override { written_ += frames; }
  int BufferedFrames() override { return Buffered(); }
  int underruns = 0;

 private:
  static const int kCapacity = 44100 / 20;   // 50 ms
  int Buffered() {
    const int64_t played = static_cast<int64_t>(
        std::chrono::duration<double>(Clock::now() - start_).count() * rate_);
    if (played > written_) {
      // Ran dry: what played meanwhile was silence the device made up itself.
      ++underruns;
      written_ = played;
    }
    return static_cast<int>(written_ - played);
  }
  Clock::time_point start_;
  int64_t written_ = 0;
  int rate_ = 44100;
  bool playing_ = false;
};

class CountingPresenter : public Presenter {
 public:
  void Present(const VideoFrame& frame) override {
    ++presents;
    last_number = frame.number;
  }
  void Resize(int, int) override { ++resizes; }
  std::atomic<int> presents{0};
  std::atomic<int> resizes{0};
  std::atomic<uint64_t> last_number{0};
};

struct MachineRun {
  bool ok = false;
  uint64_t checksum = 0;
  uint64_t instructions = 0;
};

// Boots the BIOS on a machine thread with the limiter off and stops at exactly
// `frames` frames. `during`, if given, is called over and over from this
// thread while the machine runs - to throw requests at it.
MachineRun RunBios(const std::string& bios, int frames,
                   const std::function<void(Machine&)>& at_frame = nullptr,
                   const std::function<void(Machine&)>& during = nullptr,
                   const std::string& load_state_first = std::string(),
                   const std::function<void(Machine&)>& on_halt = nullptr) {
  MachineRun result;
  // On the heap: a System is far too big for a thread's stack.
  std::unique_ptr<System> system_owner = std::make_unique<System>();
  System& system = *system_owner;
  if (system.Initialize(bios.c_str()) != 0)
    return result;
  system.config().frame_limiter = false;
  FrameMailbox mailbox;
  SampleRing ring(8192);
  std::atomic<bool> reached{false};
  int count = 0;
  Machine::Hooks hooks;
  hooks.after_frame = [&](Machine& machine) {
    ++count;
    if (at_frame)
      at_frame(machine);
    if (count == frames) {
      machine.SetPaused(emulation::host::kPausedByUser, true);
      reached = true;
    }
  };
  hooks.halted = on_halt;
  Machine machine(&system, &mailbox, &ring, hooks);
  if (!load_state_first.empty()) {
    machine.Post([load_state_first](Machine& m) {
      m.system().LoadState(load_state_first);
    });
  }
  machine.Start(0);
  const Clock::time_point start = Clock::now();
  while (!reached.load() && MsSince(start) < 120000.0) {
    if (during)
      during(machine);
    else
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  machine.Stop();
  result.ok = reached.load();
  result.checksum = FramebufferChecksum(system);
  result.instructions = machine.instructions();
  system.Deinitialize();
  return result;
}

void MachineChecks(const std::string& bios) {
  printf("machine thread\n");

  // boot_runner bios/SCPH1001.BIN --frames 400: Docs/Test-Suite.md's baseline.
  const uint64_t kBaselineChecksum = 0xc7c8db90c5984798ull;
  const uint64_t kBaselineInstructions = 93049815ull;

  const Clock::time_point start = Clock::now();
  const MachineRun plain = RunBios(bios, 400);
  printf("    400 frames on the machine's thread in %.0f ms: %llu instructions, checksum %016llx\n",
         MsSince(start), static_cast<unsigned long long>(plain.instructions),
         static_cast<unsigned long long>(plain.checksum));
  Check(plain.ok && plain.instructions == kBaselineInstructions &&
            plain.checksum == kBaselineChecksum,
        "a threaded BIOS boot lands on boot_runner's instruction count and checksum");

  // Pause and resume thrown at it from another thread, as fast as it will take
  // them: pausing happens between frames, so the machine must not notice.
  {
    std::mt19937 random(3);
    int posted = 0;
    const MachineRun interrupted = RunBios(bios, 400, nullptr, [&](Machine& machine) {
      machine.Post([](Machine& m) { m.SetPaused(emulation::host::kPausedForMenu, true); });
      std::this_thread::sleep_for(std::chrono::microseconds(random() % 3000));
      machine.Post([](Machine& m) { m.SetPaused(emulation::host::kPausedForMenu, false); });
      std::this_thread::sleep_for(std::chrono::microseconds(random() % 3000));
      posted += 2;
    });
    printf("    %d pause and resume requests posted during the run\n", posted);
    Check(interrupted.ok && interrupted.instructions == kBaselineInstructions &&
              interrupted.checksum == kBaselineChecksum,
          "pausing and resuming at random from another thread changes nothing");
  }

  // The debugger halting the machine mid-frame, and another thread letting it go again - by a
  // plain continue or a single step - must not change what it computes (Docs/Debugger-Plan.md).
  {
    std::mt19937 random(5);
    std::atomic<bool> waiting{false};
    std::atomic<int> halts{0};
    int frame = 0;
    uint32_t hits = 0;
    const MachineRun debugged = RunBios(
        bios, 400,
        [&](Machine& machine) {
          if (++frame == 100)
            machine.system().debugger().AddBreakpoint(0xB0);   // the BIOS's B0 call vector
        },
        [&](Machine& machine) {
          // Cleared before the request goes, so a halt that follows it is never missed.
          if (!waiting.exchange(false)) {
            std::this_thread::sleep_for(std::chrono::microseconds(200));
            return;
          }
          std::this_thread::sleep_for(std::chrono::microseconds(random() % 2000));
          const int n = halts.load();
          machine.Post([n, &hits](Machine& m) {
            emulation::psx::Debugger& debugger = m.system().debugger();
            if (n >= 60) {
              if (!debugger.breakpoints().empty())
                hits = debugger.breakpoints()[0].hits;
              debugger.ClearBreakpoints();
              debugger.Resume();
            } else if (n % 2 == 0) {
              debugger.StepInto();
            } else {
              debugger.Resume();
            }
          });
        },
        std::string(),
        [&](Machine&) {
          ++halts;
          waiting = true;
        });
    printf("    %d halts mid-frame, %u of them at the breakpoint, each let go from another thread\n",
           halts.load(), hits);
    Check(debugged.ok && halts.load() >= 60 && hits > 0 &&
              debugged.instructions == kBaselineInstructions &&
              debugged.checksum == kBaselineChecksum,
          "halting at a breakpoint and stepping from another thread changes nothing");
  }

  // A state saved on the machine's thread at frame 200 and loaded - by
  // request, before the first frame - into a fresh machine that then runs the
  // other 200 must land on the same picture as running all 400.
  {
    const std::string path =
        (std::filesystem::temp_directory_path() / "psxemu_host_test.state").string();
    std::string save_error = "not saved";
    int frame = 0;
    const MachineRun saved = RunBios(bios, 400, [&](Machine& machine) {
      if (++frame == 200)
        save_error = machine.system().SaveState(path);
    });
    const MachineRun resumed = RunBios(bios, 200, nullptr, nullptr, path);
    std::remove(path.c_str());
    Check(saved.ok && save_error.empty() && resumed.ok && resumed.checksum == saved.checksum &&
              saved.checksum == kBaselineChecksum,
          "a state saved on the machine's thread and loaded by request plays on identically");
  }

  // Stopped at any moment - mid-frame, mid-pace, paused - it comes back
  // within a frame or so, every time.
  {
    std::mt19937 random(4);
    double worst_ms = 0.0;
    int stopped = 0;
    for (int i = 0; i < 40; ++i) {
      // On the heap: a System is far too big for a thread's stack.
      std::unique_ptr<System> system_owner = std::make_unique<System>();
      System& system = *system_owner;
      if (system.Initialize(bios.c_str()) != 0)
        break;
      system.config().frame_limiter = (i % 2) == 0;
      FrameMailbox mailbox;
      SampleRing ring(8192);
      Machine machine(&system, &mailbox, &ring, Machine::Hooks());
      machine.Start((i % 5) == 0 ? emulation::host::kPausedByUser : 0);
      std::this_thread::sleep_for(std::chrono::milliseconds(random() % 40));
      const Clock::time_point stop_at = Clock::now();
      machine.Stop();
      worst_ms = std::max(worst_ms, MsSince(stop_at));
      ++stopped;
      system.Deinitialize();
    }
    printf("    %d machines stopped mid-run, the slowest in %.1f ms\n", stopped, worst_ms);
    Check(stopped == 40 && worst_ms < 250.0,
          "stopping a running, pacing or paused machine returns promptly every time");
  }

  // A paused machine answers a request at once: it is waiting on its doorbell,
  // not polling.
  {
    // On the heap: a System is far too big for a thread's stack.
    std::unique_ptr<System> system_owner = std::make_unique<System>();
    System& system = *system_owner;
    system.Initialize(bios.c_str());
    FrameMailbox mailbox;
    SampleRing ring(8192);
    Machine machine(&system, &mailbox, &ring, Machine::Hooks());
    machine.Start(emulation::host::kPausedByUser);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    std::atomic<double> latency_ms{-1.0};
    const Clock::time_point posted = Clock::now();
    machine.Post([&](Machine&) { latency_ms = MsSince(posted); });
    while (latency_ms.load() < 0.0 && MsSince(posted) < 2000.0)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    machine.Stop();
    system.Deinitialize();
    printf("    request answered %.2f ms after posting\n", latency_ms.load());
    Check(latency_ms >= 0.0 && latency_ms < 20.0,
          "a paused machine runs a request at once rather than on its next look round");
  }
}

// The whole pipeline at real speed: the machine paced by the frame limiter,
// its sound pulled by an audio thread from a device on its own clock, its
// frames taken by a video thread. For three seconds nothing may run short,
// overflow or go unshown.
void PipelineChecks(const std::string& bios) {
  printf("machine, audio and video threads together\n");

  // On the heap: a System is far too big for a thread's stack.
  std::unique_ptr<System> system_owner = std::make_unique<System>();
  System& system = *system_owner;
  if (system.Initialize(bios.c_str()) != 0) {
    Check(false, "the BIOS loads");
    return;
  }
  system.config().frame_limiter = true;

  ClockedDevice* device = nullptr;
  std::string opened_backend = "none yet";
  AudioOutput audio(
      [&device](const std::string& backend, std::string* opened) -> std::unique_ptr<IAudioEngine> {
        auto engine = std::make_unique<ClockedDevice>();
        engine->Initialize(emulation::psx::Spu::kSampleRate, 2);
        device = engine.get();
        *opened = backend;
        return engine;
      },
      [&opened_backend](const std::string& opened) { opened_backend = opened; });

  CountingPresenter* presenter = nullptr;
  VideoOutput video([&presenter]() -> std::unique_ptr<Presenter> {
    auto made = std::make_unique<CountingPresenter>();
    presenter = made.get();
    return made;
  });

  std::vector<MachineReport> reports;
  std::mutex reports_mutex;
  Machine::Hooks hooks;
  hooks.report = [&](const MachineReport& report) {
    std::lock_guard<std::mutex> lock(reports_mutex);
    reports.push_back(report);
  };
  Machine machine(&system, &video.frames(), &audio.samples(), hooks);

  audio.Start("clocked");
  video.Start();
  const Clock::time_point started = Clock::now();
  machine.Start(0);
  // Pausing and coming back is the commonest thing a machine does, and the one most likely to
  // leave the sound short: the ring drains while it is paused and has to be full enough the
  // moment frames start again.
  // Spaced out rather than rapid-fire, so a full second of running still falls between them:
  // the report window restarts on every resume, by design - the time spent paused is not slow
  // frames - so a machine paused twice a second would never report a frame rate at all.
  std::thread pauser([&] {
    for (int i = 0; i < 2; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1400));
      machine.Post([](Machine& m) { m.SetPaused(emulation::host::kPausedForMenu, true); });
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
      machine.Post([](Machine& m) { m.SetPaused(emulation::host::kPausedForMenu, false); });
    }
  });
  // Watch the ring's level, and say when - if ever - it ran short.
  uint64_t last_short = 0;
  int low_water = 1 << 30;
  while (MsSince(started) < 4600.0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    const int level = audio.samples().Available();
    // Only while the machine is producing: a paused one empties the ring by design, and the
    // audio thread filling that with silence is what it is for.
    if (MsSince(started) > 200.0 && audio.samples().producing())
      low_water = std::min(low_water, level);
    const uint64_t now_short = audio.samples().short_frames();
    if (now_short != last_short) {
      printf("    ran short by %llu frames at %.0f ms (ring at %d)\n",
             static_cast<unsigned long long>(now_short - last_short), MsSince(started), level);
      last_short = now_short;
    }
  }
  printf("    ring low water after the first 200 ms: %d frames (%.1f ms)\n", low_water,
         low_water * 1000.0 / 44100.0);
  pauser.join();
  video.Post([](VideoOutput& v) {
    v.presenter()->Resize(800, 600);
    v.PresentAgain();
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  machine.Stop();
  video.Stop();
  audio.Stop();

  const SampleRing& ring = audio.samples();
  std::lock_guard<std::mutex> lock(reports_mutex);
  // The last report of a *running* machine: the run pauses three times, and a pause reports
  // itself with no frame rate at all.
  double fps = 0.0;
  for (const MachineReport& report : reports) {
    if (!report.paused)
      fps = report.fps;
  }
  printf("    %zu reports, last at %.2f fps (display %.2f Hz); ring short %llu, dropped %llu\n",
         reports.size(), fps, system.gpu().refresh_hz(),
         static_cast<unsigned long long>(ring.short_frames()),
         static_cast<unsigned long long>(ring.dropped_frames()));
  printf("    video: %llu published, %d presented, %llu dropped; device underruns %d\n",
         static_cast<unsigned long long>(video.frames().published()),
         presenter ? presenter->presents.load() : -1,
         static_cast<unsigned long long>(video.frames().dropped()),
         device ? device->underruns : -1);

  Check(opened_backend == "clocked", "the audio thread opened its device and said which");
  Check(reports.size() >= 2 && fps > 58.0 && fps < 61.0,
        "the frame limiter holds the machine at the display's rate on its own thread");
  Check(ring.short_frames() == 0 && ring.dropped_frames() == 0 && device != nullptr &&
            device->underruns == 0,
        "three seconds of sound with nothing short, nothing dropped and no underrun");
  Check(presenter != nullptr && video.frames().dropped() == 0 &&
            presenter->presents.load() >= static_cast<int>(video.frames().published()),
        "every frame the machine finished was presented");
  Check(presenter != nullptr && presenter->resizes.load() == 1,
        "a resize request reaches the presenter on its own thread");
  system.Deinitialize();
}

}  // namespace

// host_test [bios]
//
// The channel checks need nothing. The thread checks run a real BIOS - the
// same one Docs/Test-Suite.md's baselines use - and are skipped, loudly,
// without one.
int main(int argc, char** argv) {
  const std::string bios = (argc > 1) ? argv[1] : "bios/SCPH1001.BIN";

  printf("host channels\n");
  DoorbellChecks();
  RequestQueueChecks();
  SampleRingChecks();
  FrameMailboxChecks();
  InputExchangeChecks();

  FILE* probe = fopen(bios.c_str(), "rb");
  if (probe != nullptr) {
    fclose(probe);
    MachineChecks(bios);
    PipelineChecks(bios);
  } else {
    printf("\nno BIOS at %s - the thread checks did not run\n", bios.c_str());
    ++failures;
  }

  printf("\n%d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
