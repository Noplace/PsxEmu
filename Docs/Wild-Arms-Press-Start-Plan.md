# Wild Arms: blank after "press start"

The opening film plays with sound, the menu appears and asks for Start, and
after Start the screen goes blank and stays there.

## What is already known

Measured over 5000 frames (about 83 seconds of emulated time), with no button
ever pressed:

```
instructions   1621250547
display        vram (0,0) 320x240, enabled
non-black      61440 of 76800          (exactly 320x192 - a letterboxed film)
cdrom          8337 sectors, 1142 XA audio sectors decoded
mdec           914 commands, 218880 macroblocks
bios calls     2335997                 (no A0(40) - nothing has crashed)
unimplemented  0 paths hit
```

So the machine is healthy right up to the point the player has to do something.
Nothing is stuck, nothing has faulted, and the film is still running. Whatever
goes wrong happens **after** input, which is the one thing that has never been
tested.

## The blocker: the harness cannot press a button

`boot_runner` never calls `Sio::set_buttons`. Every result in this project so
far comes from a machine with a controller that is connected and permanently
idle. A bug that needs Start pressed cannot be reached at all, let alone
bisected.

That is the first piece of work, and it is worth doing regardless of this bug -
nothing past a title screen is testable without it.

### Step 1: input in the harness

```
--press <button>@<frame>[+<frames>]     press a button at a frame, hold n frames
--press start@1800+6 --press cross@2100+6
--input <file>                          a script of the same, one per line
```

Default hold of about six frames, because software debounces and a one-frame
press is often missed. `Sio::set_buttons(0, mask)` on the frame boundary, clear
it after the hold. The button names already exist as `Sio::kStart` and friends.

**Check it works before trusting it:** the BIOS shell responds to the pad - its
menu moves. A run that presses Down and then Cross in the shell must produce a
different framebuffer checksum from one that presses nothing. If it does not,
the input path is broken and every conclusion after this is worthless.

### Step 2: reproduce it headlessly

Find the frame the menu appears on - the checksum stops changing, or nearly -
then press Start a little after that and run on. Expect the same blank screen
the front end shows. Until this reproduces in the harness there is nothing to
bisect.

If it does *not* reproduce, the fault is in the front end's input rather than
the core, and the investigation moves to `ReadKeyboardPad` and the SIO
sequencing instead.

## Once it reproduces

Work through these in order. Each is cheap and each rules out a class.

### 1. Is it a crash, a stall, or a blank picture?

The three look identical on screen and are completely different problems. The
signature of each, all already reported:

- **Crashed:** `A0(40)` climbing into the millions. The BIOS is spinning in
  SystemError. `trace_system_error.txt` gets written with the Cop0 state.
- **Stalled:** interrupts taken, CD commands and MDEC commands all frozen
  between two frame counts while BIOS calls climb. This is what bug 24 looked
  like.
- **Running but not drawing:** GP0 word count still climbing, `display ...
  DISABLED`, or drawing somewhere the display window is not looking. The
  `display vram (x,y) WxH` line says where the window is; `--vram` dumps the
  whole of VRAM, which shows whether the picture exists somewhere else.

That third case is real and has happened before: Legend of Mana looked black
because the game had switched the display off to load and died before switching
it back on.

### 2. If it is a crash, the tools already exist

`trace_fatal_exception.txt` gives the Cop0 cause and the last ten thousand
instructions. Decode the cause: 4 is an unaligned load, 5 an unaligned store, 7
a data bus error. Then `--watch-ram <address>` says who wrote the value that
caused it, CPU or DMA, and `--dis <address>:<n>` disassembles around it.

That chain found both of the last two game bugs and should be reached for
first.

### 3. If it is a stall, find what it is waiting for

The likely candidates, in order of how often they have been the answer:

- **An interrupt that never arrives.** Compare `interrupts by source` against a
  healthy run. A source that stops is the lead.
- **A CD-ROM command with no response.** The command histogram plus the
  seek/sector log show what was asked for and what came back. `0 unknown` means
  nothing was outright unrecognised, but a command answering with the wrong
  interrupt kind looks fine in that count.
- **A register we drop.** `unimplemented paths` now names the file and line.
  Zero of them is not proof - a register handled by falling into a `default`
  that returns zero counts as handled - but non-zero is a direct answer.

### 4. Suspect what changes at exactly this moment

Pressing Start ends the attract loop and starts the game proper. That is the
first point at which several things happen for the first time:

- **The controller is actually read for state, not just presence.** Everything
  before this only needed the pad to answer its ID. Now the button halfwords
  matter, and so does the acknowledge timing between bytes.
- **A memory card is looked for.** A game checks for saves on entering its
  menu. `MC::LoadFile` refuses anything that is not exactly 128 KB, and the
  front end may have no card inserted at all. A game that mishandles "no card"
  is a real possibility, and it is trivial to test both ways.
- **An overlay is loaded.** The console output already shows Wild Arms loading
  overlays and re-running `ResetGraph`; the transition out of the menu is
  likely another one.

The memory card is the cheapest to eliminate: run it with a card and without,
and see whether the two diverge.

## What would make this faster next time

Input is the missing half of the harness. Once `--press` exists, the natural
follow-on is a **scripted playthrough regression**: a short input script per
game, a frame count, and an expected checksum. That turns "does Wild Arms still
get past its menu" into something a build can answer, rather than something
that has to be noticed by hand.
