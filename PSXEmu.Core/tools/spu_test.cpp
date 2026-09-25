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
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using emulation::psx::Cdrom;
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
const uint32_t kReverbVolL  = 0x1F801D84;
const uint32_t kReverbVolR  = 0x1F801D86;
const uint32_t kReverbOnLow = 0x1F801D98;
const uint32_t kReverbBase  = 0x1F801DA2;
const uint32_t kCdVolL      = 0x1F801DB0;
const uint32_t kCdVolR      = 0x1F801DB2;
const uint32_t kMainNowL    = 0x1F801DB8;   // where the main volume has got to
const uint32_t kReverbRegs  = 0x1F801DC0;   // 32 of them, dAPF1 first
const uint32_t kVoiceNow    = 0x1F801E00;   // each voice's current left/right volume

// Control register bits.
const uint16_t kControlEnable = 0x8000;
const uint16_t kControlUnmute = 0x4000;
const uint16_t kControlIrq    = 0x0040;
const uint16_t kControlReverb = 0x0080;   // the reverb master enable
const uint16_t kControlCd     = 0x0001;   // CD audio into the mix

class Machine {
 public:
  Machine() : system_(new emulation::psx::System()) {
    system_->InitializeWithoutBios();
    // The front end's own gain sits on top of the hardware's main volume, and
    // what is under test here is the hardware. Left at its default these
    // checks measure that default as much as the mixer - which is how the
    // main volume coming out at half went unseen: the default was 2x, and the
    // two cancelled exactly.
    system_->config().audio_volume = 1.0f;
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

// How many of these frames are effectively silent. The counterpart to PeakOf:
// a loop that takes in a block it should not is not quieter overall, it is
// intermittently silent, and a peak cannot see that at all.
int QuietFrames(const std::vector<int16_t>& frames, int channel,
                int16_t threshold) {
  int quiet = 0;
  for (size_t i = channel; i < frames.size(); i += 2) {
    const int16_t value = frames[i] < 0 ? static_cast<int16_t>(-frames[i])
                                        : frames[i];
    if (value < threshold)
      ++quiet;
  }
  return quiet;
}

// Where a voice loops back to is set two ways - by a block carrying the
// loop-start flag, and by software writing the repeat address itself - and
// which of them wins, when, is the whole of bug 39. Final Fantasy VII sets
// the loop point by register and then keys on, for every note of its prelude;
// a key-on that resets the repeat address to the start address throws that
// away and loops over three blocks where the game asked for one, playing the
// whole piece a twelfth flat through a waveform it never asked for.
void TestLoopAddress(Machine& m) {
  printf("adpcm loop address\n");

  BeginTest("a repeat address written before key-on survives the key-on");
  m.Reset();
  // Final Fantasy VII's shape exactly: silence, silence, then the waveform,
  // with the loop pointed at the waveform alone and nothing in the data
  // saying where the loop starts.
  m.Upload(0x1000, SilentBlock(0x00));
  m.Upload(0x1010, SilentBlock(0x00));
  m.Upload(0x1020, LoudBlock(0x03));       // end and repeat, no loop-start
  m.WriteVoice(0, 0x6, 0x1000 / 8);        // start at the silence
  m.WriteVoice(0, 0xE, 0x1020 / 8);        // loop the waveform only
  m.WriteVoice(0, 0x4, 0x1000);            // one sample per output frame
  m.WriteVoice(0, 0x0, 0x3FFF);
  m.WriteVoice(0, 0x2, 0x3FFF);
  m.WriteVoice(0, 0x8, 0x00FF);            // fast attack, no decay
  m.Write(kKeyOnLow, 0x0001);
  m.Run(128);                              // past the silence and settled
  const std::vector<int16_t> looped = m.Run(112);   // four blocks' worth
  CheckEqual(QuietFrames(looped, 0, 1000), 0,
             "no silence once looping - the loop is the waveform alone");
  Check(PeakOf(looped, 0) > 1000, "and it is audible");

  BeginTest("the repeat address reads back what was written, after key-on");
  CheckEqual(m.ReadVoice(0, 0xE), 0x1020 / 8, "repeat address unchanged");

  BeginTest("without one written, the loop-start flag still sets it");
  m.Reset();
  m.Upload(0x1000, SilentBlock(0x00));
  m.Upload(0x1010, LoudBlock(0x04));       // loop starts here
  m.Upload(0x1020, LoudBlock(0x03));       // end and repeat
  m.WriteVoice(0, 0x6, 0x1000 / 8);
  m.WriteVoice(0, 0x4, 0x1000);
  m.WriteVoice(0, 0x0, 0x3FFF);
  m.WriteVoice(0, 0x2, 0x3FFF);
  m.WriteVoice(0, 0x8, 0x00FF);
  m.Write(kKeyOnLow, 0x0001);
  m.Run(128);
  const std::vector<int16_t> flagged = m.Run(112);
  CheckEqual(QuietFrames(flagged, 0, 1000), 0,
             "the flag excludes the leading silence from the loop");
  CheckEqual(m.ReadVoice(0, 0xE), 0x1010 / 8,
             "the repeat address is the flagged block");

  BeginTest("software's repeat address outranks the loop-start flag");
  // Same three blocks, but the loop is redirected back over the silence while
  // the voice is running. The flagged block is inside the new loop, so the
  // flag gets a chance to take the loop point back every time round - and
  // must not, until the voice is keyed on again.
  m.WriteVoice(0, 0xE, 0x1000 / 8);
  m.Run(256);                              // let it come round several times
  const std::vector<int16_t> redirected = m.Run(112);
  Check(QuietFrames(redirected, 0, 1000) > 0,
        "the silence is back in the loop and stays there");
  CheckEqual(m.ReadVoice(0, 0xE), 0x1000 / 8,
             "the flag did not take the loop point back");

  BeginTest("a key-on lets the loop-start flag win again");
  m.Write(kKeyOnLow, 0x0001);
  m.Run(128);
  const std::vector<int16_t> rekeyed = m.Run(112);
  CheckEqual(QuietFrames(rekeyed, 0, 1000), 0,
             "the flagged loop point is in force after keying on");
}

void TestEnvelope(Machine& m) {
  printf("adsr envelope\n");

  BeginTest("the attack phase ramps up rather than starting at full");
  m.Reset();
  // A looping block: an unlooped one runs off the end after 28 samples into
  // zeroed sound RAM and correctly goes silent, which would hide the ramp.
  // Loop-start as well as end-and-repeat (07h, not 03h) - a block that ends
  // and repeats without ever saying where the loop begins does not loop back
  // to itself on hardware, it loops to whatever the repeat address happens to
  // hold. See bug 39.
  m.Upload(0x1000, LoudBlock(0x07));
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

// ---------------------------------------------------------------------------
// Volume sweeps
//
// Bit 15 of a volume register makes it a sweep: the level moves every sample
// by the same envelope ADSR uses. The expected values are worked from psx-spx's
// rate encoding (bits 0-1 the step, 2-6 the shift) and the hardware behaviour
// DuckStation measured, and the code matches DuckStation's sweep exactly over
// every register value (Bugs-Found 101). Rate 28h is used throughout because it
// steps every sample by an amount easy to follow: 7 or -8, shifted left once.

int16_t MainNow(Machine& m) { return static_cast<int16_t>(m.Read(kMainNowL)); }

void TestSweep(Machine& m) {
  printf("volume sweeps\n");

  BeginTest("a fixed volume is its register doubled, at once");
  m.Reset();
  m.Write(kMainVolL, 0x3FFF);
  CheckEqual(MainNow(m), 0x7FFE, "3FFFh is 7FFEh");
  m.Write(kMainVolL, 0x4000);
  CheckEqual(MainNow(m), -0x8000, "bit 14 is the sign: 4000h is -8000h");

  BeginTest("a linear increase steps every sample and stops at the top");
  m.Reset();
  m.Write(kMainVolL, 0x0000);
  m.Write(kMainVolL, 0x8028);         // sweep, increase, rate 28h: +14 a sample
  m.Run(10);
  CheckEqual(MainNow(m), 140, "ten samples on, 10 x 14");
  m.Run(3000);
  CheckEqual(MainNow(m), 0x7FFF, "and it holds at 7FFFh");

  BeginTest("a linear decrease stops at zero");
  // It used to carry on past zero to full volume with the phase inverted, so
  // a fade-out ended loud.
  m.Reset();
  m.Write(kMainVolL, 0x1000);         // 2000h
  m.Write(kMainVolL, 0xA028);         // sweep, decrease, rate 28h: -16 a sample
  m.Run(10);
  CheckEqual(MainNow(m), 0x2000 - 160, "ten samples on, 10 x -16");
  m.Run(1000);
  CheckEqual(MainNow(m), 0, "and it rests at zero");

  BeginTest("a slow rate steps less often, and a write starts it afresh");
  m.Reset();
  m.Write(kMainVolL, 0x0000);
  m.Write(kMainVolL, 0x8030);         // rate 30h: +7 every second sample
  m.Run(10);
  CheckEqual(MainNow(m), 35, "five steps in ten samples");
  m.Write(kMainVolL, 0x8030);
  m.Run(1);
  CheckEqual(MainNow(m), 35, "rewritten, the first sample is half way to a step");
  m.Run(1);
  CheckEqual(MainNow(m), 42, "and the second takes it");

  BeginTest("an exponential decrease shrinks with the level and ends at zero");
  m.Reset();
  m.Write(kMainVolL, 0x3FFF);         // 7FFEh
  m.Write(kMainVolL, 0xE028);         // sweep, exponential, decrease, rate 28h
  m.Run(1);
  CheckEqual(MainNow(m), 0x7FFE - 16, "-16 x 7FFEh / 8000h, rounded down");
  m.Run(20000);
  CheckEqual(MainNow(m), 0, "it reaches zero and stays");

  BeginTest("an exponential increase slows above 6000h");
  m.Reset();
  m.Write(kMainVolL, 0x2FF8);         // 5FF0h
  m.Write(kMainVolL, 0xC028);         // sweep, exponential, increase, rate 28h
  m.Run(2);
  CheckEqual(MainNow(m), 0x5FF0 + 28, "+14 a sample below 6000h");
  m.Run(2);
  CheckEqual(MainNow(m), 0x5FF0 + 28 + 7, "then +7 every other sample");
  m.Run(2);
  CheckEqual(MainNow(m), 0x5FF0 + 28 + 14, "and again");

  BeginTest("the phase bit turns an increase toward -8000h");
  m.Reset();
  m.Write(kMainVolL, 0x0000);
  m.Write(kMainVolL, 0x9028);         // sweep, increase, inverted phase
  m.Run(10);
  // Inverted, the step is the bitwise NOT of +7: -8, shifted to -16.
  CheckEqual(MainNow(m), -160, "ten samples on, 10 x -16");
  m.Run(3000);
  CheckEqual(MainNow(m), -0x8000, "and it holds at -8000h");

  BeginTest("the phase bit turns a decrease toward zero from below");
  // It used to flip the step, which sent this one up past zero to 7FFFh.
  m.Reset();
  m.Write(kMainVolL, 0x7000);         // -2000h
  m.Write(kMainVolL, 0xB028);         // sweep, decrease, inverted phase
  m.Run(10);
  CheckEqual(MainNow(m), -0x2000 + 140, "rising by 14 a sample");
  m.Run(1000);
  CheckEqual(MainNow(m), 0, "and resting at zero");

  BeginTest("the phase bit does nothing to an exponential decrease");
  m.Reset();
  m.Write(kMainVolL, 0x3FFF);
  m.Write(kMainVolL, 0xF028);         // as above, with the phase bit
  m.Run(1);
  CheckEqual(MainNow(m), 0x7FFE - 16, "the same first step");

  BeginTest("rate 7Fh never moves");
  m.Reset();
  m.Write(kMainVolL, 0x1000);
  m.Write(kMainVolL, 0x807F);
  m.Run(500);
  CheckEqual(MainNow(m), 0x2000, "still where it started");

  BeginTest("each voice's current volume can be read back");
  m.Reset();
  m.WriteVoice(3, 0x0, 0x1234);
  m.WriteVoice(3, 0x2, 0x7000);
  CheckEqual(static_cast<int16_t>(m.Read(kVoiceNow + 3 * 4)), 0x2468, "voice 3 left");
  CheckEqual(static_cast<int16_t>(m.Read(kVoiceNow + 3 * 4 + 2)), -0x2000, "voice 3 right");

  BeginTest("a voice's sweep is what it is mixed at");
  m.Reset();
  m.Upload(0x1000, LoudBlock(0x07));   // loops on itself, so it keeps sounding
  m.WriteVoice(0, 0x6, 0x1000 / 8);
  m.WriteVoice(0, 0x4, 0x1000);
  m.WriteVoice(0, 0x0, 0x0000);
  m.WriteVoice(0, 0x0, 0x8030);        // left: up from silence, slowly
  m.WriteVoice(0, 0x2, 0x0000);
  m.WriteVoice(0, 0x8, 0x00FF);
  m.WriteVoice(0, 0xA, 0x0000);
  m.Write(kKeyOnLow, 0x0001);
  const int16_t early = PeakOf(m.Run(64), 0);
  m.Run(4000);
  const int16_t late = PeakOf(m.Run(64), 0);
  Check(early < 400, "quiet while the sweep is low");
  Check(late > 4 * early + 1000, "louder as it rises");
}

// ---------------------------------------------------------------------------
// Reverb
//
// A network plain enough to follow by hand: vIIR and vCOMB1 at full, vLIN and
// vRIN at full, every all-pass and wall coefficient zero. The same-side
// reflection then writes the input into mSAME, the first comb tap reads it
// straight back, and each all-pass stage delays it by one reverb step - so a
// steady input comes out of the reverb at the level it went in. Every stream
// the network writes is 2,048 halfwords from the next, so none overwrites
// another within the runs below. `scale` shrinks the addresses for a small
// work area: the hardware wraps an address back into the area once, by adding
// mBASE, so one larger than the area points outside it.

const uint32_t kSameL = 0x100, kSameR = 0x300, kDiffL = 0x500, kDiffR = 0x700;
const uint32_t kApf1L = 0x900, kApf1R = 0xB00, kApf2L = 0xD00, kApf2R = 0xF00;

void PlainReverb(Machine& m, uint16_t base_units, uint32_t scale = 1) {
  uint16_t regs[32] = {};
  regs[0] = 1; regs[1] = 1;               // dAPF1, dAPF2: each all-pass stage reads what it wrote four steps ago
  regs[2] = 0x7FFF;                       // vIIR
  regs[3] = 0x7FFF;                       // vCOMB1
  regs[10] = kSameL / scale; regs[11] = kSameR / scale;   // mLSAME, mRSAME
  regs[12] = kSameL / scale; regs[13] = kSameR / scale;   // mLCOMB1, mRCOMB1: read it straight back
  regs[18] = kDiffL / scale; regs[19] = kDiffR / scale;   // mLDIFF, mRDIFF
  regs[26] = kApf1L / scale; regs[27] = kApf1R / scale;   // mLAPF1, mRAPF1
  regs[28] = kApf2L / scale; regs[29] = kApf2R / scale;   // mLAPF2, mRAPF2
  regs[30] = 0x7FFF; regs[31] = 0x7FFF;   // vLIN, vRIN
  for (int i = 0; i < 32; ++i)
    m.Write(kReverbRegs + i * 2, regs[i]);
  m.Write(kReverbBase, base_units);
}

// Voice 0 as a steady tone at a quarter of full volume, optionally sent to
// the reverb. The block carries the loop-start flag as well as end and repeat,
// so it loops on itself rather than jumping to address 0 and going quiet.
void SteadyVoice(Machine& m, bool to_reverb) {
  m.Upload(0x1000, LoudBlock(0x07));
  m.WriteVoice(0, 0x6, 0x1000 / 8);
  m.WriteVoice(0, 0x4, 0x1000);
  m.WriteVoice(0, 0x0, 0x1000);
  m.WriteVoice(0, 0x2, 0x1000);
  m.WriteVoice(0, 0x8, 0x00FF);
  m.WriteVoice(0, 0xA, 0x0000);
  m.Write(kReverbOnLow, to_reverb ? 0x0001 : 0x0000);
  m.Write(kKeyOnLow, 0x0001);
}

int16_t RamHalf(Machine& m, uint32_t byte_address) {
  const uint8_t* ram = m.spu().ram();
  return static_cast<int16_t>(ram[byte_address] | (ram[byte_address + 1] << 8));
}

double AverageOf(const std::vector<int16_t>& frames, int channel) {
  double sum = 0;
  int n = 0;
  for (size_t i = channel; i < frames.size(); i += 2, ++n)
    sum += frames[i];
  return n ? sum / n : 0.0;
}

void TestReverb(Machine& m) {
  printf("reverb\n");
  const uint16_t kBase = 0x7000;          // byte 38000h, well clear of the sample
  const uint32_t base = kBase * 8u;

  BeginTest("nothing in, nothing out");
  m.Reset();
  m.Write(kControl, kControlEnable | kControlUnmute | kControlReverb);
  m.Write(kReverbVolL, 0x7FFF);
  m.Write(kReverbVolR, 0x7FFF);
  PlainReverb(m, kBase);
  CheckEqual(PeakOf(m.Run(2000), 0), 0, "silence");

  BeginTest("a voice sent to the reverb comes back at the level it went in");
  m.Reset();
  m.Write(kControl, kControlEnable | kControlUnmute | kControlReverb);
  m.Write(kReverbVolL, 0x7FFF);
  m.Write(kReverbVolR, 0x7FFF);
  PlainReverb(m, kBase);
  SteadyVoice(m, false);
  m.Run(4000);
  const double dry = AverageOf(m.Run(200), 0);
  m.Reset();
  m.Write(kControl, kControlEnable | kControlUnmute | kControlReverb);
  m.Write(kReverbVolL, 0x7FFF);
  m.Write(kReverbVolR, 0x7FFF);
  PlainReverb(m, kBase);
  SteadyVoice(m, true);
  m.Run(4000);
  const double wet = AverageOf(m.Run(200), 0);
  Check(dry > 5000, "the dry tone is there");
  Check(wet > dry * 1.98 && wet < dry * 2.02, "dry plus the same again from the reverb");

  BeginTest("with the reverb volume at zero it is not heard");
  m.Reset();
  m.Write(kControl, kControlEnable | kControlUnmute | kControlReverb);
  PlainReverb(m, kBase);
  SteadyVoice(m, true);
  m.Run(4000);
  CheckEqual(static_cast<int64_t>(AverageOf(m.Run(200), 0)), static_cast<int64_t>(dry),
             "exactly the dry tone");

  BeginTest("it steps at 22,050 Hz, one halfword at a time");
  m.Reset();
  m.Write(kControl, kControlEnable | kControlUnmute | kControlReverb);
  PlainReverb(m, kBase);
  SteadyVoice(m, true);
  m.Run(2000);
  // Step k writes the same-side sample at mBASE + mLSAME*8 + 2k. 2,000
  // samples are 1,000 steps: the last written is k = 999.
  const uint32_t trail = base + kSameL * 8;
  Check(RamHalf(m, trail + 2 * 999) != 0, "step 999 was written");
  CheckEqual(RamHalf(m, trail + 2 * 1000), 0, "step 1000 was not");

  BeginTest("it stays inside its work area");
  m.Reset();
  const uint16_t kTopBase = 0xFE00;       // byte 7F000h: 4 KB to the end of RAM
  std::vector<uint8_t> marker(0x100, 0xA5);
  m.Upload(kTopBase * 8u - 0x100, marker);
  m.Write(kControl, kControlEnable | kControlUnmute | kControlReverb);
  PlainReverb(m, kTopBase, 16);        // addresses 10h-F0h, inside a 200h-unit area
  SteadyVoice(m, true);
  m.Run(10000);                           // 5,000 steps round a 2,048-halfword area
  bool intact = true;
  for (uint32_t i = 0; i < 0x100; ++i)
    intact = intact && m.spu().ram()[kTopBase * 8u - 0x100 + i] == 0xA5;
  Check(intact, "the RAM just below mBASE is untouched");
  bool voice_intact = m.spu().ram()[0x1000] == LoudBlock(0x07)[0] &&
                      m.spu().ram()[0x1001] == LoudBlock(0x07)[1];
  Check(voice_intact, "and so is the voice's sample");

  BeginTest("with the master enable off nothing is written");
  m.Reset();
  m.Write(kControl, kControlEnable | kControlUnmute);
  m.Write(kReverbVolL, 0x7FFF);
  PlainReverb(m, kBase);
  SteadyVoice(m, true);
  m.Run(4000);
  bool clean = true;
  for (uint32_t a = base; a < Spu::kRamSize; a += 2)
    clean = clean && RamHalf(m, a) == 0;
  Check(clean, "the work area is still empty");

  BeginTest("but what the work area already holds is still heard");
  // The enable stops the echo being fed, not played: Mednafen's and
  // DuckStation's reading of the hardware. The old reverb went silent.
  m.Reset();
  std::vector<uint8_t> held(Spu::kRamSize - base);
  for (size_t i = 0; i < held.size(); i += 2) {
    held[i] = 0x00;
    held[i + 1] = 0x10;                   // 1000h in every halfword
  }
  m.Upload(base, held);
  m.Write(kControl, kControlEnable | kControlUnmute);
  m.Write(kReverbVolL, 0x7FFF);
  m.Write(kReverbVolR, 0x7FFF);
  PlainReverb(m, kBase);
  const std::vector<int16_t> out = m.Run(2000);
  std::vector<int16_t> tail(out.end() - 200, out.end());
  const double heard = AverageOf(tail, 0);
  Check(heard > 0x1000 * 0.98 && heard < 0x1000 * 1.02, "the buffer's 1000h comes out");
  CheckEqual(RamHalf(m, base + kSameL * 8), 0x1000, "and the buffer is as it was");
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

  BeginTest("the mute bit leaves CD audio alone");
  // psx-spx: bit 14 mutes the SPU, "don't care for CD audio". It used to
  // silence everything, so XA or CD audio under muted voices played nothing.
  m.Reset();
  m.Write(kControl, kControlEnable | kControlCd);   // muted, CD audio on
  m.Write(kCdVolL, 0x7FFF);
  m.Write(kCdVolR, 0x7FFF);
  {
    std::vector<int16_t> cd(2 * 512, 0x2000);
    m.spu().QueueCdSamples(&cd[0], 512, 44100);
  }
  Check(PeakOf(m.Run(256), 0) > 0x1000, "CD audio is heard with the voices muted");

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

// ---------------------------------------------------------------------------
// XA-ADPCM
// ---------------------------------------------------------------------------

namespace {

// Builds one 128-byte sound group. `shift` and `filter` go into every block's
// parameter byte, and `nibbles` supplies the packed sample data.
void BuildSoundGroup(uint8_t* group, uint8_t shift, uint8_t filter,
                     const uint8_t* words112) {
  const uint8_t parameter = static_cast<uint8_t>((filter << 4) | shift);
  for (int i = 0; i < 4; ++i) {
    group[4 + i] = parameter;      // blocks 0..3
    group[8 + i] = parameter;      // blocks 4..7
    group[0 + i] = group[4 + i];   // the duplicate copies the disc carries
    group[12 + i] = group[8 + i];
  }
  memcpy(group + 16, words112, 112);
}

// A whole sector of sound groups, every sample the same nibble value.
void BuildFlatSector(uint8_t* groups, uint8_t shift, uint8_t filter,
                     uint8_t nibble) {
  uint8_t data[112];
  const uint8_t packed = static_cast<uint8_t>((nibble << 4) | nibble);
  memset(data, packed, sizeof(data));
  for (int group = 0; group < 18; ++group)
    BuildSoundGroup(groups + group * 128, shift, filter, data);
}

}  // namespace

// A group whose first four blocks and last four blocks carry different
// parameters, laid out the way a disc does: bytes 4..7 hold the parameters for
// blocks 0..3 and bytes 8..11 those for blocks 4..7, with 0..3 and 12..15
// holding the duplicate copies the format carries for redundancy.
void BuildSplitSoundGroup(uint8_t* group, uint8_t shift_low, uint8_t shift_high,
                          uint8_t filter, const uint8_t* words112) {
  const uint8_t low = static_cast<uint8_t>((filter << 4) | shift_low);
  const uint8_t high = static_cast<uint8_t>((filter << 4) | shift_high);
  for (int i = 0; i < 4; ++i) {
    group[4 + i] = low;            // blocks 0..3
    group[8 + i] = high;           // blocks 4..7
    group[0 + i] = group[4 + i];   // the copies
    group[12 + i] = group[8 + i];
  }
  memcpy(group + 16, words112, 112);
}

// The CD input volume register is a plain signed 16-bit level, not a sweep
// register, and this is where the BIOS CD player's music - and every game's
// XA-ADPCM FMV audio - is actually let through or silenced. Running it through
// the voice/main volume conversion turned the maximum (7FFFh, which the BIOS
// writes) into -1 and muted the lot while the sector counter still ticked up.
void TestCdInputVolume(Machine& m) {
  const uint32_t kCdVolL = 0x1F801DB0;
  const uint32_t kCdVolR = 0x1F801DB2;

  // A CD-DA sector of one constant value, so what comes out the other side is
  // easy to read: 588 stereo pairs of +8000, left and right.
  auto QueueTone = [&](int16_t level) {
    std::vector<uint8_t> sector(2352, 0);
    for (int i = 0; i < 588; ++i) {
      const int off = i * 4;
      sector[off + 0] = static_cast<uint8_t>(level & 0xFF);
      sector[off + 1] = static_cast<uint8_t>((level >> 8) & 0xFF);
      sector[off + 2] = static_cast<uint8_t>(level & 0xFF);
      sector[off + 3] = static_cast<uint8_t>((level >> 8) & 0xFF);
    }
    m.spu().QueueCdAudio(sector.data());
  };

  // Loudest output sample over a run, taking the sign into account.
  auto PeakOf = [&](const std::vector<int16_t>& out) {
    int32_t peak = 0;
    for (int16_t s : out) {
      const int32_t a = s < 0 ? -s : s;
      if (a > peak) peak = a;
    }
    return peak;
  };

  BeginTest("cd input volume");

  // Enable bit (control bit 0) has to be set or the CD input is not mixed at
  // all - a separate gate from the volume.
  m.Reset();
  m.Write(kControl, kControlEnable | kControlUnmute | 0x0001);
  m.Write(kMainVolL, 0x3FFF);           // ~unity after the >>15
  m.Write(kMainVolR, 0x3FFF);

  // Maximum CD volume. This is the exact value the BIOS writes and the exact
  // one the old code silenced.
  m.Write(kCdVolL, 0x7FFF);
  m.Write(kCdVolR, 0x7FFF);
  QueueTone(8000);
  const int32_t loud = PeakOf(m.Run(300));
  Check(loud > 3000, "a full CD volume of 7FFFh is audible, not silent");

  // Both stages at unity, so what goes in comes out: 7FFFh is unity on a
  // plain input level and 3FFFh is unity on a sweep-format main volume, whose
  // bits 14-0 are the level halved. Decoding that without the doubling - the
  // (reg << 1) >> 1 that cancels itself - put this at 4000, half of what the
  // hardware mixes, everywhere a voice or the main volume was involved.
  Check(loud > 7800 && loud < 8200,
        "a tone of 8000 at unity comes out at 8000, not halved");

  // Zero volume really is silence.
  m.Reset();
  m.Write(kControl, kControlEnable | kControlUnmute | 0x0001);
  m.Write(kMainVolL, 0x3FFF);
  m.Write(kMainVolR, 0x3FFF);
  m.Write(kCdVolL, 0x0000);
  m.Write(kCdVolR, 0x0000);
  QueueTone(8000);
  CheckEqual(PeakOf(m.Run(300)), 0, "a CD volume of zero is silent");

  // Half volume is quieter than full - the level scales rather than being all
  // or nothing.
  m.Reset();
  m.Write(kControl, kControlEnable | kControlUnmute | 0x0001);
  m.Write(kMainVolL, 0x3FFF);
  m.Write(kMainVolR, 0x3FFF);
  m.Write(kCdVolL, 0x4000);
  m.Write(kCdVolR, 0x4000);
  QueueTone(8000);
  const int32_t half = PeakOf(m.Run(300));
  Check(half > 0 && half < loud, "half CD volume is quieter than full");

  // The enable bit gates it: same volume, bit 0 clear, no output.
  m.Reset();
  m.Write(kControl, kControlEnable | kControlUnmute);   // bit 0 clear
  m.Write(kMainVolL, 0x3FFF);
  m.Write(kMainVolR, 0x3FFF);
  m.Write(kCdVolL, 0x7FFF);
  m.Write(kCdVolR, 0x7FFF);
  QueueTone(8000);
  CheckEqual(PeakOf(m.Run(300)), 0,
             "CD audio is silent while the enable bit is clear");
}

void TestXaParameterOffsets(Machine&) {
  printf("xa parameter offsets\n");

  // The eight parameters of a group live at bytes 4..11, and bytes 0..3 and
  // 12..15 are copies. Reading them from the wrong place is invisible when
  // every block shares a parameter - which is what the other tests here build -
  // because the copy holds the same value. It stops being invisible the moment
  // the two halves of the group differ, which is what this builds.
  //
  // Reading from byte 0 rather than byte 4 would give blocks 4..7 the
  // parameters belonging to blocks 0..3, so the quiet half would come out loud.
  std::vector<uint8_t> groups(18 * 128, 0);
  std::vector<int16_t> out(Cdrom::kXaFramesPerSector * 2, 0);
  Cdrom::XaState state;

  uint8_t data[112];
  memset(data, 0x44, sizeof(data));            // every nibble is 4
  for (int group = 0; group < 18; ++group)
    BuildSplitSoundGroup(&groups[group * 128], 0, 4, 0, data);

  state.Reset();
  const int frames = Cdrom::DecodeXaAdpcm(&groups[0], 0x00, &state, &out[0]);
  Check(frames == 4032, "mono 4-bit is 4032 frames");

  // Mono block b fills frames b*28 onwards. Blocks 0..3 use shift 0 and
  // blocks 4..7 shift 4, so the second half of every group is sixteen times
  // quieter than the first.
  const int16_t loud = out[0];
  const int16_t quiet = out[112 * 2];
  CheckEqual(static_cast<uint16_t>(loud), static_cast<uint16_t>(4 << 12),
             "blocks 0..3 used the parameters at byte 4");
  CheckEqual(static_cast<uint16_t>(quiet), static_cast<uint16_t>((4 << 12) >> 4),
             "blocks 4..7 used the parameters at byte 8");
  Check(abs(quiet) < abs(loud), "the two halves of a group differ");
}

void TestXaFrameCounts(Machine&) {
  printf("xa frame counts\n");

  std::vector<uint8_t> groups(18 * 128, 0);
  std::vector<int16_t> out(Cdrom::kXaFramesPerSector * 2, 0);
  Cdrom::XaState state;
  state.Reset();

  // Four-bit mono: eighteen groups of eight blocks of 28 samples.
  int frames = Cdrom::DecodeXaAdpcm(&groups[0], 0x00, &state, &out[0]);
  CheckEqual(frames, 18 * 8 * 28, "4-bit mono frames per sector");

  // Four-bit stereo: the same samples, but a pair of blocks makes one frame.
  state.Reset();
  frames = Cdrom::DecodeXaAdpcm(&groups[0], 0x01, &state, &out[0]);
  CheckEqual(frames, 18 * 4 * 28, "4-bit stereo frames per sector");

  // Eight-bit mono: four blocks to a group instead of eight.
  state.Reset();
  frames = Cdrom::DecodeXaAdpcm(&groups[0], 0x10, &state, &out[0]);
  CheckEqual(frames, 18 * 4 * 28, "8-bit mono frames per sector");

  state.Reset();
  frames = Cdrom::DecodeXaAdpcm(&groups[0], 0x11, &state, &out[0]);
  CheckEqual(frames, 18 * 2 * 28, "8-bit stereo frames per sector");

  // Nothing may run past what it said it wrote.
  Check(frames * 2 <= Cdrom::kXaFramesPerSector * 2, "output stays in bounds");

  // The sample rate comes out of the coding byte.
  CheckEqual(Cdrom::XaSampleRate(0x00), 37800, "default rate is 37800");
  CheckEqual(Cdrom::XaSampleRate(0x04), 18900, "rate bit selects 18900");
}

void TestXaSilenceAndShift(Machine&) {
  printf("xa silence and shift\n");

  std::vector<uint8_t> groups(18 * 128, 0);
  std::vector<int16_t> out(Cdrom::kXaFramesPerSector * 2, 0);
  Cdrom::XaState state;

  // All-zero data through filter 0 is silence, and must stay silence: the
  // filter has no input to carry forward.
  state.Reset();
  BuildFlatSector(&groups[0], 0, 0, 0);
  int frames = Cdrom::DecodeXaAdpcm(&groups[0], 0x00, &state, &out[0]);
  bool silent = true;
  for (int i = 0; i < frames * 2; ++i) {
    if (out[i] != 0)
      silent = false;
  }
  Check(silent, "zero data with no filter is silence");

  // A constant nibble with filter 0 is a constant sample, and the shift is
  // what scales it: shift 0 is loudest, and each step halves it.
  state.Reset();
  BuildFlatSector(&groups[0], 0, 0, 4);        // +4 in the top of a halfword
  Cdrom::DecodeXaAdpcm(&groups[0], 0x00, &state, &out[0]);
  const int16_t loudest = out[0];
  CheckEqual(loudest, 4 << 12, "shift 0 puts the nibble at the top");

  state.Reset();
  BuildFlatSector(&groups[0], 1, 0, 4);
  Cdrom::DecodeXaAdpcm(&groups[0], 0x00, &state, &out[0]);
  CheckEqual(out[0], loudest / 2, "each shift step halves it");

  state.Reset();
  BuildFlatSector(&groups[0], 4, 0, 4);
  Cdrom::DecodeXaAdpcm(&groups[0], 0x00, &state, &out[0]);
  CheckEqual(out[0], loudest / 16, "shift 4 is a sixteenth");

  // A nibble of 8 or more is negative: this is four-bit two's complement, not
  // an unsigned value with a bias.
  state.Reset();
  BuildFlatSector(&groups[0], 0, 0, 0x0F);     // -1
  Cdrom::DecodeXaAdpcm(&groups[0], 0x00, &state, &out[0]);
  Check(out[0] < 0, "the top bit of a nibble is a sign");
  CheckEqual(out[0], -4096, "nibble F is -1 at the top of a halfword");
}

void TestXaMonoAndStereo(Machine&) {
  printf("xa mono and stereo\n");

  std::vector<uint8_t> groups(18 * 128, 0);
  std::vector<int16_t> out(Cdrom::kXaFramesPerSector * 2, 0);
  Cdrom::XaState state;

  // Mono puts the same sample in both channels.
  state.Reset();
  BuildFlatSector(&groups[0], 2, 0, 5);
  const int frames = Cdrom::DecodeXaAdpcm(&groups[0], 0x00, &state, &out[0]);
  bool matched = true;
  for (int i = 0; i < frames; ++i) {
    if (out[i * 2] != out[i * 2 + 1])
      matched = false;
  }
  Check(matched, "mono fills both channels alike");

  // Stereo takes its two channels from alternating blocks, so a sector whose
  // even and odd blocks differ must come out with the channels differing.
  uint8_t data[112];
  // Even nibbles (blocks 0,2,4,6 - the left channel) are 4; odd ones are 0.
  memset(data, 0x04, sizeof(data));
  for (int group = 0; group < 18; ++group)
    BuildSoundGroup(&groups[group * 128], 0, 0, data);

  state.Reset();
  const int stereo_frames =
      Cdrom::DecodeXaAdpcm(&groups[0], 0x01, &state, &out[0]);
  Check(stereo_frames > 0, "stereo produced frames");
  Check(out[0] != out[1], "stereo channels come from different blocks");
  CheckEqual(out[0], 4 << 12, "the left channel took the low nibble");
  CheckEqual(out[1], 0, "the right channel took the high nibble");
}

void TestXaFilterCarriesForward(Machine&) {
  printf("xa filter\n");

  std::vector<uint8_t> groups(18 * 128, 0);
  std::vector<int16_t> out(Cdrom::kXaFramesPerSector * 2, 0);
  Cdrom::XaState state;

  // Filter 1 is a pure feedback of the previous output at 60/64. Feeding one
  // non-zero sample and then zeroes must produce a decay, not a step: this is
  // what fails if the filter tables are wrong or the history is not kept.
  uint8_t data[112];
  memset(data, 0, sizeof(data));
  data[0] = 0x04;                              // one sample in block 0 only
  for (int group = 0; group < 18; ++group)
    BuildSoundGroup(&groups[group * 128], 0, 1, data);

  state.Reset();
  Cdrom::DecodeXaAdpcm(&groups[0], 0x00, &state, &out[0]);

  Check(out[0] != 0, "the impulse arrived");
  // Successive frames are two apart: out[n*2] is the left of frame n, and
  // out[n*2+1] its right, which in mono is the same sample again.
  Check(out[2] != 0, "the filter carried it into the next sample");
  Check(abs(out[2]) < abs(out[0]), "filter 1 decays");
  Check(abs(out[4]) < abs(out[2]), "and keeps decaying");

  // Filter 0 has no feedback at all, so the same data must stop dead.
  for (int group = 0; group < 18; ++group)
    BuildSoundGroup(&groups[group * 128], 0, 0, data);
  state.Reset();
  Cdrom::DecodeXaAdpcm(&groups[0], 0x00, &state, &out[0]);
  Check(out[0] != 0, "the impulse arrived without a filter too");
  CheckEqual(out[2], 0, "filter 0 carries nothing forward");

  // The history must survive a sector boundary: a stream is continuous and
  // resetting between sectors would click every tenth of a second.
  memset(data, 0, sizeof(data));
  for (int group = 0; group < 18; ++group)
    BuildSoundGroup(&groups[group * 128], 0, 1, data);
  const int16_t before = state.old[0];
  state.old[0] = 10000;
  state.older[0] = 0;
  Cdrom::DecodeXaAdpcm(&groups[0], 0x00, &state, &out[0]);
  Check(out[0] != 0, "an all-zero sector still decays from the last one");
  (void)before;
}

void TestXaSaturates(Machine&) {
  printf("xa saturation\n");

  std::vector<uint8_t> groups(18 * 128, 0);
  std::vector<int16_t> out(Cdrom::kXaFramesPerSector * 2, 0);
  Cdrom::XaState state;

  // The loudest possible nibble through the strongest filter, over and over,
  // must saturate rather than wrap. A wrap here is a loud crack.
  BuildFlatSector(&groups[0], 0, 2, 0x07);
  state.Reset();
  const int frames = Cdrom::DecodeXaAdpcm(&groups[0], 0x00, &state, &out[0]);
  bool in_range = true;
  for (int i = 0; i < frames * 2; ++i) {
    if (out[i] == -32768 && i > 4)
      continue;
    if (out[i] < -32768 || out[i] > 32767)
      in_range = false;
  }
  Check(in_range, "output stays inside a signed sample");

  bool saturated = false;
  for (int i = 0; i < frames; ++i) {
    if (out[i * 2] == 32767)
      saturated = true;
  }
  Check(saturated, "a loud stream does reach the top and stop there");
}


const Group kGroups[] = {
  { "registers", TestRegisters },
  { "keyonoff",  TestKeyOnOff },
  { "adpcm",     TestAdpcm },
  { "loopaddr",  TestLoopAddress },
  { "envelope",  TestEnvelope },
  { "mixer",     TestMixer },
  { "sweep",     TestSweep },
  { "reverb",    TestReverb },
  { "timing",    TestTiming },
  { "noiseirq",  TestNoiseAndIrq },
  { "cdvolume",  TestCdInputVolume },
  { "xaparams",  TestXaParameterOffsets },
  { "xacounts",  TestXaFrameCounts },
  { "xashift",   TestXaSilenceAndShift },
  { "xastereo",  TestXaMonoAndStereo },
  { "xafilter",  TestXaFilterCarriesForward },
  { "xasat",     TestXaSaturates },
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
    // A default label, so a group that never calls BeginTest reports its own
    // name rather than inheriting whatever the previous group set last. A
    // failure attributed to the wrong test is worse than one with no name.
    BeginTest(kGroups[i].name);
    const int before = g_failures;
    kGroups[i].run(machine);
    if (g_failures == before)
      printf("  ok\n");
  }

  printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
