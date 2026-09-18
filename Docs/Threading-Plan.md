# Moving the machine off the message thread

## The conclusion first

Yes, it is worth doing, and yes it can be done - but the reason to do it is
**responsiveness and pacing, not speed**, and the first third of the benefit
costs almost nothing and needs no thread at all. Do it in three stages, ship
each one, and stop whenever the remaining benefit stops justifying the risk.

## What runs where today

*As of 2026-09-18, after stage 1 and bug 63.* `App::MainLoop` does everything
on the one thread Windows delivers messages on:

```cpp
while (running_) {
    if (!PumpMessages(&message)) break;
    if (paused_) { EnterStall(); Sleep(16); continue; }
    LeaveStall();          // restarts the sound device after a stall
    ApplyPendingStates();
    PollInput();
    RunOneFrame();         // ~500k instructions, until the GPU's frame counter moves
    PumpAudio();           // takes what fits and never waits - stage 1
    UpdateSpeedReadout();
    PresentFrame();        // can block on vsync
    LimitFrameRate();      // sleeps out the rest of the frame
}
```

Sharing one thread goes wrong in both directions.

**The machine holds up the window.** While a frame is being run or presented,
messages wait: the window does not redraw, the menu does not open, and a drag
does not move it. Two things can make that long:

1. **Present.** With vsync on, `PresentFrame` blocks until the monitor is
   ready - one refresh at most, 6 ms at 165 Hz.
2. **A slow frame.** A disc read from the network share, or a heavy scene.

There used to be a third, and it was the worst: `QueueAudio` waited for room in
the device's buffer, so the whole loop stopped whenever the sound card was
behind - and at 200% speed a device draining 44,100 samples a second was a hard
brake. Stage 1 below took the wait out, and the resampler in
[Emulation-Speed-Plan.md](Emulation-Speed-Plan.md) now hands the device 44,100
samples a second at any speed.

**The window holds up the machine.** A menu, a drag or resize of the window,
and a dialog each run a modal loop of Windows' own inside a message this loop
dispatched, and `MainLoop` gets no control back until it ends. The machine
stops for as long as the menu stays open or the mouse button stays down. Since
bug 63 the sound device stops with it - `WM_ENTERMENULOOP`, `WM_ENTERSIZEMOVE`
and `WM_ENTERIDLE` call `EnterStall`, and the next frame's `LeaveStall` starts
it again - where before, DirectSound replayed its last second on a loop. The
freeze itself is what one thread costs, and only stage 3 removes it.

None of this is a correctness bug. It is responsiveness: the frame limiter
composes with a blocking present rather than fighting it (see its comment), and
`LeaveStall` resets it so a stall is not made up afterwards.

## What makes this harder than it looks

**The menu mutates the machine directly.** `WM_COMMAND` handlers call
`BootDiscFromFile`, `ResetMachine`, `SetControllerType`, `system_->LoadDisc`,
`system_->mc(slot).LoadFile` and so on, straight from the message thread. With
the machine on another thread every one of those is a data race.

The codebase already contains the answer in miniature: save and load states are
*not* done in the handler. They set `pending_save_slot_` / `pending_load_slot_`
and `ApplyPendingStates()` performs them at the top of a frame. That pattern,
generalised into a command queue, is the whole of stage 2.

**The framebuffer is read by the presenter while the GPU writes it.** Today
that is safe because they are the same thread. Afterwards it is not, and there
are two ways out: hand the presenter a snapshot taken between frames, or keep
presenting on the emulation thread and let the UI thread do nothing but pump
messages. D3D11's immediate context is not free-threaded, so "present from
whichever thread feels like it" is not an option.

**Input has a foot in both camps.** The mouse arrives as `WM_INPUT` on the
message thread; the keyboard and XInput are polled. The emulation thread needs
a consistent snapshot per frame, not a half-updated one.

**Nothing in `PSXEmu.Core` is thread-aware**, and it should stay that way. The
core is owned by `App`; the threading belongs entirely to the front end. (Until
2026-09-18 `System` still carried `Run`, `Stop` and a `thread_func` that spun a
wall-clock-paced `Step` on a `std::thread`. Nothing started it, and it had no
audio, no present and no locking, so it was removed rather than built on.)

## Stage 1: stop the audio brake (no threads) - **done, 2026-09-16**

Implemented as described below. `IAudioEngine::QueueAudio` now returns how many
samples it took and never waits: WASAPI writes what fits in the device buffer,
DirectSound writes whole frames up to its safety margin, and both report the
count. `App::PumpAudio` keeps the remainder in `audio_pending_` and offers it
again next frame, capped at half a second so a device that has stopped draining
cannot grow it without bound.

Baselines unchanged: BIOS boot `c7c8db90c5984798` / 97,749,265 instructions,
Wild Arms and Captain Tsubasa J identical at all three checkpoints, 1,000
checks green. Which is the expected result - none of this touches emulated
time - but it is the assertion worth making.

What is *not* verified is the thing it was done for: that the window stays
responsive while the sound card is behind. That needs the front end, which
cannot be run from an agent session.

Make `QueueAudio` never block: write what fits, and either drop the rest or
keep a small ring of its own. Today's blocking wait exists because dropping
samples crackles - but blocking the *emulator* to protect the audio device is
the wrong trade, and it is the single biggest cause of the window going
unresponsive.

- Cheap and correct: return the number of samples actually queued and let
  `PumpAudio` keep the remainder for next frame in a small buffer. The SPU
  already produces into `audio_scratch_`, so the extra state is one index.
- The frame limiter, not the sound card, then paces the machine - which is what
  bug 49 decided it should be.

**Worth it on its own**, independent of everything below, and the only stage
that can be verified headlessly (a `frame_limiter_test`-style harness around a
fake audio sink).

## Stage 2: one door into the machine

Add a command queue - a mutex plus `std::vector<std::function<void()>>`, or a
small tagged struct if that reads better here - drained at the top of each
frame, in the same place `ApplyPendingStates` already runs.

Every `WM_COMMAND` case that touches `system_` becomes an enqueue. Nothing else
changes yet; the machine still runs on the message thread, so this is a pure
refactor with no races to debug, and it can be reviewed by grepping for
`system_->` in the handler and finding nothing.

Do this **before** the thread, not with it. It is the change that makes stage 3
small, and on its own it costs nothing at runtime.

## Stage 3: the thread

- `std::thread` started after `CreateMachine`, running its own loop: drain the
  command queue, apply pending states, take the input snapshot, run a frame,
  pump audio, pace.
- The UI thread's loop becomes `GetMessage`/`DispatchMessage` and nothing else.
- **Present from the emulation thread**, which keeps the D3D context on one
  thread and avoids a framebuffer copy. `WM_PAINT` on the UI thread does
  nothing; resize is the one case that needs care - enqueue it and let the
  emulation thread act on it between frames.
- Input: the UI thread writes a `PadState` snapshot behind a mutex (or two
  buffers and an atomic index); the emulation thread reads one per frame.
- Shutdown: `running_ = false`, join before `App`'s destructor releases
  anything. The window closing must not outrun the thread still touching the
  device.

### What to watch for

- **Save states** must be taken on the emulation thread. A state written from
  the UI thread mid-frame is a torn state, and the harness will never see it
  because the harness is single-threaded.
- **The menu's own reads** - ticking a checkbox from `system_->config()` - are
  reads of data the emulation thread writes. Keep a front-end copy of the
  settings, or route the reads through the queue too.
- **Pause** stops the emulation thread's work but must not stop it draining
  commands, or the menu deadlocks against a paused machine.

Found by reading the code on 2026-09-18, and not yet in the bullets above:

- **Calls that wait for the UI thread.** `UpdateSpeedReadout` sets the title
  with `SetWindowTextW`, which from another thread is a `SendMessage` that
  waits for the window's own thread to answer. The moment the UI thread waits
  on the emulation thread - joining it at shutdown - that is a deadlock. Post
  the text instead. The same goes for `ShowError` and `ShowWarning`, which
  `ApplyPendingStates` and machine setup raise and which would otherwise open
  from the emulation thread.
- **Input is mostly thread-free already.** The keyboard (`GetAsyncKeyState`)
  and XInput can be polled from the emulation thread directly. Only the mouse,
  which arrives as `WM_INPUT` on the UI thread and accumulates in `mouse_`,
  needs the snapshot described above.
- **The stall wiring comes out.** `WM_ENTERMENULOOP`, `WM_ENTERSIZEMOVE` and
  `WM_ENTERIDLE` call `EnterStall` because today a menu stops the machine
  (bug 63). On its own thread the machine keeps running under a menu, and
  leaving them in would silence a game that is still playing - from the wrong
  thread, too. Only pause should stall it then.

## What this does not buy

**Throughput.** Measured one run at a time, the interpreter runs the BIOS boot
at 1.71x real time and the heavier discs at 1.69-1.73x (about 99 fps); the work
is one CPU-bound thread either way and moving it does not make it faster. If the goal
is headroom - and at 200% speed it is - that is
[Recompiler-Plan.md](Recompiler-Plan.md), not this.

## How it gets verified

Badly, is the honest answer, and that is the real cost of stage 3. The front
end cannot be run from an agent session (Gaps.md), the harnesses are all
single-threaded by construction, and races surface as rare corruption rather
than as a failing check. What can be done:

- Stage 1 gets a real unit test around a fake sink.
- Stage 2 is verifiable by inspection plus the existing disc baselines, since
  behaviour should not move at all: **every checksum in Test-Suite.md must be
  unchanged**.
- Stage 3 gets the same baseline run - unchanged checksums prove the machine
  still computes the same thing - plus a soak: an hour of a game running with
  the menu being used, states saved and loaded, discs swapped.

Stage 3 is the first change in this project whose correctness a checksum cannot
establish. That is not a reason to skip it, but it is a reason to do stages 1
and 2 first and see how much of the problem they solve.
