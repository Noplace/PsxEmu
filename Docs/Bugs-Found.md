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

---

## 23. The MDEC, and what the plan for it got wrong

Implemented per [MDEC-Plan.md](MDEC-Plan.md), in `psx/mdec.h` and
`psx/mdec.cpp`, with DMA channels 0 and 1 and a `mdec_test` harness.

**The plan's step 3 described work that does not exist.** It called for "the
bitstream decoder - a run-length and variable-length coded stream, MPEG-1
style", and said it was "the fiddly part and the part most worth testing in
isolation". That was wrong, and it was the largest and riskiest part of the
estimate.

The MDEC does no variable-length decoding at all. Software unpacks the Huffman
stream itself - that is what libpress's `DecDCTvlc` is for - and hands the
hardware already separated run/level pairs, two 16-bit codes to a 32-bit word:

- the first code of a block is `(quant_scale << 10) | dc`, a 6-bit factor and a
  10-bit signed DC;
- every code after it is `(run << 10) | level`, a 6-bit run of zeroes and a
  10-bit signed value;
- `FE00h` ends a block, and is also what software pads with.

Half a day was nearly spent transcribing an MPEG-1 VLC table from memory into
a decoder that would have been fed data it was never going to match. What
caught it was checking the format against the documentation before writing the
code rather than after it failed - and it only came up because a mis-typed
entry in a table like that produces silent garbage, so it seemed worth being
sure of.

**Everything else the plan said stands**, including the traps: the output
accumulates across a command, decode is not free, and MDEC output goes to RAM.

### What it does

Dequantise, inverse transform, colour convert, pack. One command carries as
many macroblocks as software cares to send. Colour blocks arrive as Cr, Cb and
four luminance blocks and come out as 16x16 pixels at 15 or 24 bits;
monochrome depths take one luminance block at a time.

### The one thing that had to be got right twice

`EmitMacroblock` originally wrote each macroblock to the start of the output
buffer. That is wrong in a way that is easy to miss: a game sends one command
per video frame - measured at 2400 words of compressed data - and drains the
result afterwards, thirty-two words at a time. Overwriting kept only the last
macroblock of three hundred, and the screen showed 256 non-black pixels, which
is exactly one 16x16 block. The output now accumulates for the whole command.

### How it was checked

`mdec_test`, 59 checks: the parameter countdown, the DMA request bits appearing
only with their enables, table unpacking, a DC-only block coming out flat, an
empty block coming out mid-grey, macroblock sizes at each depth, the colour
matrix in both directions, the run and zigzag walk, padding not being mistaken
for blocks, and the registers being reachable through the memory map.

The tests were then checked themselves, by breaking the code on purpose:

- swapping Cr and Cb: 4 failures, all in the colour checks.
- removing the zigzag: **0 failures**. The run/level check only used
  coefficient 1, and `kZigzag[1] == 1`, so it was a fixed point of the very
  thing it meant to test. A check on coefficient 2 - which belongs at position
  8, the first vertical basis function, and so must vary down columns and not
  along rows - was added, and now catches it.

That second one is the reason to do this at all. A suite that passes on broken
code is worse than no suite.

### End to end

Legend of Mana, 1800 frames: 235 MDEC commands, 69,900 macroblocks, no unknown
commands, no short blocks, no overflows. 69,900 over 235 commands is exactly
300 each, which is 20x15 macroblocks - a 320x240 frame. One command per frame
of video.

The screen went from entirely black to entirely painted. Pixels sample as
natural gradients with the horizontal pairing that 2x2 chroma subsampling
implies. Frames half a second apart differ by 53 to 66 of 255 where unrelated
noise scores 102, and the difference grows with the gap between them: the
output is temporally coherent, which decoded rubbish is not.

The BIOS shell checksum did not move (`bd888bab645a63a9`) and all 506 harness
checks pass.

**Not verified:** nothing here is compared against real hardware output. The
transform is a straightforward matrix multiply rather than the hardware's exact
sequence, so individual pixels will differ slightly from a console's. That
shows up as a picture that is very slightly soft, not as a wrong one.

---

## 24. Byte and halfword writes to the DMA registers were dropped

**Symptom.** Wild Arms loaded, played its opening, and then went black and
stayed there. Not a slow decode - dead: between frames 1800 and 3000 the
interrupts taken, the CD-ROM command count and the MDEC command count were all
frozen while BIOS calls climbed from 33 million to 124 million. `A0(40)`,
SystemError, was called 22.9 million times.

**How it was found.** The `TrapCounter` said "32 paths hit" and nothing else -
it counted unimplemented paths without recording which. Making it record the
file and line of each site turned that into:

```
unimplemented paths
  PSXEmu.Core\psx\io_interface.cpp line 176, 16 hits
  PSXEmu.Core\psx\io_interface.cpp line 391, 16 hits
```

which are the fall-through cases of `Read08` and `Write08`. The hardware
register access log had exactly one register with sixteen of each:

```
  1F8010F6        16 reads        16 writes
```

`1F8010F6h` is the upper half of `DICR`, the DMA interrupt control register:
the per-channel interrupt enables, the master enable, and the write-one-to-clear
flags.

**Cause.** `Read08`, `Read16`, `Write08` and `Write16` handled the CD-ROM, the
SIO and the SPU by range and fell through everything else into `BREAKPOINT`,
which counts and returns. The DMA block is 32-bit registers and software
reaches into it a halfword at a time - the enables live in the upper half and
get written on their own. Those writes were silently discarded, so DMA
interrupts were never enabled and the completion the game waited for never
came. It carried on with a buffer that was not ready, its software Huffman
decoder ran off the end of its output, and what it wrote over was a table index
it later used - `lw a0, -0x2960(at)` with an index of `0x14001` produced
`a0 = 0x00008021` and an address error.

Legend of Mana never touched those registers as halfwords, which is why it was
unaffected and why this looked like an MDEC problem rather than a DMA one.

**Fix.** `ReadSubWord` and `WriteSubWord` synthesise byte and halfword access
from the 32-bit accessors for the whole DMA block. The one subtlety is DICR's
write-one-to-clear flags: a read-modify-write of the half software did not
touch would acknowledge whatever was pending in it, so those bits are dropped
from what gets carried back rather than written out set.

**Result.** Wild Arms boots, runs and plays its opening: 156 MDEC commands,
36,960 macroblocks, no SystemError, BIOS calls down from 22.9 million to 2.3
million, CD sectors up from 365 to 1,889, and 56,292 of 76,800 pixels painted.
Zero unimplemented paths hit, where there were 32.

Legend of Mana is unchanged, the BIOS shell checksum did not move
(`bd888bab645a63a9`), and all 506 harness checks pass.

**The lesson worth keeping.** A counter that says "something is missing" and
not what is nearly useless. `TrapCounter` had been reporting a non-zero number
for the whole project and it was never actionable. Recording the site cost
about twenty lines and turned a black screen into a named register in one run.

---

## Open: FMV audio (XA-ADPCM) does not exist

The MDEC gives full-motion video its picture. Its sound is a separate thing
entirely and none of it is implemented.

What exists is CD-DA - a redbook audio track, 2352 bytes of 16-bit stereo PCM
straight into `Spu::QueueCdAudio`. That is what a game playing a music track
uses, and it works.

What FMV audio actually uses is XA-ADPCM: compressed audio interleaved with the
video in the same track, sector by sector. None of the pieces are there:

- **Setmode bits 6 and 3** - XA-ADPCM enable and the filter - are stored and
  never read.
- **Setfilter (0x0D)** is acknowledged and its file and channel discarded.
- **The subheader is not looked at.** A sector whose submode bit 2 says it is
  audio should go to the ADPCM decoder and *not* raise INT1, so software never
  sees it in the data stream. Every sector currently goes to the data FIFO.
- **There is no ADPCM decoder.** An XA sector is 18 sound groups of 128 bytes,
  each holding eight blocks of 28 samples, at 37800 or 18900 Hz, mono or
  stereo, with the filter and shift in the group header.

That third point is not only about sound: a game that interleaves audio and
video and relies on the drive to keep them apart will be handed audio sectors
in its video stream. Neither disc tried so far does that - Wild Arms only ever
sets mode 80h or A0h, never the XA bit - but a game that does will fail in a
way that looks like a broken video decoder.

---

## 25. XA-ADPCM: full-motion video has sound

Implemented in `psx/cdrom.cpp` and `psx/spu.cpp`, with five test groups in
`spu_test` and a `--wav` option in the harness.

### What was needed

Four pieces, all of them named in the note that closed bug 24:

- **Setmode bits 6 and 3** - XA-ADPCM enable and the Setfilter filter - are now
  read rather than only stored.
- **Setfilter** keeps the file and channel it is given instead of discarding
  them. A disc carries several interleaved streams in the one track and
  software picks one.
- **The subheader decides where a sector goes.** A sector whose submode says it
  is audio, on a drive with XA enabled, goes to the decoder and raises *no*
  data-ready interrupt. Software never sees it, so what it reads is an unbroken
  run of video.
- **The decoder.** Eighteen sound groups of 128 bytes, sixteen parameter bytes
  and 112 of packed samples. Four-bit mode packs eight blocks of 28 samples one
  nibble per block of each word; eight-bit mode, four blocks a byte each. The
  parameter byte carries a shift, where zero is loudest, and one of four
  filters. In stereo the blocks alternate channels and each keeps its own
  two-sample history.

Output is resampled from 37800 or 18900 Hz onto the mixer's 44100, with the
fractional position and the last frame carried between sectors so the joins are
not audible - one sector is about a tenth of a second, and a discontinuity
every tenth of a second is a rattle.

### The third point is not only about sound

Handing software every sector puts compressed audio in the middle of its video
stream. Wild Arms went from 36,960 macroblocks decoded to 61,200 and from 1,889
sectors to 2,581 purely from the drive keeping the two apart - the video got
better because the audio stopped being in the way. A game that interleaves and
relies on the drive to separate them would have looked like a broken video
decoder, which is exactly the trap flagged when this was still an open item.

### How it was checked

Five groups in `spu_test`, 27 new checks: frame counts for all four
combinations of width and channels, silence staying silent, the shift scaling
by halves with zero as loudest, a nibble's top bit being a sign, mono filling
both channels and stereo taking them from alternating blocks, filter 1 decaying
where filter 0 stops dead, the history surviving a sector boundary, and a loud
stream saturating rather than wrapping.

Then the tests were checked by breaking the code deliberately:

- zeroing filter 1's coefficient: 3 failures, all in the filter group.
- removing the nibble's sign extension: 2 failures, both in the shift group.

Two of the checks were wrong on the first run and the decoder was right:
`out[1]` is the *right channel of frame 0*, not frame 1, and in mono those are
equal by definition. Fixed to step two at a time.

### End to end

`boot_runner --wav` now drains the mixer once a frame, as the front end does,
and writes a 16-bit stereo file - audio being the one output that cannot be
checked by looking at it.

Wild Arms, 2400 frames: **320 XA audio sectors decoded**, none filtered out.
The recording, in four-second windows of RMS, against the same run with the
decoder's output not passed to the mixer:

```
  t         with XA     no XA
  t=  0s       1093      1093     the BIOS chime, identical
  t=  8s        427       427
  t= 16s          0         0
  t= 20s         33         0     <- the movie starts
  t= 24s        104         0
  t= 36s        134         0
  t= 40s         96         0
```

Silent in exactly the window the film plays, and unchanged everywhere else.

Legend of Mana decodes no XA sectors at all and is unchanged: its opening
genuinely carries no XA audio, which is why it never set the mode bit.

The BIOS shell checksum did not move (`bd888bab645a63a9`) and all 533 harness
checks pass.

**Not verified.** The absolute level is not checked against hardware. Playback
during the film sits around an RMS of 100 against a full scale of 32767, which
is plausible for the quiet opening of a movie through whatever CD volume the
game set, but it is not proof. Resampling is linear rather than the hardware's
seven-point filter: a slight softening at the top end, not a wrong pitch.

---

## 25. Wild Arms after "press start": found, localised, not yet fixed

Worked through [Wild-Arms-Press-Start-Plan.md](Wild-Arms-Press-Start-Plan.md).
Steps 1 and 2 are done and the triage has run; the root cause is localised to
one BIOS call with a bad argument, and finding how that argument got bad is
where it stands.

### The harness can press buttons now

`--press <button>[+<button>]@<frame>[+<hold>]`, several allowed, default hold
of six frames because software debounces and a one-frame press is often missed.
Buttons change on the frame boundary, which is where the front end samples them.

Validated against the BIOS shell before trusting anything downstream, exactly as
the plan insisted:

```
no input        checksum 7c73cb5c96330316
press down@500  checksum 21d074a462629425     the cursor moved
press right@500 checksum 7c73cb5c96330316     unchanged - the menu is vertical
```

Down moves the shell cursor and Right does nothing, which is right, and proves
the whole path from option parsing through `Sio::set_buttons` to the BIOS pad
driver.

### And log a timeline

`--frame-log <n>` prints the checksum, non-black count, resolution and the
MDEC, CD and GP0 counters every n frames. One run replaces a bisection.

For Wild Arms it immediately said where the menu is:

```
frame 9000   35a3e156...   61432 non-black  320x240  mdec 461520  cd 17191
frame 9500   6d7c94ce...  200710 non-black  512x480  mdec 484560  cd 18104
frame 10000  2ab4d9a0...  200705 non-black  512x480  mdec 484560  cd 18118
frame 10500  2ab4d9a0...  200705 non-black  512x480  mdec 484560  cd 18271
```

The film runs to about frame 9000, the display switches to 512x480, and from
there the checksum is nearly static while the CD and GP0 counters keep moving:
a game drawing a still screen and waiting.

### Reproduced

```
--press start@10500+8

frame 10500  2ab4d9a0...  200705 non-black
frame 11000  f4931b15...       0 non-black    cd 18688  gp0 94169532
frame 11500  f4931b15...       0 non-black    cd 18688  gp0 94169532
```

Blank, and the CD and GP0 counters stop dead where the baseline run keeps
climbing. The same thing the front end shows, now in the harness.

### It is a crash, not a stall

`A0(40)` - SystemError - 48 million calls. And two unimplemented paths, where
the baseline run with no button pressed hits **zero**:

```
PSXEmu.Core\psx\cpu.cpp line 900, 5 hits, first 4320616C
PSXEmu.Core\psx\cpu.cpp line 932, 1 hits, first 48207265
```

Those are the unhandled cases of COP0 and COP2 - and the instruction words are
ASCII. `4320616C` is "la C" and `48207265` is "er H". The CPU is executing text.

### Where it goes wrong

The trace ring shows the control flow, and it is unambiguous:

```
0x80149C48  li   t2, 0xA0
0x80149C4C  jr   t2
0x80149C50  li   t1, 0x43          -> BIOS call A0(43)
...
0xBFC03D4C  lw   t3, 0(a0)         pc0 out of the EXEC header
0xBFC03D50  lw   gp, 4(a0)         gp0
0xBFC03D58  jalr t3                and jump to it
0x80011430  "optio"                <- which is not code
0x80011434  "ons\0"
0x80011438  0x80011C38
```

`0xBFC03CF0` is the BIOS's `Exec()`: it saves the callers registers into the
header, zeroes the bss from `b_addr`/`b_size`, sets `sp` from
`s_addr`+`s_size`, then loads `pc0` from offset 0 and jumps to it. The game
called it - from `0x801448EC`, with a header built on its own stack at
`sp+0xA8` - and the `pc0` in that header was `0x80011430`, which is in the
middle of the game's own string and pointer data. The next few hundred words it
executed are Wild Arms item names: "Silver Harp", "Blue Circle", "Clear Chime",
"Memo...".

So the game asked the BIOS to run an overlay and handed it a header pointing at
data. The remaining question is how that header came to be filled that way, and
that is the next session's work: the header is a stack local, so the way in is
to catch the write rather than the read - either by watching the address once
the stack pointer at that moment is known, or by tracing the function that
fills it before `0x801448EC`.

Nothing here suggests the MDEC or the XA decoder. The film plays to the end.

### One real bug fixed on the way

`Cpu::Store` set `BadVaddr` to `prev_pc` rather than to the address that
faulted, on both of its address-error paths. The pc is already in `EPC`, so
every address error reported `BadVaddr == EPC` - which reads like a jump into
nowhere and hides the pointer that was actually bad. The load paths were always
right; only the stores were wrong. Now both report the faulting address.

That is exactly the field this investigation wanted, and it was lying.

### And the trap counter says what, not just where

`TrapCounter::Hit` takes an optional detail value, and the two coprocessor
fall-throughs pass the instruction word. Without it, "cpu.cpp line 900, 5 hits"
was a dead end; with it, the ASCII was immediately obvious and reframed the
whole investigation - the coprocessor cases are not missing features, they are
a symptom of executing data.

All 533 harness checks pass, the BIOS shell checksum is unchanged
(`bd888bab645a63a9`), and Legend of Mana is unaffected.

---

## 26. A seek did not stop the read that was already running

**Symptom.** Wild Arms blanked and hung after "press start". Bug 25 traced it as
far as the BIOS's `Exec()` being handed a header whose `pc0` was `0x80011430`,
pointing into the game's own item-name table, and left the question of how the
header got that way.

**How it was found.** By following the header backwards, one step at a time.

The stack pointer at the `Exec` call came from `--trace-at 8014486C`, because
`addiu a0, sp, 168` reads `sp` and the tracer prints an instruction's source
register: `sp = 0x801FD360`, so the header was at `0x801FD408`. Watching that
address gave the writer:

```
pc 801447A8  4-byte write of 80011430 to 801FD408
```

which is a sixteen-byte-at-a-time copy loop ending at `t0 = 0x80011460` - the
game's `memcpy`, copying `0x80011430..0x80011460` onto the stack. That range is
exactly `pc0` through `s_size` of a PS-EXE header, and `0x80011430` is
`0x80011420 + 0x10`, which is where `pc0` sits in one.

So the game reads an executable to `0x80011420` and copies the header fields out
of it. Watching `0x80011430` showed every write to it coming from CD-ROM DMA -
the read happened. And the executable is real: at file sector 208, lba 358,

```
50 53 2d 58 20 45 58 45   "PS-X EXE"
+0x10 pc0     800b065c
+0x18 t_addr  80011420      <- exactly where the game loads it
+0x1C t_size  000c1800
```

Then the DMA log, once it kept the *last* transfers rather than the first,
said where the read actually began:

```
madr 80011420   512 words -> 00011C20  lba 359  first 80011420
madr 80011420   512 words -> 00011C20  lba 360  first 6E6F6974
```

The first sector into the buffer was **359**. And the CD-ROM command log, once
its ring was wide enough to reach back that far, said what the game had asked
for:

```
setloc lba 358      <- the game asks for 358, which is right
SeekL
setmode 80
ReadN               <- and the read starts at 359
setloc lba 359
SeekL
ReadN               <- and this one starts at 360
```

Every read began one sector after the one that was asked for.

**Cause.** `SeekL` and `SeekP` set the head position and left `reading_` alone:

```cpp
read_lba_ = seek_lba_;
seek_pending_ = false;
```

The game had a read still running - a `ReadS` with no `Pause` before the seek -
so `StepRead` was still being called. Between the seek and the `ReadN` it
delivered one more sector, from the position the seek had just set, and
incremented past it. The `ReadN` then started one sector late. On hardware a
seek aborts whatever the drive was reading.

The effect on a game loading an executable is total: the PS-EXE header is the
first sector, so losing it shifts everything by 2048 bytes. `pc0` was read from
what was actually file offset `0x1010`, deep inside the text - and the value
there, `0x80011430`, is a perfectly plausible-looking RAM address, which is why
it produced a jump into data rather than an obvious fault.

**Fix.** `reading_ = false; playing_ = false;` in the seek commands.

**Result.** Wild Arms gets past its title screen:

```
frame 10500  2ab4d9a0...  200705 non-black  512x480   waiting for input
frame 11000  21a10944...   44266 non-black  320x232   in the game
frame 11500  21a10944...   44266 non-black  320x232
frame 12000  f4600283...   44330 non-black  320x232
```

It changes resolution to 320x232, keeps drawing, keeps reading - CD sectors
19498 to 22027 across those frames - and `A0(40)` is called zero times where it
was called 48 million.

Legend of Mana, the synthetic test disc and the BIOS shell are all unchanged
(`bd888bab645a63a9`), and all 533 harness checks pass.

**Why it took three sessions to find.** The symptom was a wild jump, which
looks like a CPU or memory bug. Two things made the difference: `BadVaddr` being
fixed to report the faulting address rather than the pc (bug 25), and the
diagnostic rings being turned round to keep the *last* events rather than the
first. A crash investigation always wants the end of the log, and both the CD
event log and the DMA transfer log were keeping the beginning - so both went
quiet hundreds of thousands of sectors before the interesting part. That was
costing time on every investigation, not just this one.

## 27-31. The root counters were a sketch

Five separate defects, all in the timers, all of the same kind: nothing
crashes, nothing logs, a game just runs at the wrong speed or waits for an
interrupt that never comes. They are grouped because they were found and fixed
together, against a reference read from documentation and DuckStation's
`timers.cpp` rather than from memory.

**27. Sync modes were decoded and thrown away.** `mode.syncmode` was parsed
into the bitfield and never read by anything. A counter told to pause during
hblank, restart at vblank, or wait for the first blank and then free-run did
none of those - it free-ran from the start, always. Fixed by giving each
counter a gate (hblank for counter 0, vblank for counter 1) and implementing
all four sync modes, plus counter 2's own rule: it has no gate at all, so the
two sync modes that would wait for one simply stop it.

There was a trap attached to this one. `IOInterface::Initialize` used to set
`mode.en = 1` on all three counters. That bit is *sync enable*, not counter
enable, and with sync mode 0 it means "pause while gated" - harmless while
sync modes were ignored, but the moment they worked it would have stopped
counter 2 dead until software wrote its mode register. Implementing the
feature and leaving that line alone would have broken the machine in a way
that looked like the new code was wrong. The counters now initialise with sync
disabled, which is free-run.

**28. A target of zero never matched.** `Tick` guarded the target comparison
with `target > 0`. But zero is a legitimate target and a useful one: it
matches on every count, which is how software asks for an interrupt on every
tick of the source. A game doing that got silence. The guard is gone and the
comparison now reads `counter >= target && (old_counter < target || target ==
0)`, which is the hardware's own condition.

**29. The counter could only wrap once per step.** The wrap was a single
`counter -= limit`. Hand it a step longer than the wrap interval - a counter
with a small target on the dot clock, easily - and the counter was left far
above its own limit and the intervening matches were lost. `Tick` now walks to
each wrap point in turn, which also makes toggle mode toggle bit 10 once per
match rather than once per call.

**30. The dot clock was wrong twice over.** Counter 0's dot clock was CPU
cycles divided by a hardcoded 10. Both halves are wrong: the dot clock derives
from the *GPU* clock, which is 11/7 of the CPU clock, and the divider follows
the horizontal resolution - 10, 8, 5, 4 and 7 for 256, 320, 512, 640 and 368
pixels. The old code therefore ran counter 0 at 7/11 of the right rate at
256 wide, and at up to a quarter of it at 640. Both are now taken from the
GPU, which is the only thing that knows either number.

**31. Bit 10 was not modelled at all.** The interrupt-request bit was cleared
on the first interrupt and only restored by a mode write, so pulse and toggle
mode were indistinguishable and one-shot mode stayed latched far longer than
the hardware would. Now: pulse mode dips the bit and returns it, with a
one-shot flag re-armed by a mode write; toggle mode flips it on every match
and asserts the line only on the flips that take it low.

**A sixth, found by the test rather than by reading.** Once counter 0 was
taking its dot clocks from the GPU, `timer_test` said it counted 36,546 dot
clocks in 7,000 cycles - about 33x too many, and not even linear. The cause
was in the new code: `Gpu::Tick` computes `dots` and then, after subtracting
whole scanlines, carries the leftover *back into* `dot_accumulator_`. So that
member holds the beam's position within the scanline, not a fractional
remainder, and `dots` on any given call is that position plus the clocks
actually elapsed. Reading it as a delta re-counted most of a scanline on every
single call. The irregular increments were the giveaway - they tracked the
beam's position within the line. Fixed with a remainder of its own; the
measurement is now exactly 1,100 dot clocks per 7,000 cycles, as arithmetic
says it should be.

**What software can now see that it could not.** `IOInterface::Tick` is called
once per CPU cycle and batches to 32 before doing any work, so a counter read
used to return a value up to 32 cycles stale - a game timing a short interval
got a quantised answer. Reads and writes of any counter register now call
`RunPending()` first, which runs the partial batch immediately. Interrupts are
still up to 32 cycles late; reads are exact.

**Verification.** `timer_test` is 70 checks. Every one of the five original
defects was reintroduced deliberately afterwards to confirm the tests catch
them: the `target > 0` guard (3 failures), the single-subtraction wrap (5),
counter 2 treated like counters 0 and 1 (3), the CPU-cycles-over-10 dot clock
(1, reporting 700 against 1,100), and removing the read flush (4). The BIOS
shell checksum is unchanged at `bd888bab645a63a9` with 0 unimplemented paths,
and Legend of Mana and Wild Arms both render exactly as before.

## 32. Load delay slots

A load on the R3000A does not reach its register in time for the instruction
right after it. The value arrives one instruction later, and software written
for the machine both relies on that and works around it. This core wrote the
register immediately, which is *more* permissive than the hardware - code that
would read a stale value on a console read the fresh one here - so nothing
broke, and nothing said anything either.

**What it now does.** A load arms a record rather than writing. At the start
of the next instruction that record becomes pending; at the start of the one
after, it is written to the register file. Three details beyond the basic
delay, each of which is a way to get this subtly wrong:

- **A register write in the delay slot beats the load.** The hardware writes
  the load back in its own writeback stage, one cycle before the following
  instruction's, so the following instruction's result is the one that
  survives. Model it the other way round and the load arrives late and quietly
  overwrites whatever the slot computed. Every register write in the core now
  goes through `Cpu::WriteReg`, which cancels a load in flight to the same
  register - that is what the 44 converted write sites are for.
- **lwl and lwr forward to each other.** They are meant to be used back to
  back with no gap, which only works because the hardware forwards the first
  one's result to the second. Without that, the pair silently assembles half a
  word. `Cpu::ReadRegForwarded` is that forwarding path, and nothing else uses
  it.
- **The pipeline advances at the start of an instruction, not the end.** A
  branch runs its delay slot as a nested `ExecuteInstruction`, so
  end-of-instruction bookkeeping would run the slot's before the branch's -
  out of program order - and a load two instructions before a branch would
  reach its register one instruction late. This one is invisible except in
  exactly that arrangement.

**What it cost in the tests.** Nineteen `cpu_test` checks failed immediately,
all of them tests that read a loaded register in the very next instruction -
programs that would not have worked on the hardware. They were relying on the
old permissiveness. The harness grew a `Settle()` that runs the two
instructions a load needs to land, and the affected tests say so explicitly
rather than being quietly rewritten to expect something else.

**Verification.** A new `loaddelay` group, 8 checks, covering all four points
above plus a load into r0 and a second load to the same register. Each of the
four was then broken deliberately to confirm the tests catch it: writing the
register immediately (1 failure), dropping the write cancellation (1),
dropping lwl/lwr forwarding (2, including the pre-existing unaligned-word
test), and moving the pipeline advance to the end of the instruction (1 - the
branch case, and only that one). The BIOS shell checksum is unchanged at
`bd888bab645a63a9` and both games render exactly as before.

## 33. DMA transfers took no time at all

A DMA ran to completion inside the register write that started it and the
machine's clock did not move. A game that moves a lot of data therefore ran
faster than the hardware relative to its own timers, its CD and its SPU - not
by a little: a full display list is a few percent of a frame, every frame.

Transfers are now billed for the bus time they take: about one cycle a word,
plus one per sixteen for the DRAM page boundary, plus 8 cycles for each
linked-list node and 5 more for a node carrying data. Those cycles go to
`Cpu::TickCycles`, which advances the CPU's cycle counters - so the front
end's pacing accounts for them - and hands them to the rest of the machine in
the same 32-cycle steps ordinary execution uses. One enormous step would jump
the GPU dozens of scanlines at once and leave the display gates the root
counters watch meaningless for the whole transfer.

The rate is DuckStation's model, not a measurement of real silicon, and
[Gaps.md](Gaps.md) says so.

The visible effect is small and in the right direction: over 1800 frames
Legend of Mana executes the same work and takes 9,639 exception returns where
it used to take 9,868, and Wild Arms 3,782 against 3,817. Fewer CPU
instructions fit in the same wall of frames, because the bus is busy. Both
games render identically.

**Channel 6 still does not acknowledge.** The OTC channel has never raised its
DMA interrupt here. That is left exactly as it was - whether it should is a
separate question from how long it takes, and changing two things at once in
the DMA is how the last three bugs there got missed.

## Speed, measured for the first time

Nothing in this project had ever measured wall-clock speed, which made every
discussion of optimisation an argument about guesses.
[Recompiler-Plan.md](Recompiler-Plan.md) says measurement should come before
any optimisation work; `boot_runner` now reports it at the end of every run.

On this machine, after the load-delay and DMA-timing work above:

```
BIOS shell        6.75s emulated in  4.57s wall = 1.48x real time, 87.5 fps
Legend of Mana   30.36s emulated in 21.98s wall = 1.38x real time, 81.9 fps
Wild Arms        30.36s emulated in 19.88s wall = 1.53x real time, 90.5 fps
```

The interpreter is already faster than the console it emulates, on real game
workloads, with every diagnostic in the build compiled in. That does not make
a recompiler pointless, but it does move it out of "needed to run games at
all" and into "needed for headroom" - which is a different argument, and one
the plan should now be re-read against.

## 34. CD audio did not play, and playback was not the reason

Reported as "playing CD audio tracks, not working". The obvious place to look
is the playback path - `Play`, the sector feed, the SPU's CD input - and all of
it turned out to be correct. The fault was one step earlier and looked nothing
like an audio problem.

**What the measurement said.** The CD input had no diagnostics at all, so a
track playing silently and a track that never started were indistinguishable.
Adding counters for pairs handed over, pairs mixed, pairs dropped and pairs
thrown away with the enable bit clear answered it immediately. Ridge Racer,
booted from its cue sheet:

```
cd audio  1255968 CD-DA pairs in (peak 29648), 1255866 mixed from the CD
          input, 0 dropped, 0 muted
```

Twenty-eight seconds of music, nothing dropped, nothing muted. To rule out the
possibility that this was a data track being read as audio - which would also
be loud - the sector `Play` actually read was compared against the image:

```
file sector 140990:  2e 09 8b 00 7c 07 2e 00 62 06 f2 ff ...   smooth PCM
file sector    100:  00 ff ff ff ff ff ff ff ff ff ff 00 ...   data sync
```

Genuine music. Playback works.

**What was actually wrong.** The same game booted from its `.bin` instead of
its `.cue` mounts as one data track covering the whole disc, issues no `Play`
at all, and is silent. The track layout is not in the image: it lived in the
disc's lead-in, which a dump of the data area does not contain. So the game
asks how many tracks there are, is told one, and never asks for a note. Nothing
in the audio path ever runs, and nothing reports anything, because from the
emulator's point of view nothing went wrong.

**Two fixes.**

*A cue sheet beside the image is now used.* Picking `game.bin` when
`game.cue` is sitting next to it is an easy thing for someone to do and used
to cost them every music track on the disc. Ridge Racer opened by its `.bin`
now mounts 14 tracks and plays.

*An image with no sheet at all works out what it can.* A data sector is
recognisable by its twelve byte sync pattern and audio is not, so a binary
search - about twenty reads of a half-gigabyte file - finds where the data
track ends. On Ridge Racer it lands on sector 1737, which is exactly where the
cue sheet says track 2 begins. That is worth having on its own: reporting the
whole disc as data meant a game reading past the data track was handed music
and told it was a filesystem.

The second fix does *not* restore music for a disc with several tracks. Where
one music track ends and the next begins is recorded nowhere in the data area,
so twelve tracks still mount as two and a game that asks for track 5 by number
still gets nothing. Ridge Racer without its cue boots and stays silent, which
is the honest outcome - and [Gaps.md](Gaps.md) says so rather than implying
the case is covered.

**A measurement bug found on the way.** The first version of the counters
reported Air Combat as "0 pairs in, 2,154,314 mixed", which reads as an
emulator fault and is not one: CD-DA and XA-ADPCM share a single CD input,
because the hardware has one, and only the CD-DA side of what goes in was being
counted. The output counter is now labelled for what it measures rather than
what it was assumed to measure. `QueueCdAudio` had also been duplicating
`PushCdFrame` rather than calling it, which is why the drop counter only
covered one of the two paths; it calls it now.

**Verification.** Three new `media_test` checks groups covering the sync scan,
the all-data case, and cue adoption - 17 checks, taking the file to 132. Both
new behaviours were then broken deliberately to confirm the tests catch them:
removing the scan (6 failures) and never looking for the sibling sheet (2,
including the one that distinguishes a three-track sheet from a two-track
scan). The BIOS shell checksum is unchanged and both regression games render
exactly as before.

## 35. Three bugs between the BIOS CD player and a note of music

Reported as choosing a track in the BIOS CD player and hearing nothing. Bug 34
had already established that CD-DA playback itself works, so this was somewhere
else entirely - and it was three separate faults in a row, each of which alone
is enough to produce silence. All three live on a path no game uses: a game
reads data sectors and never asks the drive where it is or tells it to resume,
which is why every game booted with all three broken.

Getting to it needed two things the harness did not have: `--insert <disc>
<frame>` to put a disc in while the machine is already running, because the
BIOS boots any disc that is already there rather than showing its menu, and a
PNG writer so a frame could actually be looked at. The first screenshot
answered a question three runs of button-pressing had not: the cursor starts on
MEMORY CARD, and CD PLAYER is *below* it, not beside it.

**35a. Swapping a disc told software nothing.** `OpenDisc` set the status
straight to "motor on". On hardware, opening the lid sets bit 4 of the status
and it latches - it reads "is or was open" until something reads it - and that
latch is the only way software finds out the disc changed. Without it the CD
player sat with an empty track list for ever: it had no reason to look again.
With it, the player reads the table of contents and lists all fourteen tracks.

The status is now built by `StatusByte()`, which folds the latch in and clears
it on the first read once a disc is present, and every response that carries a
status goes through it.

**35b. GetlocP had a status byte in front of it.** The command returns eight
bytes of subchannel - track, index, time within the track, time from the start
of the disc, all BCD - and no status. Ours prepended one, so every field was
one byte out: software read the status as the track number, the track as the
index, and a time shifted by a byte. It also pushed the absolute frame off the
end. The CD player drives its whole display from this, which is why it sat at
00:00.

The report packet sent during playback is a *different* shape - status first,
one of the two times, then a peak level - so `GetPosition` and `GetReport` are
now separate functions rather than one used for both.

**35c. `Play` with a track number of zero was rejected as an invalid track.**
Zero is not a track: it means carry on from where the head is, and the BIOS CD
player sends exactly that every frame while a track plays. Each one came back
INT5. The player retried for ever - 1,250 Play commands and 1,249 errors in a
twenty second run - and nothing came out. Now zero means what it means, and an
out of range track is still an error.

**Also fixed on the way: the reports were sent seventy-five times a second.**
The drive reports on eight sectors of every seventy-five, alternating between
the time from the start of the disc and the time within the track, with bit 7
of the seconds byte saying which. Which eight is not arbitrary - software reads
the pattern off the absolute frame number - so the phase matters as much as the
rate. Interrupts during playback dropped from 1,891 to 255.

**What it looks like now.** Boot the BIOS with no disc, choose CD PLAYER,
insert a disc, pick track 6, press play: the player lists fourteen tracks,
seeks to track 6, and the display counts up to 0:24 while 1,831 sectors of
audio come out - the two agree exactly, 1,831 sectors being 24.4 seconds.

**Verification.** A new `media_test` group, 16 checks, covering all three.
Each was then broken deliberately to confirm the tests catch it. The first
attempt at the swap test did *not* - it ejected before loading, and an eject
sets the same bit, so it passed whether or not loading set it. That is the
whole failure mode of the original bug reproduced inside its own test. It now
swaps without ejecting, which is what the front end does.

## A rendering bug found while looking at the screen

Not investigated, recorded so it is not lost: the BIOS menus draw rainbow
noise behind their highlighted labels - the MAIN MENU items, the MEMORY CARD
buttons, the CD player's EXIT. The shapes and text are right and the noise sits
where a flat or gently shaded fill belongs, so it looks like a texture or CLUT
being read from somewhere nothing was written. It has been there all along and
nothing measured it, because nothing had looked at these screens until now.

## 36. CD and XA audio were mixed at minus one

The BIOS CD player from bug 35 could list a disc's tracks, seek to one and run
its time counter, and still play nothing. The sectors were reaching the SPU -
`cd audio 1076628 pairs in, 1076361 mixed` said so - but nothing came out of
the speakers.

The counter that said "mixed" was measuring the wrong side. It ticked when the
CD-audio-enable bit was set, *before* the volume multiply. Adding a second
counter for the peak of the CD contribution *after* its volume answered it in
one line: `cd-out-peak 1`, from an input whose peak was 32767 and a CD volume
register reading 7FFFh - full.

**The volume register was run through the wrong conversion.** The CD and
external input volumes (1F801DB0h..) are plain signed 16-bit levels,
-8000h..+7FFFh. The voice and main volumes (1F801C00h+N*10h, 1F801D80h) are a
different format: bit 15 selects a volume sweep, and the level underneath is
doubled. `VolumeOf`, which decodes the second kind, was being applied to the
first. On the maximum the BIOS writes, 7FFFh, it read bit 15 as the sweep flag
and the doubling trick underneath wrapped the top half of the range negative -
7FFFh came out as -1. Every CD-DA and XA track was multiplied by roughly minus
one and vanished.

The fix is a second one-line helper, `InputVolumeOf`, that takes the register
as the plain signed value it is, and the CD mix uses it. `cd-out-peak` went
from 1 to 32767 and the output WAV's peak from 16906 - which was the menu blips
alone - to 32766.

**This is the same bug behind "Wild Arms FMV has no audio", from the very
first commit of this effort.** XA-ADPCM shares the one CD input the hardware
has, so it was silenced by the same multiply. Bug 25 had verified the XA
*decoder* - the PCM it produced had the right shape - but never the mix that
carried it to the output, and the mix was throwing it away. Wild Arms sets its
CD volume to 6EDCh, which `VolumeOf` turned to about -4388; with the fix its
FMV audio mixes at a peak of 10619.

**Verification.** A new `cdvolume` group in `spu_test`, 4 checks: a full CD
volume of 7FFFh is audible rather than silent, zero is silent, half is quieter
than full, and the enable bit still gates the whole thing. The bug was then
reintroduced - `VolumeOf` back on the CD path - and the tests caught it (2
failures, the full and half cases). All 99 spu_test checks pass, the BIOS shell
checksum is unchanged, and both regression games render identically.

Left alone deliberately: `VolumeOf`'s own doubling has an extra shift that
cancels it, so the voice and main volumes it decodes come out at half the level
the hardware would give. That is a real discrepancy - it is part of why audio
has always been quiet enough to want the 2x master gain - but it touches every
game's balance and the gain default at once, so it is a measured change for its
own pass, not a rider on this one. Noted in [Gaps.md](Gaps.md).

## 37. The last five CD-ROM commands

The drive answered 23 of the controller's 28 commands; the other five fell
through to the "unknown command" error. None had been asked for by anything
that boots, because they are the commands a *player* drives, not a game: the
CD player's scan buttons, a session switch, a controller reset, and a raw TOC
read. Implemented together, against psx-spx and DuckStation's `cdrom.cpp` for
the exact responses.

**04h Forward / 05h Backward** — fast scan during CD-DA play. Each sets a scan
level in its direction; while it is non-zero the playing loop skips a block of
sectors per sector time instead of one, so the disc runs fast. Repeating the
command bumps the level, which is how a real drive scans faster the longer the
button is held; a `03` Play with no parameter drops back to ordinary speed at
the scanned position, which is what the CD player sends on release. Forward off
the end of the last track stops the motor with INT4; Backward into the first
track resumes ordinary play. Sending either to a stopped drive is a no-op that
still returns status.

**12h SetSession** — seeks to a session. Session 1, the one every game disc
has, acknowledges then completes like a seek; any other session on these
single-session images acknowledges then fails with a seek error, because there
is nothing there to seek to.

**1Ch Reset** — reboots the controller: mode back to zero, anything in flight
abandoned, head to the start, motor spun up if a disc is in. Answered like
Init (acknowledge then complete) so software waiting on it is not left hanging.

**1Dh GetQ** — returns one subchannel-Q entry from the table of contents. The
TOC is the track table, so that is what it reports: control/adr, the track and
index asked for, and the track's absolute start in minutes, seconds and
frames. DuckStation stubs this as an error; a real ten-byte answer built from
the track table is no more code and is not a lie about what the drive can do.

**Scanning needed the playing loop restructured.** It used to read a sector and
unconditionally `++read_lba_`. Now the advance is a branch: normal play steps
one and ends the track at the last sector; a forward scan jumps a block and
stops the motor at the end; a backward scan jumps back and turns into play at
the start. The scan level is cleared by Play, Stop, Pause, Init, a seek and
Reset - every command that establishes a fresh idea of where the head is.

**Verification.** A new `media_test` group, 19 checks: that ordinary play moves
about one sector per sector time, that Forward moves far faster, that Play
after a scan returns to normal speed, that SetSession 1 completes and 2 errors,
that Reset clears the mode, and that GetQ returns ten bytes whose absolute time
is the track's start. Each behaviour was then broken deliberately to confirm
the tests catch it: Forward not scanning (1 failure), SetSession accepting any
session (1), Reset not clearing the mode (1), GetQ reporting the wrong time (2).
All 167 media_test checks pass, the BIOS shell checksum is unchanged, and every
regression game - including Ridge Racer's CD music and Wild Arms' FMV audio -
is byte-for-byte as before.

What is approximate is stated in [Gaps.md](Gaps.md): the scan rate is a
plausible number rather than a measured one, SetSession assumes the single
session these disc formats carry, and GetQ has only the track table to build
its Q bytes from. Nothing tested needs more.

## 38. A DMA channel's busy bit cleared before anything could see it

Bug 33 made a transfer cost real cycles, but the *result* of that transfer -
the busy bit clearing, and the completion interrupt - still happened inside
the very register write that triggered it, before any of those cycles had
been spent. Software that starts a transfer and then polls CHCR, or waits on
the interrupt, to find out when it is done saw it finish before its own next
instruction could possibly run: the wait always fell straight through. That
is not a subtle timing difference - it is the one thing software waiting on a
transfer actually checks.

The fix splits triggering a transfer from finishing it. `Dma::RunChannel`
still moves the data immediately and unconditionally - nothing about *what* a
transfer does changed, so every existing result stays byte-for-byte identical
- but now leaves the busy bit set and defers clearing it, and raising the
interrupt, to `Dma::Tick`, called from the same 32-cycle batches everything
else advances in through `IOInterface::RunPending`. A channel's busy window
now survives for as many of the machine's real cycles as bug 33 already said
the transfer cost, crossed by the CPU's own ordinary instruction-by-instruction
ticking rather than resolved all at once inside the triggering write.

That distinction is why the cost is charged through a new `Cpu::AccountCycles`
rather than `TickCycles`: `TickCycles` also drives the GPU, CD-ROM, SPU and
timers forward by the same amount, synchronously, before the CPU's next
instruction runs - which would make the busy window invisible to anything
checking in between, defeating the point of having one. `AccountCycles` only
advances the CPU's own counters; the rest of the machine catches up through
its normal per-instruction ticking, at the same pace software polling the
channel experiences.

DMA register reads and writes now flush the pending batch (`RunPending`)
before touching channel state, for the same reason root counter reads already
do: without it, a channel could read as busy for up to 32 cycles after its
transfer had actually finished. It also mattered for a guard that already
existed: channels 2, 5 and 6 refuse a new trigger while their own busy bit is
still set, but that check could never previously have blocked anything -  the
bit it reads was always already clear by the time a second write could
possibly arrive. Now that the bit stays set for real, the guard does too, and
without the flush a legitimate retrigger arriving just after a transfer's real
completion time could have been wrongly rejected.

**Verification.** Two new `media_test` groups: "dma busy bit" checks that the
bit is set the instant a transfer is triggered, stays set across one tick
still inside its own transfer time, and only clears - with the completion
interrupt landing at the same moment - once that time has elapsed; "a busy
channel 2 refuses a new trigger" checks that a second CHCR write arriving
while busy is dropped whole rather than merged in. Reverting the fix to the
old instant-completion behaviour was confirmed to fail both groups (5 checks,
with exactly the stale values the old behaviour would produce), then restored.
All 719 checks across every harness (`media_test`, `cpu_test`, `gte_test`,
`timer_test`, `sio_test`, `mdec_test`, `spu_test`) pass, the BIOS shell
checksum is unchanged, and Legend of Mana, Wild Arms and Ridge Racer's CD
player all render and behave identically to before.

**What this did not fix.** This was written chasing
[Air-Combat-FMV-Plan.md](Air-Combat-FMV-Plan.md)'s freeze - a game that
triggers a DMA and polls to learn when it is done, which is exactly the shape
of gap this closes. It did not resolve that freeze: the mechanism actually
stuck there turned out to be a different wait than the one this fix targeted.
See that plan for what is still unexplained. The fix stands on its own
merits regardless - see [Gaps.md](Gaps.md) for what is still approximate
about it (data still moves eagerly; only the completion signal is paced).

## 39. Key-on threw away the loop point, and Final Fantasy VII played a twelfth flat

Final Fantasy VII reaches its title screen and starts the prelude at 33.5
seconds. Every note was too low, and the tone was wrong as well as the pitch -
buzzy where it should have been smooth. FMV and CD audio were unaffected,
which pointed at the voice path, because nothing else in the SPU is shared
between the two.

### What the capture said, before any code was read

`boot_runner --wav` over 3600 frames, then `wav_pitch` over the result. The
arpeggio came out as a clean ladder - C2 F2 G2 A2, C3 F3 G3 A3, C4 F4 G4 A4,
C5, turning at F5 698.21 Hz and descending the same way. Across 27 seconds and
1173 analysis windows: **lowest B1, highest F5, every note within a few cents
of equal temperament, no dropouts and no silences.**

That shape was the finding. A clamp or a fold hits high notes and leaves low
ones alone; this was a **constant** factor, applied to everything, with every
interval preserved exactly. It ruled out the two suspects that reading the code
had produced - `VxPitch` being masked to 14 bits rather than clamped, and the
pitch-modulation path - before either was touched. Both would have detuned some
notes and not others.

### What the game was actually asking for

New counters on `Spu::Stats`, printed by `boot_runner`, and a `--trace-spu` log
of every key-on. One run settled it:

```
spu requests   max pitch 3FFFh (4.00x), 0 writes over 3FFFh; volumes 0 sweeps / 4006 levels
spu modes      pmon 000000  noise 000000  reverb FFFFFF  control C0B5
```

No pitch above 3FFFh anywhere (the one 3FFFh is the BIOS zeroing the voices at
0.146s), so the mask never fired. No pitch modulation. No volume sweeps, so the
unimplemented sweep was not flattening the mix either. Three hypotheses gone in
one line each.

The trace showed all 438 prelude notes playing **one** sample, at start
address 186Ah, across six voices - and this, on every single note:

```
33.569  v10  REPEAT= 186E  (start 186A)
33.569  v10  KEYON  pitch 017E ( 0.093x)  start 186A  repeat-was 186E
```

FF7 sets the loop point by register and *then* keys on. `KeyOn` was resetting
`repeat_address` to `start_address`, throwing it away every time.

The sound RAM dump (`--spu-ram`) showed why that mattered so much. At 186Ah:
two blocks of silence, then at 186Eh a block whose nibbles decode to
`0,1,2,3,4,5,6,6,5,4,3,2,1,0,0,-1...-6,-6...-1,0` - a 28-sample triangle,
flagged end-and-repeat, with no loop-start flag anywhere in the sample.

| | Loop | Samples | Base at 1.0x |
|---|---|---|---|
| What FF7 asked for | 186Eh alone | 28 | 1575 Hz |
| What it got | 186Ah..186Eh | 84 | 525 Hz |

Exactly three times too long, so exactly a third of the frequency: a perfect
twelfth, on every note, with every interval intact. And the loop carried two
blocks of silence for every one of waveform, so a continuous triangle came out
as a pulse train - the wrong timbre and the wrong pitch, from one cause.

### The fix

`KeyOn` no longer touches the repeat address. Key-on sets where a voice
*starts*; where it *loops* belongs to software and to the loop-start flag in
the data. The `repeat_set` flag, which was written in three places and read in
none, became `ignore_loop_start` and got its real meaning: while software has
set the loop point since the last key-on, a block carrying the loop-start flag
does not overwrite it.

FF7 writing that register 438 times, once before every note, is the evidence
that hardware keeps it. A sound driver does not make the same pointless
register write 438 times.

### Verification

Every note in the capture moved by exactly three:

| Before | After |
|---|---|
| C2 65.37 Hz | G3 196.06 |
| F2 87.25 | C4 262.38 |
| G2 97.94 | D4 293.64 |
| A2 109.98 | E4 329.19 |
| C3 130.77 | G4 391.70 |
| **F5 698.21 (top)** | **C7 2095.77** |

The arpeggio cell is now C-D-E-G - C major pentatonic, which is the Prelude's
actual figure - where before it was an F-based cell. Mean RMS over the music
rose from 612 to 1041, because two thirds of the loop is no longer silence.

A new `loopaddr` group in `spu_test`, 8 checks, taking spu_test from 99 to 107.
Four of the eight fail against the old behaviour - measured, by putting it back
and running the group against it. The other three harnesses' counts are
unchanged, and 3600 frames of FF7 produce an identical framebuffer checksum,
instruction count and interrupt count before and after: the change is audible
and nothing else.

**Why no test caught this.** `spu_test` had 99 passing checks over the ADPCM
path, including "a looping sample keeps playing". Nothing asserted *where* a
voice loops back to - only that it was still making a noise afterwards, which a
voice looping over three times as much sample as it should does perfectly well.
One of the existing envelope tests had to be corrected as part of this: it
looped a block flagged end-and-repeat with no loop-start flag and relied on
key-on to supply the loop point, which is the very behaviour that was wrong.
It encoded the implementation rather than the hardware, exactly what section 6
of the standards warns about.

**Still open, and separate.** `reverb FFFFFF` with the reverb master enable
set: FF7 routes all 24 voices through a reverb that this core implements as a
two-tap delay rather than the hardware's comb-and-all-pass network. That is a
real difference in what the prelude sounds like and it is untouched by this
fix. See [Gaps.md](Gaps.md) and
[FF7-Prelude-Pitch-Plan.md](FF7-Prelude-Pitch-Plan.md).

## 40. GP0(1Fh) Interrupt Request did nothing

`psx/gpu.cpp`

Every GP0 command below `20h` - clear cache, the six nops, and `1Fh` interrupt
request - fell into one `if (command < 0x20) { return; }` and was treated as a
nop. Clear cache genuinely is one, nothing here models a texture cache to
clear. `1Fh` is not: per psx-spx it sets GPUSTAT.24 and raises IRQ1 at the
interrupt controller, acknowledged only by GP1(02h) - a real interrupt source,
silently dropped.

No game or BIOS revision seen by this project issues it - a run through the
whole BIOS shell (`bios/SCPH1001.BIN`, 700 frames) shows zero - which is
consistent with psx-spx's own note that it is rarely used. Found while looking
for GPU register-level gaps rather than chasing a symptom, which is also why
it was safe to add without anything to regress against: nothing exercises the
path yet.

### The fix

```cpp
if (command == 0x1F) {  // interrupt request
  if (!status_.irq) {
    status_.irq = 1;
    system().io().SetInterrupt(kInterruptGPU);
  }
  return;
}
```

Guarded on the 0-to-1 edge rather than raised unconditionally: GPUSTAT.24 is a
level, and I_STAT is a set of sticky flags each latched once when their source
first asserts, not re-latched by a source that is already asserted and has not
yet been acknowledged. Software that calls this in a loop before acknowledging
should not see a second interrupt for the one it has not handled yet. This
half is reasoned from how every other interrupt source in this codebase
behaves, not from a hardware measurement - flagged here in case it needs
revisiting.

### Verification

New `gpu_test` harness - this codebase's first, 13 checks - covering reset
clearing both GPUSTAT.24 and I_STAT.GPU, the command setting both, GP1(02h)
acknowledging and allowing a fresh edge, and a repeated request while
unacknowledged raising no second I_STAT edge. Also pins down, as a fact about
the current code rather than an assumption, that the three GPUSTAT readiness
bits report ready unconditionally - see "no drawing time" in
[Gaps.md](Gaps.md).

All seven existing harnesses unchanged (cpu_test 189, gte_test 99, media_test
175, spu_test 107, mdec_test 59, timer_test 70, sio_test 28 - all still 0
failures), and `bios/SCPH1001.BIN` at 700 frames produces the identical
framebuffer checksum (`e6eff3d9569871e6`) before and after: nothing that was
already working moved.

## 41. A raw side-loaded PS-EXE never got a working machine to run on

Requested to add a Win32 menu command to load a standalone PS-EXE directly,
for running real test suites rather than games. The natural implementation
was to expose `System::LoadPsExe` - already used by `boot_runner --exe` -
through a file picker: reset, eject the disc, load, jump to the entry point.

It built, ran, and matched `--exe`'s existing behaviour exactly. It was also
useless for its actual purpose. amidog's `psxtest_cpu`, `psxtest_gpu` and
`psxtest_gte` - real PS1 hardware test suites, not games - all loaded and ran
millions of instructions with no crash and no unimplemented path hit, and none
of them ever turned the display on.

### What a fresh side-load skips

`LoadPsExe` sets `pc`, `$gp` and `$sp` from the header and nothing else. It
runs on whatever `Cpu::Reset()` left behind, which is the R3000A's power-on
state, not a PS1 that is ready to run software: after 3000 frames of
`psxtest_cpu` with no side-loaded BIOS having ever executed a single
instruction, `SR` was still `10900000` - **BEV and Isolate Cache both still
set**, exactly as they come out of reset.

BEV routes exceptions to the ROM vectors instead of the RAM ones the game's
own handler lives at, and Isolate Cache stops data stores reaching real
memory. Both are cleared in the first few instructions of every real BIOS,
before it does anything else, on a real console and in this one. A raw
side-load never runs those instructions, and neither does it call whatever the
BIOS uses to set up a default video mode - which is the direct reason nothing
appeared: the test suites assume that environment because it is the one thing
that is *always* true when a PS-EXE gets control on real hardware. There is no
path to running one at all that does not go through the BIOS first.

`LoadPsExe` used alone, with nothing depending on that environment, is not
wrong - a synthetic snippet that sets up everything it touches is exactly what
`media_test`'s own PS-EXE tests are. It just is not what a third-party test
suite can assume.

### The fix

The same mechanism `--auto-boot` already uses for a disc: let the BIOS run for
real, and act only once `pc` reaches `0x80030000` - the address it hands
control onward at, whether to a disc or to the shell. `System` gained a second,
parallel flag (`set_auto_boot_exe`), checked in `StepInstruction` right after
the existing disc one:

```cpp
if (auto_boot_exe_ && cpu_.context()->pc == 0x80030000) {
  auto_boot_exe_ = false;
  LoadPsExe(auto_boot_exe_path_.c_str());
}
```

The Win32 menu command now arms this instead of side-loading immediately: cold
reset, eject the disc, arm, unpause. The player sees the ordinary boot logo for
a second or two - the BIOS is genuinely running - before the test suite takes
over. `boot_runner --exe` gained the same behaviour when combined with
`--auto-boot`; alone, `--exe` still side-loads immediately, unchanged, since
that immediate form is still what a self-contained snippet wants.

### Verification

`boot_runner bios/SCPH1001.BIN --auto-boot --exe test/psxtest_cpu/psxtest_cpu.exe`:
display goes from `256x240, DISABLED, 0 non-black` to `512x222, enabled,
12,705 non-black`, and the written frame is the test's real results screen -
a legible grid of opcode and exception tests, each marked, with a totals line.
`psxtest_gpu` and `psxtest_gte` come up the same way, as their interactive
category-selection menus rather than results, which is correct: both need a
button pressed to actually run.

All eight harnesses unchanged (cpu_test 189, gte_test 99, media_test 175,
spu_test 107, mdec_test 59, timer_test 70, sio_test 28, gpu_test 13 - still 0
failures). Nothing about a disc's `--auto-boot` path changed; `--exe` without
`--auto-boot` is byte-for-byte the same immediate side-load it always was.

## 42. Every GTE command cost the same one cycle, and two real hazards beside it were never modelled

With bug 41's route to real test suites open, `test/psxtest_gte/` was run to
its results screen for the first time. REG and COMPLEX - every register and
every command's computed value and FLAG bits, checked against amidog's own
reference rather than this project's - passed outright. TIMING did not: all
22 opcodes it exercises came back ERROR, uniformly.

### The bug that was findable

`Cpu::COP2()` ran every GTE command through the same `Tick()` every ordinary
instruction gets:

```cpp
system_->gte().Execute(context_->code);
Tick();
```

One cycle, whether the command was `SQR` (5 cycles on real hardware) or
`NCCT` (39). `Gte::Execute` decodes the opcode already, for its own dispatch
switch, so it now returns what that command costs, and `Cpu::COP2` charges
that instead - via `Cpu::TickCycles`, which existed for exactly this
(DMA's bus-hold comment names it) but had never been called from anywhere.
Figures are DuckStation's `AddGTETicks` table, the same source already
trusted for DMA timing:

| Op | Cycles | Op | Cycles | Op | Cycles |
|---|---|---|---|---|---|
| SQR | 5 | AVSZ3 | 5 | AVSZ4 | 6 |
| GPF | 5 | GPL | 5 | OP | 6 |
| NCLIP | 8 | DPCS | 8 | INTPL | 8 |
| MVMVA | 8 | DPCL | 8 | CC | 11 |
| CDP | 13 | NCS | 14 | NCCS | 17 |
| DPCT | 17 | NCDS | 19 | RTPS | 15 |
| RTPT | 23 | NCT | 30 | NCCT | 39 |
| NCDT | 44 | | | | |

### Two more, found while reading what real hardware documents for this

Neither is what the timing figures above are about, but both are real,
separately documented, and both were completely unmodelled:

- **CFC2/MFC2 write their register the instant they run.** psx-spx: "Using
  CFC2/MFC2 has a delay of 1 instruction until the GPR is loaded with its new
  value" - the exact same one-instruction delay `LB`/`LH`/`LW` already have,
  which Tekken 2's geometry is the named case of depending on. Both now go
  through `ArmLoad`, the pending-load mechanism ordinary loads use, instead of
  writing the register directly.
- **Nothing stalls the CPU for a GTE command still in flight.** psx-spx: "If
  an instruction that reads a GTE register or a GTE command is executed
  before the current GTE command is finished, the CPU will hold until [it]
  has finished." `Cpu` now tracks the cycle a command's result becomes ready
  (`gte_busy_until_cycles_`) and, before dispatching a new command or an
  MFC2/CFC2 read, bills the remaining wait if that cycle has not arrived yet.
  MTC2/CTC2 are not documented to wait on this and do not check it.

### Verification

New `gtedelay` group in `cpu_test`, 5 checks: the delay-slot shapes
`TestLoadDelaySlot` already checks for an ordinary load, aimed at MFC2
instead (old value in the delay slot, new value one instruction later, a
write in the delay slot beating the load) - all three would fail against the
write-it-immediately behaviour this replaced - plus one check that issuing
`SQR` and reading a result register back on the very next instruction costs
at least SQR's own 5 cycles, not the 2 two ordinary instructions would.
cpu_test 189 -> 194. All seven other harnesses unchanged, and
`bios/SCPH1001.BIN` at 700 frames gives the same framebuffer checksum as
before (`e6eff3d9569871e6`) - the BIOS shell issues zero GTE commands, so
none of this had anywhere to move it.

**What did not move: amidog's TIMING column.** Identical before and after,
pixel for pixel (`0e4bc3dba27e48af`, non-black 10538/114688, both times). That
sent this back to the disassembly, further this time, and it settled the
question rather than leaving it open.

### What TIMING actually measures, and why fixing the GTE was not enough

`test/psxtest_gte`'s loop for each opcode is: reset Root Counter 2 (system
clock, no gate - a mode write of 0 restarts it at 0, which `RootCounter` here
already does correctly), run a fixed body **501 times** in a `bgtz` loop
(`SQR` or whichever opcode, then `CFC2 $t1, FLAG`, a `nop`, an accumulate, the
loop branch and its delay slot), then read the counter and store it. Reading
the counter back at every reset/read pair in this core's own model - a
temporary instrument, not a shipped change - gives, for every one of dozens of
measurements across every opcode:

    delta = 501 * (opcode_cycles + per-opcode_loop_overhead) + 4

and `opcode_cycles` recovered from that formula is **exactly** the bug 42
table above, every time: 5 for `SQR`, 6 for `OP`, 8 for `MVMVA`, and so on,
with no exceptions across every opcode this reached. That is about as direct
a confirmation as this project has had for anything: the GTE timing fix is
not just plausible, it is measured, from inside the test that was failing,
to produce precisely its own documented reference figures.

So the per-opcode piece is right, and the test still fails, which only
leaves one place for the discrepancy: the other 500-odd cycles per
iteration - `CFC2`, a `nop`, an `addu`, a branch and its delay slot, none of
them GTE - and the loop that repeats them 501 times to average out
measurement noise. This core charges every one of those a flat one cycle,
which is [Gaps.md](Gaps.md)'s "Cycle timing is modelled, not measured...
the per-instruction costs beneath it are uniform where real ones are not" -
already known, already documented, and not particular to the GTE at all.
Multiplied by 501, a real per-instruction error too small to matter almost
anywhere else becomes the entire result of this test. Passing amidog's GTE
TIMING column outright needs that gap closed - real branch and load/store
timing for the R3000A - which is a CPU-wide project of its own, not a GTE
one, and is exactly the work [Recompiler-Plan.md](Recompiler-Plan.md)
already names as coming after measurement, not before it.

None of that takes anything away from what did get fixed here. REG and
COMPLEX passing is real confirmation, from a source this project did not
write, that GTE value and flag correctness agree with hardware - not just
with a description read twice. The per-command cycle table, the busy stall,
and the MFC2/CFC2 load delay are each independently correct and tested,
verified now by direct measurement rather than by argument, and the
`gtedelay` test would catch a regression in any of the three.

## 43. Multiply and divide cost the same one cycle as ADD, and a not-taken branch cost nothing at all

[CPU-Timing-Plan.md](CPU-Timing-Plan.md) exists because of bug 42: the GTE's
own timing was right, but everything *else* in the loop that measured it -
ordinary CPU instructions - was still charged the uniform one cycle this core
charges everywhere, and that gap is a CPU-wide project of its own. This is
phases 1 and 2 of it.

### Phase 1: multiply and divide

Verified directly against a primary fetch of
`psx-spx.consoledev.net/cpuspecifications/` (not a search-result summary):

| Instruction | rs magnitude | Cost |
|---|---|---|
| `MULT`/`MULTU` | 0..0x7FF (or, for `MULT`, 0xFFFFF800..0xFFFFFFFF) | 6 |
| `MULT`/`MULTU` | 0x800..0xFFFFF (or 0xFFF00000..0xFFFFF801) | 9 |
| `MULT`/`MULTU` | 0x100000.. (or 0x80000000..) | 13 |
| `DIV`/`DIVU` | any | 36, fixed |

This core charged all four the same one cycle `ADD` gets - exactly the gap
the plan named as "the single most-repeated-in-real-code instance of the
uniform-cost gap": fixed-point math, audio mixing and software division all
lean on these constantly, and `cpu_test`'s existing `muldiv` group checks
their *results*, not their timing, so a wrong cycle count passed every check
there.

The source also documents a hazard identical in shape to bug 42's GTE one:
"the mul/div opcodes are starting the multiply/divide operation, starting
takes only a single clock cycle, however, trying to read the result from the
hi/lo registers while the mul/div operation is busy will halt the CPU until
[it] has completed." `Cpu` now tracks `hilo_busy_until_cycles_` the same way
it tracks `gte_busy_until_cycles_`, and `MFHI`/`MFLO` charge the remaining
wait if it hasn't elapsed. `MTHI`/`MTLO` are not documented to wait on this,
matching the same asymmetry `MTC2`/`CTC2` already have.

One boundary is genuinely ambiguous in the primary source itself: its
`MULT` table gives Fast's negative range as `FFFFF800h..FFFFFFFFh` and Med's
as `FFF00000h..FFFFF801h` - a one-value overlap at `FFFFF801h`/`FFFF00001h`
that isn't a typo this project introduced. The classification here
(`MultiplyCyclesSigned` in `cpu.cpp`) resolves it toward the simpler,
symmetric magnitude-banding reading and is not pinned to that exact byte -
it costs at most 3 cycles either way, for two specific `rs` values out of
four billion, and no primary source was found that resolves the overlap
itself.

### Phase 2: the branch/loop-overhead question, and a real bug found while answering it

The plan's numbered question: bug 42's own SQR loop, measured, gave 9 cycles
per iteration, while a naive flat-one-cycle count of the loop's instructions
(`SQR`, `CFC2`, a `nop`, an accumulate, a branch, its delay slot) gives 10 -
a one-cycle gap the plan flagged as possibly the branch costing less than
assumed, possibly something else, and explicitly not to be guessed at.

Reconstructing that exact loop shape in `cpu_test` (`sqrloop` group) and
measuring this core's own `cycles` counter directly - the same method bug 42
used, this time needing no internal instrumentation because the public test
harness already exposes `Cpu::context()->cycles` - answered it: **this core
already produces exactly 9.000 cycles per iteration**, matching bug 42's
hardware-recovered value precisely. The naive flat-count's extra cycle was
never real; it came from assuming every instruction in the loop, including
the taken branch, costs a flat 1, when two things were already true here
that the naive count didn't account for:

- The GTE busy-wait stall bug 42 built already absorbs `CFC2`'s cost into
  `SQR`'s 5-cycle window - `CFC2` right after `SQR` doesn't cost a separate
  1 cycle on top of a separate wait, it costs the wait *and then* 1, with no
  double-charge.
- A *taken* branch already cost 0 extra beyond its delay slot's own cycle in
  this codebase - `BEQ`/`BNE`/`BLEZ`/`BGTZ`/`BLTZ`/`BGEZ` never called
  `Tick()` themselves, only `Jump()` did, via the delay slot instruction's
  own normal cost. That happens to be exactly what independent sources
  describe for the R3000A (branch resolved in decode, one delay slot
  suffices, nothing extra to charge), and this measurement is now the
  direct confirmation for it, the way bug 42's table was for the GTE.

No branch-timing code changed as a result of this - the measurement
confirmed the existing behaviour rather than finding it wrong. `J`/`JAL`
still `Tick()` once for themselves *and* run the delay slot (2 cycles for
the pair, unlike every taken conditional branch and `JR`/`JALR`'s 1) - this
measurement doesn't reach jumps, so that asymmetry is left alone rather than
"fixed" on the strength of an inference from a different instruction; it's
recorded here as an open question for whoever next has a way to measure it.

### The bug that was findable in the same code reading

A *not-taken* conditional branch called no `Tick()` at all - 0 cycles,
instead of the uniform 1 every other non-branch instruction in this
interpreter charges. `Jump()` (which ticks, via the delay slot) only runs
when the branch is taken; the not-taken path fell through to nothing. The
delay-slot instruction's own execution was never in question - psx-spx: "the
instruction following the branch will always be executed", and it does,
picked up by the ordinary fetch loop either way - only the branch
instruction's own charge was silently zero. Fixed by adding `Tick()` on the
not-taken path of `BEQ`, `BNE`, `BLEZ`, `BGTZ`, `BLTZ`, `BGEZ`;
`BLTZAL`/`BGEZAL` inherit it through `BLTZ`/`BGEZ`.

This doesn't touch the SQR loop above (its branch is taken every iteration
but the last), but it under-counted every not-taken branch in every game,
silently, since this core's very first commit.

### Verification

New `cpu_test` groups: `muldelay` (11 checks - the magnitude-boundary cycle
costs for both `MULT` and `MULTU`, `DIV`/`DIVU`'s fixed 36, and a busy-window
stall check for each, in the same shape `gtedelay`'s SQR check already
established) and `sqrloop` (1 check, reproducing the bug 42 loop and
asserting exactly 9 cycles/iteration). `TestBranches` gained a per-case
cycle-cost check: 1 cycle for the branch+delay-slot pair when taken, 1 for
the branch alone when not, plus 1 more for the instruction after it.
`cpu_test`: 194 -> 239 checks, 0 failures. `gte_test` and all six other
harnesses unchanged (99, 107, 70, 28, 13, 59, 175 checks respectively - `0`
failures across the board).

`bios/SCPH1001.BIN --frames 400`: framebuffer checksum, non-black pixel
count, unimplemented-path count and CD-ROM command count are all identical
to before this change (`bd888bab645a63a9`, 305,920/305,920, 0, 3). What did
move, exactly as expected from correctly charging cycles that were
previously undercounted: raw instructions executed in the same 400 frames
dropped from 115,547,800 to 97,749,265, and RFEs/interrupts taken in that
same window rose slightly (881 -> 919 RFEs, 869 -> 907 interrupts) - more
accurate per-instruction timing means more real time (and so more timer/VBlank
activity) elapses per instruction executed, not a different boot path. The
picture on screen did not change at all.

Note for whoever runs these next: the baselines in
[Test-Suite.md](Test-Suite.md) predate several unrelated fixes already on
this branch (SPU, CD-ROM audio) and were already stale before this change -
`media_test` reports 175 checks here, not the 103 on record, and the
pre-existing (not newly introduced) `boot_runner` checksum was already
`bd888bab645a63a9`, not the doc's `d357591479cbd199`, before any of this
bug's changes landed. Worth a separate pass to refresh the whole document
rather than folding into this one.

## 44. Save states, and what the plan's own inventory missed

Implemented per [Save-States-Plan.md](Save-States-Plan.md): `StateIO` in
`psx/state.h`/`state.cpp`, a `Serialise(StateIO&)` on every component that
holds real machine state, `System::SaveState`/`LoadState`, and
`boot_runner --save-state`/`--load-state` plus F1-F8 (load) /
Shift+F1-F8 (save) slots in the Win32 front end.

**Three things the plan's own inventory table got wrong, all found by
reading the actual code rather than trusting the table:**

- **`CpuContext::cpr2[32]` is not GTE state.** The plan doesn't mention it
  either way, but it would have been an easy field to "helpfully" wire up to
  `Gte`'s registers on the assumption that's what it's for. It is zeroed once
  in the constructor and never read or written anywhere else in the tree -
  `Gte` keeps its own complete, separate register file. `cpr2` rides along in
  `CpuContext`'s otherwise-flat `Serialise` as 128 bytes of inert padding;
  special-casing it out would have been more code than the bytes are worth.
- **The MDEC has real state the table never lists.** A macroblock can be
  mid-decode - `state_`, partially-filled quantisation tables, `blocks_`,
  `coefficient_index_`, an output FIFO - none of it mentioned in "what a
  state has to contain". Skipping it would have meant a state taken mid-FMV
  silently corrupted or hung decode on load, the exact failure mode the
  plan's own opening line ("everything the machine can be asked about that
  is not derivable") says a state must not have. Added `Mdec::Serialise`.
- **The CD-ROM's `pending_` deque already stores a relative delay.** The plan
  specifically warns "must be saved as such" as if this needed converting;
  reading `cdrom.cpp` shows `PendingResponse::delay` is already a countdown
  decremented every tick, not an absolute cycle timestamp, so saving it
  mid-countdown is already correct with no conversion - the warning was
  right to raise the question, and the answer was "already fine."

**One design gap the plan left for whoever implemented it:** a component's
`Serialise` is asked to "never fail" - it just moves bytes - but `Cdrom`
reopening its `Disc` from a saved path is a real operation that can fail
(the image moved), and the plan requires that be refused with a reason, not
silently ignored. `StateIO` gained a small `SetError`/`error()` pair for
exactly this one case: every `Serialise` still returns `void` and always
runs to completion, but a load-time side effect that fails records why, and
`System::LoadState` reports the first reason rather than whatever broke
downstream because of it.

**The front end's own keybinding needed a decision the plan didn't make.**
"Numbered slots on F1-F8 with F5/F9 for quick save and load" is
self-contradicting: F5 is both the fifth numbered slot and a distinct
"quick" slot under that reading. Resolved as plain F1-F8 = load slot N,
Shift+F1-F8 = save slot N, with no separate quick slot - F1 already gives
one-key access to *a* slot, which is what "quick" was asking for.

### Verification

Exactly the plan's own "How to know it works", run against
`bios/SCPH1001.BIN`, both with and without a disc mounted (`media_test`'s
`make_test_disc` output, to specifically exercise `Cdrom`/`Disc` reopening -
the part the plan itself flags as most likely to silently diverge):

- **900 straight frames vs. 600 frames + save + (separately) load + 300
  more**: framebuffer checksums identical (`6a4dca42586b6a5a` with no disc,
  `ab719c80299ee383` with one), non-black pixel counts identical, and the
  two PPM files byte-identical - not just checksum-equal.
- **Load, then immediately re-save**: the two state files are byte-identical.
- **A state loaded against a different BIOS** (`SCPH1000.BIN` against a
  state made with `SCPH1001.BIN`): refused - `save state was made with a
  different BIOS`.
- **A state whose disc image was moved after saving**: refused - `save
  state: disc image not found: <path>`.
- All eight existing harnesses unchanged (`cpu_test` 239, `gte_test` 99,
  `spu_test` 107, `timer_test` 70, `sio_test` 28, `gpu_test` 13, `mdec_test`
  59, `media_test` 175 - 0 failures throughout), and `bios/SCPH1001.BIN
  --frames 400`'s framebuffer checksum unchanged (`bd888bab645a63a9`) -
  `Serialise` is new code, only ever called from `SaveState`/`LoadState`,
  never from the hot path, so normal execution has nothing to move.

Not attempted here: `Docs/Save-States-Plan.md` names no version-migration
path, and none was built - a state file's version is checked and a mismatch
is refused outright, per the plan's own "cheap now, impossible to retrofit"
framing rather than an oversight.

## 45. A polyline's terminator word was drawn as its own vertex

Reported from a screenshot: a game's menu, drawn with line-strip rectangle
borders, showed extra pink lines fanning out from the top-left corner of the
screen to the corner of every box on screen - one spurious segment per
polyline.

### The bug

`Gpu::WriteData` collects a variable-length polyline (`CommandLength`
returns -1 for one) word by word until it sees the terminator - any word
matching `(data & 0xF000F000) == 0x50005000`, which is how real hardware
recognises it too, not just the single value `0x55555555`:

```cpp
} else if (fifo_needed_ < 0) {
  if ((data & 0xF000F000) == 0x50005000) {
    fifo_needed_ = fifo_count_;
  } else {
    ...
    return;
  }
}
// falls through into the shared tail below
if (fifo_count_ < ...) fifo_[fifo_count_++] = data;
if (fifo_count_ >= fifo_needed_) { ExecuteCommand(); ... }
```

Recognising the terminator only *froze* `fifo_needed_` at the word count
collected so far - it did not `return`, so the terminator word itself fell
through into the shared tail and was appended to the fifo like one more
vertex, which immediately satisfied the just-frozen `fifo_needed_` and
dispatched. `Gpu::CmdLine` then decoded that extra word as a real point:
`X = data & 0x7FF`, `Y = (data >> 16) & 0x7FF`. For the terminator value
most software actually sends, `0x50005000`, both fields mask to exactly
zero - decoding to screen position (0,0). Every polyline the game drew grew
an uninvited last segment from its real final point straight to the corner
of the screen.

### The fix

The terminator carries no vertex data and must never reach the fifo. When
it's recognised, dispatch immediately with what has already been collected
and `return` - the same shape the "real data" branch two lines below it
already used, just missing on this path.

### Verification

New `gpu_test` check, `TestPolylineTerminatorIsNotAVertex`: draws a two-point
polyline nowhere near the origin, opens the drawing area to cover both the
line and (0,0) (GP1(00h) resets it to a single pixel, which would have
clipped the bug's own line out and passed for the wrong reason), then reads
VRAM back. `gpu_test`: 13 -> 16 checks, 0 failures. All seven other
harnesses unchanged, and `bios/SCPH1001.BIN --frames 400`'s checksum
unchanged too - the BIOS shell draws no polylines, so this bug had nowhere
to move that number; it only ever showed up in a game or menu that actually
uses the primitive, which is exactly why it survived every check run so far.

## 46. A driver that polled instead of using the interrupt never saw the pad answer, and then didn't believe it once it could

**Symptom.** Reported as "input not working" in Ace Combat 3. Worked through
in [Ace-Combat-3-Input-Plan.md](Ace-Combat-3-Input-Plan.md): the disc boots
and renders correctly, `--press` itself was validated against the BIOS
shell, but on the title screen every one of 1402 traced pad exchanges
aborted right after the address byte - never once sending the actual poll
command - regardless of whether Start was held. `I_MASK` never enabled
SIO0, so this is a custom driver that polls the status register directly
rather than waiting on the interrupt the BIOS's own pad library uses, and
the plan doc left it there as a localised but unverified hypothesis.

**Cause.** `SIO0_STAT` bit 7 (DSR / `/ACK`) was modelled as a flag: set the
instant a device acknowledges, and cleared only by software writing the
acknowledge bit of `SIO0_CTRL` - which this driver, never touching the
interrupt path at all, had no reason to ever do. On real hardware bit 7
reflects the *live level* of the `/ACK` line: the device pulls it low for
"circa 100 clock cycles" and releases it back to high entirely on its own -
psx-spx even calls out that software "must first wait until SIO0_STAT.7=0"
before an acknowledge write does anything, because clearing it directly
isn't a thing real hardware lets software do. A driver that polls for that
release, rather than only ever reacting to the interrupt, would poll this
core's bit 7 forever and never see one - which is exactly the shape of the
abort that was traced: every exchange stalling a few hundred cycles after
the address byte, comfortably past this core's own acknowledge delay.

**Fix.** `Sio` now arms a countdown (`ack_pulse_timer_`, `kAckPulseCycles =
100`) whenever it sets the acknowledge bit, and `Sio::Tick` clears the bit
on its own once that countdown reaches zero - independent of whether
software ever writes the control register's acknowledge bit at all. The
software acknowledge write itself now only clears the latched interrupt-
request bit (9), matching the primary source: bit 7 was never software's to
clear in the first place.

**Result, first pass.** Re-traced the same exchange the plan doc captured:
the driver now completes full command exchanges from the very start of the
run - 0x42 polls, 0x43 (enter/exit configuration mode) and 0x45 (status
query) all run to their natural end and stop cleanly, rather than aborting
after the address byte. With Start held, the poll reply's button-low byte
reads `0xF7` - exactly `~0x0008` - so the actual press was reaching the
driver correctly.

That looked like the whole fix, but it wasn't: with Start held anywhere on
the title screen, the game still went on to exactly the same next screen at
exactly the same frame with exactly the same checksum as never pressing it
at all - true whether the press was 60 frames or the full ~540-frame title-
screen window, and true again on the title screen's second loop after that
first sequence finished and looped back. Communication had improved, but
the game still wasn't acting on it.

**Second cause.** The driver's repeated 0x43 exchanges are not it failing to
leave configuration mode - psx-spx documents that as the correct way to
avoid config mode's own watchdog reset ("be sure to keep issuing joypad
reads even when not needing user input"), and it always sent the "stay in
configuration mode" byte, exactly as that section describes. The bug was
this core's own reply *while* in that state. psx-spx, in the section this
project's earlier survey had only seen the title of: "while in config mode,
the ID bytes are always F3h 5Ah" and command 42h there "same as command 42h
in normal mode, but with **forced analog response** ... even in Digital
Mode". `PadIdByte` computed the low nibble from `analog_mode` even inside
config mode, replying `F1h` instead of `F3h`, and `total_length` only forced
the eight-byte shape for commands other than 0x42 - so a config-mode poll
from a still-digital pad got the ordinary four-byte reply and stopped two
bytes short of what real hardware sends. A driver checking either of those
against the documented fixed shape before trusting the payload would never
have trusted the button bytes at all - they were numerically right and it
still didn't matter.

**Second fix.** `Sio::PadIdByte` returns `0xF3` unconditionally while
`pad.config_mode` is set, instead of deriving the low nibble from
`analog_mode`. `ExchangeController`'s `total_length` forces the eight-byte
shape whenever `pad.config_mode` is set, not only when `analog_mode` is.

**Result.** With Start held on the title screen, the game now takes a
visibly different path than not pressing it: instead of continuing into the
CD-streamed sequence, it cuts to Ace Combat 3's own main menu - "Select game
mode." / NEW GAME / LOAD / RE-OPEN, with a LOG-IN prompt - confirmed by eye
from a `--ppm` capture, not just a changed checksum. `sio_test` gained a
direct check for the acknowledge pulse itself (`TestAckPulseSelfReleases`,
ticking the port past the pulse width with no control-register write in
between): 28 -> 31 checks, and every existing DualShock-handshake check in
that harness - which exercises config mode and analog mode directly - still
passes, since neither the fixed ID byte nor the forced length change
anything about a pad that was never asked into config mode in the first
place. All eight harnesses stay at 0 failures, and
`bios/SCPH1001.BIN --frames 400`'s checksum is unchanged - the BIOS shell's
own pad driver goes through interrupts and never enters config mode, so it
had nowhere to move either time.

## 47. The presenter let the framebuffer's own width:height ratio decide the aspect

**Symptom.** Reported directly: in Ace Combat 3, the screens from the "Now
loading." bar through the main menu found while chasing bug 46 render
visibly narrower than normal gameplay - "the width is shrinked" - while
everything else in the game fills the window the way it should.

**Cause.** `D3D11Presenter::Present` computed the letterbox rectangle from
`width` and `height` - the framebuffer's own pixel dimensions - straight
off: `target_aspect = width / height`. Those two numbers are not one
setting, though - horizontal resolution (`GP1(08h)`, 256/320/368/512/640)
and the vertical display range plus interlace bit are independent
registers, and both are sampling the *same* fixed, roughly 4:3 physical
frame real hardware always drives. A game asking for fewer horizontal
samples is asking for coarser detail, not a narrower screen, and a
non-interlaced 240-line frame is not a shorter screen than an interlaced
480-line one - it is the same physical height, drawn once instead of twice.
The menu found while verifying bug 46 does exactly what a 2D menu commonly
does: pair a lower horizontal sample rate with the full interlaced range for
crisp text without the fill cost of 640 columns, reporting a `320x480`
framebuffer. Fed through the old formula that is `2:3` - a portrait
rectangle - so the presenter letterboxed it far narrower than the `640x480`
(or `320x240`, `4:3` either way) frames on either side of it in the same
boot, even though a real TV shows every one of them at the same width.

**Fix.** The letterbox rectangle is now computed from a fixed `4:3` target,
not from the frame's own pixel counts - `ComputeLetterboxRect`, extracted
into `PSXEmu.Core/tools/letterbox.h` specifically so it takes no window
handle and no graphics device, the same reason `boot_runner`'s own helpers
live there. `D3D11Presenter::Present` calls it with `4.0f / 3.0f`;
`width`/`height` are still used for the texture upload, just no longer for
the aspect.

**Result.** New `letterbox_test` harness, headless (no device, no window):
a `4:3` window is filled exactly with no bars; a wide window pillarboxes to
a fixed `4:3` rectangle rather than following the frame's own ratio; a tall
window letterboxes the same way. 12 checks, 0 failures. `build_tools.bat`
builds it alongside the other harnesses now: nine headless executables, not
eight. All other harnesses unchanged at 0 failures, and
`bios/SCPH1001.BIN --frames 400`'s checksum is untouched - this is a
presentation-only change in `PSXEmu.Win32`, nothing `boot_runner` or any
`PSXEmu.Core` harness can see.

What this project's own conventions could not do here is put the fixed
picture in front of a person: this machine has no usable Direct3D device to
present to (`D3D11CreateDeviceAndSwapChain` either fails outright or
produces a swap chain nothing ever composites), reproduced even for a
BIOS-only boot with no disc at all, which renders correctly in well under a
second through `boot_runner --ppm`. The fix is verified by the arithmetic
that was actually wrong - `320/480` letterboxed as `2:3` before, exactly
`4:3` after, checked directly - not by a screenshot of the running game.
That is a real gap, not a formality skipped: someone with a working
interactive build should confirm the menu fills the window the same way the
title screen either side of it does before calling this closed.

---

## 48. The drive teleports, so the boot logo screen is half as long as a console's

**Not a bug - a deliberate simplification, now measured and made optional.**
Reported as the intro "quickly disappearing" compared with a real PlayStation.

**What is actually on screen.** Measured frame by frame with
`boot_runner --frame-log 1`, Air Combat on SCPH1001, at the 59.29 Hz the GPU
actually runs at:

| frames | seconds | screen |
|---|---|---|
| 1-119 | 0 - 2.0 | black; display off, then on but empty |
| 120-524 | 2.0 - 8.8 | the SONY COMPUTER ENTERTAINMENT diamond, grey ground |
| 525-694 | 8.8 - 11.7 | "PlayStation / Licensed by SCEA", black ground |
| 695+ | | the game |

Two facts narrow it to one screen. A **no-disc run is identical through frame
524** - same checksums, same frame numbers - so everything up to the diamond
fading out is BIOS animation with no disc in it at all. And the BIOS console
has **no `VSync: timeout` lines**, so the vblank-paced waits are not returning
early the way they did in bug 16. The software-paced part of the intro is
already the right length.

That leaves the logo screen, which is the only one whose length is set by
anything other than a frame counter: it is up for exactly as long as the drive
takes to spin up, seek, read SYSTEM.CNF and load the executable.

**What the drive costs.** For the whole BIOS boot of Legend of Mana - 8
`Setloc`, 8 `SeekL`, 8 `ReadN`, 8 `Pause`, 2 `Init`, 3 `GetID`, 107 sectors:

- `kSeekDelay` is flat 400,000 cycles (11.8 ms) whatever the distance. The
  boot makes two full-stroke jumps, lba 173 to 45173 and back; each is charged
  the same as a one-sector nudge.
- `ReadN` after a `Setloc` teleports - `read_lba_ = seek_lba_`, first sector
  one sector time later. Most of the boot's repositioning is these implicit
  seeks and they cost **nothing at all**.
- The motor is on from `Cdrom::Initialize`. No spin-up, no focus, no TOC read.
- No rotational latency anywhere.

Total: **~0.8 s of drive time for the whole boot**, and 0.71 s of that is just
the 107 sectors streaming at 2x.

**The setting.** `EmuConfig::cdrom_mechanical_timing`, off by default,
`cdrom_mechanical_timing` in the settings file, Emulation > CD-ROM Mechanical
Timing in the front end, `--cd-mechanical` in `boot_runner`. On, the drive is
charged 1 s to spin up from a standstill, 20 ms to move and settle plus 28
cycles per sector of distance (a full stroke across a 74-minute disc lands near
300 ms), and one sector time of rotational latency on the first sector of a
read. Off, every one of those returns the flat value it always did.

With it on, Air Combat's logo screen goes from 170 frames to 327 - **2.9 s to
5.5 s** - and the boot reaches the game at frame 920 rather than 695.

**Two things measured rather than assumed:**

**Init must keep its flat delay.** Stretching its second response to the 0.12 s
it is often quoted as breaks the boot outright: the bootstrap loader's CdInit
gives up waiting, retries `Init` eight times and never reaches SYSTEM.CNF -
`BOOTSTRAP LOADER` is the last line on the BIOS console and the screen sits on
the logo for ever. It is out of scope anyway; Init is firmware waiting, not a
head moving. Only the spin-up is added to it.

**A read that has not moved must not be charged a seek.** `Play` with no track
argument means "carry on from here", and the BIOS CD player sends one every
frame for the whole of a track. Charging each a settling time would stutter CD
audio 60 times a second for a head that never moved. `FirstSectorCycles`
returns just the sector time when `from == to`, which is also most ordinary
reads - the ones that continue where the last stopped. Verified: 2,072 CD-DA
sectors over 27.8 s of playback with the setting on, against 1,794 over 23.9 s
with it off. Both are exactly 75 sectors a second.

**Regression check.** With the setting off: BIOS boot at 400 frames still
97,749,265 instructions and `bd888bab645a63a9`; Air Combat at 900 frames still
240,999,718 instructions, `aedac3154f8a0383`, 237 CD commands, 623 sectors,
2,225 interrupts - every figure unchanged to the digit. All harnesses pass (860
checks, 0 failures). Save states round-trip byte-identically in both modes, on
both the BIOS-only and the disc path; `kStateVersion` is 3, because `spun_up_`
is now in the drive's `Serialise`.

---

## 49. The front end never paced the machine, so it ran at the speed of the monitor

**This is the one that actually made the intro short.** Bug 48 above is real
and worth having, but it was found by measuring emulated *frames* and it could
not have found this: it never asked how long a frame takes on a wall clock.

**Symptom.** The BIOS intro goes past far faster than a console's. Unchanged by
bug 48's fix, which is what said the diagnosis was incomplete.

**Cause.** `RunOneFrame` runs the machine until the GPU finishes one frame and
returns. The main loop then presents and goes round again. **Nothing anywhere
in it waits.** Its comment claimed the pace was "tied to the emulated display
rather than to a timer here", but running one emulated frame per iteration
paces nothing at all - it only decides how much work each iteration does, not
when the next one starts.

So the rate came from whatever happened to block first:

- **`Present(1, 0)`.** `vsync_` defaults true in both the D3D11 and D3D12
  backends, so the loop is capped at the *monitor's* refresh rate. This
  machine's display runs at **165 Hz**, against an emulated display producing
  59.29. That is **2.78x real speed**.
- **`QueueAudio`.** Both audio engines block when the device's buffer is full.
  The SPU produces 744 audio frames per emulated video frame, and a device
  consuming 44,100 a second will therefore only take 59.3 of them a second - so
  with a working audio device this accidentally paces the machine to very
  nearly the right rate. That is luck, not design. It does nothing when
  `CreateAudioEngine` returned null, and it is why the same build can run at
  the right speed on one machine and at 2.8x on another.

Neither is the machine's clock, and neither belongs in charge of it.

**What it cost.** Every emulated-frame figure in bug 48 was being realised at
165 Hz, so what was actually on screen was:

| | before both fixes | bug 48 alone | with the limiter |
|---|---|---|---|
| logo screen | 1.0 s | 2.0 s | **5.5 s** |
| power-on to game | 4.2 s | 5.6 s | **15.5 s** |

The intro is also the *worst* case, which is why it was the visible symptom: it
is light enough to hit the 165 Hz cap, while a heavy game scene is limited by
the host CPU and lands much nearer 1x. "Only the intro is fast" was the clue.

**Fix.** `platform/frame_limiter.h` - a wall-clock deadline per frame at
`Gpu::refresh_hz()`, which is 59.29 in NTSC and 49.76 in PAL, derived from the
same GPU clock and scanline count the rest of the timing uses rather than
assumed to be 60. Called last in the loop so it absorbs whatever the rest of
the iteration did not take, and composes with the two accidental brakes instead
of fighting them. A host that falls more than four frames behind gets a fresh
deadline rather than sprinting to catch up.

In Core, not the front end, because how fast the machine should run is a
property of the machine - standards section 1.

**And a way to see it.** The window title now carries `59.3 fps (100%)`,
updated once a second: emulated frames per second of wall clock, against what
the emulated display is producing them at. Docs/Gaps.md had listed the absence
of a speed display and said it mattered more than it sounded. It did: at 280%
an intro just looks like a short intro, and there was no number to tell a fix
from a placebo.

**And a switch.** `EmuConfig::frame_limiter`, `frame_limiter` in the settings
file, Emulation > Frame Limiter in the menu. **On by default**, unlike
`cdrom_mechanical_timing` above: that one is a choice between two defensible
models of a drive, this one is the difference between running at a defined
speed and running at whatever the host happens to allow, and only one of those
is a PlayStation. A settings file written before the key existed leaves it on,
which is checked rather than assumed.

Off restores the pre-fix behaviour exactly - paced by vsync or the sound
device - which is worth having for getting through a long load or an
unskippable intro, and for reading the host's real headroom off the title bar,
a number that is pinned at 100% while the limiter is doing its job.

**What could not be checked here.** The front end does not reach its main loop
in this session at all - the process starts, creates its window, and burns no
further CPU, which is bug 47's "no usable Direct3D device on this machine"
again. So the limiter is verified by `frame_limiter_test` (six checks: 59.02
fps measured against a 59.29 target, PAL at 49.66, and a slow host running slow
rather than catching up) and by the arithmetic above, **not** by watching the
intro. Someone with a working interactive build should confirm the title reads
about 100% before calling this closed.

**Regression check.** All harnesses pass (866 checks, 0 failures, including the
6 new ones). Both boot_runner baselines unmoved: BIOS boot 400 frames still
97,749,265 instructions and `bd888bab645a63a9`, Air Combat 900 frames still
240,999,718 and `aedac3154f8a0383`. Nothing about the core's own timing
changed - only how often the front end asks it to advance.

---

## 50. The display width was the resolution GP1(08) asked for, never the window GP1(06) opened

**Symptom.** Reported directly, from Metal Gear Solid (`SLES-01370`, disc 1)
at save state 1 - the codec screen: the picture is right, but a strip down the
right-hand side is garbage, and it changes every frame. "I think the screen
width is wider than it should be, so showing garbage vram on the right."

**Cause.** `Gpu::UpdateDisplaySize` took the horizontal resolution field of
`GP1(08h)` - 256/320/368/512/640 - as the width of the picture, full stop.
It is not. That field sets the *dot clock*: how many GPU clocks one pixel
takes on its way out (10, 8, 7, 5 or 4 of them). How many pixels get out is
`GP1(06h)`'s business - the beam is only on between X1 and X2, so the visible
width is that window divided by the dot clock. The height was already read
this way, off `GP1(07h)`'s scanline range; the width was the one half that was
assumed.

The two agree for every game that leaves the window at the standard 512..3072
GPU clocks, which is 2560 clocks and divides into exactly 256, 320, 512 and
640 - which is why this survived every boot measured so far, Metal Gear
Solid's own gameplay included (`GPUSTAT=d4122200`, window 624..3184, 2560/8 =
320, the nominal width to the pixel).

The codec screen is the case where they part. It switches to the 368-pixel
mode - `GPUSTAT=d4112200`, bit 16 set, so a dot is 7 GPU clocks - and opens a
window of only 742..2968, which is 2226 clocks, or **318** pixels. Its
framebuffers are 320 apart in VRAM, at x=0 and x=320, flipped every frame.
Reading 368 columns from each of those in turn is what produced the strip and
what made it move: from the buffer at x=0 the 50 extra columns are the left
edge of the other buffer - Snake's portrait, a second time - and from the
buffer at x=320 they are past the framebuffers entirely, in the texture and
CLUT area, which resolves as coloured noise.

**Fix.** `UpdateDisplaySize` now computes the visible width as
`(horizontal_display_end_ - horizontal_display_start_) / dot_clock_divider()`
and uses it in place of the mode width. Two guards, both deliberate: a window
*wider* than the mode does not widen the frame - that is overscan a TV paints
off its own edge, and following it would mean sampling past the framebuffer
for exactly the reason above - and an inverted or empty window falls back to
the mode width rather than producing a zero-width frame nothing can present.

Nothing in the front end needed changing: `ComputeLetterboxRect` has targeted
a fixed 4:3 since bug 47 rather than the frame's own ratio, so a 318-wide
frame fills the same rectangle a 368-wide one did, and both presenters already
recreate their upload texture when the frame's dimensions change.

**Result.** The codec screen renders clean on both buffers - frame 61 (the
buffer at x=320, the coloured-noise one) and frame 140 (the buffer at x=0, the
duplicated-portrait one) checked as PNGs either side of the fix.

**Regression check.** By construction this can only ever narrow a frame, never
widen one, and only when a game narrows its own window - and measured that
way too. Metal Gear Solid's own boot is untouched at frames 900, 1800 and
3000: `640x240` with window 640..3200 (2560/4 = 640) and `320x256` with window
624..3184, every checksum identical to a baseline `boot_runner` built from
`HEAD`. Both documented baselines unmoved: BIOS boot 400 frames still
97,749,265 instructions and `bd888bab645a63a9`, Air Combat 900 frames still
`aedac3154f8a0383`. `gpu_test` grows a case that pins the rule in both
directions - each of the five modes still produces its nominal width from the
standard window, 368 mode produces 365 from it (2560/7 does not divide
evenly, and the beam cannot paint the 366th pixel), the codec screen's own
registers produce 318, and the two guards hold - 31 checks, 0 failures, up
from 23. `timer_test` unchanged at 70, and the solution builds.

`boot_runner` prints the CRTC line the diagnosis needed and did not have:
`crtc  hdisp 742-2968 gpu clocks, GPUSTAT=d4112200`. The display window was
serialised into save states and reachable from nowhere else, so "how wide does
this game actually think its screen is" had no answer short of a debugger.

**What could not be checked here.** The live front end, as in bugs 47 and 49:
`PSXEmu.Win32.exe` still never reaches its main loop on this machine. The fix
is verified through `boot_runner` renders of the same save state the report
came from, not by watching the codec screen in the running emulator.

## 51. GetlocL had a status byte in front of it, and Bomberman took a frame number for an open lid

**Symptom.** Reported directly: Bomberman Party Edition (`SLUS-01189`, a bare
`.bin`) stops at the Hudson logo and never goes on. Reproduced from a cold
`--disc` boot: from frame 1300 the picture is the logo, checksum
`dedbf5071e81061a`, for as long as the run lasts - 3000 frames of it, and
3000 more from the save state that came with the report. The game is not
hung. It draws the logo every frame, polls the pads, ticks its sound driver
and spends the rest of the frame in VSync. What never stops is the drive: a
`ReadS` of `XA/NORORG2.XA` streams straight on past the end of the file and
into `OPENING.STR`, and nothing ever pauses it. Not a disc-format problem
either - the ISO volume covers 280,940 of the image's 280,942 sectors, so a
bare `.bin` mounted as one data track is the right layout.

**Cause.** The game drives the CD through a small manager of its own, run
from a VSync callback, and the logo waits for that manager to go idle. After
starting the logo's jingle with `ReadS`, it asks `GetlocL` for the header of
the sector under the head. It takes the drive status out of every reply by
position - a table in the game names the byte for each command - and for
`GetlocL` the table says byte 3.

On hardware `GetlocL` is eight bytes: minute, second and frame in BCD, the
mode, then file, channel, submode and coding info, and no status at all.
Byte 3 is the mode, `02h`, which reads as a drive with its motor on - exactly
what the game expects. Ours put a status byte in front, the same mistake bug
35b fixed for `GetlocP`, so byte 3 was the *frame*. The read had started at
52:05:69, the sector under the head was 52:05:70, and frame `70h` has bit 4
set: the shell-open bit.

So the game decided the lid had been opened mid-jingle and ran its own
recovery - Getstat, wait for the motor, GetTN, then wait for the status to
read exactly `02h`, motor on and nothing else. After a real lid-open that is
what a drive reports, because opening the lid stops the read. Here nothing had
stopped it, the status stayed `22h`, and the manager waited for ever. The main
thread will not queue the `Pause` that ends the jingle until the manager is
idle, so that never came either.

Whether a boot hangs depends on the frame number at that instant - any BCD
frame with an odd tens digit has bit 4 set - but the emulator is deterministic,
so this boot hit it every time.

**How it was found.** Nothing in a trace of the hang itself looks wrong; it is
a healthy loop waiting for a condition. `--hot` put 85% of the time in VSync
with the logo drawn every frame. The trace showed a VSync callback sending
Getstat each frame and a completion handler comparing the answer with `02h`.
`--dis` against the save state gave the manager's whole state machine, and
`--watch-ram` did the rest: on its sub-state it showed the recovery beginning,
on its copy of the status it found the one bad value - `70h`, a status no
drive reports, seeking and reading with the motor off - and on its command byte
it gave the order: `Setloc`, `ReadS`, `GetlocL`, then the recovery's `GetTN`.

**Fix.** `GetlocL` answers with the eight bytes of header and subheader of the
last sector read, `sector_[12..19]`, and nothing in front. That also brings
back the coding-info byte, which the status had pushed off the end.

**Result.** From a cold boot the logo gives way at frame 1300 and the checksum
changes at every hundred-frame mark from there to 3000, by which point the
opening film is playing - 492 MDEC commands, 147,000 macroblocks, none of which
the hung boot ever reached. The drive does what a working session looks
like: 2 `ReadS`, 24 `Pause`, 30 `GetlocL`, and a single `GetTN` - the start-up
disc check, not a recovery. 625 XA sectors are decoded where there had been
none, so the jingle is heard now.

**Regression check.** `media_test` grows the case that pins it - Setloc and
ReadN, then GetlocL: eight bytes, minute, second, frame and mode in that order
- 206 checks, 0 failures, up from 200. Both documented baselines unmoved: BIOS
boot 400 frames still 97,749,265 instructions and `bd888bab645a63a9`, Air
Combat 900 frames still `aedac3154f8a0383`. Legend of Mana and Wild Arms at
1800 frames are byte-identical between `boot_runner` built from `HEAD` and
with the fix; neither sends `GetlocL` in that window, so neither could move.

**A save state made at the hang does not recover.** It holds the game already
inside its recovery with the read still running, and loaded into the fixed
build it sits on the logo exactly as before - 600 frames, same checksum. Boot
the disc again.

**What could not be checked here.** The live front end, as in bugs 47, 49 and
50: `PSXEmu.Win32.exe` still never reaches its main loop on this machine, so
this is verified through `boot_runner` cold boots of the same disc, not by
playing it.

## 52. A pad answered commands it did not understand with every button held

**Symptom.** Reported directly, as soon as bug 51 let Bomberman Party Edition
past its logo: "the input is whacky, the emulator itself is pressing keys."
Reproduced headlessly with the front end's own port setup - port 1 a digital
pad, port 2 empty or a DualShock. With nothing pressed, the game skips its
opening film by itself between frames 1300 and 1400, and by frame 3000 it is
on the title menu with the cursor moved down to BATTLE GAME. With a DualShock
in port 1 the same boot plays the film.

**Cause.** Two faults in how a pad answers a command, both in
`ExchangeController`, both producing the same bytes: a reply shaped like a poll
whose button bytes are zero. Buttons are active low, so zero is every button
held.

- A command the pad did not understand was acknowledged anyway and answered
  "in the shape of an ordinary poll", full of zeros. A plain digital pad
  (SCPH-1080) understands a poll and nothing else; on hardware - and in
  DuckStation and Mednafen - anything else ends the transfer at the command
  byte: the id goes out while the command comes in, and then no /ACK. Ours
  acknowledged 0x43 and 0x45 from a digital pad, and 0x44-0x4D from a
  DualShock outside configuration mode.
- 0x43 sent in normal mode answered zeros as well. psx-spx: there it answers
  with the same joypad data as 0x42; only inside configuration mode is its
  reply zeros.

Bomberman's pad driver is its own - it polls the status register rather than
use the BIOS's - and it tries to put the pad into configuration mode, taking
its buttons from whatever reply comes back. A digital pad never enters
configuration mode, so the driver kept trying, on a three-frame cycle - 0x43,
0x45, 0x42 - and two of every three answered `00 00`. The game saw the whole
pad held on two frames out of three.

A DualShock was not immune, only luckier: the game's start-up handshake sends
0x43 in normal mode four times (frames 731-775), and each of those used to
answer `00 00` too. Nothing on screen reacts that early, which is why a
DualShock boot looked right.

**Fix.** A pad acknowledges only what it understands (`PadUnderstands`): a
digital pad, 0x42; the DualShock line, 0x42 and 0x43 always and the
configuration set only inside configuration mode. Anything else gets the id and
no /ACK. 0x43 outside configuration mode answers with the pad's buttons (and
sticks, in analog mode) in a poll's own length, and it now switches
configuration mode on its last byte rather than its first, because the mode
decides that very transfer's length - the same point DuckStation applies it.
The one byte that has to be carried to that point reuses the exchange's
existing scratch byte, renamed `exchange_scratch_`, so the save-state layout
does not change.

**Result.** With the front end's setup the driver's 0x43 is refused at the
command byte, it concludes the pad is digital and polls with 0x42 from then
on, and the boot is byte-identical to a DualShock boot all the way to frame
3000 - film and all - whether port 2 is empty or holds a DualShock. The
DualShock boot's own handshake now answers 0x43 with `FF FF`, and that boot is
unchanged frame for frame.

**How it was found.** Not with `boot_runner` as it stands: it never sets a
port's controller type, so it always runs a DualShock in port 1 and nothing in
port 2, and that is exactly the setup where this bug does not show. A scratch
copy of the core, built with the port types taken from the environment and
every SIO transfer logged as sent and received bytes, put the three-frame
0x43/0x45/0x42 cycle on the first page of its output.

**Regression check.** `sio_test` grows three cases - a digital pad refuses 0x43
and 0x45 at the command byte and still polls in full; 0x43 in normal mode
carries the button actually held, enters configuration mode on its last byte
and leaves it again; a DualShock outside configuration mode refuses 0x44 and
0x45 and is left unchanged by them - 96 checks, 0 failures, every existing one
unchanged, bug 46's configuration-mode checks included. BIOS boot 400 frames
still 97,749,265 instructions and `bd888bab645a63a9`, Air Combat 900 frames
still `aedac3154f8a0383`. Legend of Mana and Wild Arms at 1800 frames are
byte-identical between `boot_runner` built from `HEAD` and with the fix, and so
is Ace Combat 3 - the game bug 46 made use configuration mode on a DualShock -
with Start pressed on its title screen: `--frames 2100 --press start@1900+60`
reaches bug 46's own "Select game mode." menu in both, and not pressing gives a
different frame. Bug 46's recorded press, `start@1755+60`, now ends before the
title screen accepts input, in both builds alike - the title's timing has moved
since then, not its input.

**What could not be checked here.** The live front end, as before: this is
verified through headless boots with the front end's port setup reproduced in a
scratch build, not by playing.

## 53. A multitap reading all four players ignored what the host said to each one

**Symptom.** Found while chasing "the multitap doesn't work" in Bomberman
Party Edition. With a multitap in either port, the game's pad driver takes
every player through the DualShock handshake, and it never got past the first
step: the SIO log shows 0x43 (enter configuration mode) going to player A,
then B, then A again, every other frame, for as long as the multitap was
plugged in.

**Cause.** In method 1 - one transfer reads all four players, after an id of
5A80h - each player's eight bytes are a whole command exchange with that
player's pad: the host puts a command and its parameters in them, and the
pad's answer comes back. `ExchangeMultitap` built every block as a poll reply
worked out on the spot and threw the host's bytes away, so 0x43 never reached
a player. DuckStation's `multitap.cpp` forwards each block to its controller
and hands the answers back one long transfer later - what it sends at each
position is what was stored there during the previous one. The game's driver
behaves as if written for exactly that: each query is followed by a poll that
collects its answer.

**Fix.** Each block is now forwarded to its player as a real exchange, and
the answers are kept in `Multitap::replies` for the next long transfer - all
`0xFF` until one has been asked anything. `ExchangeController` takes its
progress as a `PadExchange` (step, command, scratch byte, acknowledge) so a
multitap can run four of them inside one transfer; the bus keeps its own in
the registers it always had, through `ExchangeBusPad`, so a single pad's
save-state layout is unchanged. `Multitap::Serialise` gained the replies and
the block in progress, so `kStateVersion` is 6.

**Result.** The handshake completes: 0x43 in, 0xF3 back a transfer later, the
0x45/0x4C/0x47/0x46 queries answered with the right bytes, 0x43 out, and plain
polls from frame 772 on.

**It was not why the game took no input.** That was the port the multitap was
in, and bug 54. Tested the other way round, with the multitap in port 2, the
game decodes all four multitap players' buttons with this fix and without it.
What this changes is everything a game configures per player - analog mode,
the rumble mapping - none of which reached a multitap player before.

**Regression check.** `sio_test` 105 checks, 0 failures. The long-response
shape test now reads two long transfers - the first all `0xFF`, nothing asked
yet - and a new case sends player B 0x43 inside one, finds its answer in the
next, and finds player B in configuration mode in the one after. Bomberman
with the default pad at 3000 frames, BIOS boot 400 frames, Air Combat 900
frames and Ace Combat 3's Start press are all byte-identical to before.

## 54. Swapping a controller in the menu never let the port go empty, and Bomberman went on reading a pad as a multitap

**Symptom.** Reported with the multitap: "if i switch to multitap there is no
response, and even when i switch back to any other type of controller it
doesnt work, but if i close and open the emulator again, i can switch between
the different types of controllers no problem."

**Two causes, one of them the game's.**

*The multitap goes in port 2.* Bomberman Party Edition's five-player setup is
a pad in port 1 and a multitap with four more in port 2, and its pad decoder
(`800577D0`) only reads a multitap in port 2. Every frame it recomputes a
word of flags from the id in each port's latest reply - bit 0 for a multitap
(80h) in port 1, bit 1 for one in port 2 - and with bit 0 set it clears every
player's decoded input and skips decoding altogether. A multitap in port 1 is
not something that game understands, and while one is there it takes no input
from anything. With the multitap in port 2 all five players decode: `0400`
("connected, nothing held") for each, and L1 on the multitap players gives
`0404` in words one to four.

*The swap back.* Once the multitap had been in port 1, choosing a pad again
did not help: the flags word stayed at `FFFFFFFD`. The game's driver keeps its
own processed copy of each port's reply, and that copy's id byte stayed 80h -
the driver never re-read the port as a single pad, because nothing told it the
multitap had gone. The menu swapped the device between two frames. On a
console the port is empty while one controller is unplugged and the next
plugged in, and that is what the driver resets on: with the port empty for 30
frames in between, the flags word dropped back to `FFFFFFFC` and Start opened
the menu exactly as it does on a pad that was never swapped. One, three or ten
empty frames were not enough.

**Fix.** Choosing a different controller type in the Input menu leaves the
port empty for `kControllerReplugFrames` - 60 frames, about a second, roughly
what swapping one by hand takes - before the new controller is plugged in.
`App::SetControllerType` starts the countdown and `App::PollInput` holds the
port at `kNone` until it runs out. Only a change made in the menu does this; a
reset, a boot or a loaded state applies the configured type at once, as
before.

*Since 2026-09-18 both halves live on the machine's thread, in `App::ApplyInput`
(Docs/Threading-Plan.md): the countdown starts when that sees the configured
type differ from the one currently plugged in, rather than when the menu sets
it, so the thread that runs the machine owns the whole of it. Same 60 frames,
same behaviour.*

**How it was found.** With a scratch `boot_runner` that could change a port's
type on a schedule, take presses on either port and dump RAM at chosen frames.
Pressing L1 - which does nothing on the title screen - in two otherwise
identical runs and diffing the RAM isolates exactly the bytes a press touches:
with a pad, the driver's raw reply, its processed copy and the decoded words;
with a multitap in port 1, the raw reply and processed copy only.
`--watch-ram` on a decoded word found the decoder, and its disassembly the
flags word.

**What could not be checked here.** The front end, as ever: the menu path
itself has not been run. What was verified is the sequence it produces - a
multitap, then nothing for 60 frames, then a pad - driven through a scratch
`boot_runner`, where Start then opens the menu as it should.

## 55. A DMA flag outlived its enable, and two intro films went black on their first frame

`psx/dma.cpp`

**Symptom.** Captain Tsubasa J - Get in the Tomorrow `[SLPS-00310]`: "after
ps logo, no output just black screen". Cold boot, frame ~640: the display
goes to 256x240 and is disabled, MDEC decodes one burst and stops, and the
CPU ends in an endless bus-error loop at `81081084`. Air Combat
`[SLUS-00001]` showed the same thing at the same point - the open question in
[Air-Combat-FMV-Plan.md](Air-Combat-FMV-Plan.md).

**Cause.** DICR bit 31 was derived as the master enable AND a latched flag
*whose channel was also enabled*. The per-channel enables decide whether a
flag latches. They do not hide a flag that has already latched.
DuckStation's `UpdateMasterFlag` has the same rule. The chain in Captain
Tsubasa, traced with a scratch `boot_runner` logging every DICR write and
flag latch:

1. Frame ~380: the game's CD library puts a DMA handler (`80058628`) at the
   head of the kernel's interrupt chain. It claims every DMA interrupt,
   acknowledges only channel 3, and ends the chain with ReturnFromException.
   The libetc dispatcher that had been acknowledging GPU (channel 2)
   completions never runs again. A channel 2 flag stays latched and bit 31
   stays high, so there are no more rising edges.
2. Frame ~560: the BIOS loads the second executable. Every sector latches a
   channel 3 flag that nothing acknowledges.
3. The movie player's setup writes DICR `00900000` - master enable, channel 4
   only. Masked by the enables, bit 31 dropped to 0 and nothing fired. The
   stale flags survived.
4. The player installs its channel 3 callback (`8014D1FC`, "sector landed in
   the ring") and enables channel 3. The stale flag fires it before a single
   stream sector has been read. It marks ring slot 0 complete with an
   all-zero header.
5. `StGetNext` hands over that empty "frame". `DecDCTvlc` prints
   `MDEC_vlec: invalid VLC ID`, and `DecDCTin` sends a BCR of `00000020` -
   block count 0, which is 65536 blocks. `Dma0` pushes 2,097,152 words (all of
   RAM, four times) through the MDEC in one go. The garbage decodes, and
   channel 1 writes it from `801BEAD0` through the stack and around into
   kernel RAM.

**Fix.** Bit 31 is `bus error || (master enable && any flag)`. A flag latches
only when both its channel enable and the master enable are set, which is
DuckStation's `ShouldSetIRQFlag`. With the flags visible, the `00900000`
write in step 3 raises the interrupt, the player's dispatcher acknowledges
the stale flags (no callback is installed yet), and the stream starts clean.

**Verified.** Captain Tsubasa: the film plays (59,100 macroblocks) and the
title screen is up at frame 3000. Air Combat: the film decodes continuously
(243,400 macroblocks by frame 3000, display enabled). Air Combat's chain was
not traced step by step, but the same zero-block-count MDEC transfer shows in
the old run, and this change alone clears it. A/B over 3000 frames on the
same sources with and without the change: identical checksums at every
100-frame mark for the BIOS boot, Wild Arms, Wild Arms 2, Vandal Hearts,
Legend of Mana, Ridge Racer, Bomberman Party Edition, Area 51, Final Fantasy
VII and Ace Combat 3. Six of those move by a handful of instructions and at
most two interrupts. `cpu`, `gte`, `timer`, `sio`, `spu` and `mdec` tests
pass. `gpu_test` (shared edge column) and `media_test` (audio peak meter)
each have one failure, identical without this change.

**Still open.** `Dma0` still moves a block-count-0 transfer in one
synchronous burst. The hardware is request-paced. This change removes the
way these two games reached that path, not the path, so the next game to
decode a bad frame will wreck RAM the same way. Also, the Captain Tsubasa
`.bin` on the share has zeroed XA subheaders (the `.mdf` beside it does not),
so the film's audio sectors reach the CPU as data and the film plays silent
from that image.

## 56. An MDEC transfer moved every word inside the CHCR write, and a decode that ended short of a block was never handed over

`psx/dma.cpp`, `psx/mdec.cpp`

**Symptom.** None on its own - this is the crash *path* bug 55 left open. A
`DecDCTin` given an empty frame writes a BCR of `00000020`: 32-word blocks,
block count 0, which means 65,536 of them. `Dma0` looped over all 2,097,152
words before the CPU's next instruction, so the whole of RAM went through the
decoder four times over and channel 1 wrote what it decoded from `801BEAD0`
through the stack and round into kernel RAM.

**Cause.** Channel 0 was the last DMA path that still moved its data eagerly.
On hardware the MDEC takes a block only when it has room, and drains its input
at decode speed, so the CPU keeps running and the game's own next `DecDCTin` or
`DecDCTout` restarts the channel long before that much RAM is touched.

**Fix.** In request mode channel 0 moves one block, then waits: the block's bus
time (charged to the CPU, which the bus really does stop) plus 2,688 cycles for
each macroblock the block completed - DuckStation's `TICKS_PER_BLOCK * 6`,
which the CPU is *not* stopped for. `Dma::Tick` moves the next when that runs
out, MADR and BCR run down as they go the way channel 1's already did, and a
CHCR write that clears the start bit abandons the rest. Burst mode is
unchanged. `mdec_in_wait_` is the only new state, so `kStateVersion` is 7.

**A second bug, found by wiring up a test nobody called.**
`TestOutputDmaStartedFirst` went in with `535949b` (the Area 51 fix) and was
never added to `main()`. Called at last, its last two checks failed: a decode
that ends with less than a block of output - a monochrome one ending partway -
finished without telling channel 1, because `Mdec::WriteWord` only pokes it
from `EmitMacroblock`, and `HasBlockReady` refuses a short tail while the
command is still running. So the tail sat in the decoder and the transfer
waited for ever. `WriteWord` now pokes channel 1 again when a decode command
goes idle with output still in hand.

**Verified.** `mdec_test` is 85 checks, up from 60: the two DMA tests now run,
and a new `TestInputDmaIsPaced` checks that the CHCR write moves nothing
itself, that one tick moves one block, that the whole transfer still completes
given the time, that a zero block count moves a few thousand words in a
4,096-cycle tick rather than two million, and that stopping the channel
abandons it. Against the old `Dma0` that test fails five of its checks.

Test-Suite.md's save-state procedure passes on the new version, BIOS-only and
with a disc, plus a fourth case for this change specifically: a state taken at
frame 750 of Air Combat - mid-film, with channel 0 part way through feeding the
MDEC - resumed to frame 900 gives a byte-identical frame to a straight run.

A/B over 3,000 frames, HEAD against this change (with bugs 55 and 57): the BIOS
boot, Wild Arms 2, Ridge Racer and Final Fantasy VII are identical at every
100-frame mark; Wild Arms, Bomberman, Legend of Mana and Area 51 end on the
same checksum with a few frames of film shifted in between; Air Combat and
Captain Tsubasa J are bug 55's two fixes. Vandal Hearts and Ace Combat 3 end on
a different checksum, both mid-film, with the same CD sector count and within
1.5% of the same macroblock count - their films are a fraction of a frame out
of phase, which the final frames confirm: the same picture, one film frame
apart. That shift is the point of the change, not a side effect of it: a decode
that used to complete inside one CHCR write now finishes when the decoder would
have finished it.

**A methodology note that cost half an hour.** The first A/B run of this had
Vandal Hearts, Legend of Mana and Ridge Racer changing under a *binary that had
not changed*. Not non-determinism - three runs of one binary agree to the
instruction - but the network share the discs live on starving under eight
concurrent `boot_runner`s: the run stopped getting sectors at frame ~900 and
its log ended early. Keep the concurrency low, and compare the `cdrom ...
sectors` line between the two builds before believing any checksum.

## 57. A fixed-level volume was doubled and then halved again, so the whole mix came out at a quarter

`psx/spu.cpp`

**Symptom.** Quiet output, with nothing wrong relative to anything else - the
reason the front end shipped a 2x master gain and this document's own Gaps.md
claimed "a PlayStation is quiet by modern standards: the mix peaks at about a
fifth of full scale".

**Cause.** A sweep-capable volume register in fixed-level mode stores the level
*halved*: bits 14-0 hold `volume / 2`, -4000h..+3FFFh standing for
-100%..+100%. To use it the field is sign-extended and doubled into the
-8000h..+7FFEh range that every mix site multiplies by and shifts 15 back out
of. `Spu::VolumeOf` did the doubling and then shifted it straight back out:

```cpp
return static_cast<int16_t>(static_cast<int16_t>(sweep.reg << 1) >> 1);
```

The two cancel. A game asking for unity, 3FFFh, mixed at half - and both the
voice volume and the main volume go through this, so a full-scale game came out
at a quarter of amplitude, about -12 dB. `Spu::StepSweep`'s fixed-level branch
had the same expression. The CD and external *input* volumes are a different,
plain-signed format and were already right (bug 36).

**Why it survived.** `EmuConfig::audio_volume` defaulted to 2.0, applied after
the main volume inside `GenerateFrame`, which cancelled exactly one of the two
halvings. `spu_test` measured through that default, so its "a full CD volume of
7FFFh is audible" check saw 8000 for a tone of 8000 and read as correct: the
bug and the compensation agreed at the one point being measured.

**Fix.** `VolumeOf` and `StepSweep` return `(int16_t)(reg << 1)`, and
`audio_volume` defaults to 1.0 - the hardware's own level. **A `psxemu.ini`
that already says 2.0 keeps it and will be twice as loud as before**; the Audio
menu changes it.

**Verified.** `spu_test` sets `audio_volume` to 1.0 in its harness now, so it
measures the mixer rather than the front end's taste, and a new check reads the
absolute level: a CD tone of 8000 with both stages at unity comes out at 8000
(7,998), where before the fix it was 3,999. 108 checks, no failures. On real
discs, at `--volume 1` on both builds, the SPU's peak over 2,400 frames goes
7,114/5,803 to 28,461/23,222 - exactly the 4x of two stages - with no clipping
(28,461 of 32,767). FF7 and Wild Arms report the same peak because the loudest
thing in either run is the BIOS's own boot chime.

## 58. The CD audio peak meter's own test played silence, and blamed the meter

`tools/media_test.cpp`

**Symptom.** `media_test`: *Audio Peak Meter / Report peak is non-zero for loud
audio*, with `cdda_sectors=76 cdda_failures=0 audio_peak=0`. Sectors read, none
of them failing, and a peak of zero - which reads as a drive that plays audio
without measuring it.

**Not a regression, and not the drive.** The peak meter and this test went in
together with `209943e`, so it had never passed; `535949b` is green only
because neither existed yet. The meter works.

**Two things wrong with the test.**

*It played silence.* The image was written as 150 sectors of silence - an
imagined lead-in - followed by 76 loud ones, and then played from 00:02:00.
But an image file does not contain the lead-in: the cue sheet's
`INDEX 01 00:00:00` says its first sector *is* the first sector of track 1,
which the drive addresses as 00:02:00. So the loud audio actually sat at
00:04:00 and the drive dutifully played the silence in front of it. This is
the off-by-150 that `TestIsoImage` a few hundred lines above warns about in
as many words.

*It read the wrong side of the interface.* It sampled `Cdrom::audio_peak()`,
the register the drive accumulates into, after a fixed number of ticks. That
register is reset by every Report - "the peak since the last report" is what it
means - so what it holds depends on where between two reports the clock
stopped. The Report packet is what a CD player reads.

**Fix.** The test writes a track of one constant sample from its first sector,
plays it, and reads bytes 6 and 7 out of the first Report packet. It also plays
a second track at half scale: *"non-zero for loud audio"* passes on a meter
that latches any constant, and the point of a meter is that it follows the
level.

**Verified.** Full scale reports 32,767 and half scale 16,384. `media_test` is
246 checks, 0 failures - the first time it has been green since `535949b`.

## 59. The top-left rule had its vertical test inverted, and half-open raster loops turned that into a gap

`psx/gpu.cpp`

**Symptom.** `gpu_test`: *the shared edge column blended exactly once, not
twice: got 00000000 want 00000008*. Two semi-transparent quads sharing a
vertical edge, and the shared column came out **unblended** - not blended
twice, which is what that test was written for (Silent Hill's hatching), but
not drawn at all.

**Cause, in two halves that were each survivable alone.**

*The rule was upside down.* `RasterTriangle` normalises every triangle to a
positive signed area, y growing downwards. Work the edge function out for a
vertical edge under that winding: an edge running *up* the screen (`dy < 0`)
has the interior to its right - a left edge, which the top-left rule keeps -
and one running down (`dy > 0`) is a right edge, which it drops. `EdgeBias`
tested `dy > 0`, so it kept right edges and dropped left ones. Horizontal
edges were the right way round, which is why the quad-diagonal case the rule
was written for still worked: for the two triangles either side of a shared
edge the direction reverses, so exactly one of them claims it whichever way
the test points. All the inversion did on its own was hand a shared column to
the left-hand neighbour instead of the right-hand one.

*Then the loops became half-open.* The September GPU commits changed
`for (y = top; y <= bottom)` / `x <= right` to `<`. That is the right
convention - a quad from x=400 to x=416 covers sixteen columns, and
DuckStation's own rasterizer walks spans with the bound exclusive - but it
means the left-hand primitive can no longer paint its rightmost column at all.
With the inverted rule the right-hand primitive was refusing that same column
as "not a left edge", so between them nobody drew it.

Opaque draws were unaffected because the bias is gated on
`state.semi_transparent`, which is why only one check failed.

**Fix.** `EdgeBias` tests `dy < 0` for the vertical case. A left edge is kept,
a right edge dropped, which with the half-open loops gives each shared column
exactly one owner - the right-hand primitive, as on hardware.

**Verified.** `gpu_test` 31/31. Every harness is green: cpu 251, gte 99,
timer 70, sio 105, spu 108, gpu 31, mdec 85, media 246 - 995 checks, no
failures.

On discs, 3,000 frames each, ten of twelve are byte-identical at every
100-frame mark. The two that move are the two the rule can move, and both move
the right way:

- **Final Fantasy VII**, title screen: 478 pixels change, all of them inside
  exactly **two columns** (x=0 and x=255), each changed down 239 of its 240
  rows. 471 of them go from black to coloured and **none** go the other way -
  two full-height seams, filled in. The GP0 word count is identical, so this is
  the same drawing landing in pixels that were being left blank.
- **Ridge Racer**: three pixels, none of them black either before or after -
  ownership of a boundary shifting by a column on a few semi-transparent
  edges, which is what correcting the rule is *supposed* to do.

**Left alone deliberately.** The bias is still gated on semi-transparency.
That gate was bug-fix scaffolding from when the rule was inverted: applying an
upside-down rule to opaque draws gave the shared column to the wrong tile and
put seams through Wild Arms' overworld. With the polarity corrected and the
loops half-open the gate should be unnecessary - hardware's fill rule does not
know what blending is - but un-gating it changes which texel wins on every
adjacent opaque tile edge in the game that showed the seams, and that wants
checking against the game rather than against a unit test.

## 60. A half-open loop against an inclusive clip, and the screen lost its last column and row

`psx/gpu.cpp`

**Symptom.** None that anyone reported - it is one column and one row at the
edge of the picture. It was found by refreshing Test-Suite.md's baselines: the
BIOS boot drew 304,803 of 305,920 non-black pixels where the build from before
the September GPU commits drew all 305,920.

**Cause.** `RasterTriangle` clips a primitive's bounding box to the drawing
area, and the two are not the same kind of bound.

A primitive's extent is half-open: the BIOS's background is the quad
`(0,0)-(640,0)-(0,480)-(640,480)`, and 0 to 640 means columns 0 to 639 - 640 of
them, the width of the screen. The drawing area is inclusive: `GP0(E4)` states
the bottom-right *corner*, and the BIOS sets 639 x 479 for that same screen.
`Plot()` has always read it that way (`x > draw_area_right_` rejects, so 639 is
inside).

The September commits made the raster loops half-open - correct for the
primitive, and what DuckStation's own span walk does - but left the clip
expression as it was:

```cpp
const int32_t right = std::min(max_x, draw_area_right_);   // 639
for (int32_t x = left; x < right; ++x)                     // stops at 638
```

So the drawing area's last column and row were discarded whenever a primitive
reached them, which a full-screen background does every frame.

**Fix.** Take the primitive's last pixel and clip it inclusively:

```cpp
const int32_t right = std::min(max_x - 1, draw_area_right_);
for (int32_t x = left; x <= right; ++x)
```

**Verified.** The BIOS boot is back to 305,920 of 305,920. Diffed against
`535949b` pixel by pixel there are now **no** missing pixels at all - 131
interior pixels differ, none black in either build, which is bug 59's corrected
fill rule handing shared columns to the right-hand primitive as intended.
Diffed against the broken build, 1,117 pixels went black to coloured and none
the other way: exactly column x=639 (478 rows) plus row y=477 (639 more), which
is `width + height - 1`, and nothing else moved.

`gpu_test` stays 31/31 - the shared-edge tests are unaffected, since they turn
on which of two primitives owns a column rather than on where the clip ends.
All eight harnesses green, 1,000 checks. Across the twelve baseline discs only
**Ridge Racer** moved (frames 2000 and 3000): its primitives reach the drawing
area's edge where the other eleven do not.

**Worth remembering.** This is what a refreshed baseline is *for*. The bug was
in every frame of every game for four days, invisible in a checksum nobody had
a reference for, and it took comparing against a build from before the change
to see it at all.

## 61. DirectSound clicked, because taking out a blocking wait made a stale number load-bearing

`audio/dsoundaudioengine.cpp`

**Symptom.** With Settings > Audio > Output on DirectSound, a click every second
or two during play. WASAPI was clean.

**Cause.** Not one bug but two old ones, uncovered by a change that was
correct in itself.

`QueueAudio` used to block: it slept whenever the one-second secondary buffer
was nearly full. So the game ran with about 990 ms queued, and nothing about
DirectSound's accounting ever mattered with that much in hand. Making it
non-blocking - so a sound card a few milliseconds behind could no longer stall
the machine - handed the buffer's depth to the front end's rate control, which
aims for 25 ms. Two things that 990 ms had been hiding then came out:

1. **`GetQueuedSampleCount()` was a frame stale.** It returned `m_queuedBytes`
   as the last `QueueAudio` had left it, which does not subtract what played
   since - a comment above it called it "purely informational". The rate
   control reads it *before* queuing each frame, so it always saw about 16.7 ms
   more than was there, and held the real buffer near 8 ms while believing it
   held 25. WASAPI's version asks the device (`GetCurrentPadding`), which is why
   it never had the problem.
2. **An underrun on DirectSound is loud.** The secondary buffer loops, so when
   the play cursor overtakes the data it plays on into whatever the buffer held
   a second earlier - a burst of the wrong waveform, not a gap. And the resync
   jumped the write position by 4410 bytes, which is 1102.5 frames: half a frame
   off, so every sample after it had left and right swapped.

On top of both, the rate control can only trim by half a percent, so from an
empty buffer it takes about five seconds to build up to 25 ms - which DirectSound
started from at every launch, every unpause and every switch to it.

**Fix.** The count reads the play cursor, so it is live. `Play()` starts with
25 ms of silence already queued rather than none. Every write leaves 100 ms of
silence after the data, uncounted, so an underrun plays a gap rather than stale
sound. The resync lands on a whole frame, over that silence, and counts it.

**Verified**, by replaying `App::PumpAudio` against the real engine - a 59.94 Hz
loop off `steady_clock`, the real `SpeedResampler`, the rate control copied
exactly - and counting underruns, since a click cannot be heard from a test:

| | underruns in 10 s | queue held at |
|---|---|---|
| before | 6 | ~8 ms, reported as 27 |
| live count only | 2-5 | 18 ms |
| all three fixes | **0**, three runs | 24.6-24.9 ms |

and a minute of it: 0 underruns, 25.0 ms. The engine keeps an `underruns()`
counter now, since that is the number that says whether the output is healthy.

**Worth remembering.** A blocking wait is a buffer, and a large one. Taking it
out does not just change who waits - it changes how much slack every piece of
code downstream of it has been quietly relying on. The number that was "purely
informational" at 990 ms was the one steering the device at 25.

**Second round - the first fix was incomplete, and the check that passed it
was blind in the same place.** The user still heard clicking. The engine's own
underrun count said zero, because it only ever compared the data against the
*play* cursor. DirectSound has two, and measured on this machine the **write
cursor runs 30 ms ahead of the play cursor**, which moves in **20 ms steps**.
Everything between the two cursors has already been handed to the mixer, and a
sample written there is simply never played. Holding 25 ms ahead of the play
cursor therefore started every write about 5 ms *inside* that region - a loss on
every frame, with the play cursor never once overtaking the data, so nothing was
counted.

Fixed by measuring from the write cursor instead: `GetQueuedSampleCount` reports
only what lies beyond it, the underrun test is "the data no longer reaches past
the write cursor", and a resync lands a prime's worth beyond the write cursor
rather than the play cursor.

**And a second finding along the way.** On this machine `sleep_for(1ms)` takes
about 15.5 ms - nothing in the project raises the timer resolution, and even
`timeBeginPeriod(1)` did not change it in a console process - so the frame
limiter delivers frames anywhere from 0 to 30 ms apart rather than every 16.7.
A 25 ms queue can be drained by one late frame plus one 20 ms cursor step. So
the queue target is no longer one global constant: `IAudioEngine::
TargetQueuedSamples()` lets each output say what it needs - 25 ms for WASAPI,
50 ms for DirectSound, which puts DirectSound's total latency near 80 ms with
the write cursor's own lead on top.

**Verified**, paced by the real `FrameLimiter` rather than a tidy sleep, with the
engine's count now watching the write cursor: 20 s at the old 25 ms target and
20 s at the new 50 ms, **0 underruns** each.

**Not verified: the sound itself.** A loopback capture of the output was built
to count clicks independently of the engine, and it cannot be used here - the
tone sent peaks at 0.244 and the tone captured peaks at 0.326, so something in
this machine's audio path (a driver enhancement, most likely) reshapes the
signal after it leaves the process, and a slope detector reads that as clicks.
The ear that reported the bug is the one that has to confirm the fix.

**Worth remembering, twice over.** An instrument that shares the thing-under-
test's assumptions cannot find that assumption's bug - the underrun counter
lived in the same engine and looked at the same cursor. And a measurement of
the output is only independent if nothing between the two changes the signal.

*Superseded on 2026-09-18.* The machinery above - the guard silence, the prime,
the queue target per engine, the resync on write - is gone with the push model
itself. The audio thread pulls instead (Docs/Threading-Plan.md phase 3), so the
device asks for what it wants when it wants it and there is no queue for the
machine to guess at. What this bug taught is built into the engine rather than
bolted on: DirectSound writes past the *write* cursor, never the play cursor,
and keeps silence beyond its data so running out is a gap. Re-measured against
both real devices with the BIOS running: 0 frames short, 0 resyncs.

## 62. The frame limiter held the right average by delivering frames 0 to 30 ms apart

`platform/frame_limiter.h`

**Symptom.** Found while chasing bug 61, and part of it: audio fed in bursts
with gaps longer than the sound device's buffer. Also uneven frame pacing, and
a machine running very slightly slow - 58.75 fps against NTSC's 59.29.

**Cause.** The limiter slept off each frame with `sleep_for(1ms)` in a loop,
under a comment saying "Sleep's granularity is around a millisecond". It is
not, unless something raises the system timer resolution, and nothing in this
project does. Measured: `sleep_for(1ms)` took **15.5 ms** at the median, and
`timeBeginPeriod(1)` did not change it. One such sleep landing near the end of a
frame put that frame 13 ms late; the deadline arithmetic then ran the next one
immediately to catch up. Frames came out anywhere from 0 to 30 ms apart.

Every check passed, because every check measured the *average* rate - and the
overshoots and catch-ups very nearly cancel. The harness had four rate checks
and nothing about the spacing between two frames, which is the thing the audio
pump, once a frame, actually depends on.

**Fix.** A high-resolution waitable timer (`CREATE_WAITABLE_TIMER_HIGH_
RESOLUTION`, Windows 10 1803 and later), which wakes within about half a
millisecond without changing the timer resolution of the whole system the way
`timeBeginPeriod` would. The limiter sleeps until a millisecond before the
deadline and spins the last one. On an older Windows the flag is refused and it
falls back to the old sleep, keeping the average rate if not the spacing.

**Verified.** `frame_limiter_test` gained a spacing check, which failed against
the old limiter before the fix went in:

| | p5 | median | p95 | worst | NTSC rate |
|---|---|---|---|---|---|
| before | 14.8 ms | 15.9 ms | **29.5 ms** | 31.6 ms | 58.75 fps |
| after | 16.87 ms | 16.87 ms | 16.87 ms | 16.9 ms | **59.29 fps** |

against a period of 16.87 ms. The cost is the spin: 5% of one core, measured
over five seconds of an idle loop.

**Worth remembering.** A test of an average cannot see anything that averages
out, and "exactly on rate" was hiding frames arriving at twice the interval
followed by frames arriving at none.

## 63. Paused, or with a menu held open, DirectSound replayed the last second of sound on a loop

`PSXEmu.Win32/app.cpp`

**Symptom.** Found by reading the code, not reported: with DirectSound as the
output, pausing (Space, or Emulation > Pause), holding a menu open, or dragging
the window made the last second of sound repeat, after about a tenth of a
second of silence, for as long as it lasted. WASAPI went quiet instead.

**Cause.** Two things together. Pausing never stopped the sound device: the
paused branch of the loop only stopped feeding it, on the understanding - a
comment in `SetAudioBackend` said so - that an unfed device "plays as silence".
WASAPI's does. DirectSound's secondary buffer loops, and bug 61's guard puts
only 100 ms of silence after the data; past that, the play cursor runs on into
the rest of the one-second ring, which still holds the audio from a lap ago,
and round again.

The other half is the single thread. A menu, a drag or resize of the window,
and a dialog each run a modal loop of Windows' own inside a message the main
loop dispatched, and the loop gets no control back until it ends - so each of
those was a pause nobody asked for, with the same result.

**Fix.** The device plays only while the loop is running frames.
`App::EnterStall` stops it, and is called from the paused branch and from
`WM_ENTERMENULOOP`, `WM_ENTERSIZEMOVE` and `WM_ENTERIDLE` (sent to the owner
while a dialog or message box sits idle). `App::LeaveStall` starts it again at
the top of the next frame and resets the frame limiter, the resampler, the
pending audio and the speed readout, none of which should count the gap. The
device is now opened stopped and started by the first frame, and switching
backend leaves starting the new one to the loop as well.

A dialog opened from a menu command needs nothing extra: the menu has already
stalled the loop, and nothing restarts it until the frame after the command -
dialog included - has returned.

**Verified.** Against the real devices, with a probe that makes the loop's own
calls in the same order. Before, on DirectSound: 0.1 s of silence, then the
last second of sound on repeat, still going when the measurement stopped at
5 s - 86% of the ring was old audio. After:

| | during a stall | after it |
|---|---|---|
| DirectSound | stopped, play cursor frozen over 1.5 s | ring 0% old audio; no underruns in the next 2 s, nor across 20 stalls of 100-400 ms |
| WASAPI | stopped, 1,688 samples held over 1 s | lowest queue in the next 2 s: 1,252 samples - never dry |

The window messages themselves were not exercised: the front end cannot be run
from an agent session. What is verified is the device half. The one thing to
check by hand is a menu held open over a game with DirectSound selected.

**Worth remembering.** "An unfed device plays silence" is true of WASAPI, not
of sound devices in general: a looping buffer plays whatever it was last given.
And on a single-threaded front end, every menu is a pause.

*Superseded on 2026-09-18.* `EnterStall` and `LeaveStall` are gone: the machine
has its own thread, so a menu no longer stops it and there is nothing to stall
(Docs/Threading-Plan.md phase 5). The bug cannot come back either way - the
audio thread writes silence into whatever the ring cannot fill, so a device is
never left playing what it happens to still hold. Whether a menu pauses the
machine is now a choice: Emulation > Pause While in Menus, off by default.

## 64. Getparam answered zeros where the Setfilter file and channel belong

`Getparam` (0Fh) replied `stat, mode, 0, 0, 0`, with a comment saying the
file and channel were not tracked. They were: `Setfilter` already stored them
in `filter_file_` and `filter_channel_`, and the XA decoder filters on them.
Only the reply had not caught up.

psx-spx gives the layout as `INT3(stat,mode,null,file,channel)`: a byte that is
always zero sits between the mode and the file. That byte matters as much as
the values. Software reads each field by its position, so leaving it out would
hand a game the channel as the file, the same kind of mistake as GetlocL's
status byte (bug 51).

**The fix.** The reply is now `stat, mode, 00, file, channel`.

**Verified.** A `Getparam` group in `media_test` (6 checks, 259 in all) sets a
mode of C8h and a filter of file 1, channel 5, and checks every byte of the
reply. The four values are different from each other and none equals the
status, so a byte in the wrong position cannot pass. With the old reply put
back, the file and channel checks fail. All eight harnesses pass. The BIOS
boot is unchanged (`c7c8db90c5984798`, 97,749,265 instructions), and so are
Air Combat, Wild Arms and Captain Tsubasa J at frames 1000/2000/3000.

**What that does not show.** None of those three discs sends Getparam at all:
their command histograms have no 0Fh. So the game runs show nothing broke,
not that anything was fixed. No game known to need this has been found. The
fix was written in an agent worktree and ported to master afterwards.

Captain Tsubasa J's `.mds`/`.mdf` gives a different frame 3000 from the table
(1,863 sectors where the table has 3,776, and 1,192 XA sectors filtered out).
That is the image, not this change: the pre-fix build gives the same numbers
on it. The table was taken from the `.cue`/`.bin`, whose zeroed subheaders
(Gaps.md) mean nothing is ever filtered.

## 65. A side-loaded PS-EXE started with the BIOS shell in its uninitialised data

`test/PadTest 1.1/padtest.exe` ran from File > Boot PSX-EXE (and
`boot_runner --auto-boot --exe`) with the display off and no interrupts. The
same program booted from `padtest.cue` worked. The disc holds a byte-identical
`PADTEST.EXE`, so the difference was the machine it started on, not the
program.

**What the program did.** The first `printf` in `main` goes through a
wrapper at `800306B8` that tests a word at `800638C0`. If the word is zero,
the output goes through the BIOS's own `printf` (A0h table). If not, it goes
to a serial-port routine that spins on SIO1_STAT bit 2 until the transmitter
is ready. SIO1 is not emulated (Gaps.md), so that bit never sets, and the
program spent the whole run reading `1F801054`, 13 million times in 600
frames. `800638C0` is past the end of the executable's text (`80010000` +
`33000h`), in data the program assumes starts out zero and never clears
itself: its header has no memfill range.

Booted from the disc, that word was zero. Side-loaded, it was `00001000`,
because `set_auto_boot_exe` fired when the pc reached `80030000`. That is where
the BIOS jumps into its shell, after copying the shell's image into RAM from
`80030000` upward. The executable was copied over the shell's code, but its
uninitialised data still held the shell's bytes.

**The fix.** Side-load when the BIOS writes POST code 7 to `1F802041`, which
is what DuckStation does (`Bus::KernelInitializedHook`, fired from the same
POST write). At that point the kernel is set up and the shell has not been
copied yet. What bug 41 needed from the BIOS is already done by then: the
status history shows BEV and Isolate Cache cleared at `BFC0023C`, in the
BIOS's first few hundred instructions. The program now starts at instruction
114,044 instead of about 2.2 million.

**Verified.** PadTest side-loaded draws `e1cab1e7d11c00a3`, 17,089 non-black
pixels at 320x240. That is exactly the frame the disc boot settles on, and it
takes 593 interrupts, no longer zero. amidog's `psxtest_cpu`, `psxtest_gte`
and `psxtest_gpu` give byte-identical checksums under the old and new hook
(`267629a6082f2a88`, `b0c2a767b6b76380`, `c6fbcf66c24453c9`), so bug 41's
results screen is unaffected. All harnesses pass, and the BIOS baseline is
unchanged; neither goes through this path.

**Still true.** `LoadPsExe` does not honour the header's memfill
(`b_addr`/`b_size`) range, and it ignores `s_size` when it sets the stack.
Neither matters for PadTest, whose header sets neither. Both are small and
both are what DuckStation's `InjectExecutable` does. A program that reaches
SIO1 for real will still hang, because SIO1 is still not emulated.

## 66. With the recompiler on, the BIOS console recorded nothing, and the interpreter recorded format strings nobody printed

Found while adding a BIOS console window (Emulation > BIOS Console). The core
already recorded the BIOS's console calls, putchar (A0h:3Ch, B0h:3Dh) and puts
(A0h:3Eh, B0h:3Fh), in `Kernel::Call`, for `boot_runner`'s `bios console`
section. The window needed that feed live, and it turned out to be wrong in
two ways.

**Recompiled, it recorded nothing.** The hook was at the end of
`Cpu::ExecuteInstruction`: if the pc had just landed on A0h, B0h or C0h, it
called `Kernel::Call`. Compiled code never goes through `ExecuteInstruction`,
so `boot_runner --recompiler` printed no `bios console` section at all.

**Interpreted, it recorded too much.** A 400-frame BIOS boot captured 427
characters. That included raw format strings, such as `%s` on a line of its
own and `System Controller ROM Version %02x/%02x/%02x %02x`, next to the
formatted lines they produced, and the `ResetCallback` line twice. The BIOS
never printed those. They came from checking where the pc ended up, at the end
of an instruction, rather than what was about to run.

**The fix.** `System::StepInstruction` checks the pc before the step, after
any interrupt has moved it. That is the one place both CPUs pass through. The
recompiler bridge's `Fetch` also refuses to compile at the three vectors, so
compiled code has to come back to that point before a call runs. Measured,
that refusal is not needed today: software reaches the vectors by `jr`, and an
indirect jump already ends a compiled chain. It guards against a block running
into a vector, and against indirect jumps being linked in the future. The
kernel also gained a drainable feed (`TakeConsoleText`) and a session counter,
which the front end uses to put a separator between boots.

**Verified.** Both CPUs now record the same 339 characters over the BIOS boot:
every line once, fully formatted. The checksum is unchanged
(`c7c8db90c5984798`), because recording a call has no effect on the machine.
A new `cpu_test` group, `biosconsole` (12 checks), makes putchar, puts and a
C0h call through bare `jr ra` stubs, on each CPU in turn. It checks the text
and that each call was counted exactly once. With the new hook removed, the
interpreted half fails. The front end was driven end to end with posted menu
commands:
- the window opens from the menu and fills with the BIOS's output;
- a separator appears on each reset, on the interpreter and on the recompiler;
- closing the window with its X unticks the menu and saves
  `show_bios_console = 0`;
- the app exits cleanly.

**Not covered.** Anything a program sends to the serial port (SIO1) or the
expansion port's DUART directly, bypassing the BIOS. PadTest's debug output
when side-loaded is the first kind (bug 65), and neither port is emulated.

## 67. DMA channel 6 ran only for PsyQ's exact control word, and kept bits the hardware does not have

JaCzekanski's `test/test suite/dma/otc-test` passed 5 of 15 tests. The 10
failures were of two kinds.

**No transfer.** `Dma6` returned unless CHCR was exactly `11000002h`, the
value PsyQ's `ClearOTagR` writes. The tests that start the channel the same
way but with the step set forward, the direction set to "from RAM", or sync
mode 1, 2 or 3 got no transfer at all, and their buffers came back untouched.
On hardware none of those bits exist for channel 6. It clears an ordering
table backwards and nothing else, so what matters is start (bit 24) and
trigger (bit 28), with the channel enabled in DPCR.

**The wrong register.** The write stored the whole word. Hardware keeps only
bits 24, 28 and 30, reads bit 1 (the backwards step) as always 1, and reads
every other bit as 0. The trigger clears when the transfer begins, and the
busy bit is already clear when the CPU next reads it. Here, writing
`70770703h` read back as written instead of `50000002h`, and writing 0 read
back 0 instead of 2. A finished transfer read back its trigger bit still set,
and its busy bit too, because bug 38 keeps a channel busy for its transfer
time while the CPU runs on. That is right for some channels, but not for this
one. `testOtcControlBitsAfterTransfer` reads CHCR immediately after starting
32K words and expects it idle: an OTC transfer holds the bus, so the CPU
never sees it running. This holds with chopping on as well.

**The fix.** The CHCR write keeps `51000000h` of what was written and forces
bit 1 on, from an initial value of `00000002h`. DuckStation's `OTC_WRITE_MASK`
and `OTC_FIXED_BITS` are the same. Start plus trigger with the channel enabled
runs the clear whatever the other bits say, clears the trigger, and then
completes at once. The transfer's cycles are still charged to the CPU as a
stall. Channel 6 still raises no DMA interrupt on completion, exactly as
before. Whether it should is a separate question, and this test doesn't ask
it. Loading an older save state masks the stored CHCR the same way.

**Verified.**
- **otc-test:** all 15 pass, run as `boot_runner --auto-boot --exe` and read
  from its `bios console` section.
- **Other DMA tests:** `dma/dpcr` gives the same results as before
  (`writeToSPURAM` and `testSPUDMARead` pass, and the same transfers time
  out). `dma/chopping` and `dma/chain-looping` report timings only, all on
  channel 2.
- **Harnesses:** all eight emulation harnesses pass.
- **BIOS boot:** the checksum is unchanged. It runs 1,667 fewer instructions
  (97,747,598), because its own wait on a channel 6 clear now ends sooner.
  `host_test`'s baseline constant and Test-Suite.md are updated to match.
- **The twelve-disc table:** all 36 checksums are unchanged. Every game clears
  ordering tables every frame, so this was the real test of the change. The
  one difference is Area 51 having read 5,510 sectors by frame 3000 instead
  of 5,511, with identical frames.

**Seen while verifying, not caused by this.** `host_test`'s two real-time
checks (the frame limiter holding 59.29 Hz, and three seconds of sound with
nothing short) failed on every run that afternoon. The machine thread
managed only 47.9 fps. The CPU was clocked at 2.47 of its 4.0 GHz on the
Balanced power plan, and the build from before this change was just as slow
(5.3-5.4 s for the 400-frame BIOS boot, against 4.4 s earlier the same day).
Those two checks measure the host, and they need it at full speed.

## 68. amidog's CPU suite: overflow traps, a faulting load, 28 branch encodings and jalr

amidog's `test/psxtest_cpu` ran to its end with errors in `sub`, `addi`,
`sltiu`, `lh`/`lhu`/`lw` and their `_d` forms, the four branch-and-link
groups, `jalr`, and 60 of the 64 `BRA ADV` groups (all but rt 00h/01h), and a final
`Result: 00000909`. Read by group, they were six separate
instruction-level bugs. None of them was about timing, which is what
CPU-Timing-Plan.md phase 0 had expected them to be.

- **`sub` and `addi` never trapped.** Signed overflow has to raise exception
  0Ch and leave the destination unwritten. `add` already did; its two
  siblings wrapped silently and wrote the wrapped result.
- **`sltiu` compared against a zero-extended immediate.** The immediate is
  sign-extended first and then compared unsigned, so `sltiu rt, rs, -1`
  compares against `FFFFFFFFh`. The recompiler already did this correctly, so
  the two CPUs disagreed.
- **A faulting load still delivered.** A misaligned `lh`/`lhu`/`lw` did raise
  AdEL (exception 4) with the right BadVaddr. But the instruction then armed
  the 0 that `Load` returns on a fault, and that overwrote the destination.
  All five loads now check `LoadFaulted` and deliver nothing.
- **28 of the 32 REGIMM encodings did nothing.** Only rt 00h/01h/10h/11h were
  in the table; the rest were `UNKNOWN`, a no-op. On hardware every encoding
  branches: bit 0 picks BGEZ over BLTZ, and the link happens when
  `(rt & 1Eh) == 10h`. That was the whole `BRA ADV` column.
- **`bltzal`/`bgezal` read `rs` after writing `$ra`.** So `bltzal $ra` tested
  the link address instead of the old `$ra`. The link is also written whether
  or not the branch is taken.
- **`jalr` wrote `rd` before reading `rs`.** So `jalr t0, t0` jumped to its own
  link address. And a misaligned target now faults as the branch is taken:
  AdEL with EPC and BadVaddr both the target, before the delay slot runs,
  matching DuckStation's `CPU::Branch`. It previously went nowhere in
  particular.

**Verified.**
- **psxtest_cpu:** every group now reports only "Done", and `Result` reads
  `00000101`. The results screen was sampled by pixel, above the legend row.
  It contains only black, white, cyan headings, green (OK) and brown (N/A):
  no yellow warning and no red error anywhere, TIMING column included.
- **New `cpu_test` group:** `cpuedges`, 24 checks, one or more per fix, with
  expected values from the hardware rules rather than from this code. Built
  against the old `cpu.cpp`, 15 of them fail. The other nine hold behaviour
  that was already right, so a later fix can't break it.
- **Games:** the twelve-disc table is unchanged, all 36 checksums and every
  sector count. Every fix is in a path a game reaches only if it would also
  fault or misbehave on a real console, apart from `sltiu` with a negative
  immediate, which none of the twelve changed on.
- **Everything else:** all harnesses pass, including `rec_test` and
  `host_test`. The BIOS boot and `psxtest_gte`/`psxtest_gpu` checksums are
  unchanged.

**The recompiler.** It compiles none of the instructions changed here: the
trapping `add`/`addi`/`sub`, the linking or undocumented REGIMM encodings, and
anything after a fault all go to the interpreter. Its `sltiu` and `jalr` were
already right. One case was reasoned about, not tested: a compiled `jr`/`jalr`
to a misaligned target sets a misaligned pc without going through
`Cpu::Jump`. Whatever the interpreter then does with that fetch, it is not
this fault raised at the branch, so EPC can differ from the interpreted case.
No game jumps to a misaligned address.

## 69. Memory cards: written a sector at a time, created unformatted, leaked on insert, and nothing could read one

Built from [Memory-Cards-Plan.md](Memory-Cards-Plan.md). Three of the problems
it names were bugs in what was already there:

- **Every 128-byte sector opened, wrote and closed the card file.** A one-block
  save did that 64 times. It was slow, and a crash part-way through left a
  card with half a save on it. The card is now held in memory and written
  whole: to a temporary file beside it, then renamed over it, so the file is
  always either the old card or the new one. A write goes to disk a second
  after the game stops writing (`MC::OnFrame`, called from the machine
  thread's loop), and at once on pause, eject, cold boot and exit.
- **A new card was 128 KB of zeroes.** The BIOS read that as unformatted and
  offered to format it before a game could save. `CreateFile` now writes a
  formatted card: the "MC" header, fifteen free directory frames and an empty
  broken-sector list, each with a valid checksum.
- **Inserting a card over another leaked the old one.** Worse, anything a game
  had written to the old card and not yet flushed was dropped. `LoadFile` now
  ejects the old card first, which saves it.

**What was added.**
- **Core parsing (`psx/mc_directory.h`):** pure functions over the 128 KB
  image, with no file or UI. They list saves, with the Shift-JIS title decoded
  and the icon converted to ARGB, count free blocks, delete, undelete, export
  and import `.mcs`, and format.
- **Delete keeps the links.** It marks a save's blocks A1h/A2h/A3h but leaves
  its next-block links intact, as the BIOS does, so a multi-block save
  undeletes whole. DuckStation clears the links on delete, which leaves its
  undelete restoring only the first block.
- **Front end:** File > Memory Cards has Insert, New and Eject for each slot,
  all usable while a game runs, plus the Memory Card Editor.
- **Editing a live card:** the editor gets snapshots of the cards from the
  machine thread. Every change runs against the live card there, between
  frames, and then sets the card's "new card" flag, the signal a physical
  swap gives, so a running game re-reads the directory.

**Verified.**
- **`mc_test`:** 77 checks. Formatting, with checksums worked out by hand.
  Import and listing, including a full-width Shift-JIS title and the icon
  palette. Export. Delete then undelete, giving back the card byte for byte,
  and undelete refused once a block is reused. An export-format-import round
  trip. The card file: a write held in memory until the idle flush, a flush on
  eject, and a round trip out of the slot and back from disk.
- **Mutation-tested:** a flush that wrote nothing failed six of the checks;
  a delete that cleared the links failed the byte-for-byte undelete.
- **Real saves:** `mc_test <card>` listed copies of real cards (Wild Arms,
  Wild Arms 2, Vandal Hearts, NASCAR Thunder 2004), with the right titles,
  block counts and one-to-three-frame icons.
- **The front end, driven by posted commands with a throwaway disc name:**
  - booting a disc created both per-disc cards already formatted;
  - the editor opened showing both slots with 15 of 15 blocks free;
  - Eject emptied slot 1, and the open editor showed it within a second;
  - the app exited cleanly.
- **Everything else:** all harnesses pass, including `sio_test`, which covers
  the card protocol, and `host_test`, which covers the machine loop the flush
  now runs in. The BIOS baseline is unchanged.

**Not verified.** No game has been played through a save and a load on the
new write path. The sector protocol and the in-memory image are unchanged,
and `sio_test` still passes, but the first real save is worth watching.
Pressing the editor's buttons wasn't driven either, since that needs clicks
inside its window. The operations behind the buttons are what `mc_test`
checks.

## 70. Recent discs and keyboard bindings, and a list index held across a message box

Two front-end features, and one bug they turned up.

**File > Recent Discs** lists the last eight discs played, most recent first,
so the first entry is always the last disc played. A disc is added once it has
actually mounted: from Boot disc, from Swap disc, or from the command line.
One whose image has since gone (an offline share, a renamed folder) is dropped
from the list with a warning, instead of booting into an empty drive. The list
lives in `psxemu.ini` as `recent_disc_1`..`8`, beside the core's keys. It's a
front-end preference, so it isn't part of `EmuConfig`.

**Settings > Input > Keyboard Bindings.** Before this, the keyboard map was a
table compiled into `const.h`; it is now only the defaults.
- **Changing a binding:** double-click a button, or select it and press Set
  Key, then press the key. Escape cancels.
- **One key per button:** giving a key to one button takes it from any other,
  and the window says which.
- **Refused keys:** the ones the window already uses (Space, F1-F8).
- **Storage:** the map is saved as readable names (`key_cross = X`,
  `key_start = Return`). A button missing from the file keeps its default; one
  present but empty stays unbound, so clearing a key survives a restart.
- **Threading:** the input thread holds the map as fourteen atomics, so a
  change applies from its next poll without a lock.

**The bug, found by the test that drove it.** Picking a recent disc that no
longer existed showed the warning *first* and removed the entry *after*. A
message box runs its own message loop, so commands still arrive while it is
up. The test sent Clear Recent Discs during the warning, and it ran, emptying
the list. When the box closed, the code would then have erased entry 2 of an
empty list. The entry is now removed before the warning appears. The same
shape (a list index, or anything else that can change, held across a modal box
on the UI thread) is worth checking for wherever a warning follows a lookup.

**Verified** through a scratch build with its own `psxemu.ini`, driven by
posted commands and read back from outside the process (the menu through the
menu API, the bindings list through a buffer in the target process):
- **Recent Discs:**
  - the menu listed the seeded discs;
  - picking the missing one warned, and the entry was already gone from the
    menu and the ini while the box was up;
  - Clear emptied both.
- **Keyboard Bindings:**
  - the window loaded all fourteen buttons right, including an edited key, a
    missing one (its default) and an empty one ("(none)");
  - binding V to Cross, then V to Circle, took it from Cross and said so;
  - Space was refused while still waiting for a key, and Escape then left
    Triangle as it was;
  - the ini ended with exactly those changes.

**Not verified:** a game played with rebound keys. The input thread's side is
a straight read of the new map, but nobody has pressed a rebound key in a game
yet.


## 71. A debugger's core: breakpoints and stepping that halt the machine without changing it

Phase 0 of [Debugger-Plan.md](Debugger-Plan.md). This is a feature, not a fix,
recorded here for what it changed in the core and the two bugs its test found
before they shipped.

**What is in.** `psx/debugger.h`, owned by `System` and so by the machine thread.
- **Execute breakpoints** match on the physical address, so KSEG0, KSEG1 and
  KUSEG aliases are one breakpoint. They can be disabled, and each counts its hits.
- **Stepping:** into, over and out, plus run-to and a break request.
  - Step over targets the instruction after the call's delay slot, at the same
    call depth. A `syscall` counts as a call.
  - Step out halts on the `jr ra` that leaves the current function, counting
    calls in and out. A linking REGIMM branch counts as a call only if it is taken.
- **Where the check runs:** in `System::StepInstruction`, after interrupts and
  before the BIOS-call hook. A halt runs nothing, so it takes no cycles.
- **The recompiler** is bypassed while the debugger is armed (DuckStation does
  the same). A plain continue disarms it, and compiled code comes straight back.
- **`host::Machine`:** a halt mid-frame returns from `RunOneFrame` without
  counting an instruction, and pauses for a new reason, `kPausedByDebugger`. The
  frame is not published. Any request that un-halts the debugger clears the
  pause, and the frame carries on towards the same boundary.
- **`boot_runner --break`** prints the registers and a disassembly at each hit,
  then carries on.

**The two bugs `debug_test` found:**
- A plain continue left the debugger armed forever, because the one-shot "let
  the halted instruction run" flag was cleared without re-computing `armed`.
  The machine stayed on the interpreter for no reason.
- A second `StepInstruction` while halted counted the same breakpoint again.
  It now returns straight away while halted. Mutation-tested: without that
  guard, the check sees 2 hits, where it wants 1.

**Verified, 2026-09-19:**
- **Determinism:** the BIOS boot, 400 frames, ended on 97,747,598 instructions
  and `c7c8db90c5984798`, with a 339-character BIOS console, in each run:
  - with no breakpoints;
  - with `--break B0` (1,259 hits);
  - with `--break BFC02B68` (426,288 hits);
  - with both and the recompiler on (427,547 hits).
- **Threaded:** a `host_test` run halted mid-frame 60 times (31 at a breakpoint,
  the rest single steps), each let go from another thread, and it too ended on
  those numbers.
- **The harnesses:** `debug_test` 48 checks, and every other harness green.
  `host_test`'s real-time audio check failed once in four runs, as it has
  before on this machine when the host CPU is throttled. It is unrelated.
- **The twelve-disc table:** all 36 checksums (frames 1000, 2000 and 3000)
  unchanged. With nothing armed, the only new work on the hot path is one
  `armed()` test per instruction.
- **The build:** the front end builds.

**Not verified:** there is no window yet, so nothing in the GUI can set a
breakpoint. The machine-thread halt path is covered only by `host_test`.

## 72. The debugger window, and what a load in flight looks like from outside the CPU

Phase 1 of [Debugger-Plan.md](Debugger-Plan.md): **Emulation > Debugger**.

**What it shows.**
- **Disassembly:** 256 instructions around the pc.
  - Markers: the pc is marked ►, an enabled breakpoint ●, a disabled one ○.
  - Colours: the pc's line is yellow, an enabled breakpoint's line red, and
    delay slots and anything that is not memory are grey.
  - Moving about: Go To takes an address (Ctrl+G), and Enter or the context
    menu follows a branch. Arrowing or paging off either end fetches the next
    stretch.
- **Registers:** the 32, then hi, lo, pc, SR, Cause, EPC and BadVaddr.
  - SR and Cause are decoded (IEc, IM, IsC, BEV; ExcCode by name, IP, BD).
  - What the last step changed is in red.
  - A load still on its way is shown against its register.
- **Breakpoints:** a list with hit counts and a checkbox to disable each.
- **Controls:** Continue F5, Break (Ctrl+Break or Pause), Step Into F11,
  Step Over F10, Step Out Shift+F11, Run to Cursor Ctrl+F10, and F9 or a
  double-click to toggle a breakpoint.

**How it is built.**
- **Snapshots and requests:** the plan's decision 1. The window never reads the
  machine. It sends requests, and the machine thread answers each with
  `Debugger::Capture`, a copy of the registers, the breakpoints and the
  disassembly (disassembled there). A halt sends one unasked, through
  `Machine::Hooks::halted`, which also brings the window forward.
- **Refreshes:** a boot, a reset or a state load refreshes an open window.
- **The disassembler** moved from `tools/` into Core as `psx/disasm.h`, and now
  names every encoding the CPU runs:
  - REGIMM aliases, named by what they do (bug 68);
  - GTE commands;
  - Cop0 registers;
  - cop2 moves.
  `boot_runner` uses it too.

**The load delay.** A register view of this CPU can mislead, because a load's
value is not in its register yet. `Cpu` models this in two stages. The window
shows both, on the register they are headed for:
- "loading X - next instr sees it" is the stage that is written before the next
  instruction runs;
- "loading X - after next instr" is the one written a step later.

`debug_test` walks one `lw` through both stages and into the register.

**Decisions that differ from the plan, or that it left open:**
- **The registers while running.** They are greyed: the last halt's values,
  as the plan says. The disassembly and the breakpoint list do refresh while
  running, because a breakpoint set while the game runs has to appear.
- **No flicker.** A single step would briefly grey the window. It doesn't
  grey unless no halt comes back within 150 ms.
- **The changed-register red is per halt.** A snapshot asked for while still
  halted (a breakpoint toggled) is the same machine, so it keeps the red rather
  than comparing the state with itself.
- **Closing the window while halted continues the machine.** A game frozen
  behind a closed window helps nobody. The breakpoints stay, and the next one
  to hit opens the window again.
- **Loading a save state ends a halt** (`System::LoadState` calls
  `Debugger::Reset`). The machine is somewhere else, and a half-taken step
  means nothing there. Breakpoints stay, as across a reset.

**Step Out is only as good as the code's returns.** Stepping out of the B0
dispatcher during a BIOS boot stopped somewhere other than the caller. The
call was B0:17h, ReturnFromException, which reloads `ra` from the saved
context and leaves through `rfe`. It never returns to its caller, so "run
until this function returns" stops at the next `jr ra` at that depth, which
belongs to the interrupted code. That is right by the definition, but not what
a person expects. The same goes for longjmp.

**Verified, 2026-09-19:**
- **`debug_test`:** 75 checks. The new groups are the snapshot (registers,
  both load stages, the listing's centre, delay-slot and branch-target marking,
  no wrap below 0, no hardware-register reads, a state load ending a halt) and
  the disassembler.
- **A real BIOS boot, from outside the process.** A scratch build with its own
  ini (volume 0) was driven by posted commands and keys, reading the window's
  lists and status back:
  - opened while running, it showed "Running" with the steps disabled;
  - a breakpoint set on B0 from the address box halted there with hits=1;
  - the main title read "paused";
  - F11 walked B0, B4, B8 and then through `jr t0` to 5E0;
  - F10 stepped;
  - Step Out came back as above;
  - F5 hit the breakpoint again (hits=2);
  - unticking it let the machine run at 59.6 fps;
  - Break halted on the interrupt vector;
  - Run to Cursor stopped at its target, matched physically (800000A8 reached
    as 000000A8);
  - Go To BFC00000 centred there;
  - closing the window while halted left the game running at 59.8 fps;
  - the app exited cleanly.
- **The front end builds,** with no new warnings.
- **The harnesses and determinism:** every harness is green. The BIOS boot
  still ends on 97,747,598 instructions and `c7c8db90c5984798`, with and
  without `--break` and with the recompiler.

**Not verified:**
- The twelve-disc table was not re-run. Nothing on the per-instruction path
  changed in this phase: the snapshot is taken only on request or at a halt.
- What the window looks like: colours, fonts, layout at other DPIs.
- A real F10 or Ctrl+Break from a keyboard. The test posted the keys; a
  physical F10 arrives as WM_SYSKEYDOWN, which the code handles but which
  nothing drove.
- Anything but the BIOS: no game has been debugged with it yet.

## 73. The debugger's memory view, and which hardware registers can be looked at

Phase 2 of [Debugger-Plan.md](Debugger-Plan.md): a memory pane in Emulation >
Debugger, memory and register editing, and a peek path that never disturbs
the machine.

**What it does.**
- **The memory pane:** 32 rows of 16 bytes, hex and ASCII.
  - Moving about: View takes an address, Prev and Next move by half a page,
    and arrowing off either end moves too. Right-click follows any of a row's
    four words, into the pane or the disassembly.
  - Colour: rows whose bytes changed since the last snapshot are red.
  - Live: while the machine runs, the pane refreshes twice a second, so a
    counter or a player's health can be watched. The registers still don't
    refresh (bug 72).
- **Writing memory:** double-click a row to put its address and bytes in the
  edit boxes, change them, and press Write. Bytes are in memory order. It works
  whether the machine is running or halted.
- **Editing registers:** double-click a register and Set it. That covers the 31
  that aren't zero, hi, lo and the pc. It only works while halted: a new value
  between two frames of a running game means nothing anyone could predict.
  Right-click shows a register's value in the memory pane or the listing.

**Reading without side effects (`Debugger::PeekData`).** The plan's warning
was right, and it applies to more than the CD-ROM:
- a timer's mode read clears its reached flags;
- the CD-ROM, SIO and MDEC registers and GPUREAD pop FIFOs;
- the timer counter and DMA read paths first run the pending batch of cycles,
  which moves the machine on.

So the peek never calls a device's read. It copies the state behind the
register:
- memory control, RAM_SIZE, I_STAT and I_MASK, and the cache control register;
- the DMA registers and GPUSTAT (their reads were checked and are pure);
- the timers' three fields, read directly: `mode.raw`, not `ReadMode()`;
- the SPU (its read is a pure lookup);
- the expansion region.

Everything else shows `??`. A timer's count and a DMA channel's busy bit can
be a batch of cycles stale, which is the price of not running the machine to
look at it.

**Writing (`Debugger::WriteMemory`).**
- **What it writes:** RAM, the scratchpad and the expansion region, all or
  nothing. The BIOS is refused as read-only. Every hardware register is
  refused, because writing one does more than store a value. The window shows
  the reason.
- **Invalidation:** RAM written this way is reported through `NoteBulkWrite`,
  as a DMA is, so compiled code built from it is dropped.
  `debug_test` patches a running loop and checks the new instruction runs with
  the recompiler on. Without the `NoteBulkWrite`, that check fails.
- **The plan said** writes would go "through the same store path a CPU store
  does". They don't: the CPU's store path has side effects of its own
  (Isolate Cache turns a store into a cache invalidation) and the bulk report
  is what DMA already uses.

**A test that could not fail, removed.** The first version also invalidated
the instruction cache's lines, with a test that ran the patched loop with the
cache enabled. With the invalidation removed, that test still passed. The
reason: instruction fetch is a plain `Load` from RAM, and the icache model's
data is never read ("The instruction and data caches are not modelled" in
[Gaps.md](Gaps.md)). There was no cached copy to go stale. The invalidation
and its test were removed rather than kept as a test that passes either way.

**Registers (`Debugger::SetRegister`).**
- **A load in flight is dropped.** Setting a register while a load to it is
  still in flight drops the load. Otherwise the load lands a step later on top
  of the edit. That needed `Cpu::CancelLoadsTo`.
- **A new pc moves the halt with it.** Resuming runs from there, and whatever
  was skipped never runs.
- **Refused:** a misaligned pc, and register zero.

**Verified, 2026-09-19:**
- **`debug_test`:** 105 checks. The new groups are memory, patched code
  (interpreter and recompiler), and register editing. The recompiler case was
  mutation-tested as above.
- **A real BIOS boot, from outside the process.** A scratch build was driven
  by posted commands, reading the memory pane back row by row:
  - 80000000 showed the exception vector's code;
  - I_STAT and I_MASK read 1 and 9;
  - the CD-ROM row was all `??`;
  - at 1F801810, GPUREAD was `??` and GPUSTAT beside it read 1C4E220A;
  - SPU voice 0's registers read;
  - timer 2's counter differed between two refreshes a second apart while
    running, and stayed the same while halted;
  - DE AD BE EF 01 02 written to 80100000 while running read back;
  - writes to BFC00000 and to I_MASK were refused, with those reasons in a
    warning;
  - Set was disabled while running;
  - after Break, t0 and hi took new values, zero stayed zero, and a misaligned
    pc was refused;
  - Continue ran on at 59.7 fps.
- **The harnesses and determinism:** every harness is green. The BIOS boot
  still ends on 97,747,598 instructions and `c7c8db90c5984798`, with and
  without `--break` and with the recompiler. The front end builds with no new
  warnings.
- **`host_test`'s two real-time checks** (the frame limiter and three seconds
  of sound) failed for a stretch, then passed twice in a row once the machine
  was quieter. They depend on wall-clock speed, and this machine was loaded:
  - `boot_runner` swung between 1.28x and 1.66x real time from run to run;
  - yesterday's pre-debugger binary did the same.

  Timed alternately, best of six: 4.42 s for the old binary, 4.53 s for this
  one, about 2.5% apart, inside that noise. Phase 2 adds nothing per
  instruction. (Bug 74 corrects the rest of this: timed against a build with
  the debugger's checks compiled out, rather than an older binary, the
  `armed()` test turned out not to be the cost. Bug 74 found and removed the
  real one.)

**Not verified:** the context menus (following a word, showing a register's
value) and double-click filling the edit boxes were not driven. A test can't
easily post those from outside the process. What they call (`ViewMemory`,
`GoTo`, the edit boxes) was driven directly. The window's look is unseen, as
before.

## 74. Watchpoints; an idle debugger that cost 3%; and three boot_runner options with no default

Phase 3 of [Debugger-Plan.md](Debugger-Plan.md): stop when memory is read or
written, by the CPU or by DMA.

**What it does.**
- **A watchpoint:** a range (start and length), set to catch writes, reads or
  either. Each can be enabled and counts its accesses.
- **In Emulation > Debugger:** a list under the breakpoints, with a row of
  controls. A memory row's context menu can also watch that row.
- **Headless:** `boot_runner --watchpoint <hex>[:<len>][:r|w|rw]`.
- **The halt comes after the access,** before the next instruction. The
  instruction that made the access has finished, so nothing is left half done.
  That is the same determinism rule the breakpoints keep.
- **The halt says who:**
  - "the instruction at 00002710 wrote FFFFFFFF to 1F801070", or "DMA channel
    3 (CD-ROM) wrote ...";
  - the listing selects the accessing instruction;
  - the memory pane goes to the address.

**Where accesses are caught.**
- **The CPU:** `Cpu::Load` and `Cpu::Store`, behind one flag
  (`set_debug_watch`) that is set only while a watchpoint is enabled.
- **DMA:** every channel that writes RAM (1, 2, 3, 4 and 6) already reported
  each word through `Cpu::NoteExternalWrite`, tagged D0h plus the channel, for
  `--watch-ram`. The debugger hooks in there, so every channel is covered by one
  line.
- **Matching:** addresses are matched physically, and RAM's four 2 MB mirrors
  count as one.

Three details are easy to get wrong, and each has a test that failed when its
fix was taken out:
- **SWL and SWR read the word they merge into.** That is how this emulator does
  a partial store, not a read the program made, so it must not trip a read
  watchpoint (`merging_store_`).
- **A store with the cache isolated writes nothing** (the BIOS uses it to flush
  the instruction cache). So the write check comes after that case returns.
  Placed before it, the isolated store tripped the watchpoint, and every other
  store was counted twice.
- **The DMA hook:** without it, the CD-ROM and OTC writes go unseen.

**An idle debugger cost about 3%, and now costs nothing measurable.** The
plan asked for zero cost when nothing is armed. A build with every debugger
check compiled out ran the BIOS boot about 2.5-3% faster, consistently.
Isolating each check showed where the cost was:
- **The step's `armed()` test:** not it. `StepInstruction` now returns false
  on a halt, so callers don't ask `halted()` afterwards. `host::Machine` also
  checks `armed()` once per frame and runs an unarmed frame through
  `StepInstructionUnarmed`, a second copy with the check compiled out. That's
  safe because nothing can arm the debugger mid-frame: the window's requests
  run between frames. boot_runner without `--break` or `--watchpoint` does the
  same.
- **The watch hook in `Load`/`Store`:** this was the cost. A build with only
  that hook removed matched the check-free build. The flag test itself is
  cheap. The cost came from the call it guarded: setting up its arguments
  inside the interpreter's two hottest functions changed how the compiler laid
  out the rest of them.

The call moved into never-inlined helpers (`WatchLoad`, `WatchStore`) behind
an `[[unlikely]]` flag test. Best of eight BIOS boots, alternating: 4.26 s for
this build, 4.30 s with every debugger check compiled out, medians 4.31 s and
4.32 s. That is inside the noise.

**Three boot_runner options with no default.** Adding `--watchpoint` added a
member to boot_runner's `Options`. After that, a plain `--watchpoint` run came
up in `--recompiler-diff` mode and stopped with "DIVERGED". `ParseOptions`
never set `recompiler`, `recompiler_toggle` or `recompiler_diff`. `Options`
lives on the stack, so they held whatever was there. That happened to be
zero until the layout moved. All three now have defaults. The other fields
were checked against the default list and all have one; the vectors construct
themselves.

**Verified, 2026-09-19:**
- **`debug_test`:** 149 checks. The watchpoint group runs twice, once
  interpreted and once with the recompiler on. It is mutation-tested three ways,
  as above. It also found a bug in the test itself: the harness's `Reset`
  cleared breakpoints but not watchpoints, so the sub-tests had been sharing
  them.
- **Determinism, BIOS boot:** these runs all end on 97,747,598 instructions
  and `c7c8db90c5984798`:
  - `--watchpoint 1F801070:4:w`, which halts on 1,974 I_STAT writes;
  - the same with the recompiler;
  - that plus breakpoints on B0 and BFC02B68, 429,521 halts in all.
- **Determinism, with DMA:** Ridge Racer to frame 3000 with `--watchpoint
  80010000:0x100:w`:
  - DMA channel 3 (CD-ROM) was named twice, writing the executable;
  - then a BIOS store to the same range;
  - 67 halts, ending on `e363f0b4ab4b87eb`, the table's frame-3000 value.
- **The window, from outside the process, on a BIOS boot:**
  - an I_STAT write watchpoint set from the controls halted with the store's
    pc and value, selected the store in the listing, and moved the memory pane
    to 1F801070;
  - two more continues caught a word and a halfword write;
  - unticking it ran on at 59.6 fps;
  - a GPUSTAT watchpoint on either kind caught a read with its value;
  - removing it worked.
- **The phase 1 smoke test** was rerun after the machine loop changed, with the
  same results as bug 72.
- **The hot path changed** (`Cpu::Load`/`Store` and the step). Checked after
  that change:
  - the twelve-disc table: all 36 checksums unchanged;
  - every harness green, `host_test` included;
  - `--recompiler` on the BIOS: the same 9,161,469 steps and checksum as the
    build with every debugger check compiled out.
- **The front end builds.**

## 75. A BIOS call log, a call stack, labels and device panes, and 12 KB that cost 5%

Phase 4, the last, of [Debugger-Plan.md](Debugger-Plan.md). The debugger's
window gains four tabs under the listing (Memory, BIOS Calls, Call Stack,
Devices) and labels.

**What it does.**
- **The BIOS call log.** Every call through A0h, B0h or C0h is recorded, armed
  or not, where the kernel's console capture already notices them. It keeps the
  last 256, newest first, with the function by name, a0-a2, where it returns to,
  and the cycle.
  - Right-click a call to break on that function: the machine halts at the
    vector, before the function runs, and the status line names it.
  - Double-click a call to go to its caller.
  - The names come from the table that used to exist only in debug builds
    (`debug_assist.cpp`, under `_DEBUG`). It now lives in `psx/bios_calls.cpp`,
    built everywhere, and the debug build's CSV log uses it from there.
- **The call stack, approximate.** Every call taken and not yet returned from,
  innermost first. Double-click a frame to go to the function it entered.
  - It is recorded only while "Track calls" is on, which arms the debugger, so
    the machine runs interpreted.
  - A return pops back to the frame it returns into. A return into no frame (a
    longjmp, a return through an exception) leaves it alone.
  - As the plan says, it is a guess, and says so.
- **Labels.** Select a line, type a name, press Name. The label shows on its
  line and against every branch to it. Labels load from and save to a text
  file, one hex address and name per line. Blank lines, `#` and `;` comments,
  and anything that doesn't parse are skipped.
- **Devices,** read-only, through the same side-effect-free peek as the memory
  pane:
  - the interrupt sources, pending or enabled;
  - DPCR and DICR, and each DMA channel's registers and mode;
  - the three timers, with their mode decoded;
  - GPUSTAT decoded (resolution, video mode, depth, interlace), the display
    area, the texture page and frame counts;
  - the CD-ROM's disc, last command by name, and sector counts;
  - the SPU's control and status, and all 24 voices (pitch in Hz, start and
    repeat, envelope level, sounding or ended).
- **`boot_runner --track-calls`** keeps the stack for a whole run and prints
  the innermost frames at the end.

**Left out.**
- **PsyQ `.SYM` files.** No sample exists locally or on the share to test a
  parser against. A parser written from a description of the format, tested
  only against files made from the same description, would prove nothing.
  Labels load from text until a real `.SYM` file turns up.
- **The CD-ROM command log the plan mentions.** `Cdrom::Stats` keeps the
  *first* 4,000 events, which is right for boot_runner's end-of-run report. It
  is wrong for a live pane, which wants the latest. The pane shows the last
  command and the counters instead.

**12 KB that cost 5%.** With the log first written as a 256-entry array inside
`Debugger`, the BIOS boot ran 5% slower than the build with every debugger
check compiled out: 4.57 s against 4.36 s, best of six. Phase 3 had left
them level, and nothing new ran per instruction. `Debugger` sits inside
`System`, before the GTE and the config, and the config is read on every
step. The inline array pushed those members 12 KB further along. With the
array moved to the heap, the two builds are level again: 4.39 s against 4.41 s,
best of eight.

**Verified, 2026-09-19:**
- **`debug_test`:** 174 checks. The new groups are the call log and BIOS-call
  breaks, the call stack, labels, and the devices. The call stack is
  mutation-tested: returns that never pop fail it.
- **Determinism, BIOS boot:** these all end on 97,747,598 instructions and
  `c7c8db90c5984798`, with a 12-deep stack at the end:
  - `--track-calls`, interpreted;
  - `--track-calls` with the recompiler;
  - `--track-calls` with a breakpoint and a watchpoint, 3,233 halts.

  The debugger's log counted 20,758 BIOS calls, the same as the kernel's own
  tally.
- **The disc table:** all 36 checksums unchanged, before the log moved to the
  heap. After the move, a change of layout only, the harnesses and the BIOS
  determinism runs were repeated.
- **The window, from outside the process, on a BIOS boot:**
  - the BIOS Calls tab listed 256 named calls;
  - the tabs switched by mouse and by arrow keys;
  - the Devices tab showed its 46 rows with live values;
  - with tracking on, a Break showed a four- or five-deep stack;
  - naming a line labelled it;
  - Labels > Load Labels went through the real file dialog and labelled
    BFC00000 and 80000080, skipping the malformed line;
  - Labels > Save Labels wrote both back out, RAM in KSEG0 and the BIOS in
    KSEG1;
  - a right-click break on B0h:17h (ReturnFromException) halted at 000000B0
    with the call named, and again on Continue.

**Not verified:** PsyQ `.SYM` (not built), the window's look, and any game.
