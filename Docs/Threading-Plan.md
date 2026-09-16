# Moving the machine off the message thread

## The conclusion first

Yes, it is worth doing, and yes it can be done - but the reason to do it is
**responsiveness and pacing, not speed**, and the first third of the benefit
costs almost nothing and needs no thread at all. Do it in three stages, ship
each one, and stop whenever the remaining benefit stops justifying the risk.

## What runs where today

`App::MainLoop` does everything on the one thread Windows delivers messages on:

```cpp
while (running_) {
    if (!PumpMessages(&message)) break;
    if (paused_) { Sleep(16); frame_limiter_.Reset(); continue; }
    ApplyPendingStates();
    PollInput();
    RunOneFrame();       // ~500k instructions, until the GPU's frame counter moves
    PumpAudio();         // can block - see below
    UpdateSpeedReadout();
    PresentFrame();      // can block on vsync
    LimitFrameRate();    // sleeps out the rest of the frame
}
```

Three things in that loop can stop the machine, and while any of them does, the
window is not pumping messages: it does not redraw, the menu does not open, and
a drag does not move it.

1. **Audio back-pressure.** `WASAPIAudioEngine::QueueAudio` waits for room:
   `while (availableFrames < frameCount && m_playing) sleep_for(1ms)`. Whenever
   the device buffer is full, the whole loop stops there.
2. **Present.** With vsync on, `PresentFrame` blocks until the monitor is ready.
3. **A slow frame.** A disc read from the network share, or a heavy scene.

None of this is a correctness bug, and the frame limiter is written to compose
with the first two rather than fight them (see its comment). It is a
responsiveness problem, and it gets worse the moment speed control lands: a
device that only drains 44,100 samples a second is a hard brake at 200%, which
is the subject of [Emulation-Speed-Plan.md](Emulation-Speed-Plan.md).

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
core is owned by `App`; the threading belongs entirely to the front end.

## Stage 1: stop the audio brake (no threads)

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

## What this does not buy

**Throughput.** The interpreter already runs the BIOS boot at 1.57x real time
on this machine, and the heavier discs at around 0.94-1.2x; the work is one
CPU-bound thread either way and moving it does not make it faster. If the goal
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
