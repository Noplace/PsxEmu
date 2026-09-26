# Threading: the window, the machine, video, audio and input

**Complete, 2026-09-24.** All eight phases are done. The machine, video, audio,
input and now the rasteriser each have a thread, and the UI thread does nothing
but answer the window. What each phase cost and bought is in the phase table;
what was measured is under "What it buys, in numbers".

Phase 7 landed last (bug 91) and is the only one that made the machine faster
rather than merely better paced: 13-19% recompiled, and the BIOS shell's ceiling
in the front end goes from 144-165% to 184-188% at a 300% setting. It is off by
default - Settings > Emulation > Rasterise on a GPU Thread - because it is the one phase
whose benefit depends on the workload. Its second half, disc read-ahead, is
built and byte-identical but measured as noise on this host: Windows' own file
cache was already doing the job.

What is *not* finished is the part no harness can reach - see "Not covered"
under How it was verified. An hour of real play, with the menus in use, states
saved and loaded, discs swapped and the window dragged, is still the soak this
cannot perform on its own.

## The conclusion first

It can be done, it is how the performance-focused emulators are built, and it
now is. The front end has six threads - the window, the machine, video, audio,
input and the rasteriser - sharing nothing except a handful of typed channels.
The emulated machine itself (`psx/`) is exactly as single-threaded and deterministic as it
was, so every checksum in [Test-Suite.md](Test-Suite.md) still means what it
meant - and `host_test` proves it, by running a threaded BIOS boot to
`boot_runner`'s own instruction count.

What it bought is **pacing and responsiveness**, not speed. The thread running
the machine does nothing but emulate: no upload or present, no sound-card calls,
no pad polling, no waiting for the monitor. Menus, drags and dialogs no longer
freeze the game. Sound is pulled at the device's own pace, and running short is
a gap rather than noise.

For phases 0 to 6 that was all it bought: the CPU core is about 90% of the work,
it stays one thread, and the recompiler is what moves it. Phase 7 is the one
exception - the rasteriser is not the CPU, so moving it off the machine's thread
did make the machine faster, by 13-19% recompiled. The CPU core itself is still
one thread and still the ceiling.

It was built in the phases below, in order, each verified before the next. The
risky one - the machine leaving the window's thread - came after everything it
depends on already worked.

What made phase 7 safe is that the rasteriser stopped reading live state - each
piece of drawing carries the state it was issued under - so every read of VRAM
can simply wait for it, and a threaded run is byte-identical to an unthreaded
one. The GPU's *timing* did not move: draw ticks, the GP0 queue and GPUSTAT's
ready bits are things a game can see, so they stay on the machine thread.

## What the standard is

The performance-focused emulators split the same way:

| | Threads | Sound | Frames |
|---|---|---|---|
| DuckStation | UI (Qt), core, video, the audio backend's own | the core thread writes a lock-free ring (50 ms by default); the backend's thread pulls from it, with time-stretching to absorb drift | the video thread owns the renderer - hardware and software - and presents; at most two frames queued between it and the core |
| PCSX2 | UI (Qt), EE/IOP, GS ("MTGS"), optionally VU1 ("MTVU"), the audio backend's own | a ring pulled by the backend, time-stretched | the GS thread renders and presents from a ring of GS packets |
| Dolphin | UI (Qt), CPU, GPU ("dual core", the default), the audio backend's own | a mixer pulled by the backend | the GPU thread runs the command FIFO and the video backend |

The DuckStation column is read from its source (`Temp/duckstation`):
`VideoThread` is on by default with `MaxQueuedFrames` 2,
`CoreAudioStream` is a ring with atomic read and write positions that the
backend's thread drains through `ReadFrames`, and `System` polls input on the
core thread, right after the frame's throttle wait.

What they have in common is the design here:

1. **Nothing emulated runs on the UI thread.** The UI only asks. The machine's
   thread applies the request between frames - DuckStation's
   `Host::RunOnCoreThread`.
2. **One owner per object.** Only the machine's thread touches the machine,
   only the video thread touches the graphics device, and so on.
3. **Output is decoupled by buffers of a stated depth**: a ring of samples the
   device pulls from, and a queue of one or two frames.
4. **Pacing has one master** - a throttle on the machine's thread - and every
   other thread follows it through rate control rather than blocking it.
5. **They split only where the coupling is loose.** The CPU, the SPU, DMA and
   the timers interact every few hundred cycles, and DuckStation keeps all of
   them on its core thread; putting the SPU on a thread of its own would cost
   more in synchronisation than it saves. The GPU's command stream is a FIFO,
   so it *can* be split, and all three split it. Host I/O is always split.

## What runs where today

*As of 2026-09-18, with phases 0 to 6 built.* Five threads, each owning one
thing and talking to the others only through the channels below.

```cpp
// The machine's thread - host::Machine::Run, in PSXEmu.Core/host/
while (!stop) {
    requests.Drain(*this);               // boot, reset, states, settings, pause
    if (paused) { doorbell.Wait(100ms); continue; }
    apply_input(system, input.Take());   // what the input thread last read
    RunOneFrame();                       // until the GPU's frame counter moves
    PublishFrame();                      // one atomic exchange to the video thread
    PumpAudio();                         // resampled into the ring the audio thread pulls
    Pace();                              // the frame limiter, and nothing else
}
```

The UI thread is `GetMessage`/`DispatchMessage` and nothing else. The video
thread waits for a frame and presents it - vsync blocks there and nowhere else.
The audio thread is paced by the device: wait, ask how much room, fill it from
the ring, put silence in whatever the ring could not supply. The input thread
polls the pads, the keyboard and the mouse a thousand times a second.

What that fixed, in the order it used to hurt:

- **A menu, a drag or a dialog no longer stops the machine.** They block the UI
  thread only. Whether the machine *should* keep running under a menu is now a
  setting - Settings > Emulation > Pause While in Menus, off by default, which is what
  DuckStation, PCSX2 and Dolphin do.
- **A long frame no longer stops the window.** Messages are answered while the
  machine is mid-frame, so the window redraws, the menu opens and a drag moves.
- **The machine no longer pays for the output.** Uploading and presenting the
  frame, feeding the sound card and polling the pads have left its frame
  budget. The one that could actually stall it is gone too: when the machine
  outruns the monitor - 200% on a 60 Hz display - `Present` blocks the video
  thread, and the mailbox drops the frames nobody could show, rather than the
  monitor pacing the machine.
- **Sound is pulled, not pushed.** Running short is silence the audio thread
  puts in deliberately, counted, instead of whatever the device's buffer held
  last - which is what bugs 61 and 63 both were.

What one thread still costs: nothing about emulation itself. A frame of the
BIOS shell is the same 12 ms of interpreter either way.

## The design

### Threads, and what each one owns

| Thread | Owns - and nothing else touches it | What it does |
|---|---|---|
| **UI** (today's main thread) | the window, the menus and dialogs, the settings file, and its own copy of the settings for ticking menus | `GetMessage`/`DispatchMessage` and nothing else. A menu command becomes a request to the thread that owns what it changes; so does a hotkey (Space, F1-F8). |
| **Machine** | `System` - every emulated component - with the frame limiter, the speed resampler, save states, memory cards and the recompiler | take requests; read the input snapshot; `RunOneFrame`; hand the frame to video and the samples to audio; set the rumble; pace. While paused it waits for a request instead of running frames. |
| **Video** | the `IGraphicsEngine` - D3D11 or D3D12 - with its swap chain and filters | wait for a new frame or a request; apply resize, filter, renderer and vsync changes; upload, draw, present. Vsync blocks this thread and no other. |
| **Audio** | the `IAudioEngine` - WASAPI or DirectSound | paced by the device. WASAPI in event-driven mode wakes when the device wants data; DirectSound runs on a 5 ms timer and tops up ahead of its write cursor. Both read the sample ring and write silence for whatever it cannot supply. Registered with MMCSS as "Pro Audio", as render threads normally are, so a busy machine cannot starve it. |
| **Input** | the XInput pads, the keyboard, the raw mouse (through a message-only window of its own) and the rumble motors | poll at 1 kHz and publish a snapshot; apply rumble when it changes. |

### The channels

Every exchange between threads goes through one of these. Anything not in this
table is owned by exactly one thread.

| From → to | Carries | How | Depth, and what happens at the edges |
|---|---|---|---|
| UI → machine | boot, reset, pause, save and load states, discs, cards, settings, View VRAM | a queue of requests - a mutex around a vector of `std::function<void(Machine&)>`, swapped out whole - plus a doorbell to wake a paused machine | unbounded but tiny: one menu click is one entry |
| UI → video | resize, filter, renderer | the same kind of queue | drained before every present |
| UI → audio | which backend | the same | drained on every wake |
| machine → video | finished frames | a three-slot mailbox: the machine fills a free slot and publishes it with one atomic exchange; video takes the newest | never more than one frame behind. A frame video did not get to is dropped and counted - which is also exactly how 200% on a 60 Hz monitor shows every other frame |
| machine → audio | samples | a single-producer, single-consumer lock-free ring, 8,192 frames - 186 ms | empty: silence, counted. Full: the newest samples are dropped, counted. The rate trim holds it at its target so neither happens in steady state; if it ever ends up four times past the target anyway, the audio thread skips down to twice it rather than playing a backlog late |
| input → machine | pads, keyboard, mouse buttons and focus | the latest snapshot behind a small lock; mouse motion adds up until the machine takes it | latest wins, and it is never more than a millisecond old |
| machine → input | motor levels | two atomics per pad | latest wins |
| any → UI | the title-bar speed, errors, "this is now the state" | `PostMessage` with a small payload | **never `SendMessage`** - see the rules |

The frame copy costs about 1.2 MB of `memcpy` a frame, a tenth of a
millisecond. `Gpu::framebuffer` is resolved at the start of vblank, which is
exactly when `RunOneFrame` returns, so the machine copies a finished picture and
never a half-drawn one. View VRAM ships the raw 1 MB of VRAM instead and video
converts it.

### Pacing

One master, as now: the frame limiter on the machine's thread, at the emulated
refresh rate times the speed setting. The others follow it.

- **Audio**: the resampler's half-percent trim steers by the ring's fill level
  instead of the device's queue - the same control loop, measuring a different
  buffer. The target is 40 ms (`Machine::kAudioTargetFrames`): enough to cover
  the machine's 17 ms bursts plus the device taking its 10 ms, with room to
  spare. DuckStation holds 50 ms for the same reason.
- **Video**: shows the newest frame at the monitor's next refresh with vsync
  on, or at once with it off.

Standard options this makes possible, and which are not part of this plan:
syncing to the host's refresh rate on a 60 Hz display, variable refresh rate,
time-stretched audio, and a pre-frame sleep to cut input latency.

### Pause, menus and dialogs

A menu, a drag and a dialog block only the UI thread, so the game keeps running
under them - which is what DuckStation, PCSX2 and Dolphin do. Bug 63's wiring
went with the thread: `EnterStall` and `LeaveStall` are gone, and left in they
would have silenced a game that was still playing.

Whether a menu *should* stop the game is a matter of taste, so it is a setting:
**Settings > Emulation > Pause While in Menus**, `EmuConfig::pause_in_menus`, off by
default. On, `WM_ENTERMENULOOP` and the dialogs this thread opens itself hold a
counted `kPausedForMenu` on the machine - counted because a file picker can open
over a menu, and the machine should come back only when the last of them closes.

Pause is a request like any other. The machine stops running frames and waits on
its doorbell rather than polling, so it still answers requests at once - a
paused machine ran one 0.04 ms after it was posted. The ring runs dry and the
audio thread fills silence; video keeps the last frame and shows it again on a
resize, so the window does not go blank while it is dragged.

### Starting and stopping

**Start**: the UI creates the window and the machine, puts the settings file's
choices on it, and mounts a disc named on the command line - all while it is
still the only thread there is. Then it starts them outputs-first: input, audio,
video, and the machine last, so nothing is producing before there is somewhere
to put it. Each device is created on the thread that will use it and reports
what actually opened by posted message - which is how the menu comes to tick the
renderer that opened rather than the one that was asked for. The UI never waits
for any of it.

**Stop**, in this order: the machine first, so nothing more is produced; then
video, which releases the swap chain before the window goes; then audio; then
input; then `DestroyWindow`. The UI joins each thread with
`MsgWaitForMultipleObjects`, so it keeps pumping messages while it waits.

### Where the code goes

- **`psx/`: unchanged.** Still single-threaded, still deterministic.
- **`PSXEmu.Core/host/`: new, and the only thread-aware part of Core.** The
  channels - `doorbell.h`, `request_queue.h`, `sample_ring.h`,
  `frame_mailbox.h`, `input_exchange.h` - and the three threads that use them:
  `Machine`, which owns a `System` and runs the loop above, `AudioOutput` and
  `VideoOutput`. No window, no device and no message loop anywhere in it; the
  front end passes those in as a factory or an interface.

  This deliberately changes the old rule that nothing in Core is thread-aware.
  The machine still is not; the new code is the loop that drives it. Putting
  that loop in Core is what lets `host_test` run the real thing headlessly -
  the only way threading gets a test other than a person watching a window.
- **`PSXEmu.Win32/`**: the UI thread - `App`, now the window, the menus and the
  settings - plus the two pieces the threads in `host/` need from Windows:
  `video_presenter.h/.cpp` (the Direct3D engine the video thread draws with) and
  `input_thread.h/.cpp` (the pads, keyboard and raw mouse).
- **`IAudioEngine` turns from push into pull**: no `QueueAudio`, but
  `WaitForRoom`, `WritableFrames` and `WriteFrames`, which is the loop
  `AudioOutput`'s thread runs. The engines own no thread themselves - one owns
  them - which is what keeps `host/` the only place in Core that starts one.
  `IGraphicsEngine` keeps its interface; only the thread calling it changed.

## Phases

| # | Change | Done | What it bought |
|---|---|---|---|
| 0 | Per-stage times in the title bar - Emulation > Show Timings - plus the Pause While in Menus setting beside it | yes | the "before" numbers, from a build kept as `Build\x64\Release\PSXEmu.Win32.single-thread.exe` |
| 1 | The channels in `host/`: doorbell, request queue, sample ring, frame mailbox, input exchange, with `host_test` | yes | the foundation, and 20 checks that hold it - each one proved by planting a bug in a copy of the header and watching it fail |
| 2 | One door: every menu command is a request; the UI keeps its own `EmuConfig` and the machine is sent a copy | yes | folded into phase 5 rather than shipped alone: the `App` restructure is one change |
| 3 | Audio thread: pull, paced by the device, silence when short | yes | bug 61 and 63's DirectSound machinery retired - no guard, no prime, no resync-on-write. 0 shortfalls over 8 s on both backends |
| 4 | Video thread and the frame mailbox | yes | present and the upload left the machine's frame; a resize repaints from the video thread; frames the monitor cannot show are dropped and counted |
| 5 | Machine thread; the UI becomes a pure `GetMessage` pump; the bug 63 stall wiring comes out | yes | menus, drags and dialogs no longer freeze the game - and Pause While in Menus is there for anyone who wants the old behaviour |
| 6 | Input thread, at 1 kHz, owning raw mouse input on a message-only window of its own | yes | an empty XInput slot's once-a-second probe can no longer hitch a frame; the reading a frame takes is at most a millisecond old |
| 7 | A GPU thread (DuckStation-style - registers and timing stay with the machine, rasterising moves behind a FIFO), and disc read-ahead for images on the network share | yes | bug 91. 19.4% on the BIOS shell, 17.7% on Wild Arms, 13.1% on Ridge Racer, recompiled - at or above the 12% guessed here. Off by default: Settings > Emulation > Rasterise on a GPU Thread, or `gpu_thread` in the settings file. In the front end it lifts the BIOS shell from 144-165% to 184-188% at a 300% setting. The read-ahead measured as noise on this host: Windows' file cache was already doing it |

### What it buys, in numbers

Measured on the BIOS boot, from the title-bar readout, single-threaded build
against threaded one (Emulation > Show Timings shows all of these live):

| | single-threaded | threaded |
|---|---|---|
| emulate | 6.9-12.6 ms | 7.7-12.2 ms |
| present | 0.3-0.4 ms, **in the machine's frame** | 0.44-0.67 ms, on the video thread |
| audio + input | 0.05 ms, in the frame | 0.03-0.17 ms hand-off, in the frame |
| idle (headroom) | 3.8-9.5 ms | 4.6-9.0 ms |
| closing the window | 266 ms | 48-271 ms |

So the machine's own frame got about half a millisecond back in the ordinary
case - 3% of a frame at 100%, and the honest answer to "does threading make it
faster". What it actually bought is everything that is no longer *possible*: a
menu cannot stop the machine, a slow frame cannot stop the window, and a
monitor slower than the machine cannot pace it.

- **Emulation is unchanged**: Wild Arms is 10.0 ms of interpreter per frame
  (1.69x) and 4.4 ms recompiled (3.89x) either way -
  [Recompiler-Plan.md](Recompiler-Plan.md). Threads move work off the machine's
  thread; they do not make the machine faster.
- **Sound**, measured against both real devices for 8 s with the BIOS running:
  0 frames short, 0 dropped, 0 DirectSound resyncs. WASAPI sits at 45-66 ms in
  the ring plus 23.5 ms in the device; DirectSound at 53-96 ms plus 60 ms. The
  ring is held at 40 ms by the same half-percent trim as before, now measuring
  the ring rather than the device's own queue.
- **Opening a device is slow**: WASAPI took ~600 ms to open on this machine,
  during which the machine can already be producing. The audio thread catches up
  by skipping the ring down to twice the target rather than playing a backlog
  late for the twenty seconds the trim would need - `AudioOutput::Run`.

## The rules

1. **One owner per object.** `System` belongs to the machine thread, the
   graphics engine to video, the audio engine to audio, the window and menus to
   the UI. Anything else goes through a channel.
2. **Nothing waits for the UI thread.** No `SendMessage` to our window, and
   therefore no `SetWindowTextW` on it and no `MessageBox` from another thread,
   because both send and wait - joined at shutdown, that is a deadlock.
   Everything a thread has to say goes through `App::PostToUi`, which posts a
   `std::function` the window procedure runs and deletes. That is why
   `CreateGraphicsEngine` returns the text of its fallback warning instead of
   putting the dialog up itself: it runs on the video thread now.
3. **The UI waits for nothing**, except the joins at shutdown, and those pump.
4. **Save states and memory cards are touched only on the machine thread,
   between frames.** A state written from anywhere else is torn, and no
   single-threaded harness will ever see it.
5. **Pause keeps taking requests**, or the menu deadlocks against a paused
   machine.
6. **Menus tick from the UI's own copy of the settings**, never from
   `system_->config()`, which the machine thread writes.
7. **COM is per thread.** The audio thread joins its own apartment and creates
   its WASAPI objects there.
8. **The swap chain lives and dies on the video thread**: created, resized,
   presented and released there, and released before the window is destroyed.
9. **Every channel counts its edges** - frames dropped, samples short, samples
   dropped - and the counts are shown or logged. A channel that silently loses
   data is exactly what a checksum cannot see.

## How it was verified

The machine stays single-threaded, so **every checksum in Test-Suite.md had to
be unchanged** - and is. That proves the machine computes the same thing, and
nothing at all about the threads. What was done for those:

- **`host_test`, 33 checks** (Test-Suite.md has the list): every channel under
  two threads with sequence numbers in each item; the machine's thread landing
  on `boot_runner`'s exact instruction count and checksum, again under 432
  pause and resume requests, and again across a state saved and reloaded;
  forty machines stopped mid-run, the slowest back in 17 ms; and all three
  threads together for five seconds with nothing short, dropped or unshown.
- **The channel checks were mutation-tested.** Three bugs planted in copies of
  the headers - a producer that keeps writing into the slot it handed over, a
  ring write that loses its wrapped half, motion replaced instead of
  accumulated - and each one failed the check that should catch it.
- **Both real sound devices**, with the BIOS running and the machine paced
  normally: 8 s each, 0 frames short, 0 dropped, 0 resyncs.
- **The built emulator itself**, driven by posted `WM_COMMAND`s with its frame
  rate read back from the title bar - it turns out the front end *can* be run
  from an agent session after all, which the old note here denied. Pause, View
  VRAM, both renderers, both sound backends, the recompiler, 200% speed, reset,
  reboot, window resizes and the menu-loop messages, over 45 seconds: 59.3 fps
  throughout and a clean exit every time.
- **Not covered**: what a frame actually looks like, and what anything sounds
  like. Those are still yours - an hour of a game with the menus in use, states
  saved and loaded, discs swapped and the window dragged is the soak this
  cannot do.

## History

- **2026-09-16 - stage 1, the audio wait.** `IAudioEngine::QueueAudio` used to
  wait for room in the device's buffer, which stopped the machine, and with it
  the window, whenever the sound card was behind. It now takes what fits and
  returns the count, and `App::PumpAudio` offers the rest again next frame.
  BIOS boot `c7c8db90c5984798` / 97,749,265 instructions and both disc
  baselines unchanged.
- **2026-09-18 - bug 63.** Pausing, holding a menu open or dragging the window
  made DirectSound replay its last second of sound. `EnterStall` and
  `LeaveStall` now stop the device while the loop is not running frames. This
  is a single-thread fix and phase 5 removes it.
- **2026-09-18 - dead thread code.** `System` still carried `Run`, `Stop` and
  a `thread_func` that spun a wall-clock-paced `Step` on a `std::thread`.
  Nothing started it, and it had no audio, no present and no locking, so it was
  removed rather than built on.
- The first version of this plan had three stages: the audio wait, then one
  door into the machine, then one thread for the machine that also presented.
  The first was done on its own; the other two became phases 2 and 5, with
  presenting, sound and input each given a thread of their own instead.
- **2026-09-18 - phases 0 to 6.** Built in the order above, over one sitting.
  Two things found on the way that were not threading at all:
  - `Sio::motor_state` took a parameter called `small`, which is a macro in
    Windows' own RPC headers (`#define small char`). Any file that reached
    `<dsound.h>` or `<objbase.h>` before `sio.h` stopped compiling with a
    syntax error pointing at the wrong line. Renamed.
  - The SPU still had an optional push sink - `set_audio_engine`, pushing every
    frame at an `IAudioEngine` - that nothing had ever set. It went with the
    push API.
