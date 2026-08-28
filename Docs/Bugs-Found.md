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
