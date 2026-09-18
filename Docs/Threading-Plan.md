# Threading: the window, the machine, video, audio and input

## The conclusion first

Yes, it can be done, and it is how the performance-focused emulators are built.
The design below gives the front end five threads - the window, the machine,
video, audio and input - that share nothing except a handful of typed channels.
The emulated machine itself (`psx/`) stays exactly as single-threaded and
deterministic as it is today, so every checksum in
[Test-Suite.md](Test-Suite.md) still means what it means now.

What it buys is **headroom, pacing and responsiveness**. The thread running the
machine stops doing anything but emulate: no upload or present, no sound-card
calls, no pad polling, no waiting for the monitor. Menus, drags and dialogs stop
freezing the game. Sound is pulled at the device's own pace, and running short
becomes a gap, never noise. What it does not buy is a faster CPU core: that is
about 90% of the work, it stays one thread, and the recompiler is what moves it.

Build it in phases, each one shippable and verified on its own. The risky one -
the machine leaving the window's thread - comes after everything it depends on
already works.

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

*As of 2026-09-18.* `App::MainLoop` does everything on the one thread Windows
delivers messages on:

```cpp
while (running_) {
    if (!PumpMessages(&message)) break;
    if (paused_) { EnterStall(); Sleep(16); continue; }
    LeaveStall();          // restarts the sound device after a stall
    ApplyPendingStates();
    PollInput();           // XInput, the keyboard, the mouse -> Sio
    RunOneFrame();         // ~500k instructions, until the GPU's frame counter moves
    PumpAudio();           // takes what fits and never waits
    UpdateSpeedReadout();
    PresentFrame();        // upload, draw, present
    LimitFrameRate();      // sleeps out the rest of the frame
}
```

Sharing one thread goes wrong in both directions.

**The machine holds up the window.** While a frame is being run or presented,
messages wait: the window does not redraw, the menu does not open, and a drag
does not move it. A slow frame - a disc read from the network share, a heavy
scene - makes that visible.

**The window holds up the machine.** A menu, a drag or resize of the window,
and a dialog each run a modal loop of Windows' own inside a message this loop
dispatched, and `MainLoop` gets no control back until it ends. The machine
stops for as long as the menu stays open or the mouse button stays down. Since
bug 63 the sound device stops with it rather than DirectSound replaying its
last second, but the freeze itself is what one thread costs.

**The machine pays for the output.** Uploading and presenting the frame,
feeding the sound card and polling the pads all come out of the frame budget
the emulation needs. Present is cheap while the machine produces frames more
slowly than the monitor shows them - the swap chains are two-buffer flip model,
and at 59 fps on a 165 Hz display a back buffer is normally free - but when the
machine outruns the monitor, `Present` blocks and the monitor paces the
machine: 200% speed on a 60 Hz display cannot happen with vsync on.

## The design

### Threads, and what each one owns

| Thread | Owns - and nothing else touches it | What it does |
|---|---|---|
| **UI** (today's main thread) | the window, the menus and dialogs, the settings file, and its own copy of the settings for ticking menus | `GetMessage`/`DispatchMessage` and nothing else. A menu command becomes a request to the thread that owns what it changes; so does a hotkey (Space, F1-F8). |
| **Machine** | `System` - every emulated component - with the frame limiter, the speed resampler, save states, memory cards and the recompiler | take requests; apply pending states; read the input snapshot; `RunOneFrame`; hand the frame to video and the samples to audio; set the rumble; pace. While paused it waits for a request instead of running frames. |
| **Video** | the `IGraphicsEngine` - D3D11 or D3D12 - with its swap chain and filters | wait for a new frame or a request; apply resize, filter, renderer and vsync changes; upload, draw, present. Vsync blocks this thread and no other. |
| **Audio** | the `IAudioEngine` - WASAPI or DirectSound | paced by the device. WASAPI in event-driven mode wakes when the device wants data; DirectSound runs on a 5 ms timer and tops up ahead of its write cursor. Both read the sample ring and write silence for whatever it cannot supply. Registered with MMCSS as "Pro Audio", as render threads normally are, so a busy machine cannot starve it. |
| **Input** | the XInput pads, the keyboard, the raw mouse (through a message-only window of its own) and the rumble motors | poll at 1 kHz and publish a snapshot; apply rumble when it changes. |

### The channels

Every exchange between threads goes through one of these. Anything not in this
table is owned by exactly one thread.

| From → to | Carries | How | Depth, and what happens at the edges |
|---|---|---|---|
| UI → machine | boot, reset, pause, states, discs, cards, settings | a queue of requests - a mutex around a vector of `std::function<void(System&)>`, swapped out whole - plus an event to wake a paused machine | unbounded but tiny: one menu click is one entry |
| UI → video | resize, filter, renderer, vsync, View VRAM | the same kind of queue | drained before every present |
| UI → audio | which backend | the same | drained on every wake |
| machine → video | finished frames | a three-slot mailbox: the machine fills a free slot and publishes it with one atomic exchange; video takes the newest | never more than one frame behind. A frame video did not get to is dropped and counted - which is also exactly how 200% on a 60 Hz monitor shows every other frame |
| machine → audio | samples | a single-producer, single-consumer lock-free ring, about 200 ms | empty: silence, counted. Full: the newest samples are dropped, counted. The rate trim holds it at its target so that neither happens in steady state |
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
  instead of the device's queue - the same control loop, measuring a
  different buffer. Target about 50 ms, which is DuckStation's default.
- **Video**: shows the newest frame at the monitor's next refresh with vsync
  on, or at once with it off.

Standard options this makes possible, and which are not part of this plan:
syncing to the host's refresh rate on a 60 Hz display, variable refresh rate,
time-stretched audio, and a pre-frame sleep to cut input latency.

### Pause, menus and dialogs

A menu, a drag and a dialog block only the UI thread, so the game keeps running
under them - which is what DuckStation, PCSX2 and Dolphin do. The bug 63 wiring
(`WM_ENTERMENULOOP`, `WM_ENTERSIZEMOVE` and `WM_ENTERIDLE` calling
`EnterStall`) comes out with the thread; left in, it would silence a game that
is still playing. "Pause while a menu is open" can be a setting later if anyone
wants it.

Pause becomes a request. The machine stops running frames and waits on its
request event, not a `Sleep(16)` poll, so it still answers requests. The ring
runs dry, so audio plays silence. Video keeps the last frame and presents it
again on a resize - the window stops going blank while it is dragged.

### Starting and stopping

**Start**: the UI creates the window, then starts input, audio and video. Each
creates its own device on its own thread and reports success or failure by
posted message. Then it builds the machine and starts its thread, and enters the
message loop. The UI never waits for another thread's answer.

**Stop**, in this order: the machine first, so nothing more is produced; then
video, which releases the swap chain before the window goes; then audio; then
input; then `DestroyWindow`. The UI joins each thread with
`MsgWaitForMultipleObjects`, so it keeps pumping messages while it waits.

### Where the code goes

- **`psx/`: unchanged.** Still single-threaded, still deterministic.
- **`PSXEmu.Core/host/`: new, and the only thread-aware part of Core.** The
  channels (request queue, sample ring, frame mailbox, input snapshot) and
  `MachineThread`, which owns a `System`, runs the loop above, and talks to the
  outside only through channels. No Win32 in it.

  This deliberately changes the old rule that nothing in Core is
  thread-aware. The machine still is not; the new code is the loop that drives
  it. Putting that loop in Core means a headless harness can run it, which is
  the only way threading gets a test other than a person watching a window.
- **`PSXEmu.Win32/`**: the UI thread - `App` shrinks to the window, the menus
  and the settings - and the three device threads on the far side of the
  channels.
- **`IAudioEngine` turns from push into pull**: each engine runs its own
  thread and asks a source for samples, the way DuckStation's `AudioStream`
  does. `IGraphicsEngine` keeps its interface; only the thread that calls it
  changes.

## Phases

| # | Change | What it buys | Verified by |
|---|---|---|---|
| 0 | Per-stage times in the title-bar readout: emulate, present, audio, input | the "before" numbers, so the later phases can be measured against them rather than assumed | you reading them |
| 1 | The channels in `host/`, with a `host_test` harness | nothing visible - the foundation | two threads pushing sequence-numbered, checksummed data through each channel: nothing lost, duplicated or torn, and the edge counts right |
| 2 | One door: every menu command becomes a request; the UI keeps its own copy of the settings; still one thread | nothing visible - it is what makes phase 5 small | every checksum unchanged, and no `system_->` left in a UI handler |
| 3 | Audio thread: pull, paced by the device, silence when short | a short ring sounds like a gap, never garbage, by construction - bug 61 and 63's DirectSound machinery retires; latency can come down | `host_test` with a fake device pulling in real time; your ear |
| 4 | Video thread and the frame mailbox | upload and present leave the machine's frame; 200% on a 60 Hz display; the window keeps its picture while being resized | `host_test` on the mailbox; your eye |
| 5 | Machine thread: `MachineThread` runs the loop, the UI becomes a pure pump, the stall wiring comes out | menus, drags and dialogs no longer freeze the game; a slow frame never makes the window stop responding | the threaded loop matching `boot_runner`'s checksums; stopping it under load 100 times; your soak |
| 6 | Input thread | device stalls (the once-a-second probe of each empty XInput slot) can no longer hitch a frame; input at most 1 ms old | `host_test` on the snapshot; your hands |
| 7 | Only if phase 0 asks: a GPU thread (DuckStation-style - the GPU's registers and timing stay with the machine and rasterising moves behind a FIFO), and a disc read-ahead thread for images on the network share | the rasteriser is 4-5% of a game's time interpreted ([GPU-SPU-Optimisation-Plan.md](GPU-SPU-Optimisation-Plan.md)), and about 12% recompiled - its 0.76 s of Wild Arms' 1,500 frames against the 6.5 s they take at 3.89x | the twelve-disc table, byte for byte |

Phases 3 and 4 work with the machine still on the UI thread, which is why they
come first: each takes something off the machine's thread and can be judged on
its own before the big move.

### What it buys, in numbers

- **Emulation today**: Wild Arms runs 10.0 ms of emulation per 16.9 ms frame
  interpreted (1.69x) and 4.4 ms recompiled (3.89x) -
  [Recompiler-Plan.md](Recompiler-Plan.md). At 200% the whole budget is 8.4 ms.
- **What leaves the machine's thread**: the upload and present, the audio
  calls and the pad polls. Their cost has never been measured - it cannot be
  from a headless run - and phase 0 is what measures it. The expectation is
  around a millisecond a frame in the ordinary case, plus the stalls: `Present`
  blocking whenever the machine outruns the monitor, and the empty-slot XInput
  probe.
- **What does not move**: emulation itself. On one thread or five, a frame of
  Wild Arms is 10.0 ms interpreted.

## The rules

1. **One owner per object.** `System` belongs to the machine thread, the
   graphics engine to video, the audio engine to audio, the window and menus to
   the UI. Anything else goes through a channel.
2. **Nothing waits for the UI thread.** No `SendMessage` to our window, and
   therefore no `SetWindowTextW` on it and no `MessageBox` from another thread,
   because both send and wait. `UpdateSpeedReadout` and the `ShowError` calls
   in `ApplyPendingStates` do exactly this today, and joined at shutdown they
   are a deadlock. Post instead.
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

## How it gets verified

The machine stays single-threaded, so **every checksum in Test-Suite.md must be
unchanged at every phase**. That proves the machine still computes the same
thing, and nothing about the threads. For those:

- **`host_test`** puts each channel under stress: two threads, millions of
  sequence-numbered items with checksummed payloads. Nothing lost, nothing
  duplicated, no frame torn, and the edge counters counting what they should.
- **The threaded machine, headless.** `MachineThread` with a fake video that
  checksums every frame it gets and a fake audio that pulls at 44,100 a second.
  With no requests, its last frame must match `boot_runner`'s checksum for the
  same run - the BIOS boot's `c7c8db90c5984798`. With requests fired at random
  times - pause, save, load, reset - nothing hangs or crashes, and a state saved
  by the threaded run must load into `boot_runner` and play on identically.
- **Stopping under load**: start, run, stop, a hundred times, each within a
  timeout.
- **AddressSanitizer** on `host_test`, for anything used after a thread has
  released it. MSVC has no ThreadSanitizer, so races are ruled out by rules 1
  and 2 rather than detected.
- **The front end** still cannot be run from an agent session, so the last
  word is yours: an hour of a game with the menus in use, states saved and
  loaded, discs swapped, backends switched, the window dragged and resized.

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
  The first is done. The other two are phases 2 and 5 above, with presenting,
  sound and input each given a thread of their own.
