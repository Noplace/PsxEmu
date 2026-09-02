# A recompiler for the R3000A

## The conclusion first

Dynamic, not static - and for a better reason than self-modifying code. And
almost certainly **not** on the existing `RecCore` library.

## Why dynamic

Static recompilation of a PSX game is not really possible, and self-modifying
code is only part of it:

- **The code is not all there at the start.** A game streams overlays off the
  disc all through a session. Wild Arms' own console output shows it doing
  exactly that - a second `CD_init`, a second `ResetGraph`, code arriving at
  `0x8015F000` that was not in the executable. There is nothing to statically
  compile until it has been loaded.
- **Where code is cannot be known without running it.** Jump targets come out
  of tables in RAM. The crash chased in bug 24 was a `lw` from a dispatch table;
  finding those statically is the halting problem with extra steps.
- **Self-modifying code, yes**, but the PSX case is milder than its reputation:
  it is mostly overlay loading, which is a whole region replaced at once rather
  than an instruction patched in place.

So: dynamic. The interesting question is not that, it is what to invalidate on
and how much of the interpreter to keep.

## The honest problem with RecCore

The old Game Boy recompiler at `GBEmu/archive/emulation/gb/cpu_recompiler.cpp`
uses it, and the interface is pleasant:

```cpp
using namespace reccore::intel;
IA32 ia32(&e);
ia32.PUSH(EBP);
ia32.MOV(EBP, EA(ESP));
ia32.CALL(cast1.b);
```

Three things about it need saying plainly before anyone commits to it:

1. **It is IA32.** `EBP`, `ESP`, `EAX`, and a class called `IA32`. This project
   builds and runs x64 - `Build\x64\Release` is what the solution produces and
   what every measurement in these documents came from. A 32-bit emitter cannot
   emit code that runs in a 64-bit process. Either the emitter grows x64
   support, or the emulator goes back to 32-bit, and the second is a bad trade
   for a machine that wants a 1 MB VRAM buffer, a 2 MB RAM buffer and 512 KB of
   sound RAM mapped at once.
2. **It is not in this tree.** `#include <RecCore/Lib/reccore.h>` resolves
   through an include path that no longer points anywhere on this machine - a
   search under `C:\dev` finds no `RecCore`. Before anything is planned around
   it, it has to be found and its x64 story established.
3. **The code that used it is in `archive/`.** It was abandoned. That is not
   proof it was abandoned for a bad reason, but it is worth knowing why before
   building on it.

If RecCore turns out to be x64-capable and the source is to hand, using it is
reasonable and saves real work. If it is IA32 only, the choice is between
teaching it x64 - a large job in its own right, and one that has nothing to do
with emulating a PlayStation - and taking an existing x64 emitter. That decision
should be made deliberately and early, because everything downstream depends on
it.

## The thing to settle before any of that

**Is speed actually the problem?**

Nothing in this project has measured it. Every run so far has been the headless
harness, which deliberately runs as fast as it can and reports instructions, not
wall-clock. What is known:

- A 2400-frame run of Wild Arms is 1.6 billion instructions.
- The front end runs the machine on the message-loop thread, frame by frame,
  with an 8-million-instruction guard per frame.

What is **not** known is whether that keeps up with 60 frames a second on this
machine. A recompiler is the single largest and riskiest change this project
could take on, and it should not be started on a hunch.

### Step 0, before anything else

Report wall-clock speed. Add a frames-per-second and an
instructions-per-second line to `boot_runner`, and a title-bar readout to the
front end. Then:

- If it already runs at full speed, a recompiler buys nothing that matters and
  the effort belongs in compatibility, where every recent hour has produced a
  game that works.
- If it is at 60 to 90 percent, profile first. The GPU rasteriser plots a
  hundred million pixels a run in scalar C++, and the SPU generates a sample at
  a time. Either could be the cost, and both are far cheaper to fix than a JIT.
- If it is at 20 percent, a recompiler is justified.

This step is an afternoon and it decides whether the rest of the document is
worth reading.

## If it is justified

### Shape

Block-at-a-time, threaded through the existing interpreter rather than
replacing it:

- Compile a **basic block** - from an entry point to the next branch, including
  its delay slot - and cache it by physical address.
- Keep the interpreter. It is correct, it is tested by 181 checks, and it is
  the fallback for anything the compiler does not handle. A recompiler that
  must handle every instruction before it runs at all never ships.
- Start by compiling only the common arithmetic and load/store forms, and bail
  to the interpreter for the rest. Coverage grows; correctness never regresses.

### Register allocation

The R3000A has 32 registers and x64 has 16, several spoken for. Do not attempt
a full allocator first. Keep the guest registers in the existing `CpuContext`
in memory and load and store around each operation. That is slower than a real
allocator and still several times faster than an interpreter dispatch loop,
because what an interpreter mostly costs is the dispatch, not the work.

Add allocation later, per block, for the registers a block touches most.

### Invalidation

This is where a recompiler goes wrong, and where the PSX makes it easier than
it looks:

- **Track which pages hold compiled code**, at 4 KB or so. A store into such a
  page throws away the blocks in it.
- The store path is already one function, `Cpu::Store`, with a region decode.
  That is where the check goes, and it is one compare on a bitmap in the common
  case.
- **`icache` invalidation is a gift.** Software that overwrites code has to
  invalidate the instruction cache before running it, and on the PSX that means
  writing the cache control register at `0xFFFE0130`. Treating that as "throw
  everything away" is correct, cheap and catches the overlay case, which is the
  common one.

### Timing

The interpreter charges cycles per instruction and per memory access - bug 16
was exactly that, and getting it wrong made the BIOS give up waiting for a
frame. A compiled block must charge the same total, or every timing-sensitive
thing regresses at once.

Sum the cycles at compile time and add them once at the end of the block. The
awkward part is that an interrupt can only be taken at a block boundary, which
makes delivery coarser than the interpreter's. Blocks are short enough that this
is usually invisible, but it is the first thing to suspect when a game works
interpreted and not compiled.

### How to know it is right

The harness already answers this, and it is the strongest argument for doing
this project here rather than anywhere else:

- **The framebuffer checksum must not move.** `bd888bab645a63a9` for the BIOS
  shell at 400 frames. A recompiler that changes it has changed behaviour, and
  the checksum says so immediately.
- **All 533 checks must pass** with the recompiler on, and `cpu_test` should be
  run in both modes - it is 181 checks aimed at exactly the semantics a
  compiler is most likely to get subtly wrong.
- **A per-block differential mode**, worth building early: run a block
  compiled, run it interpreted from the same state, compare every register. Any
  divergence names the block and the instruction. This is the tool that makes
  the difference between a recompiler that takes a month and one that takes a
  year.

## Recommendation

1. **Measure the speed.** One afternoon, and it may end the discussion.
2. **If it is slow, profile before assuming it is the CPU.** The software
   rasteriser is the other candidate and is much cheaper to improve.
3. **Settle the RecCore question** - find it, establish whether it can emit
   x64 - before designing around it.
4. Only then, and only if the answer is still yes, build the block cache with
   the interpreter as the fallback and the differential mode from day one.

The honest summary: this is the most expensive item on any of these lists, and
it is the only one where nobody has yet shown there is a problem to solve. The
last several sessions have each turned a broken game into a working one for a
few hours' work. That rate is unlikely to be beaten by a JIT.
