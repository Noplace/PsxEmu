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
