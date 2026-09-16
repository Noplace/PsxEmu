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

## Correction, 2026-09-16: RecCore is x64-capable, and step 0 has now run

Two things in the sections below are out of date. They are kept because the
reasoning around them still holds, but read this first.

**RecCore is not IA32-only.** The repository is
`https://github.com/Noplace/RecCore`, and `Lib/intel/intel.h` defines
`Reg64 = RAX,RCX,RDX,RBX,RSP,RBP,RSI,RDI,R8..R15` alongside `Reg32`, plus
`Reg_xmm`/`Reg_ymm`. `Lib/intel/ia32.h` carries `Reg64` overloads
(`MOV(Reg64, uint64_t)`, `ADD(Reg64, uint32_t)`, `IDIV(Reg64)`) and an
`emitREX(Emitter*, EA)` that encodes the high register bits into the prefix -
and there is VEX encoding in there too. The class is *called* `IA32`; it emits
x86-64. The objection below - "a 32-bit emitter cannot emit code that runs in a
64-bit process" - is simply wrong, and it was the main argument against using
it. The rest of the layout: `Lib/{reccore.h, emitter.h/.cpp, instructionset.h,
mem_mgr.h, types.h}` and `Lib/intel/{intel.h, ia32.h, addressing.h, IS.h,
ia32_a.cpp .. ia32_z.cpp}`, with a `Test/` project beside it.

**It is to be copied, not depended on.** Vendor it into
`PSXEmu.Core/lib/reccore/`, keep its `reccore::intel` namespace, and record the
upstream commit in a short `README` beside it so a later re-sync is a diff
rather than an archaeology exercise. No submodule, no include path pointing off
this tree - which is what made it unbuildable here in the first place.

**Step 0 has run, and the answer is "yes, but not urgently".** Wall-clock speed
is measured now, by `boot_runner`'s own summary and the front end's title bar:

| Run | Wall-clock |
|---|---|
| BIOS boot, 400 frames | 1.57x real time |
| Captain Tsubasa J, 3000 frames | 0.94x |
| Air Combat, Wild Arms, Vandal Hearts, 3000 frames | roughly 1.0-1.2x |

So the interpreter is at or a little above real time on this machine: games are
playable, and a recompiler is **not** needed for them to run. What it is needed
for is headroom - and there is now a concrete consumer of that headroom, since
[Emulation-Speed-Plan.md](Emulation-Speed-Plan.md)'s 200% setting cannot be
reached on real games at 1.0x. That is a much better justification than "faster
is better", and it also sets the bar: **2x on the discs above, or the feature it
exists for does not work.**

Before committing to the JIT, spend the afternoon the section below already
asks for on the two obvious scalar hot spots - the rasteriser plotting ~84
million pixels a run, and the SPU generating one sample at a time. If either is
a third of the time, it is far cheaper to fix than a JIT is to write.

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

- **The framebuffer checksum must not move.** `c7c8db90c5984798` for the BIOS
  shell at 400 frames, and the twelve-disc table in
  [Test-Suite.md](Test-Suite.md) for real games - which did not exist when this
  was written and is the better instrument: a JIT that breaks one game's timing
  and nothing else shows up there and nowhere else.
- **All 1,000 checks must pass** with the recompiler on, and `cpu_test` should
  be run in both modes - it is 251 checks aimed at exactly the semantics a
  compiler is most likely to get subtly wrong.
- **A per-block differential mode**, worth building early: run a block
  compiled, run it interpreted from the same state, compare every register. Any
  divergence names the block and the instruction. This is the tool that makes
  the difference between a recompiler that takes a month and one that takes a
  year.

## The order to build it in

Steps 0 and 3 of the old recommendation are done: speed is measured, and
RecCore emits x64. What follows assumes the profiling pass came back saying the
CPU really is the cost.

**1. Vendor and prove the emitter.** Copy the repository into
`PSXEmu.Core/lib/reccore/`, add it to the solution and to
`tools/build_tools.bat`, and write a `rec_test` harness whose first check is:
emit a function that returns 42, mark the page executable, call it, get 42.
Then one that adds two arguments, to pin the calling convention down - Windows
x64 passes in RCX/RDX/R8/R9 and the callee owns RBX/RBP/RSI/RDI/R12-R15, and
getting that wrong produces corruption that looks like a CPU bug. `mem_mgr.h`
is where RecCore keeps its block allocation; check what it does about
`VirtualAlloc` and W^X before trusting it.

**2. A block decoder with no compiler behind it.** Walk from a PC to the next
branch plus its delay slot, record the instructions, the cycle total and the
registers touched - and then *interpret* it. No code generated yet. This is
where the block boundary rules, the delay-slot handling and the cycle
accounting get debugged, against an interpreter that is already right, with
`cpu_test` as the judge.

**3. Compile the easy third.** The register-to-register ALU forms and the
immediate forms: ADDU/ADDIU/AND/OR/XOR/SLT/shifts. Guest registers stay in
`CpuContext` in memory, loaded and stored around each operation. Anything else
in the block ends the block and falls back. This is the point at which numbers
appear, and the point at which the differential mode below earns its keep.

**4. Memory and branches.** Loads and stores call the existing `Cpu::Load` /
`Cpu::Store` as C functions first - correctness before speed, and it keeps the
region decode, the timing and the store-invalidation check in one place.
Branches end blocks; block linking (patching a compiled block to jump straight
to the next) comes after, and only with a measurement behind it.

**5. Invalidation and the cache-control write**, as described above.

**6. Then, and only then, register allocation**, if the measurements say the
load/store traffic is what is left to win.

## The differential harness, built first

Build this before step 3, not after. A `boot_runner --recompiler-diff` mode
that, for every block: snapshots the `CpuContext`, runs the block compiled,
snapshots again, restores, runs it interpreted, and compares all 32 registers
plus HI/LO/PC. On the first mismatch it prints the block's address, its
instructions and the register that diverged, and stops.

This is the difference between a month and a year. It also composes with
everything already here: run it over the twelve baseline discs and it covers
far more instruction combinations than any test suite anyone would write by
hand.

## Where the risk actually is

- **Timing, not correctness.** The semantics are testable and the tests exist.
  What a JIT quietly changes is *when* interrupts land, and this project's whole
  timing story - bugs 16, 42, 43, 48 - depends on per-instruction accounting.
  Charging a block's cycles in one lump at the end is the standard trick, and
  the twelve-disc table is what will say whether a particular game noticed.
- **Save states.** A state must never contain compiled code or a pointer into
  it. Flush the block cache on load, and keep the JIT entirely out of
  `Serialise` - `kStateVersion` should not move for this at all.
- **The diagnostic tooling is interpreter-shaped.** `--trace`, `--trace-at`,
  `--watch-ram` and the boot-runner statistics all assume instruction-at-a-time
  execution. Keep an interpreter-only switch permanently, and expect to use it
  every time something goes wrong.
- **It is still the most expensive item on any of these lists.** The sessions
  that produced bugs 55-60 each cost hours and each fixed something a player
  would see. A JIT is weeks, and at 1.0x real time nobody is currently waiting
  on it - except the 200% speed setting, which is a real but modest prize.
