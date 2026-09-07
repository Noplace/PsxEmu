# Ace Combat 3: input never reaches the game

Reported as "input not working" on
`\\superserverx\D\Games\Sony\PSX\ISO\Ace Combat 3\Ace Combat 3 - Electrosphere
[SLUS-00972].cue`. Worked through the same way
[Wild-Arms-Press-Start-Plan.md](Wild-Arms-Press-Start-Plan.md) was: validate
the harness before trusting it, then localise. The harness is fine; the
localisation is done; the fix is not.

## What is already known

**The disc boots correctly and renders correctly**, checked frame by frame
with `--ppm`: the Sony and Namco logos render pixel-correct (confirmed by
eye), a real "Now loading." screen follows, and the title screen -
`ACE COMBAT 3 / electrosphere / PRESS START BUTTON` - renders correctly too.
None of this was in doubt before; it is now confirmed rather than assumed.

**`--boot-disc` is the wrong way to test this game** - it reproduces bug 19
exactly (`Docs/Bugs-Found.md`): calling `System::BootDisc` directly, without
letting the BIOS run first, lands the CPU in a hard instruction-bus-error
loop at `pc=0x80B05090` (well outside RAM) within the first few hundred
frames, 475+ repeats, 0 RFEs ever executed. Plain `--disc <path>` (no
`--boot-disc`, no `--auto-boot`) lets the BIOS boot it the way a console
does, and that works correctly all the way to the title screen. Use that
form for anything further on this game.

**`--press` itself works.** Validated against the BIOS shell exactly as the
Wild Arms plan insists before trusting anything downstream - the first
attempt used too early a frame (250-600, still mid boot-animation) and
wrongly looked broken; frame 500 (matching the historical bug 25
validation) reproduces it: `down@500` changes the shell's checksum,
`right@500` does not (correct - the shell menu is vertical).

**On Ace Combat 3's own title screen, `--press start` has no effect at
all**, at any timing tried (default 6-frame hold, a full 60-frame hold,
pressed anywhere from frame 1755 - the exact frame the "PRESS START BUTTON"
text is first on screen - through the window before the screen changes on
its own). The screen that follows - a chaotic tangle of cyan wireframe,
looking like garbage 3D geometry - appears at the same frame (and with the
same framebuffer checksum, `89b72545dc9a7961`) whether Start was pressed or
not. Whatever that screen is (most likely an attract-mode flight demo,
unconfirmed), the game reaches it on its own timer, not because Start was
seen.

## Localised: the pad is never actually polled

Traced every `Sio::Exchange` call and every `SIO0_CTRL` write (temporary
instrumentation, not shipped, same spirit as bug 42's) across the whole run
up to and past the title screen. The result is unambiguous:

- **1402 device-select attempts, zero of them ever reach a second byte.**
  Every single one is: assert /CS, write `0x01` (controller address) or
  `0x81` (memory card address), then drop /CS again a few hundred cycles
  later - `SIO_CTRL` written with bit 1 (DTR/CS) clear - before the command
  byte (`0x42` to poll buttons) is ever sent. `grep`ing the whole trace for
  an exchange at `transfer_step_ > 0` returns nothing. This is true from the
  very first attempt (long before Start is ever pressed), so it is not
  something that starts failing only once the pad is asked for real data.
- **This is not the BIOS kernel's pad driver.** `I_MASK` at the end of the
  run is `0000002D` - VSYNC, CD-ROM, DMA, Timer 1. Bit 7 (SIO0) is not set.
  Per psx-spx, "the kernel's controller driver only ever uses the DSR
  (/ACK) interrupt" - with that interrupt masked off entirely, whatever is
  driving this port is a custom, non-kernel routine that must be polling
  `SIO0_STAT` directly rather than waiting on the IRQ. `SetInterrupt` for
  SIO0 is in fact wired correctly (`IOInterface::Tick` calls `sio.Tick`
  every batch, which raises it - checked directly), so this is not an
  "interrupt never fires" bug in the ordinary sense; the driver simply
  never uses it.
- **The abort happens ~700 cycles after the address byte**, comfortably
  after this core's own 500-cycle acknowledge delay (`kAcknowledgeCycles`
  in `sio.cpp`) has already elapsed and the corresponding status bits would
  already read as set on real hardware. That the driver aborts anyway,
  every single time, at a consistent interval, argues for a genuine status-
  bit mismatch rather than a timeout race.

## What is ruled out

- **The protocol shape.** `Sio::ExchangeController` matches psx-spx's
  documented byte sequence exactly (address → command → ID lo → ID hi →
  data → data for a `0x42` poll) - checked directly against the primary
  source, not from memory.
- **The mechanism generally.** The BIOS shell's own pad driver, going
  through the identical `Sio::Exchange`/`ExchangeController` code, works -
  moves its cursor on `down`, does not on `right`. Whatever is wrong is
  specific to how Ace Combat 3's own driver decides a device answered, not
  to the exchange logic itself.
- **A CPU-timing regression from this session's own earlier work.** The
  pattern is present from the very first poll attempt of the run, before
  Start is ever pressed and long before the title screen - not something
  that only appears after a specific amount of elapsed time that a cycle-
  cost change could have shifted.

## Leading hypothesis - not verified, not fixed

`SIO0_STAT` bit 7 (DSR, wired to /ACK) is modelled here as a flag that gets
set the instant an acknowledging exchange happens and stays set until the
port is next reset - never releasing back to 0 on its own. Real hardware's
/ACK is a physical line pulled low for "at least 2 µs" and then released;
bit 7 is documented as reflecting the *current level*, not a latched event.
A driver that polls for an actual pulse (asserted, then released, as
confirmation the device is really there and not just a stuck bus) would
never see that shape from this implementation.

This was not implemented and tested here, on purpose: modelling the
release without knowing the real pulse width, and without a way to confirm
it actually explains the abort rather than just moving the symptom, is
exactly the kind of guess this project's own standard says to write down
rather than ship (`Emulator-Project-Standards.md` section 6, "do not
overfit"). It is the most concrete lead to start from, not a conclusion.

## Next steps for whoever picks this up

1. **Model the /ACK pulse's release** as the first experiment - clear
   `kStatusAcknowledge` a short, deliberately-short-of-`kAcknowledgeCycles`
   number of cycles after it is set, so a driver polling for a genuine
   pulse can see one. Re-run the same trace; the signature of success is an
   exchange that finally reaches `transfer_step_ > 0` for the connected
   pad, slot 0.
2. **If that doesn't move it**, the next candidate is `SIO0_STAT` bit 9
   (Interrupt Request) - this core sets it unconditionally alongside
   raising the interrupt, regardless of whether the enabling bits in
   `SIO0_CTRL` (8-9, 10, 11, 12) are actually set at delivery time; a
   driver reading STAT.9 as its own polling flag (bypassing the interrupt
   controller entirely, consistent with I_MASK never enabling SIO0) could
   be tripped up by that same never-modelled edge.
3. **Reproducing exactly this trace** is one `boot_runner` command away
   once instrumented again: `bios/SCPH1001.BIN --disc <path> --frames 1850
   --press start@1755+60`, no `--boot-disc`, no `--auto-boot`. The title
   screen is confirmed to render at frame ~1750; nothing before that frame
   is worth tracing for this bug.
4. Once a fix changes the trace's shape, confirm it actually reaches the
   game (not just a longer exchange) with `--ppm` at a frame well past the
   press and a manual look at the picture, the same way this investigation
   found the title screen in the first place - a changed checksum alone is
   not proof of the right kind of change.
