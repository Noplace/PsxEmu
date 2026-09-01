# Bugs found in the revived code

Kept per section 7 of
[Emulator-Project-Standards.md](Emulator-Project-Standards.md): record what was
found and what it cost, so nobody rediscovers it.

Every one of these was found with `boot_runner`, and most produced no
symptom a person watching a window could have distinguished from any other kind
of hang.

---

## 1. BLEZ never branched, and destroyed its own operand

`psx/cpu.cpp`

```cpp
bool cond = ((context_->gp.reg[rs_] & 0x80000000)==1) || (context_->gp.reg[rs_] = 0 );
```

Two bugs in one line:

- `(x & 0x80000000) == 1` is never true. The mask yields `0` or `0x80000000`,
  never `1`.
- `(reg = 0)` is an **assignment**, not a comparison. Every `blez` zeroed the
  register it was testing, and evaluated to false.

So `blez` never took its branch and silently corrupted a register on the way
past. `bgtz` sat directly underneath it, written correctly, which is how the
pattern got missed.

**Symptom:** the boot never left the BIOS kernel.

## 2. Data loads went through the instruction cache

`psx/cpu.cpp`, `ICache2::GetBufferAndOffset`

`Cpu::Load` routed every RAM read through `ICache2`, an *instruction* cache
model. While the cache was disabled it happened to return RAM. The BIOS enables
the cache during boot, and from that moment on every data load came out of a
line buffer that is indexed by byte address (`address & 0xFFF`) but filled one
32-bit word at a time - so three bytes in four were stale and the fourth was
usually from the wrong line.

**Symptom:** the BIOS read its own character-class table as zero, decided a
space was not a printable character, and expanded a single tab into an infinite
run of spaces.

This was the expensive one to find, because everything *looked* right: RAM held
the correct bytes, the disassembly was correct, and the load instruction was
correct. Only the path between them was wrong.

**Fixed by removing the cache from that path entirely.** A cache is a
performance model; modelling it wrongly is worse than not modelling it. Doing
it properly is a Phase 5 item.

## 3. SWL and SWR merged into uninitialised memory

`psx/cpu.cpp`

```cpp
uint32_t data;                                  // never assigned
switch (virtual_address & 0x3) {
  case 0: data = (data & 0xFFFFFF00) | (reg >> 24); break;
  ...
```

An unaligned store has to preserve the bytes it does not cover, so it must read
the existing word first. Both instructions merged the register into an
indeterminate value and stored the result, corrupting three bytes out of four.
The *masks* were all correct, which is what made it read as finished code.

**Symptom:** none yet, on the current boot path - the BIOS had not reached an
unaligned copy before hanging for other reasons. It would have corrupted every
`memcpy` the moment it did.

## 4. Acknowledging an interrupt set every other interrupt

`psx/io_interface.cpp`

```cpp
case 0x1F801070: io.interrupt_stat = data & io.interrupt_mask; return;
```

Writing `I_STAT` acknowledges: a zero bit clears that flag, a one bit leaves it
alone. The register is `&=`, not `=`. Assigning the written value meant that
clearing the vertical-blank flag - which is done by writing all ones except
bit 0 - **set** the CD-ROM, timer, DMA and SIO flags at the same time.

**Symptom:** phantom interrupts on every acknowledge.

## 5. The DMA linked-list walk sent the same word repeatedly

`psx/dma.cpp`

```cpp
for (int i = 0; i < count; ++i)
  gpu->WriteData(baseAddrL[(dmaMem>>2) + count]);   // + count, not + i
```

Indexed by the loop *bound* instead of the loop variable, so every word of a
display-list packet was the same word - and that word was one past the end of
the packet.

**Symptom:** would have made every DMA-driven display list garbage. Not yet
reachable when it was found.

## 6. Hardware registers were decoded on the virtual address

`psx/cpu.cpp`, `Load` and `Store`

The region decode matched `0x1F801000-0x1F802FFF` only. Every PSX register also
appears at `0x9F80xxxx` (KSEG0) and `0xBF80xxxx` (KSEG1), and software uses all
three. A register access through either of the other two fell off the end of the
decode and returned zero - without even reaching the `BREAKPOINT` that would
have flagged it.

**Fixed by translating to a physical address first and decoding that**, which is
what the hardware does and removes the whole class of miss.

---

## Two things the harness changed about finding these

**`BREAKPOINT` used to compile to nothing in release.** Every unimplemented path
in the core is marked with it, and in a release build the marker vanished
entirely. It now increments a counter, so a run can report "hit 12 unimplemented
paths" instead of quietly producing a wrong frame. That number is in every
`boot_runner` run.

**The register access log is what turned guessing into reading.** Printing which
hardware registers were touched, and how often, immediately showed that the GPU
had never received a single word - and later that GPUSTAT was being read 33
million times in a 400-frame run, which is a poll loop and nothing else.

---

## 7. An interrupt made the instruction it interrupted run twice

`psx/system.cpp`, `System::StepInstruction`

The interrupt check ran *after* the instruction, and set `EPC` to `prev_pc` -
the instruction that had just finished. On return the handler therefore resumed
at an instruction that had already executed, and ran it a second time.

For most instructions that is invisible. The first vertical blank of the boot
landed on the `rfe` at the end of another handler, and running `rfe` twice pops
the Cop0 status stack twice: `IEc` was restored from a stale slot, came back
zero, and **no interrupt was ever delivered again for the rest of the run.**
One vertical blank in, and the machine was deaf.

Fixed by taking the interrupt *before* the next instruction, with `EPC` pointing
at that instruction. Nothing needs to check for a branch delay slot, because
`Jump()` runs the delay slot inside the same `ExecuteInstruction` call, so
control never arrives at the check partway through a branch.

**Symptom before:** exactly one interrupt taken per run, then nothing.
**Symptom after:** the handler runs correctly, identifies the interrupt, walks
the BIOS handler chain and delivers the vertical-blank event.

This fix made the *observable* output worse - the boot had been accidentally
getting further on the broken path, and now stops earlier with less drawn. That
is not a reason to revert it. See the note in `Docs/Roadmap.md`.

---

The next four were found by `cpu_test` on its very first run, before it had
been pointed at anything in particular. None of them had produced a symptom yet.

## 8. Dividing the most negative integer by -1 killed the process

`psx/cpu.cpp`, `Cpu::DIV`

```cpp
context_->low = (int32_t)reg[rs] / (int32_t)reg[rt];
```

`0x80000000 / -1` has no 32-bit answer, and x86 raises a hardware divide-error
for it. Handed straight to the host CPU, that is not a wrong number - it is
`STATUS_INTEGER_OVERFLOW` and the emulator is gone.

MIPS does not trap. The result is defined: the quotient stays `0x80000000` and
the remainder is zero. A game doing this by accident would have taken the
emulator down with it.

**Found by:** the test suite crashing before it printed a single line.

## 9. Division by zero returned zero

`psx/cpu.cpp`, `Cpu::DIV` and `Cpu::DIVU`

Both guarded the divisor and set `HI = LO = 0`, which is not what the hardware
does and not what software expects:

| | HI | LO |
|---|---|---|
| `div` by zero, dividend >= 0 | dividend | `0xFFFFFFFF` |
| `div` by zero, dividend < 0 | dividend | `1` |
| `divu` by zero | dividend | `0xFFFFFFFF` |

Compilers emit a divide followed by a check of the result, so returning zero
turns a caught division by zero into a wrong answer that carries on.

## 10. Bus errors were decided on the virtual address

`psx/cpu.h`, `Cpu::AddressTranslation`

The validity table was a list of *virtual* ranges, and it listed RAM and the
BIOS through all three windows but the hardware registers through only one. So
`IsBusError()` returned true for an ordinary register access through KSEG1, and
for every KUSEG RAM mirror above 2 MB.

This is the same class as bug 6, one level up - and it had been quietly
defeating that fix. Bug 6 made the *decode* work on the physical address, but
the bus-error check upstream still ran on the virtual one and rejected the
access before the decode ever saw it.

**Fixed by deciding validity after translation**, so every window onto a region
is valid exactly when the region is.

## 11. `break` did nothing

`psx/cpu.cpp`, `Cpu::BREAK`

```cpp
void Cpu::BREAK() {
  BREAKPOINT      // a host-side debug marker, and nothing else
}
```

The instruction fell through as if it were a `nop`. `break` is how a debugger
and the BIOS's own assertions stop the machine; software that hits one expects
an exception with cause code 9, and got execution carrying on into whatever
followed.

---

## What the two suites would have caught

Of the eleven bugs here, `cpu_test` covers seven directly - 1, 3, 4, 6, 7, 8,
9, 10 and 11 are each one assertion. Bug 1 (BLEZ assigning to its own operand)
took an afternoon of BIOS disassembly to find; the test for it is three lines
and runs in a millisecond.

That is the argument for writing these first, and it is section 6 of the
standards document's argument too. The two that would *not* have been caught -
bug 2 (the instruction cache corrupting data reads) and bug 5 (the DMA
linked-list index) - are both cases where the unit under test is correct and
the wiring around it is not, which is what the harnesses are for.

---

These two are what had been stopping the boot. Both were found from the Cop0
status history that `boot_runner` now prints.

## 12. A finished DMA transfer left its interrupt asserted for ever

`psx/dma.cpp`, `Dma::Tick` and the write to `DICR` (`0x1F8010F4`)

```cpp
void Dma::Tick() {
  if (interrupt_control.raw & 0x7f000000)
    system_->io().SetInterrupt(kInterruptDMA);   // every tick, for ever
}
```

`DICR` bits 24-30 are per-channel interrupt flags, and they are
**write-one-to-clear**. The write handler assigned the whole register instead,
so an acknowledge never cleared anything. Bit 31, the master flag, is read-only
and derived from the flags and the enables; it was not computed at all. And the
interrupt is an *edge*, not a level - `Tick` re-raised it on every single cycle
while any flag stood.

So the first DMA transfer of the boot latched a flag, and from that moment
`I_STAT` bit 3 was permanently set.

The consequence was several steps removed from the cause, which is why it took
the status history to see. The BIOS has no handler registered for a DMA
interrupt, so its handler chain walked every slot, found nothing that would
claim it, and took its **unhandled-exception path** at `0x00000E44` - which
unwinds with a longjmp to a saved recovery context rather than returning
through `rfe`. The Cop0 status register was therefore never popped, stayed at
`0x404` ("inside an exception") for the rest of the run, and no interrupt was
ever delivered again.

**Symptom:** 2 interrupts and 7 RFEs in a 400-frame run. After the fix, 1009
interrupts and 1023 RFEs.

## 13. GPUSTAT never reported the GPU ready to hand over VRAM

`psx/gpu.cpp`, `Gpu::ReadStatus`

```cpp
s.ready_vram_send = (transfer_mode_ == kTransferFromVram) ? 1 : 0;
```

Bit 27 says the GPU is *ready* to send VRAM to the CPU, not that a transfer is
already running. Reporting it only mid-transfer looks more honest and is
exactly wrong: software that checks readiness **before** issuing the read
command waits for a bit that this GPU would only set afterwards.

The other two ready bits (26 and 28) were already hardcoded to 1, because this
core does not model the FIFO timing. Bit 27 needed to be treated the same way,
and the inconsistency was the bug.

**Symptom:** the BIOS shell spinning in a four-instruction loop at
`0x800509AC`, reading GPUSTAT 33 million times in a 400-frame run and masking
it with `0x08000000`.

**After both fixes the BIOS boots and draws**: the intro's blue radial gradient
renders correctly across the full 640x478 frame, from 1923 primitives and 73.8
million plotted pixels, and the BIOS starts issuing CD-ROM commands. What is
still wrong on screen is the logo geometry, which is the unimplemented GTE.

---

These came from looking at the GPU after the boot started rendering but the
intro text was missing and the fade did nothing.

## 14. DMA channel 2 block mode was a stub, so textures never reached VRAM

`psx/dma.cpp`, `Dma::Dma2`

```cpp
if ((channels[2].chcr & 0x01000201) == 0x01000201) {
  BREAKPOINT      // block mode, in
}
if ((channels[2].chcr & 0x01000200) == 0x01000200) {
  BREAKPOINT      // block mode, out
}
```

Only the linked-list sync mode was implemented. Block mode is how image data -
textures and colour lookup tables - is moved into VRAM, and burst mode is used
for smaller runs. Both did nothing at all.

The failure was completely quiet, and worse than "no texture": the primitives
still drew. They sampled a texture page that had never been written, every
texel came back as the fully-transparent value zero, and the pixel was skipped.
So a run reported 1923 primitives and 73 million plotted pixels while the
screen showed no text whatsoever.

`boot_runner` now prints a per-command GP0 histogram and the reason every
rejected pixel was rejected. Those two numbers together made it obvious:
textured quads *were* being issued (412 of command `2C`), and 5.19 million
texels were being rejected as transparent with nothing clipped and nothing
mask-rejected. A texture that is not there and a texture that is drawn wrongly
look identical on screen; they do not look identical in that pair of counters.

**Symptom:** the BIOS intro drew its background and its geometry but none of
its text, and the fade did nothing. **After the fix the whole intro renders**:
"SONY" above the diamond, "COMPUTER ENTERTAINMENT" below it, and the fade
animating through.

## 15. A polygon's texture-disable bit was ignored, and written to the wrong place

`psx/gpu.cpp`, `Gpu::CmdPolygon`

```cpp
status_.raw = (status_.raw & ~0x09FF) | (page & 0x09FF);
```

Two things wrong in one line. The texpage attribute's bit 11 is *texture
disable*; GPUSTAT's bit 11 is *set mask bit when drawing*. Copying one into the
other corrupted the mask setting that software reads back - texture disable
belongs at GPUSTAT bit 15.

And `state.textured` never consulted it at all, so a primitive that asked to be
drawn untextured was textured anyway, from whatever happened to be at the
texture page.

Also fixed alongside: `GP0(E1)` bits 12 and 13, the textured-rectangle X and Y
flip, were decoded into nothing and are now honoured.

These three are correct-by-inspection fixes rather than ones with an observed
symptom - the boot checksum did not move. They are the kind of thing that shows
up later as one game with mirrored sprites, which is exactly why they are worth
fixing while the code is open rather than hunting later.

---

## 16. Loads cost nothing, so the BIOS gave up waiting for vertical blank

**Symptom.** Booting a game disc showed the PlayStation logo for a moment -
noticeably briefer than a real console - and then a blank screen with nothing
from the game.

**How it was found.** The BIOS says what is wrong, out of its serial console,
and nothing was listening. Recording `A0(3C)/B0(3D)` (putchar) and
`A0(3E)/B0(3F)` (puts) into a buffer and printing it at the end of a run gave:

```
PS-X Realtime Kernel Ver.2.5
KERNEL SETUP!
System ROM Version 2.2 12/04/95 A
ResetCallback: _96_remove ..
VSync: timeout (2:1)
VSync: timeout (3:2)
...
```

188 timeouts in 400 frames, and the printf that produced them accounted for a
third of all execution. The same 188 appeared with no disc in the drive at all,
so this was never a disc problem.

**Cause.** The GPU raised 400 vertical blanks in 400 frames and the CPU took
362 of them, so the interrupt itself was fine. What was wrong was the ratio of
work to time: `Cpu::Load` charged the two cycles the opcode already paid and
nothing for the bus, so every load from main RAM, from a hardware register and
from the BIOS ROM cost the same as an add. The CPU got through about 1.6x more
instructions per frame than the hardware would, VSync's own timeout expired
before the frame it was waiting for arrived, and it returned early - which is
exactly why the intro ran fast and then stopped.

Two experiments pinned it down. Quartering the hblank rate that VSync measures
its timeout against changed nothing, ruling out the root counters. Running the
GPU at twice speed - halving the instructions the CPU gets through per frame -
took the timeouts to zero.

**Fix.** Charge the access in `Cpu::Load`, by region, for data accesses only
(instruction fetches come through the instruction cache and are a separate
cost): 3 cycles for main RAM, 0 for the scratchpad, 3 for a hardware register,
5 for the BIOS ROM.

Timeouts went 188 to 0. The BIOS shell frame checksum did not move
(`bd888bab645a63a9`), all 447 harness checks still pass, and both disc boot
paths still render. Instructions executed per 400 frames fell from 181M to
112M, and instructions run with an interrupt pending but disabled fell from
2.3M to 735K.

**Still open.** The BIOS's own disc boot path stops after reading the licence
area at sectors 4-11 and never reads the primary volume descriptor at sector
16, so it does not hand off to a game by itself. The front end no longer
depends on it doing so - see below.

---

## 17. "Open disc" in the front end never booted the disc

`kCommandOpenDisc` in `PSXEmu.Win32/main.cpp` mounted the image and then called
`set_auto_boot(false)`, with the line that arms auto-boot commented out just
above it. That left the BIOS to find and start the executable on its own, which
it does not yet do (bug 16), so the only thing the user ever saw was the intro
followed by the shell.

The core already had the mechanism: `System::StepInstruction` watches for
`pc == 0x80030000`, the address the BIOS jumps to to run a game, and takes over
there with `BootDisc`. Arming it is what the commented-out line did.

Verified through the harness with a new `--auto-boot` flag, which drives
exactly the path the front end now takes: the test disc's green screen appears,
with the same checksum as the direct `--boot-disc` path.

**A note for real discs.** The BIOS checks sectors 4 to 11 for a 64-byte
licence string and refuses a disc without it, with no error - the symptom is
the logo followed by nothing, which is easy to mistake for bug 16. `.bin/.cue`
images normally carry it. Some plain `.iso` rips do not.

---

## 18. The BIOS disc boot path was never broken

Bug 16 said the BIOS "stops after reading the licence area at sectors 4-11 and
never reads the primary volume descriptor at sector 16, so it does not hand off
to a game by itself". That was wrong, and it was wrong because the only disc it
had ever been tried against was the synthetic one in
`tools/make_test_disc.cpp`, which the BIOS refuses for reasons of its own.

Against a real disc - `Legend of Mana [SLUS-01013].cue`, a MODE2/2352 image -
the BIOS boots it end to end, and says so on its console:

```
BOOTSTRAP LOADER Type C Ver 2.1   03-JUL-1994
setup file    : cdrom:SYSTEM.CNF;1
TCB.00000004  EVENT.00000016  STACK.801ffff0
BOOT =.cdrom:\SLUS_010.13;1
boot file     : cdrom:\SLUS_010.13;1
EXEC:PC0(8002e7a8)  T_ADDR(80010000)  T_SIZE(0002f000)
boot address  : 8002e7a8 801ffff0
Execute !
Change effective memory : 2 MBytes
```

Licence area, volume descriptor, SYSTEM.CNF, executable load, hand-off: all of
it works. The sector reads walk 4-11, then 16, 18, 22, 23, 47 and on into the
game's own files, which is exactly the sequence a console makes.

**What this cost.** A test asset that is wrong in a way the real thing is not
is worse than no test asset, because every result it gives is believed. The
synthetic disc was built to answer "does the chain work", it answered no, and
the no was about the disc.

---

## 19. Auto-boot is worse than letting the BIOS do it

The fix in bug 17 armed `set_auto_boot` so the front end took the disc over at
`pc == 0x80030000` and loaded the executable itself. Measured against a real
game, that is much worse than doing nothing:

|                     | BIOS boots it | auto-boot takes over |
|---------------------|---------------|----------------------|
| GTE commands        | 60,698        | 0                    |
| primitives drawn    | 10,054        | 0                    |
| pixels plotted      | 100,071,437   | 0                    |
| GP0 words           | 61,223        | 1                    |
| SPU key-ons         | 112           | 24                   |
| sectors read        | 169           | 62                   |

The BIOS's boot does far more than find an entry point and jump to it - it sets
up the kernel a second time with the TCB and event counts SYSTEM.CNF asks for,
installs the callbacks, and hands over with the stack the header names.
Skipping it leaves a game that runs but can do nothing.

`kCommandOpenDisc` now calls `set_auto_boot(false)` and lets the BIOS boot the
disc, which is what the console does. `--auto-boot` stays in the harness so the
two can be compared.

---

## 20. Two DMA channels ignored their sync mode, and the ordering table walked a raw pointer

Found while tracing a corrupt structure; neither turned out to be the cause of
that, but both are real.

**Channels 3 and 4 multiplied by the block count regardless of sync mode.** BCR
means different things in each mode: in burst mode the length is the low half
and the upper half is unused, so games leave whatever was there before. Only in
block mode is the total size times count. Both channels multiplied
unconditionally, so a stale upper half would turn one CD sector into a transfer
hundreds of times too long, writing over whatever followed the buffer. Now
computed by `TransferWords`, which takes the sync mode.

**Channels 2, 3 and 4 started transfers that hardware would not have.** A
channel only runs when it is enabled in DPCR - software sets a channel up while
it is switched off and expects nothing to happen until it is switched on - and
in burst mode the transfer begins on the trigger in CHCR bit 28, not on the
enable in bit 24 alone. Channels 5 and 6 checked the DPCR bit; 2, 3 and 4
checked neither. Now all of them go through `ShouldStart`.

**The ordering table channel walked a raw pointer with no bound.** `Dma6` took
`uint32_t* mem` into the RAM buffer and ran `*mem--` in a loop counted by the
whole of BCR - so a block count left in the top half, or a count of zero, which
means the maximum and underflowed to four billion, ran off the front of the
allocation and corrupted whatever the host had there. Every address is masked
into RAM now, only the low half of BCR is the length, and zero means 0x10000.

The BIOS shell checksum did not move (`bd888bab645a63a9`) and all 447 harness
checks still pass.

---

## Open: Legend of Mana boots, runs and then faults

The game loads, initialises, uploads textures, and renders - 60,698 GTE
commands and 100 million pixels into VRAM. Then at about frame 590 it takes a
data bus error and the BIOS spins in `SystemError` for the rest of the run
(`A0(40)` called 10.8 million times). The screen is black because the game
switched the display off to load and never switched it back on, which is
correct behaviour for a game that died mid-load.

The fault is at `0x80012F38`:

```
80012F20  lui   v0, 0x801E
80012F24  ori   v0, v0, 0xD800     ; v0 = 801ED800, a received stream packet
80012F28  lw    v1, 56(v0)         ; a count out of the packet
80012F30  sll   v1, v1, 4
80012F34  addu  v1, v1, v0
80012F38  lbu   a0, 64(v1)         ; v1 = 5F3EDA00 - nowhere
```

Ruled out, each by measurement rather than inspection:

- **Not RAM corruption.** A write watch on `0x801ED838` shows the CPU only ever
  wrote 0 and 1 there. The garbage came from CD-ROM DMA - legitimately: the
  game DMAs a stream packet to `0x801ED800` and parses it in place, so those
  offsets are fields of the packet, not of a structure being trampled.
- **Not the wrong sector.** The first word delivered for that transfer was
  `02350015`, which is the Mode 2 header 15:00:35 in BCD - LBA 67535, the
  sector that was asked for. The data is right and at the right offset.
- **Not the DMA length.** 512 words into a 2KB buffer, sync mode 0, exactly as
  programmed.
- **Not the sector rate.** 451584 cycles per sector single speed, half that at
  double: 75 and 150 sectors a second, both correct.
- **Not interrupts backing up.** Before the fault the machine is healthy - 700K
  instructions with an interrupt pending against 165M executed. The 121M in the
  full run is all `SystemError` spinning afterwards.

What is left is that the game's streaming state machine is out of step with the
sectors it is being handed - it dispatches on a packet type byte into a 27-entry
table and gets a handler that does not match the packet. The next thing to look
at is the order and count of INT1 deliveries against what the game consumed,
and whether a sector is being dropped or repeated at a buffer boundary.

New harness options that made this findable: `--watch-ram <hex>` (who wrote
this address, CPU or DMA), the per-channel DMA transfer log, the BIOS console
capture, and the display-window line.

---

## 21. Arming the data FIFO rewound it, so every streamed sector arrived twelve bytes out of step

**Symptom.** Legend of Mana booted, ran, started its opening movie, and then took
a data bus error at `0x80012F38` and spun in `SystemError` forever - 10.8
million calls to `A0(40)` and a black screen.

**How it was found.** By logging every CD-ROM DMA transfer with the sector it
came from and the first word it delivered. The game reads each sector in two
pieces:

```
madr 801ED940    3 words   lba 67535  first 02350015
madr 801ED800  512 words   lba 67535  first 02350015
```

Three words is the twelve-byte header and subheader, which the game reads into
a scratch buffer to see what kind of sector it has. Five hundred and twelve
words is the payload, into a ring buffer. Both came back starting `02350015` -
the Mode 2 header, 15:00:35 in BCD. The payload read was handed the header a
second time instead of continuing where the first read stopped.

**Cause.** `1F801803h.Index0`, the request register. Bit 7 loads the data FIFO
with the current sector, and the hardware only loads it if it is not loaded
already - it will not reload until bit 7 has been taken back to 0. This:

```cpp
if (data & 0x80) {
  data_read_ = 0;
} else {
  data_read_ = data_size_;
}
```

rewound on every arm. Software that reads a sector in one go never notices,
which is why the BIOS booted and why every test passed. Software that reads a
sector in two pieces - which is what anything streaming does, because it has to
look at the subheader before it knows what the payload is - got the first twelve
bytes again in place of bytes twelve onward. Every sector of the stream was
twelve bytes out of step, the game's parse of a packet eventually produced an
index of `0xFDF20020`, and `lbu a0, 64(v1)` went to `0x5F3EDA00`.

**Fix.** Track whether the FIFO holds a sector. Arming it when it does is a
no-op; clearing bit 7 unloads it; a fresh sector unloads it so the next arm
reloads.

**Result.** The bus error is gone. `SystemError` is gone - BIOS calls over 900
frames fell from 22,960,727 to 1,253,128. The game turns its display back on
(320x240 at VRAM 0,240, where before it stayed off because the game died
mid-load) and streams its movie steadily at 150 sectors a second, which is
double speed, exactly right: 976 sectors by frame 1200, 2494 by 1800, 4012 by
2400, with no faults.

The BIOS shell checksum did not move (`bd888bab645a63a9`) and all 447 harness
checks still pass.

**Worth noting for the future.** The same rewind was corrupting the BIOS's own
executable loads in whole-sector mode and nothing caught it, because the first
sector of a read is right and only the second piece of a split read is wrong.
The DMA transfer log now records the first word delivered, which is what made
it visible: an executable load whose first word is `0C007931` (`jal`) is right,
and one whose first word is a Mode 2 header is not.

---

## Open: the MDEC does not exist

With the FIFO fixed, the game gets all the way to playing its opening movie and
the screen is still black, because there is nothing to decode it with. The game
writes the MDEC command port at `0x1F801820` 235 times and polls its status at
`0x1F801824` 235 times, and neither address is mentioned anywhere in the core -
`grep -rn "mdec\|1F801820" PSXEmu.Core/psx` finds nothing. DMA channels 0 and 1,
which are MDEC in and MDEC out, have never run a transfer.

So the next thing standing between this and a picture is the motion decoder:
the run-length and variable-length decode, the inverse DCT, the YUV to RGB
conversion, and the four DMA paths that feed and drain it. That is a component,
not a fix.

---

## 22. The front end's Boot disc did not boot a disc

`File > Boot disc` called `System::BootDisc` on whatever was already mounted -
the HLE shortcut, which bug 19 measured as much worse than letting the BIOS do
it - and offered no way to choose an image. `Open disc` was the one that
actually worked, which is not where anyone would look.

The File menu now reads:

- **Boot disc...** - choose an image, then the machine starts from cold and the
  BIOS boots it, exactly as switching a console on with a game in the drive
  does. This is the one to use.
- **Swap disc...** - change the image in a running machine without resetting,
  for a game that asks for its second disc.
- **Eject disc** - open the shell.
- **Boot BIOS** - start from cold with an empty drive, into the shell.

Both disc commands report a failure to read an image instead of leaving a black
screen, and `Boot disc` puts the disc in the drive before the BIOS looks - the
other order finds an open shell and stops at the menu.

The menu also claimed Ctrl+O worked. There is no accelerator table in the front
end, so it never did; the text is gone rather than the lie left standing.
