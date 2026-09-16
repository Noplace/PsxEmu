# Emulation speed: 50%, 100%, 150%, 200%

## The conclusion first

The video half is nearly free - one multiplier where the frame limiter is
already called. The audio half is the whole job, because the sound device
drains 44,100 samples a second no matter how fast the machine is producing
them, and today it *blocks* the emulator when it cannot keep up. Without
touching audio, selecting 200% would silently give about 100%.

## What "speed" means here

Nothing about the emulated machine changes. A PlayStation runs at 33.8688 MHz
and its display at 59.29 Hz (NTSC) whatever this setting says; the SPU still
produces one sample per `Spu::kCyclesPerSample`. Speed is purely a statement
about **wall clock**: how many emulated seconds to run per real second.

That is worth stating plainly because it decides where the change goes. It does
*not* go in the core's clocking - none of the timing work, the baselines or the
cycle-cost tables move. It goes in the front end's pacing, and in the one place
where emulated time meets a real device: audio.

## The video half

`App::LimitFrameRate` already funnels everything through one call:

```cpp
if (system_->config().frame_limiter)
    frame_limiter_.Wait(system_->gpu().refresh_hz());
```

Speed is that rate times a factor:

```cpp
frame_limiter_.Wait(system_->gpu().refresh_hz() * system_->config().emulation_speed);
```

`FrameLimiter::Wait` takes an arbitrary `hz` and derives the period from it, so
50% and 200% need nothing else. Its four-frame resync guard already handles a
host that cannot keep up, which is exactly what 200% will be on a heavy scene.

Two consequences to handle rather than discover:

- **The setting only means anything with the frame limiter on.** With it off
  the loop runs as fast as vsync and audio allow, which is a different feature
  ("uncapped"). The Speed items should be greyed out when the limiter is off,
  the way the filter items are greyed out under D3D11.
- **The title-bar readout keeps working and starts being interesting**: it
  reports emulated frames per wall second as a percentage, so at 200% it should
  read ~200%, and *not* reading it is how you find out the host ran out of
  headroom.

## The audio half

The SPU produces 44,100 samples per **emulated** second. At 200% that is 88,200
samples per wall second arriving at a device that consumes 44,100. At 50% it is
22,050 into a device that wants 44,100.

Today `WASAPIAudioEngine::QueueAudio` deals with a full buffer by waiting:

```cpp
while (availableFrames < frameCount && m_playing) sleep_for(1ms);
```

So at 200% the emulator would spend its time asleep in the audio engine and the
frame limiter would never be the thing pacing it. **The speed setting would not
work, and the reason would be invisible.** This is the same blocking call that
[Threading-Plan.md](Threading-Plan.md) stage 1 removes; that stage is a
prerequisite here, not an optional tidy-up.

With it non-blocking, the options are:

1. **Resample by the speed factor** - produce 88,200, hand the device 44,100.
   The result is pitched up and shortened, which is what fast-forward sounds
   like on every emulator that has one, and what a tape sounds like. At 50% it
   pitches down. Honest, predictable, and the code to do it is a near-copy of
   `Spu::QueueCdSamples`, which already resamples 37,800 Hz XA onto the mixer's
   rate with a carried fractional position.
2. **Drop or pad** - queue what fits and discard the rest at 200%; let the
   device underrun at 50%. Free, and it crackles.
3. **Mute outside 100%.** Free, and defensible for a fast-forward key, but poor
   for 150% as a way to play.

**Recommend (1)**, with (3) as a fallback if the resampler turns out to fight
the reverb tail. Note in the menu or the docs that non-100% speeds change the
pitch: that is a property of the approach, not a defect to be surprised by
later.

## Settings and menu

Following the existing pattern exactly - the tables in `const.h` are walked to
build the menu and the nth id maps back to the nth entry:

- `const.h`: a `SpeedChoice { float value; const wchar_t* label; }` table -
  50%, 100%, 150%, 200% - and a `kCommandSpeedFirst`/`kCommandSpeedLast` run of
  four ids.
- `menu.cpp`: a Speed popup under **Emulation**, next to Frame Limiter, which is
  where the other pacing controls already live. `TickSpeed(window, value)`
  alongside the other tick functions, greying the items out when the limiter is
  off.
- `emuconfig.h`: `float emulation_speed = 1.0f;` plus a `kValidSpeeds` list, in
  the style of `kValidVideoFilters`, so a hand-edited ini cannot ask for 40x.
  `settings.h` stores and loads it, clamped on the way in the way
  `audio_volume` already is.
- `app.cpp`: `SetSpeed(float)` - set it, save the settings, re-tick, and
  `frame_limiter_.Reset()` so the change does not try to make up a debt at the
  old rate.

## What can be verified from here

- **The limiter itself**: `frame_limiter_test` (`platform/frame_limiter.h`)
  already checks that a loop is held to 59.29 and 49.76 Hz. Add the four speed
  multiples - 29.6, 59.29, 88.9, 118.6 Hz - which is a direct test of the only
  arithmetic this feature adds.
- **The resampler**: a `spu_test` check that a known tone in at 2x comes out
  half as many samples, with its period halved.
- **That nothing else moved**: every checksum in Test-Suite.md, since none of
  this touches emulated time. That is the assertion worth making loudest - if a
  disc baseline moves, the change has leaked into the core.

What cannot be verified here is how it *feels*, and whether the host actually
holds 200% on a heavy scene. On this machine the interpreter runs the BIOS boot
at 1.57x real time and the heavier discs at roughly 0.94-1.2x, so **200% will
not be reachable on real games until the interpreter gets faster** - which is
[Recompiler-Plan.md](Recompiler-Plan.md). 150% is borderline. The setting is
still worth having: 50% is immediately useful, and the readout will tell the
truth about the rest.
