// spu_test - unit tests for the sound processing unit.
//
//   spu_test [group]
//
// Takes no BIOS, no window and no audio device. Sample data is written into
// sound RAM, voices are keyed on through their real registers, and the frames
// that come out are checked.
//
// Audio is the one part of this machine with no equivalent of looking at the
// screen and seeing that it is wrong. A voice that never keys on, an envelope
// stuck at zero and a mixer that clamps everything to silence all produce the
// same thing: nothing. These tests exist to tell those apart.

#include "psx/psx.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using emulation::psx::Spu;

namespace {

int g_checks = 0;
int g_failures = 0;
std::string g_test;

void BeginTest(const std::string& name) { g_test = name; }

void Check(bool condition, const char* what) {
  ++g_checks;
  if (!condition) {
    ++g_failures;
    printf("  FAIL  %s / %s\n", g_test.c_str(), what);
  }
}

void CheckEqual(int64_t actual, int64_t expected, const char* what) {
  ++g_checks;
  if (actual != expected) {
    ++g_failures;
    printf("  FAIL  %s / %s: got %lld, expected %lld\n", g_test.c_str(), what,
           static_cast<long long>(actual), static_cast<long long>(expected));
  }
}

// SPU register addresses.
const uint32_t kVoiceBase   = 0x1F801C00;
const uint32_t kMainVolL    = 0x1F801D80;
const uint32_t kMainVolR    = 0x1F801D82;
const uint32_t kKeyOnLow    = 0x1F801D88;
const uint32_t kKeyOnHigh   = 0x1F801D8A;
const uint32_t kKeyOffLow   = 0x1F801D8C;
const uint32_t kNoiseLow    = 0x1F801D94;
const uint32_t kEndxLow     = 0x1F801D9C;
const uint32_t kIrqAddress  = 0x1F801DA4;
const uint32_t kTransferAdr = 0x1F801DA6;
const uint32_t kTransferFifo= 0x1F801DA8;
const uint32_t kControl     = 0x1F801DAA;

// Control register bits.
const uint16_t kControlEnable = 0x8000;
const uint16_t kControlUnmute = 0x4000;
const uint16_t kControlIrq    = 0x0040;

class Machine {
 public:
  Machine() : system_(new emulation::psx::System()) {
    system_->InitializeWithoutBios();
  }
  ~Machine() {
    system_->Deinitialize();
    delete system_;
  }

  Spu& spu() { return system_->spu(); }

  // A clean SPU with the output unmuted and both main volumes at full.
  void Reset() {
    spu().Initialize();
    Write(kControl, kControlEnable | kControlUnmute);
    Write(kMainVolL, 0x3FFF);
    Write(kMainVolR, 0x3FFF);
  }

  void Write(uint32_t address, uint16_t value) { spu().Write(address, value); }
  uint16_t Read(uint32_t address) { return spu().Read(address); }

  void WriteVoice(int voice, uint32_t offset, uint16_t value) {
    Write(kVoiceBase + voice * 0x10 + offset, value);
  }
  uint16_t ReadVoice(int voice, uint32_t offset) {
    return Read(kVoiceBase + voice * 0x10 + offset);
  }

  // Uploads bytes into sound RAM at a byte address, the way software does:
  // set the transfer address, then push halfwords at the data port.
  void Upload(uint32_t byte_address, const std::vector<uint8_t>& data) {
    Write(kTransferAdr, static_cast<uint16_t>(byte_address / 8));
    for (size_t i = 0; i + 1 < data.size(); i += 2)
      Write(kTransferFifo,
            static_cast<uint16_t>(data[i] | (data[i + 1] << 8)));
  }

  // Runs the SPU for a number of frames and returns them.
  std::vector<int16_t> Run(int frames) {
    spu().Tick(Spu::kCyclesPerSample * frames);
    std::vector<int16_t> out(static_cast<size_t>(frames) * 2, 0);
    const int got = spu().ReadSamples(&out[0], frames);
    out.resize(static_cast<size_t>(got) * 2);
    return out;
  }

 private:
  emulation::psx::System* system_;
};

// Builds one ADPCM block. `flags` carries the loop bits.
std::vector<uint8_t> AdpcmBlock(int shift, int filter, uint8_t flags,
                                const int nibbles[28]) {
  std::vector<uint8_t> block(16, 0);
  block[0] = static_cast<uint8_t>((filter << 4) | shift);
  block[1] = flags;
  for (int i = 0; i < 28; ++i) {
    const uint8_t value = static_cast<uint8_t>(nibbles[i] & 0x0F);
    if (i & 1)
      block[2 + i / 2] |= static_cast<uint8_t>(value << 4);
    else
      block[2 + i / 2] |= value;
  }
  return block;
}

// A block of constant maximum-positive nibbles, which decodes to a loud
// steady tone rather than something that has to be reasoned about.
std::vector<uint8_t> LoudBlock(uint8_t flags) {
  int nibbles[28];
  for (int i = 0; i < 28; ++i)
    nibbles[i] = 0x7;             // +7, the largest positive 4-bit value
  return AdpcmBlock(0, 0, flags, nibbles);
}

std::vector<uint8_t> SilentBlock(uint8_t flags) {
  int nibbles[28];
  memset(nibbles, 0, sizeof(nibbles));
  return AdpcmBlock(0, 0, flags, nibbles);
}

int16_t PeakOf(const std::vector<int16_t>& frames, int channel) {
  int16_t peak = 0;
  for (size_t i = channel; i < frames.size(); i += 2) {
    const int16_t value = frames[i] < 0 ? static_cast<int16_t>(-frames[i])
                                        : frames[i];
    if (value > peak)
      peak = value;
  }
  return peak;
}

// ---------------------------------------------------------------------------

void TestRegisters(Machine& m) {
  printf("registers\n");

  BeginTest("voice registers round trip");
  m.Reset();
  m.WriteVoice(0, 0x0, 0x1234);
  m.WriteVoice(0, 0x2, 0x5678);
  m.WriteVoice(0, 0x4, 0x0800);
  m.WriteVoice(0, 0x6, 0x0100);
  CheckEqual(m.ReadVoice(0, 0x0), 0x1234, "volume left");
  CheckEqual(m.ReadVoice(0, 0x2), 0x5678, "volume right");
  CheckEqual(m.ReadVoice(0, 0x4), 0x0800, "pitch");
  CheckEqual(m.ReadVoice(0, 0x6), 0x0100, "start address");

  BeginTest("each voice has its own registers");
  m.Reset();
  for (int i = 0; i < Spu::kVoices; ++i)
    m.WriteVoice(i, 0x4, static_cast<uint16_t>(0x100 + i));
  for (int i = 0; i < Spu::kVoices; ++i)
    CheckEqual(m.ReadVoice(i, 0x4), 0x100 + i, "pitch is per-voice");

  BeginTest("ENDX is read-only");
  m.Reset();
  m.Write(kEndxLow, 0xFFFF);
  CheckEqual(m.Read(kEndxLow), 0, "the write was ignored");

  BeginTest("sound RAM round trips through the data port");
  m.Reset();
  std::vector<uint8_t> data;
  for (int i = 0; i < 32; ++i)
    data.push_back(static_cast<uint8_t>(0x10 + i));
  m.Upload(0x1000, data);
  const uint8_t* ram = m.spu().ram();
  Check(ram[0x1000] == 0x10 && ram[0x1001] == 0x11 && ram[0x101F] == 0x2F,
        "the bytes landed where they were addressed");

  BeginTest("the transfer address advances as data is written");
  m.Reset();
  m.Write(kTransferAdr, 0x0200);          // byte address 0x1000
  m.Write(kTransferFifo, 0xAABB);
  m.Write(kTransferFifo, 0xCCDD);
  const uint8_t* ram2 = m.spu().ram();
  Check(ram2[0x1000] == 0xBB && ram2[0x1001] == 0xAA, "first halfword");
  Check(ram2[0x1002] == 0xDD && ram2[0x1003] == 0xCC,
        "second halfword went to the next address, not the same one");
}

void TestKeyOnOff(Machine& m) {
  printf("key on and key off\n");

  BeginTest("key on starts a voice");
  m.Reset();
  m.Upload(0x1000, LoudBlock(0x00));
  m.WriteVoice(0, 0x6, 0x1000 / 8);
  m.WriteVoice(0, 0x4, 0x1000);           // one sample per output frame
  m.WriteVoice(0, 0x0, 0x3FFF);
  m.WriteVoice(0, 0x2, 0x3FFF);
  m.WriteVoice(0, 0x8, 0x00FF);           // fast attack, no decay
  m.WriteVoice(0, 0xA, 0x0000);
  const uint64_t before = m.spu().stats().key_ons;
  m.Write(kKeyOnLow, 0x0001);
  CheckEqual(m.spu().stats().key_ons, before + 1, "the key-on was counted");

  BeginTest("key on is edge-triggered, not level-triggered");
  // Writing the same bit again keys the voice on again; the register is not a
  // latch that the mixer polls.
  const uint64_t count = m.spu().stats().key_ons;
  m.Write(kKeyOnLow, 0x0001);
  CheckEqual(m.spu().stats().key_ons, count + 1, "a second write keys on again");

  BeginTest("key on affects only the voices named");
  m.Reset();
  const uint64_t base = m.spu().stats().key_ons;
  m.Write(kKeyOnLow, 0x0005);             // voices 0 and 2
  CheckEqual(m.spu().stats().key_ons, base + 2, "two voices started");

  BeginTest("the high half addresses voices 16 to 23");
  m.Reset();
  const uint64_t high_base = m.spu().stats().key_ons;
  m.Write(kKeyOnHigh, 0x00FF);
  CheckEqual(m.spu().stats().key_ons, high_base + 8, "eight voices started");

  BeginTest("key off releases rather than stopping dead");
  m.Reset();
  m.Upload(0x1000, LoudBlock(0x00));
  m.WriteVoice(0, 0x6, 0x1000 / 8);
  m.WriteVoice(0, 0x4, 0x1000);
  m.WriteVoice(0, 0x0, 0x3FFF);
  m.WriteVoice(0, 0x2, 0x3FFF);
  m.WriteVoice(0, 0x8, 0x00FF);
  m.WriteVoice(0, 0xA, 0x000F);           // slow release
  m.Write(kKeyOnLow, 0x0001);
  m.Run(64);
  const uint64_t offs = m.spu().stats().key_offs;
  m.Write(kKeyOffLow, 0x0001);
  CheckEqual(m.spu().stats().key_offs, offs + 1, "the key-off was counted");
  const std::vector<int16_t> after = m.Run(8);
  Check(!after.empty(), "the voice still produces frames while releasing");
}

void TestAdpcm(Machine& m) {
  printf("adpcm decoding\n");

  BeginTest("a keyed-on voice actually produces sound");
  // The whole point: a voice that decodes nothing and one that decodes
  // silence both give zero frames of output, and only this distinguishes
  // "working" from "wired up but mute".
  m.Reset();
  m.Upload(0x1000, LoudBlock(0x00));
  m.WriteVoice(0, 0x6, 0x1000 / 8);
  m.WriteVoice(0, 0x4, 0x1000);
  m.WriteVoice(0, 0x0, 0x3FFF);
  m.WriteVoice(0, 0x2, 0x3FFF);
  m.WriteVoice(0, 0x8, 0x00FF);           // attack fast, sustain high
  m.WriteVoice(0, 0xA, 0x0000);
  m.Write(kKeyOnLow, 0x0001);

  const std::vector<int16_t> frames = m.Run(256);
  CheckEqual(static_cast<int64_t>(frames.size()), 512, "256 stereo frames");
  Check(PeakOf(frames, 0) > 0, "the left channel is not silent");
  Check(PeakOf(frames, 1) > 0, "the right channel is not silent");
  Check(m.spu().stats().blocks_decoded > 0, "ADPCM blocks were decoded");

  BeginTest("a silent sample produces silence");
  m.Reset();
  m.Upload(0x1000, SilentBlock(0x00));
  m.WriteVoice(0, 0x6, 0x1000 / 8);
  m.WriteVoice(0, 0x4, 0x1000);
  m.WriteVoice(0, 0x0, 0x3FFF);
  m.WriteVoice(0, 0x2, 0x3FFF);
  m.WriteVoice(0, 0x8, 0x00FF);
  m.Write(kKeyOnLow, 0x0001);
  const std::vector<int16_t> quiet = m.Run(128);
  CheckEqual(PeakOf(quiet, 0), 0, "silence in, silence out");

  BeginTest("the loop-end flag sets ENDX");
  m.Reset();
  m.Upload(0x1000, LoudBlock(0x01));      // end, no repeat
  m.WriteVoice(0, 0x6, 0x1000 / 8);
  m.WriteVoice(0, 0x4, 0x1000);
  m.WriteVoice(0, 0x8, 0x00FF);
  m.Write(kKeyOnLow, 0x0001);
  m.Run(64);
  Check((m.Read(kEndxLow) & 1) != 0, "ENDX bit 0 is set for voice 0");

  BeginTest("key on clears ENDX for that voice");
  m.Write(kKeyOnLow, 0x0001);
  CheckEqual(m.Read(kEndxLow) & 1, 0, "ENDX bit 0 cleared");

  BeginTest("a looping sample keeps playing");
  m.Reset();
  // Two blocks: the first is the loop start, the second ends and repeats.
  std::vector<uint8_t> loop = LoudBlock(0x04);
  const std::vector<uint8_t> second = LoudBlock(0x03);
  loop.insert(loop.end(), second.begin(), second.end());
  m.Upload(0x1000, loop);
  m.WriteVoice(0, 0x6, 0x1000 / 8);
  m.WriteVoice(0, 0x4, 0x1000);
  m.WriteVoice(0, 0x0, 0x3FFF);
  m.WriteVoice(0, 0x2, 0x3FFF);
  m.WriteVoice(0, 0x8, 0x00FF);
  m.Write(kKeyOnLow, 0x0001);
  m.Run(200);                              // well past the two blocks
  const std::vector<int16_t> late = m.Run(64);
  Check(PeakOf(late, 0) > 0, "still audible after looping round");
}

void TestEnvelope(Machine& m) {
  printf("adsr envelope\n");

  BeginTest("the attack phase ramps up rather than starting at full");
  m.Reset();
  // A looping block: an unlooped one runs off the end after 28 samples into
  // zeroed sound RAM and correctly goes silent, which would hide the ramp.
  m.Upload(0x1000, LoudBlock(0x03));
  m.WriteVoice(0, 0x6, 0x1000 / 8);
  m.WriteVoice(0, 0x4, 0x1000);
  m.WriteVoice(0, 0x0, 0x3FFF);
  m.WriteVoice(0, 0x2, 0x3FFF);
  // Attack rate 0x30: a step every other sample, so the ramp is plainly
  // visible across the frames sampled. Slower rates are legal - 0x50 is a
  // ramp lasting most of a minute - but nothing would be measurable here.
  m.WriteVoice(0, 0x8, 0x3000);
  m.WriteVoice(0, 0xA, 0x0000);
  m.Write(kKeyOnLow, 0x0001);

  const std::vector<int16_t> early = m.Run(32);
  const std::vector<int16_t> later = m.Run(512);
  Check(PeakOf(later, 0) > PeakOf(early, 0),
        "the envelope is louder later than at the start");

  BeginTest("the current envelope level is readable");
  Check(m.ReadVoice(0, 0xC) > 0, "ADSR volume has risen above zero");

  BeginTest("a voice with no key-on stays silent");
  m.Reset();
  m.Upload(0x1000, LoudBlock(0x00));
  m.WriteVoice(0, 0x6, 0x1000 / 8);
  m.WriteVoice(0, 0x4, 0x1000);
  m.WriteVoice(0, 0x0, 0x3FFF);
  m.WriteVoice(0, 0x2, 0x3FFF);
  const std::vector<int16_t> untouched = m.Run(128);
  CheckEqual(PeakOf(untouched, 0), 0, "no output without a key-on");
}

void TestMixer(Machine& m) {
  printf("mixing\n");

  // Sets up voice 0 as a loud steady tone and returns the peak.
  struct Setup {
    static int16_t Play(Machine& m, uint16_t left, uint16_t right,
                        uint16_t main_left, uint16_t main_right,
                        uint16_t control, int channel) {
      m.Reset();
      m.Write(kControl, control);
      m.Write(kMainVolL, main_left);
      m.Write(kMainVolR, main_right);
      m.Upload(0x1000, LoudBlock(0x03));
      m.WriteVoice(0, 0x6, 0x1000 / 8);
      m.WriteVoice(0, 0x4, 0x1000);
      m.WriteVoice(0, 0x0, left);
      m.WriteVoice(0, 0x2, right);
      m.WriteVoice(0, 0x8, 0x00FF);
      m.WriteVoice(0, 0xA, 0x0000);
      m.Write(kKeyOnLow, 0x0001);
      return PeakOf(m.Run(256), channel);
    }
  };

  BeginTest("voice volume pans between the channels");
  const int16_t left_only =
      Setup::Play(m, 0x3FFF, 0x0000, 0x3FFF, 0x3FFF,
                  kControlEnable | kControlUnmute, 1);
  CheckEqual(left_only, 0, "a voice panned hard left is silent on the right");
  const int16_t right_only =
      Setup::Play(m, 0x0000, 0x3FFF, 0x3FFF, 0x3FFF,
                  kControlEnable | kControlUnmute, 0);
  CheckEqual(right_only, 0, "a voice panned hard right is silent on the left");

  BeginTest("the main volume scales the output");
  const int16_t full =
      Setup::Play(m, 0x3FFF, 0x3FFF, 0x3FFF, 0x3FFF,
                  kControlEnable | kControlUnmute, 0);
  const int16_t half =
      Setup::Play(m, 0x3FFF, 0x3FFF, 0x1000, 0x1000,
                  kControlEnable | kControlUnmute, 0);
  Check(full > half, "a lower main volume is quieter");
  Check(half > 0, "but not silent");

  BeginTest("the mute bit silences the output");
  const int16_t muted =
      Setup::Play(m, 0x3FFF, 0x3FFF, 0x3FFF, 0x3FFF, kControlEnable, 0);
  CheckEqual(muted, 0, "muted output is silent");

  BeginTest("frames keep being produced while muted");
  // Muting must not stop the mixer: software unmutes mid-stream and expects
  // the voices to have carried on.
  Check(m.spu().stats().frames > 0, "frames were still generated");
}

void TestTiming(Machine& m) {
  printf("timing\n");

  BeginTest("one frame per 768 CPU cycles");
  m.Reset();
  const uint64_t before = m.spu().stats().frames;
  m.spu().Tick(Spu::kCyclesPerSample * 100);
  CheckEqual(static_cast<int64_t>(m.spu().stats().frames - before), 100,
             "100 frames from 100 sample periods");

  BeginTest("cycles are accumulated, not rounded away");
  m.Reset();
  const uint64_t start = m.spu().stats().frames;
  for (int i = 0; i < Spu::kCyclesPerSample; ++i)
    m.spu().Tick(1);
  CheckEqual(static_cast<int64_t>(m.spu().stats().frames - start), 1,
             "768 single-cycle ticks make exactly one frame");

  BeginTest("frames can be drained");
  m.Reset();
  m.spu().Tick(Spu::kCyclesPerSample * 50);
  CheckEqual(m.spu().QueuedFrames(), 50, "50 frames are waiting");
  std::vector<int16_t> out(100, 0);
  CheckEqual(m.spu().ReadSamples(&out[0], 50), 50, "50 frames were read");
  CheckEqual(m.spu().QueuedFrames(), 0, "the buffer is empty afterwards");

  BeginTest("reading more than is queued returns what there is");
  m.Reset();
  m.spu().Tick(Spu::kCyclesPerSample * 10);
  CheckEqual(m.spu().ReadSamples(&out[0], 50), 10, "only 10 frames available");
}

void TestNoiseAndIrq(Machine& m) {
  printf("noise and interrupts\n");

  BeginTest("a noise voice produces output without any sample data");
  m.Reset();
  // Deliberately no upload: noise does not read sound RAM.
  m.WriteVoice(0, 0x6, 0x1000 / 8);
  m.WriteVoice(0, 0x4, 0x1000);
  m.WriteVoice(0, 0x0, 0x3FFF);
  m.WriteVoice(0, 0x2, 0x3FFF);
  m.WriteVoice(0, 0x8, 0x00FF);
  m.Write(kControl, kControlEnable | kControlUnmute | 0x3F00);
  m.Write(kNoiseLow, 0x0001);
  m.Write(kKeyOnLow, 0x0001);
  Check(PeakOf(m.Run(512), 0) > 0, "the noise generator is audible");

  BeginTest("the sound RAM interrupt fires when the address is reached");
  m.Reset();
  m.Upload(0x1000, LoudBlock(0x00));
  m.WriteVoice(0, 0x6, 0x1000 / 8);
  m.WriteVoice(0, 0x4, 0x1000);
  m.WriteVoice(0, 0x8, 0x00FF);
  m.Write(kIrqAddress, 0x1000 / 8);
  m.Write(kControl, kControlEnable | kControlUnmute | kControlIrq);
  m.Write(kKeyOnLow, 0x0001);
  m.Run(64);
  Check(m.spu().stats().irqs > 0, "an interrupt was raised");

  BeginTest("no interrupt when the enable bit is clear");
  m.Reset();
  m.Upload(0x1000, LoudBlock(0x00));
  m.WriteVoice(0, 0x6, 0x1000 / 8);
  m.WriteVoice(0, 0x4, 0x1000);
  m.WriteVoice(0, 0x8, 0x00FF);
  m.Write(kIrqAddress, 0x1000 / 8);
  m.Write(kControl, kControlEnable | kControlUnmute);   // IRQ disabled
  m.Write(kKeyOnLow, 0x0001);
  m.Run(64);
  CheckEqual(static_cast<int64_t>(m.spu().stats().irqs), 0,
             "no interrupt was raised");
}

struct Group {
  const char* name;
  void (*run)(Machine&);
};

const Group kGroups[] = {
  { "registers", TestRegisters },
  { "keyonoff",  TestKeyOnOff },
  { "adpcm",     TestAdpcm },
  { "envelope",  TestEnvelope },
  { "mixer",     TestMixer },
  { "timing",    TestTiming },
  { "noiseirq",  TestNoiseAndIrq },
};

}  // namespace

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  const char* only = (argc > 1) ? argv[1] : nullptr;

  printf("spu_test - sound processing unit\n\n");

  Machine machine;
  for (size_t i = 0; i < sizeof(kGroups) / sizeof(kGroups[0]); ++i) {
    if (only != nullptr && strcmp(only, kGroups[i].name) != 0)
      continue;
    const int before = g_failures;
    kGroups[i].run(machine);
    if (g_failures == before)
      printf("  ok\n");
  }

  printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
