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
