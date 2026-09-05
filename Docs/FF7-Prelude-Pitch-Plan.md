# Final Fantasy VII: the prelude's high notes come out low

`Final Fantasy VII [SCUS-94163]` cd1 boots, plays the Sony and Squaresoft
logos, and if nothing is pressed it reaches the title screen and starts the
prelude. The notes are there, in time, with the right instrument - and the
ones that should be high are audibly low. FMV audio and other sampled sounds
are unaffected. There are other reported instances of a sample coming out
wrong; the prelude is the one being chased here because it is reproducible in
twenty seconds with no input at all.

## Why this points at the voice pitch path and nowhere else

FMV and CD audio never touch `VxPitch`. They arrive through
`Spu::QueueCdAudio` / `Spu::QueueCdSamples`, are resampled by frame count onto
the mixer's 44100, and are then mixed as finished samples. The prelude's notes
are SPU voices, and a voice's output frequency is determined by exactly one
quantity: `step / 0x1000`, computed in `Spu::StepVoice`
(`PSXEmu.Core/psx/spu.cpp:394`).

That also rules out the whole output path by construction. Frame *timing*
cannot produce a wrong note: each generated frame advances a voice by
`step/0x1000` of a sample regardless of when it was generated, so a mixer
running fast or slow gives underruns and clicks, not transposition. "FMV fine,
notes low" leaves eight lines of code.

## The eight lines

```cpp
int32_t step = voice.pitch & 0x3FFF;
if ((pitch_modulation_ & (1u << index)) && index > 0) {
  const int32_t factor = 0x8000 + previous_output;
  step = (step * factor) >> 15;
  step &= 0xFFFF;
}
step = std::min<int32_t>(step, 0x3FFF);
```

psx-spx, for comparison:

```
  step = VxPitch                    ;full 16 bits
  IF PMON.Bit(x) AND (x>0)
    factor = OldNoteOut + 8000h
    step = SignExtend16to32(step)
    step = (step * factor) SAR 15
    step = step AND 0000FFFFh
  IF step > 3FFFh, step = 4000h
  Counter = Counter + step
```

Three deviations: the register is **masked** instead of clamped, the mask is
applied **before** PMON instead of the clamp **after** it, and the value is
not sign-extended going into the PMON multiply.

## Hypotheses, in the order the evidence will settle them

**H1 - the mask folds high notes down.** `& 0x3FFF` does not clamp, it wraps.
The hardware's ceiling is 4x (176400 Hz); this code's behaviour above it is a
sawtooth:

| VxPitch | Hardware | This code | Heard as |
|---|---|---|---|
| 0x4000 | 4.00x | 0.00x | voice frozen - silence |
| 0x4800 | 4.00x | 0.50x | ~3.2 octaves low |
| 0x5000 | 4.00x | 1.00x | 2 octaves low |
| 0x6000 | 4.00x | 2.00x | 1 octave low |
| 0x8000 | 4.00x | 0.00x | voice frozen - silence |

That is the reported symptom exactly: the notes that should be highest come
out low, by varying amounts, with some dropping out. **The caveat that stops
this being a conclusion:** hardware clamps at 4x too, so a driver whose top
notes needed 5x would sound wrong on a real PSX as well. Whether FF7 emits
anything above 0x3FFF is a measurement, not a deduction. Phase 1 makes it one.

**H2 - PMON drags the pitch down.** `factor = 0x8000 + previous_output` spans
0x0001..0xFFFF, so a voice modulated by a loud *negative* neighbour has its
step scaled toward zero - a large, consistently downward pitch error, which is
also the reported symptom. The missing sign-extension is a real deviation on
this path. Live only if FF7 sets PMON bits during the prelude, which Phase 1
also measures.

**H3 - the sample, not the step.** A wrong `repeat_address` or loop span
changes the period of what is playing without the pitch register being
involved. Settled by dumping sound RAM and decoding the instrument offline:
its own period plus the logged VxPitch gives the frequency that *should* come
out, to compare against what does.

**H4 - the mix, not the notes.** Raised from listening, and it fits the Phase 0
measurements better than anything above. Two effects in this core are known
approximations rather than implementations:

- **Volume sweeps are not implemented.** `Spu::VolumeOf` returns 0x3FFF - full
  scale - for any volume register with bit 15 set (`spu.cpp:174`). A sequencer
  uses per-voice sweeps constantly: note attack shaping, tremolo, and
  cross-fading between layered voices. Pinned to maximum, no pitch changes, but
  every layer plays at the same level as every other. A part meant to sit well
  under the arpeggio comes up level with it, and the result reads as low and
  thick while every individual note measures perfectly in tune - which is
  precisely what Phase 0 found.
- **Reverb is a two-tap delay**, not the hardware's comb and all-pass network
  (`spu.cpp:437`, and the gap is recorded in Gaps.md). A harp arpeggio in a
  hall is mostly reverb by energy, so the wrong network is a different
  instrument, not a subtle difference in room.

Phase 0 also found the evidence that makes this concrete: the recurring
+15..+18 cent second reading on sustained notes is **two voices per note,
deliberately detuned**. If the balance between those layers is set by a sweep,
both are playing at full level.

**H5 - a layer that never sounds, or stops early.** A bright octave-up layer
that is keyed on but silenced - by an envelope that collapses, or by
`DecodeBlock`'s end-flag path forcing `level = 0` too eagerly - removes the top
of the sound without moving a single note. The ear reads that as "lower", and
no pitch measurement can see it. Distinguished from H4 by voice counts and
envelope levels rather than by frequencies.

Ruled out by construction, recorded so they are not re-investigated: the
Gaussian table (`kGauss` is the full 512-entry hardware table and interpolation
cannot change pitch), the ADPCM shift/filter decode (changes timbre and level,
not rate), the 12-bit counter and its 8-bit interpolation index (both match
psx-spx), and everything downstream of `GenerateFrame`, which FMV audio shares.

## The coverage gap that let this through

`spu_test` checks that the pitch register **round-trips**
(`PSXEmu.Core/tools/spu_test.cpp:176`) and that it is per-voice. Nothing
anywhere asserts that a given pitch produces a given output *frequency*. All
99 checks pass with the mask in place, and would pass with the pitch path
deleted entirely as long as the register still reads back.

## Plan

**Phase 0 - reproduce headlessly.** Build the harnesses, boot to the prelude
with no input, capture it to a WAV, and find the frame window it occupies.
The point is to turn "listen to it" into a shell command.

```
PSXEmu.Core\tools\build_tools.bat
Temp\tools\boot_runner.exe --disc "\superserverx\D\Games\Sony\PSX\ISO\Final Fantasy VII [SCUS-94163]\Final Fantasy VII [SCUS-94163].cd1.cue" --frames 3600 --wav Temp\ff7_prelude.wav --frame-log 300
```

**Phase 1 - measure what the game asks for, before changing anything.**
Extend `Spu::Stats` with `max_pitch`, `pitch_writes_over_3fff`, and the PMON,
noise and reverb masks seen, plus a count of volume writes that selected a
**sweep** rather than a level - printed by `boot_runner`'s existing `spu`
summary line. The Stats struct exists for exactly this, and those numbers
decide H1 against H2 against H4 in a single run: a sweep count in the hundreds
during 27 seconds of music says the mix is being flattened, whatever the
pitches turn out to be.

Add `--trace-spu <file>` for the detail - one line per key-on with frame,
voice, pitch, start and repeat address, ADSR, and both volume registers with
their sweep bit called out - and `--spu-ram <file>` to dump the 512 KB for H3.

The trace answers all five hypotheses from one capture: the pitches settle H1
and H2, the start addresses settle H3, the sweep bits settle H4, and the
key-ons against the notes actually heard settle H5.

**Phase 2 - fix the pitch path** to the psx-spx order: read the full 16 bits,
sign-extend before the PMON multiply, clamp - never mask - to 0x4000 after it.

**Phase 3 - a test that encodes the hardware, not the code.** A new `pitch`
group in `spu_test`: upload an ADPCM block that decodes to a waveform of known
period; key on at 0x0800 / 0x1000 / 0x2000 and assert the measured output
period doubles and halves; at 0x4000, 0x5000 and 0xFFFF assert all three are
identical and equal to 4x - the case that today gives silence and two
different wrong notes; at 0 assert the voice does not advance. Plus a PMON
case at full positive and full negative modulator.

**Phase 4 - verify against the game.** There is no Python on this machine, so
a small `tools/wav_pitch.cpp` (autocorrelation, dominant frequency per 100 ms
window) is the before/after instrument. The prelude's arpeggio should read as
a rising run with clean octave ratios, no folded notes and no dropouts. Then
re-capture the FMV audio and run every harness against the
[Test-Suite.md](Test-Suite.md) baselines: these eight lines feed every voice in
every game.

**Phase 5 - document** as bug 39 in [Bugs-Found.md](Bugs-Found.md) with the
measured numbers, and update the `spu_test` check count in Test-Suite.md.

## The branch point

If Phase 1 reports max pitch below 0x4000 **and** PMON zero throughout the
prelude, the pitch path is exonerated: Phase 2 becomes a correctness cleanup
worth doing on its own terms, and the investigation moves to H3 with the
sound-RAM dump. Recording that here so the next session does not start by
re-reading `StepVoice`.

## Phase 0: measured

Reproduced headlessly, no input, nothing changed in the core:

```
boot_runner bios\SCPH1001.BIN --disc "...\Final Fantasy VII [SCUS-94163].cd1.cue"
            --frames 3600 --frame-log 300 --wav Temp\ff7_prelude.wav --quiet
```

3600 frames, 62.15s emulated in 43.44s wall (1.43x real time, 82.9 fps). The
title screen is up by frame 2400, and:

```
spu   2677536 frames, 551 key-ons, 149224 blocks, peak 14200/14914
```

**The prelude starts at 33.5 s and plays to the end of the capture**, 27
seconds of it. `wav_pitch` was built for this and reads the arpeggio straight
out of the WAV:

```
  34.093    F2   87.25 Hz    -1.6 cents
  34.279    G2   97.94        -1.0
  34.488    A2  109.98        -0.3
  34.650    C3  130.77        -0.6
  34.836    F3  176.59       ...
  35.068    G3  195.86        -1.2
  35.231    A3  219.86        -1.1
  35.440    C4  261.32        -2.0
  35.616    F4  350.11        +4.4
  35.802    G4  390.68        -5.8
  35.964    A4  440.25        +1.0
  36.150    C5  522.84        -1.4
  36.336    F5  697.77        -1.7   <- turns here
  36.522    C5  524.33        +3.6
  36.707    A4  440.98        +3.9
```

A regular C-F-G-A ladder up from C2, turning at F5 and descending the same
ladder. Over all 27 seconds, 1173 analysis windows:

- **Lowest note B1 (~62 Hz), highest F5 at 698.21 Hz** - and F5 is reached in
  only 13 of the 1173 windows, twice in the whole piece (36.3 s and 48.2 s).
- The weight of the piece sits low: F2 90 windows, F3 73, A2 70, F4 69, D2 65.
- **Every note is in tune.** Most land within +/-3 cents of equal temperament.
  A recurring +15..+18 cent second reading on sustained notes alternates with
  an in-tune one on the same note, which is two voices deliberately detuned
  against each other, not an error.
- No dropouts, no silences, no out-of-tune notes anywhere in 27 seconds.

### What that does to the hypotheses

**H1's signature is not present.** Masking gives silence at 0x4000 and 0x8000
and notes at 1/3 and 1/5 of the intended frequency elsewhere; none of that is
in tune with the ladder, and none of it is here. Either the game never writes
above 0x3FFF in this passage, or every fold lands in tune by coincidence.
Phase 1 says which, and the first is far more likely. The mask is still a real
defect - it just may not be *this* defect.

**The shape of the error changed the shortlist.** The pattern is right, the
intervals are exact, the tuning is clean, and only the register is low: that is
a **constant** transposition, every note scaled by one factor. A clamp or a
fold is not constant - it hits high notes and leaves low ones alone. What is
constant is the sample the notes are played from. **H3 moves to the front**: a
voice pointed at the wrong place in the instrument bank, or an instrument
uploaded to the wrong address, transposes an entire part by a fixed ratio while
preserving every interval exactly.

The competing explanation with the same signature is that the game asked for
exactly these pitches and the SPU played them faithfully, in which case nothing
here is an SPU bug at all.

The WAV cannot separate those two, and no further listening will. Phase 1 does:
log the VxPitch the game writes and the start address it writes with it, work
out the frequency that pairing *should* produce, and compare it against the
698.21 Hz sitting at the top of this run.

## Phase 1: the answer, and it was none of the first three

The instrumentation settled H1, H2 and half of H4 in one line each:

```
spu requests   max pitch 3FFFh (4.00x), 0 writes over 3FFFh; volumes 0 sweeps / 4006 levels
spu modes      pmon 000000  noise 000000  reverb FFFFFF  control C0B5 (reverb on, unmute on)
```

- **H1 dead.** Nothing above 3FFFh is ever written. The single 3FFFh is the
  BIOS zeroing all 24 voices at 0.146 s, not a note. The mask never fires.
- **H2 dead.** No pitch modulation anywhere in the run.
- **H4's sweep half dead.** 4006 volume writes, every one a plain level. The
  unimplemented sweep is not flattening this mix. The panning that moves across
  the stereo field is the driver rewriting levels note by note, and we honour
  it.
- **H4's reverb half is live**, and remains so - see below.

`--trace-spu` then found it. All 438 prelude notes play one sample, and every
one of them looks like this:

```
33.569  v10  REPEAT= 186E  (start 186A)
33.569  v10  KEYON  pitch 017E ( 0.093x)  start 186A  repeat-was 186E
```

FF7 sets the loop point by register and then keys on. `KeyOn` was resetting
`repeat_address` to `start_address` and throwing it away. `--spu-ram` showed
what that cost: 186Ah is two blocks of silence followed, at 186Eh, by a
28-sample triangle flagged end-and-repeat, with no loop-start flag anywhere.
The intended 28-sample loop became an 84-sample one - a third of the frequency
on every note, and a pulse train instead of a continuous tone.

The measured factor of exactly 3, constant across every note, is the shape
Phase 0 predicted before any of this was read.

## Phases 2 and 3: fixed and verified

`KeyOn` no longer touches the repeat address, and the dead `repeat_set` flag
became `ignore_loop_start` with the hardware's meaning. Full write-up as
**bug 39** in [Bugs-Found.md](Bugs-Found.md).

| Before | After |
|---|---|
| C2 65.37 Hz | G3 196.06 |
| F2 87.25 | C4 262.38 |
| A2 109.98 | E4 329.19 |
| **F5 698.21 (top note)** | **C7 2095.77** |

Every note exactly three times higher, and the arpeggio cell is now C-D-E-G -
C major pentatonic, the Prelude's actual figure. New `loopaddr` group in
`spu_test`, 8 checks, 99 -> 107; four of the eight fail against the old
behaviour, measured rather than assumed. 3600 frames of FF7 give an identical
framebuffer checksum, instruction count and interrupt count before and after.

## Status

**The pitch is fixed.** H1, H2, H3 and H4's sweep half are all closed, and the
cause was none of them: it was where the voice loops back to.

**What remains, and it is the reverb.** `reverb FFFFFF` with the master enable
set - FF7 routes all 24 voices through it, and this core implements it as a
two-tap delay rather than the hardware's comb-and-all-pass network. That is a
real difference in what the prelude sounds like, it is independent of
everything above, and it is not addressed here. The delay length is derived
from the whole space above `reverb_base_`, so it is not even a fixed room; it
is whatever the game's work area size makes it. That is its own pass.

H5 - a layer keyed on but never sounding - was never needed: the six prelude
voices all key on, all play, and all measure correctly after the fix.

`PSXEmu.Core/tools/wav_pitch.cpp` is new, built by `build_tools.bat` and
documented in [Test-Suite.md](Test-Suite.md), along with `--trace-spu`,
`--spu-ram` and the `spu requests` / `spu modes` counters on `boot_runner`.
