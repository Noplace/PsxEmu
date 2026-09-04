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

#include "audio/iaudioengine.h"

namespace emulation {
namespace psx {

/*
  Sound Processing Unit.

  24 ADPCM voices mixed into a stereo pair at 44100 Hz, plus a reverb unit that
  works out of the same 512 KB of sound RAM the samples live in.

  The core owns the mixing and hands finished frames to whoever asks. A front
  end drains them with ReadSamples and pushes them at an IAudioEngine, or a
  headless harness drains them and checks the numbers - which is the only way
  any of this is testable, because audio has no equivalent of looking at the
  screen and seeing that it is wrong.
*/
class Spu : public Component {
 public:
  static const uint32_t kRamSize = 512 * 1024;
  static const int kSampleRate = 44100;
  static const int kVoices = 24;

  // The SPU produces one frame every 768 CPU cycles: 33868800 / 44100.
  static const uint32_t kCyclesPerSample = 768;

  Spu();
  ~Spu();

  int Initialize();
  int Deinitialize();

  // Advances by a number of CPU cycles, generating frames as they fall due.
  void Tick(uint32_t cycles);

  uint16_t Read(uint32_t address);
  void Write(uint32_t address, uint16_t data);

  // Sound RAM transfers over DMA channel 4.
  uint32_t ReadDataWord();
  void WriteDataWord(uint32_t value);

  // Drains generated stereo frames into `out`, which holds frames*2 samples.
  // Returns how many frames were actually copied.
  int ReadSamples(int16_t* out, int frames);
  int QueuedFrames() const;

  // Optional sink. When set, finished frames are pushed to it as well as
  // buffered, so a front end can either pull or be pushed to.
  void set_audio_engine(IAudioEngine* engine) { engine_ = engine; }

  void QueueCdAudio(const uint8_t* raw_sector);
  // Interleaved stereo at some other rate - XA-ADPCM comes out at 37800 or
  // 18900 Hz and has to be resampled onto the mixer's own 44100.
  void QueueCdSamples(const int16_t* stereo, int frames, int sample_rate);

  const uint8_t* ram() const { return ram_; }

  struct Stats {
    uint64_t frames;             // stereo frames generated
    uint64_t key_ons;
    uint64_t key_offs;
    uint64_t blocks_decoded;
    uint64_t irqs;
    uint64_t frames_dropped;     // the buffer filled and nobody drained it
    uint32_t voices_active;      // at the last frame
    int16_t peak_left, peak_right;
    // The CD audio path, which has no other way of saying whether it did
    // anything. A track that plays silently is indistinguishable from one
    // that never started unless these are counted separately.
    uint64_t cd_samples_in;      // stereo pairs handed over by the drive
    // The CD input carries CD-DA and XA-ADPCM alike - the hardware has one
    // of it - so what comes out cannot be attributed to either. Only the
    // CD-DA side of what goes in is counted separately.
    uint64_t cd_samples_out;     // pairs mixed in, from either source
    uint64_t cd_samples_dropped; // pairs overwritten before anyone read them
    uint64_t cd_frames_muted;    // pairs thrown away with the enable bit clear
    int16_t cd_peak;             // loudest CD sample seen, before volume
  };
  const Stats& stats() const { return stats_; }

 private:
  enum AdsrPhase { kAttack, kDecay, kSustain, kRelease, kOff };

  struct Voice {
    // Registers, as software sees them.
    uint16_t volume_left, volume_right;
    uint16_t pitch;
    uint16_t start_address;      // in 8-byte units
    uint16_t adsr_low, adsr_high;
    uint16_t adsr_volume;
    uint16_t repeat_address;     // in 8-byte units

    // Running state.
    uint32_t current_address;    // byte address into sound RAM
    uint32_t counter;            // 12-bit fraction plus sample index
    int16_t history[4];          // the four samples the interpolator needs
    int16_t decoded[28];
    int decoded_index;
    int16_t previous0, previous1;  // ADPCM filter history
    AdsrPhase phase;
    int32_t level;               // 0..0x7FFF
    uint32_t envelope_counter;   // samples since the envelope last moved
    bool active;
    bool repeat_set;             // the block flagged itself as the loop point
    bool ended;                  // reached a block with the end flag
  };

  Voice voices_[kVoices];
  uint8_t* ram_;

  // Global registers.
  uint16_t main_volume_left_, main_volume_right_;
  uint16_t reverb_volume_left_, reverb_volume_right_;
  uint32_t key_on_, key_off_, pitch_modulation_, noise_mode_, reverb_mode_;
  uint32_t endx_;
  uint16_t control_;
  uint16_t transfer_control_;
  uint16_t status_;
  uint16_t irq_address_;         // in 8-byte units
  uint16_t transfer_address_;    // in 8-byte units
  uint32_t transfer_cursor_;     // byte address, advances as data is written
  uint16_t cd_volume_left_, cd_volume_right_;
  uint16_t external_volume_left_, external_volume_right_;
  uint16_t reverb_registers_[32];

  // Noise generator.
  uint32_t noise_timer_;
  int16_t noise_level_;

  // Reverb working state.
  uint32_t reverb_base_;         // byte address of the reverb work area
  uint32_t reverb_cursor_;       // offset within it
  bool reverb_left_phase_;

  uint32_t sample_counter_;      // CPU cycles toward the next frame
  bool irq_pending_;

  // Generated frames, waiting to be drained. A second of audio is far more
  // than any front end needs; overflowing it means nobody is listening, which
  // is counted rather than allowed to block the emulation.
  static const int kBufferFrames = kSampleRate;
  int16_t* buffer_;
  int buffer_read_;
  int buffer_write_;
  int buffer_count_;

  // CD Audio (CD-DA) input buffer
  int16_t cd_audio_buffer_[kSampleRate * 2];
  int cd_audio_read_;
  int cd_audio_write_;
  int cd_audio_count_;
  // Resampling state for QueueCdSamples, carried between sectors so the joins
  // between them are not audible.
  uint32_t cd_resample_fraction_;
  int16_t cd_resample_last_[2];
  int16_t cd_resample_scratch_[2];
  void PushCdFrame(int16_t left, int16_t right);

  IAudioEngine* engine_;
  Stats stats_;

  // ---- helpers -----------------------------------------------------------
  inline uint16_t RamHalf(uint32_t byte_address) const {
    const uint32_t a = byte_address & (kRamSize - 1);
    return static_cast<uint16_t>(ram_[a] | (ram_[a + 1] << 8));
  }
  inline void WriteRamHalf(uint32_t byte_address, uint16_t value) {
    const uint32_t a = byte_address & (kRamSize - 1);
    ram_[a] = static_cast<uint8_t>(value);
    ram_[a + 1] = static_cast<uint8_t>(value >> 8);
  }

  void GenerateFrame();
  void DecodeBlock(Voice& voice);
  int16_t StepVoice(Voice& voice, int index, int16_t previous_output);
  void StepEnvelope(Voice& voice);
  void KeyOn(int index);
  void KeyOff(int index);
  void StepNoise();
  void ProcessReverb(int32_t input_left, int32_t input_right,
                     int32_t* output_left, int32_t* output_right);
  void CheckIrq(uint32_t byte_address);
  void PushFrame(int16_t left, int16_t right);

  // Volume registers are either a plain level or a sweep; only the level form
  // is used for mixing here.
  static int16_t VolumeOf(uint16_t reg);
};

}
}
