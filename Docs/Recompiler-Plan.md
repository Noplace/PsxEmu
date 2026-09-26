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

**Step 0 has run, twice - and the second time corrected the first.** Wall-clock
speed measured with one run at a time and nothing else on the machine:

| Run | Wall-clock |
|---|---|
| BIOS boot, 400 frames | 1.71x real time |
| Wild Arms, 1500 frames | 1.69x, 99.6 fps |
| Captain Tsubasa J, 1500 frames | 1.73x, 99.0 fps |

The earlier figures in this document - "0.94x, roughly 1.0-1.2x" - were wrong,
and wrong in the direction that flattered the case for a recompiler. They were
measured with three `boot_runner`s running at once against disc images on a
network share. Wall-clock is the one number here that is not deterministic.

At 1.7x the interpreter executes about 27 million guest instructions a second
(413 million in 15.06s for Wild Arms). So:

- Games run comfortably; a recompiler is **not** needed for them to run.
- [Emulation-Speed-Plan.md](Emulation-Speed-Plan.md)'s 150% setting is already
  reachable. **200% is not**, at 1.7x - and that is now the entire concrete
  justification for this project.

The profiling pass the section below asks for has also run, and is written up
in [GPU-SPU-Optimisation-Plan.md](GPU-SPU-Optimisation-Plan.md): the rasteriser
is 4-13% of a run and the SPU 3-4%, so neither is the answer either. The other
90% is the interpreter's dispatch and memory accesses plus DMA, CD, MDEC and
the timers - and splitting *that* with a sampling profiler is the honest next
measurement, because "the interpreter is 60% of the run" and "the bus is 60% of
the run" lead to completely different work.

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

- **The framebuffer checksum must not move.** `435bad9a6c5e4004` (it was
  `c7c8db90c5984798` before bug 105, which the runs below were measured against) for the BIOS
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

## Wired into the core, 2026-09-17

The recompiler now runs the machine, behind `System::EnableRecompiler` and
`boot_runner --recompiler`. **Off by default**: with it off, `StepInstruction`
is the code it always was, and the BIOS baseline below proves it.

`PSXEmu.Core/psx/recompiler_bridge.h` is the only file that knows about both
sides. It fills in the `HostInterface` the engine asks for out of `Cpu` -
memory access, an instruction fetch, the interpreter itself - and everything in
`rec/` still includes nothing from `psx/`.

### What it does

| Run | Interpreter | Recompiler |
|---|---|---|
| BIOS boot, 400 frames | 1.54x real time, 90 fps | **3.02x, 178 fps** |
| Wild Arms, 1500 frames | 1.69x real time, 99 fps | **3.89x, 229 fps** |

The BIOS run is **identical**: checksum `c7c8db90c5984798`, 305,920 of 305,920
non-black pixels, 1,157 primitives, 84,641,245 pixels plotted, the same
emulated seconds. Wild Arms draws the identical 13,099 primitives and
115,733,783 pixels with the same non-black count, but its final framebuffer
checksum differs - see the timing note below. 98% of its instructions ran
compiled, with no faults.

That answers the question this document opened with. The old measurement said
1.7x and "200% is not reachable, and that is now the entire concrete
justification for this project". It is reachable now.

### Switching it on and off

**Settings > Emulation > Recompiler**, and it can be changed while a game is running.

The setting is all the menu touches. `System::StepInstruction` compares it
against what is actually attached and acts on the difference - between
instructions, on the thread that runs the machine. That placement is the whole
of it: switching the recompiler off frees the compiled code, and doing that
from the message thread could free the block the machine is executing. Turning
it off mid-game simply leaves the interpreter to carry on from the current pc,
which is always valid because a block never returns without setting it.

Verified the only way it can be, since a menu cannot be clicked from a headless
harness: `boot_runner --recompiler-toggle N` switches CPU every N frames. Over a
400-frame BIOS boot, switching every 25 frames and again every 2 frames - 200
switches - the run is byte-identical to both pure runs: checksum
`c7c8db90c5984798`, 1,157 primitives, 84,641,245 pixels, no faults.

The setting persists as `recompiler` in the ini, and stays off by default.

### Invalidation reaches everything that writes memory, 2026-09-17

The first version of the wiring only heard about the CPU's own stores. That is
not where code comes from on this machine: **a DMA writes RAM directly**, never
touching `Cpu::Store`, and the CD channel dropping an overlay into RAM and
jumping to it is exactly how a PSX game loads code. Blocks compiled from
whatever used to be there would have gone on running, and nothing in the system
would have said otherwise.

Closed by generalising the store observer to carry a length, so a bulk writer
reports its whole range once - a transfer can be tens of thousands of words and
reporting each one would cost more than the transfer. `Cpu::NoteBulkWrite` is
the entry point, and it is now called from:

- **every DMA channel that writes RAM** - MDEC-out, GPU-to-RAM, CD-ROM, SPU-to-RAM
  and the ordering-table clear;
- **the cache-control write** at `0xFFFE0130`, which is software saying it has
  replaced code and previously only flushed the instruction cache;
- **a side-loaded PS-EXE** and **a restored save state**, both of which replace
  RAM wholesale.

Almost every such range is an ordering table or a sound buffer that no block was
ever compiled from, so the answer is a handful of bitmap lookups. It costs about
2% on Wild Arms (3.89x to 3.81x).

**A guess of mine that the measurement refuted.** Wild Arms' framebuffer
checksum differs between the two CPUs, and it streams overlays off the disc, so
this looked like the cause and was written up as the likely one. It is not: with
DMA invalidation in, Wild Arms executes *the same 492,090,429 compiled and
10,237,334 interpreted instructions as before*, and produces the same differing
checksum. It does not run DMA'd-over code in those 1,500 frames. The earlier
explanation - the cycle model, in the timing section below - is back to being
the likely one, and the honest state is that it is still unexplained.

**And a bug the stats line caught.** Adding the menu's reconciliation in
`StepInstruction` made `boot_runner --recompiler` a silent no-op: it called
`EnableRecompiler` directly while the setting stayed false, so the next
instruction switched it back off. Two verification runs "passed" that way before
the missing `rec blocks` line gave it away. `EnableRecompiler` now moves the
setting too. Worth remembering that a run reporting no statistics is a run that
did not do the thing.

### The differential harness, and what it settled, 2026-09-18

`boot_runner --recompiler-diff` runs **two whole machines**, one on each CPU,
and compares all 32 registers plus HI, LO and the pc at every block boundary.

Two machines rather than one machine twice. The sketch further down this
document was to snapshot the CPU, run a block compiled, restore, run it
interpreted and compare - but that performs every store twice, and a store into
the GPU's FIFO is not something to do twice. Two machines cost eight megabytes
and have no such problem.

**It found three things, and the first two were its own.** Both are worth
recording, because both are the kind of mistake that makes a harness confidently
wrong:

1. **Aligning by pc is wrong in a loop.** The first version stepped the
   interpreted machine "until its pc matches". In a loop the interpreted machine
   stops the first time round while the compiled one has been round five more,
   and the two are then compared in different iterations - which duly reported
   two registers differing by exactly five. It aligns by instruction count now.
2. **A load in flight is part of the state.** Compiled code writes a load's
   value out before its block ends; the interpreter still has it in the
   pipeline, landing at the start of the next instruction. Both are correct, and
   at a block boundary they look different - which they did, every time a
   block's last-but-one instruction was a load. `CaptureDiff` now applies the
   pending load before comparing. Without that fix the harness diverged after
   55,000 blocks; with it, after five million.
3. **And then the real answer.** On the BIOS the two CPUs agree for **19.2
   million instructions across 57 frames**, and the first genuine disagreement
   is a `lw` from `0x1F801814` - GPUSTAT - differing in bit 31, the even/odd
   field bit. On Wild Arms they agree for **2.87 million instructions**, and the
   first disagreement is an interrupt landing inside one machine's window and
   not the other's.

**So the CPU is not the problem, and the timing is.** Both of those divergences
are the same cause: compiled code charges its cycles in a lump at the end of a
chain where the interpreter ticks per instruction, so the devices advance at a
different granularity. Anything that reads a timing-dependent register, or takes
an interrupt at an instruction boundary, can then part company without either
CPU having computed anything wrong.

That is the answer to the question this file has been carrying since the wiring
went in: **Wild Arms' framebuffer differs because of when interrupts land, not
because a compiled instruction is wrong.** It took two wrong guesses - stale
compiled code, then a shrug at "sub-frame timing" - before something was built
that could actually tell.

The harness reports the interrupt counts on both sides and prints the block's
disassembly, so a future divergence says which of the two kinds it is without
anyone having to guess again.

### The vendored emitter is gone, 2026-09-17

`PSXEmu.Core/rec/emitter.h` replaces RecCore, and `lib/reccore` is no longer
built by anything - not the solution, not `build_tools.bat`. The directory and
its README stay as a record.

It was vendored to supply an x86 emitter, and in the end six functions of it
were used: allocate executable memory, free it, set a cursor, emit a byte, a
word, a dword. Every instruction encoding was already written by hand in
`rec/x86_extras.h`. Two of those six were wrong:

- **`destroy_block` never freed anything.** `VirtualFree(address, size,
  MEM_DECOMMIT|MEM_RELEASE)` is an invalid combination - `MEM_RELEASE` takes a
  size of zero and cannot be combined with `MEM_DECOMMIT` - so it returned
  failure every time, and the result was not checked. Measured before replacing
  it: 200 allocate/free cycles of 256 KB leaked all 51,200 KB. That was live in
  the emulator, since every cache-control write releases its arenas.
- **`emit8` was unchecked**, so running off the end of an arena would have
  quietly corrupted the block next to it.

`rec_test` now watches the process's own committed memory across 200 cycles, so
the first of those cannot come back unnoticed. Nothing about speed changed
(BIOS 3.15x, Wild Arms 3.43x - the same within run-to-run noise), which is the
expected answer: the emitter runs at compile time, and a whole BIOS boot
compiles about nine thousand blocks.

**The pages stay RWX**, deliberately. W^X by flipping page protection is
incompatible with how often linking patches emitted code - nearly two million
patches in a 1,500-frame run - and doing it properly means mapping the same
pages twice, writable at one address and executable at another. Worth doing if
something ever objects to RWX; not worth doing on principle.

### Three things the core had to grow, and no more

1. `Cpu::exceptions_raised()` - a counter. Compiled code has no pc of its own
   to check and cannot unwind, so a memory access asks afterwards whether the
   access it just made raised an exception.
2. `Cpu::set_store_observer` - called after every store the interpreter
   performs, so compiled code built from those words is thrown away. This is
   the hook that cannot live in the recompiler, as step 5 predicted.
3. `Cpu::LoadInFlight()` - so a block is never entered while a load is still on
   its way to a register.

### The bug that took the longest, and how it was found

Wired up, the BIOS booted to a black screen with zero interrupts. Rather than
read the compiler again, the search was narrowed by switching parts off:

- **Everything interpreted through the engine**: byte-identical to the
  baseline. So the bridge, the pc handling, the store observer and the ticking
  were all correct.
- **ALU and branches compiled, memory interpreted**: also identical, with 6.3
  million instructions running compiled. So allocation, linking and the
  compiled control flow were correct against real code.

That left compiled memory access, and the cause was not in the recompiler at
all. `Cpu::Load` and `Cpu::Store` test `IsBusError()` on the way in, and that
flag holds whatever the *last* address translation left behind. Every one of
the interpreter's memory instructions translates the address it is about to use
first - `Cpu::LW` opens with `AddressTranslation(virtual_address)` - so the flag
`Load` reads is about that access. Compiled code skipped that step, so the flag
described some unrelated earlier address and loads raised bus errors that never
happened. Two lines in the bridge.

The lesson is the one the whole project keeps relearning: the interpreter
carries state between its stages that is invisible until something runs without
those stages.

### Timing: the known gap

Compiled code does not tick as it goes. What a chain ran is charged afterwards
with `Cpu::TickCycles`, at **one cycle an instruction, flat**.

That is a measurement, not an assumption. The obvious refinement is wrong:
`Cpu::LW` calls `Tick()` twice where `Cpu::ADDU` calls it once, so charging
loads two looks more faithful - and tried, it runs the machine visibly fast,
with the BIOS shell drawing 435 primitives in 400 frames instead of 1,157. Flat
reproduces the interpreter's pacing on that run exactly. Which means the
interpreter's real per-instruction cost is not the number of `Tick()` calls in
its handler, and finding out what it actually is - rather than guessing a third
time - is the next piece of work.

Wild Arms is where that shows: same primitives, same pixels, different final
framebuffer. **Demonstrated, not guessed** - the differential harness above
puts the two CPUs in exact agreement for 2.87 million instructions and then
finds an interrupt landing in one machine's window and not the other's. Two
differences of the same family, both bounded by the budget of 64 instructions:
an interrupt raised inside a chain is not seen until the chain ends, and the
machine advances in bursts rather than one instruction at a time.

Closing it means making compiled code's cycle accounting match what the
interpreter actually charges - which is still unknown, since the obvious model
was measured and found wrong. That is the work, and the harness is now the tool
for checking it: a change that improves the timing should push the first
divergence later.

**So: correct enough to run, not yet proven equivalent.** The baselines are the
place to keep checking it, and the twelve-disc table is what should be run
against it next.

### The twelve-disc table, run 2026-09-25

It agrees. All twelve discs, 3,000 frames each with `--recompiler`, give the
interpreter's checksum, non-black count and resolution at all 36 checkpoints,
with no recompiler faults. That includes Wild Arms, whose checksum differed
at 1,500 frames when the timing note above was written. The one visible
difference is pacing: Area 51, Bomberman Party Edition and Captain Tsubasa J
have read one CD sector more or fewer by a checkpoint.

Compiled, each game runs 14-19% more instructions in the same frames, which is
the coarser cycle accounting above: compiled code is charged less per
instruction, so there is more of it per frame. With every picture the same,
the extra presumably went into waiting for the next frame. So the timing is
still not equivalent, and nothing in the table can see it. The runs were three
at a time, so their speed readings (2.9-3.7x real time compiled against
1.6-2.0x interpreted) are indicative, not measurements.

## Started, 2026-09-16: steps 1 to 6 are done, and block linking with them

### Step 7: block linking

Step 6 ended by saying the next thing was block linking rather than a cleverer
allocator, because blocks were too short for anything to amortise over. This is
that, and it is the largest single gain so far.

**The trick that makes it cheap.** A block already balances its own frame, so
its tail can restore the frame and then *jump* to the next block rather than
returning. At the jump the stack is exactly as it was on entry, with the
caller's return address still on top - so the next block's prologue sees what it
expects, and whichever block eventually executes `ret` returns to the dispatcher
that called the first of them. It is a tail call. No trampoline, no shared
frame, no change to how a block is entered.

**What gets linked.** A block's tail carries a `jmp rel32` per possible
successor, whose displacement the engine rewrites once that successor is
compiled. Until then it points at the block's own `ret`, so an unlinked slot
costs one predictable jump. A block that fell off its end or ended in `j`/`jal`
has one successor; a conditional branch has two, and the tail compares the
address the branch chose against the taken target to pick between them. That
second case is the one that matters - the back edge of a loop is a conditional
branch, and a loop that cannot link returns to the dispatcher every iteration.
`jr` and `jalr` go somewhere only they know, so they are not linked.

**What it bought**, best of three rounds:

| Program | no linking | linked | linking |
|---|---|---|---|
| nine-instruction arithmetic loop | 1212 M inst/s | 2562 M inst/s | **2.11x** |
| eleven-instruction loop with a load and a store | 506 M inst/s | 797 M inst/s | **1.58x** |
| fifty-instruction straight-line body | 2085 M inst/s | 2509 M inst/s | 1.20x |

The pattern is the mirror image of step 6's: linking helps *most* where blocks
are shortest, because that is where the dispatcher was the largest share of the
work. Short blocks were exactly the case the allocator could do nothing for.

**And it changed the allocator's verdict.** With linking on, allocation on the
long block goes from 1.35x to **1.57x**, and the sweep's crossover sharpens -
1.24x at fifteen instructions, 1.59x at sixty-three. Chained blocks spend less
time in dispatch, so register traffic is a larger share of what is left. The two
steps compound rather than overlap.

**Two things linking makes newly dangerous**, both of which the engine handles:

1. **A link into an invalidated block is worse than a stale block.** Dropping
   the cache entry is not enough: the host code is still there and still
   runnable, so a jump straight into it does not crash - it quietly runs the
   guest instructions the game has just replaced. Links are therefore indexed by
   the address they point *at*, and every one is sent back to its own block's
   `ret` before the block it targets is discarded. Verified by disabling it: the
   self-modifying tests then produce the pre-patch answer, and only with linking
   on - the unlinked passes stay clean.
2. **A chain would never come back.** A guest loop living entirely in compiled
   code would jump around inside itself forever and the emulator would never
   take an interrupt or draw a frame. Every block now charges its length to
   `BlockState::budget` on the way out and returns when it runs out. The host
   sets the budget to however long it can afford not to hear from the CPU - in
   the emulator, the cycles until the next scheduled event. It doubles as the
   instruction accounting, since what was set minus what is left is what ran.

That second one is worth noting as a *gain* and not only a hazard: the plan's
"Where the risk actually is" section says timing is the real danger, and the
budget is the hook that lets the host decide exactly how coarse the CPU's
timing is allowed to get.

**Verified** by running the whole differential suite over both switches in
every combination - linking off/on against allocation off/on, four passes, 452
checks - plus tests that a linked loop dispatches far less often than an
unlinked one, that the budget bounds an endless loop to its budget plus at most
one block, and that a store into a linked block takes the jumps into it apart.

### Step 6: register allocation, and what the measurement said

The plan made this step conditional - "then, and only then, register
allocation, if the measurements say the load/store traffic is what is left to
win" - so it starts with the measurement. `PSXEmu.Core/tools/rec_bench.cpp` is
that measurement, and it changed the answer.

**What was built.** Up to four guest registers per block live in host registers
for the block's duration instead of being loaded and stored around every
operation. They have to be callee-saved, because a block calls out for every
load and store: R12-R15, since RBX, RSI and RDI are already the state pointer,
the register file and the load in flight. The busiest registers win, a register
used once is never cached, only registers actually written are written back,
and an odd number of extra pushes is absorbed by the shadow space so the stack
stays aligned.

**What the measurement said**, on this machine, best of three rounds:

| Program | compiled | allocated | allocation |
|---|---|---|---|
| nine-instruction arithmetic loop | 1249 M inst/s | 1247 M inst/s | 1.00x |
| eleven-instruction loop with a load and a store | 545 M inst/s | 543 M inst/s | 1.00x |
| fifty-instruction straight-line body | 2068 M inst/s | 2785 M inst/s | **1.35x** |

So allocation does nothing for short blocks and a great deal for long ones.
Sweeping the block length says exactly where that turns over:

| Block | in memory | allocated | ratio |
|---|---|---|---|
| 7 instructions | 829 M/s | 733 M/s | **0.88x** |
| 11 | 1087 M/s | 1109 M/s | 1.02x |
| 15 | 1183 M/s | 1385 M/s | 1.17x |
| 27 | 1676 M/s | 2059 M/s | 1.23x |
| 51 | 2079 M/s | 2833 M/s | 1.36x |
| 63 | 2237 M/s | 3061 M/s | 1.37x |

**The finding, and it is the useful part of this step: allocation's cost is per
block *entry* and its saving is per *instruction*.** The pushes, the loads that
fill the cached registers and the write-backs are paid every time the block is
entered, and a seven-instruction block has nothing to amortise them over - it
runs at 0.88x, a real loss. So the allocator now refuses blocks shorter than
twelve instructions, which is the first length past break-even, and the two
short benchmarks go to exactly 1.00x while the long one keeps its 1.35x.

**Which means the next thing to do is block linking, not a better allocator.**
Real MIPS code branches every handful of instructions, so most blocks will sit
below the threshold and allocation will decline to do anything at all. Making
blocks longer - by linking them so control passes from one to the next without
returning to the dispatcher, or by following unconditional jumps while
decoding - is what would put them in the range where this pays. A cleverer
allocation policy cannot fix a block that is seven instructions long.

**The other numbers**, worth recording though they prove less: the compiled
code runs 7.7x the reference interpreter on register-only work, 3.3x when every
iteration has a load and a store, and 16-22x on a long straight-line body. That
reference interpreter is a small one written for the tests, not this project's
`Cpu`, which does more per instruction - timing, the load pipeline, interrupt
checks - so these are not predictions for the emulator. What they do say
honestly is that the emitted code's own throughput is in the range that makes
the exercise worth continuing.

**Verified** by running the entire differential suite twice, once with the
allocator off and once on - 254 checks each way. For that second pass to mean
anything the length threshold is lowered to one instruction in the tests, since
almost every test block is shorter than twelve and the pass would otherwise
compile exactly what the first one did. Checked for teeth by removing the
write-back: the second pass fails in eleven places and the first in none,
including every load-delay test, which is where an allocator bug would hide.

**One constraint this creates**, and it matters at wiring time: a load or store
callback must not modify the guest register file. It would be overwritten by
the block's write-back. `Cpu::Load` and `Cpu::Store` do not, but nothing
enforces it, so it is written down here and in `rec/runtime.h`.

### Step 5: the engine, and invalidation

`PSXEmu.Core/rec/recompiler.h` is the loop the other four steps were building
towards: look the address up, compile it if it is not there, run it, and hand
back the address to continue at. Anything the compiler cannot do is the
interpreter's, one instruction at a time, and that is not a failure mode - a
block that compiles nine of its ten instructions runs nine compiled and one
interpreted, and the tenth's address comes back from the block itself. No
remainder logic: the engine simply steps again at whatever address came back,
and the next block starts there.

The host supplies memory access, a word fetch and an interpreter through
`HostInterface`. `rec_test` fills those with a small machine of its own, the
emulator will fill them with `Cpu`'s, and `rec/` still includes nothing from
`psx/`.

**Three things in the engine are there because of a way this goes wrong**, and
each is worth more than the loop it sits in:

1. **A store can delete the block that is running.** A block that overwrites
   its own page has its cache entry removed while it is still executing, so
   host code can never be freed at the moment of invalidation. Compiled code
   lives in arenas that are only released at a safe point - the top of a later
   step, when nothing is inside one. The block keeps running to its end, which
   is what the hardware does too: those instructions are already in flight.
2. **The interpreter's stores have to invalidate as well.** Only some of a
   program's stores go through compiled code. This is the first thing in five
   steps that *cannot* be contained in `rec/`: `Recompiler::NoteStore` is a
   call `Cpu::Store` will have to make. Pretending otherwise would leave a game
   running code it had already replaced, and it would do so rarely enough to be
   a nightmare to find.
3. **A compiled block cannot be entered with a load in flight.** The compiler
   resolves the load delay slot when it compiles (step 4), so a block has no way
   of being told that the interpreter left a value on its way to a register.
   The engine asks the host before entering one, and interprets instead while
   the answer is yes - at most two instructions, and the pipeline is empty
   again. The case is not exotic: a branch whose delay slot is a load is not
   compiled at all, so the interpreter runs both and the next block starts with
   a value still arriving.

**Two smaller things.** Blocks are packed into 256 KB arenas rather than taking
a `VirtualAlloc` each, which would spend a 4 KB page on sixty bytes of code -
and doing that exposed a latent bug in step 3's compiler, which assumed a code
block's cursor started at zero and would have written the second block over the
first. An address the compiler can do nothing with is cached too, as an entry
with no code, so the engine does not decode and compile the same instruction on
every trip round a loop to reach the same answer.

**Verified** by running whole programs both ways - interpreted, then through
the engine - and comparing every register and every byte of memory: a ten-pass
loop with a store, a load and one instruction outside the subset; code that
rewrites itself; a store made by the interpreter rather than by compiled code;
the cache-control write; a load left in flight across a block boundary; an
uncompilable address compiled only once; and many blocks sharing one arena.
`rec_test` is 157 checks.

**The finding worth carrying forward is about the tests, not the code.** The
first version of the self-modifying test passed with invalidation disabled
entirely. The loop came back to an address no block started at, so a block was
compiled afresh from the patched words and the right answer appeared whether
or not anything had been thrown away. A block has to start *exactly* where the
loop returns to, or staleness cannot be observed at all. Both self-modifying
tests now put a branch in front of the loop top to force that boundary, and
both were re-checked by disabling invalidation and confirming they produce the
stale answer - along with the load-in-flight rule, which loses the loaded value
when removed.

**Still outstanding, and none of it is a surprise:** cycle accounting is not
emitted yet, compiled memory access cannot raise an exception (see step 4),
block linking is deliberately not done, and there is no register allocation.
Arena memory is only reclaimed on the cache-control write, so a page rewritten
over and over grows the emitted code until then - a real allocator is the fix
and the measurements should ask for it first.

### Step 4: memory and branches

The two things that make a block worth compiling at all - it reaches memory,
and it knows where it goes next. Both arrived the careful way.

**The emitted function changed shape.** It is now

```cpp
void block(BlockState* state);
```

with `BlockState` in `PSXEmu.Core/rec/runtime.h`: the register file, a context
pointer, six function pointers, and `next_pc`. One pointer arrives and
everything else hangs off it at a fixed offset.

**Loads and stores call out through those function pointers.** The region
decode, the timing and - later - the check that a store is landing on compiled
code all live in `Cpu::Load` and `Cpu::Store`. A second copy of the memory map
here is how a recompiler starts disagreeing with its own interpreter, so the
compiled code calls the real one. The pointers are what keeps `rec/` from
including `psx/`: the emulator will fill them with thunks at wiring time, and
`rec_test` fills them with a kilobyte of fake memory. Widths are extended in
the emitted code rather than in the callback (`movsx`/`movzx`), so a callback
stays the same shape as `Cpu::Load`.

**Branches do not branch.** Every guest branch is "one of two addresses", so
the emitted code is a compare, two immediates and a `cmov` into `next_pc`.
Nothing is patched and no host branch is emitted. Block linking - jumping
straight into the next block - stays where the plan put it: later, and only
with a measurement behind it.

**The load delay slot is resolved at compile time.** This core models the
R3000A's two-stage load pipeline properly (`Cpu::AdvanceLoadDelay`,
`WriteReg`, `ArmLoad`): the instruction after a load still sees the register's
old value, a write to that register cancels the load, and a second load to it
discards the first. Compiled code cannot run a pipeline, but it does not need
to - the compiler sees both instructions at once, so it keeps the loaded value
in a callee-saved register and emits the store *after* the next instruction,
or not at all if that instruction wrote the same register. This is the single
most likely way for a recompiler to diverge from its interpreter, and it would
do so silently.

**One rule falls out of that**: an instruction whose effect lands on the next
one is compiled only if the next one is compiled too. That covers loads, whose
value lands late, and branches, whose delay slot has to run. Otherwise the
compiled code would stop halfway through an effect the interpreter has no way
of being told about. `CompilablePrefix` computes it backwards, since whether an
instruction can be compiled depends on whether the one after it can be.

**Not compiled, deliberately:** `lwl lwr swl swr` (they read a register a load
is still in flight to, through `Cpu::ReadRegForwarded`), the branch-and-link
forms (they branch *and* write `r31`), and everything already outside the
subset.

**The calling convention is now load-bearing**, so it is tested rather than
reasoned about. Three callee-saved registers hold the state, the register file
and the in-flight load; the prologue pushes them and reserves the 32 bytes of
shadow space a callee is entitled to. Two tests pin it down: a caller written
in emitted code, because C++ gives no way to say "hold this value in RBX
across the call", which checks all three come back unchanged; and a callback
that checks the stack was 16-byte aligned at every call it received. Nothing
in a toy callback notices misalignment - a real `Cpu::Load` with aligned SSE
spills crashes, a long way from the cause.

**Verified**, on top of step 3's differential run: every width of load and
store through the callbacks; a load's value arriving one instruction late, and
the delay slot seeing the old one; a write and a second load each cancelling a
load in flight; a load landing inside a branch's delay slot; every branch form
taken and not taken across six register values including the boundaries;
`j`, `jal`, `jr`, `jalr` and `jalr` into its own source register; a branch
whose delay slot is uncompilable being left alone; and a sweep of every opcode
and `funct` checking that what `Compilable()` admits is exactly what gets
emitted - and that each admitted word agrees with the reference interpreter.
The reference now models the load pipeline too, written from `Cpu`'s three
functions rather than from the compiler. `rec_test` is 123 checks.

**Left for the wiring step, and written down so it is not discovered late:**
a compiled load or store cannot currently raise an exception. `Cpu::Load` can -
an unaligned address is an address error - and a compiled block has no way to
unwind out of the middle of itself. `BlockState::fault` is reserved for it so
that adding it later does not move every other field's offset, but until it is
handled the thunks must not raise, which means the wiring step either proves
they cannot or refuses to compile the instructions where they might. Cycle
accounting is not in the emitted code yet either; `DecodedBlock::static_cycles`
is sitting there waiting for it.

The tests were then checked for teeth, by breaking the compiler on purpose:
landing a load immediately, flipping `blez`'s condition, and narrowing a byte
store to a word. Each was caught, and the `blez` flip only at `rs == 0`, which
is the boundary it should be caught at. One test that did *not* catch its
mutation was strengthened until it did.

### Step 3: the easy third, compiled

> The signature quoted below became `void block(BlockState*)` in step 4, and
> `kRegsPtr` moved from RDX to RSI to survive a call. The rest still holds.

`PSXEmu.Core/rec/block_compiler.h` emits

```cpp
void block(uint32_t* regs);
```

for the arithmetic and logic that cannot fault, cannot branch and touches
nothing but the guest's own registers: `addu subu and or xor nor slt sltu`,
the shifts by immediate and by register, and `addiu andi ori xori lui slti
sltiu`. Everything else stops the compiled run and hands back to the
interpreter at exactly the instruction it stopped on - `CompiledBlock::compiled`
is that contract, and it is the first thing the differential harness checks.

Guest registers stay in memory and are loaded and stored around each
operation. No allocation, deliberately: that comes last, if the measurements
ask for it.

Two decisions worth keeping:

- **Trapping forms are not compiled.** `add`, `addi` and `sub` raise an
  overflow exception on the R3000A where `addu`, `addiu` and `subu` do not.
  Compiling one as its unsigned twin would silently drop an exception a game
  may depend on, so they end the compiled run instead. A test asserts exactly
  that.
- **`r0` is handled in the compiler, not by convention.** Reads of it emit
  `xor eax, eax` rather than a load, and writes to it emit nothing at all.

**Two things the vendored library could not do**, both found here and both
resolved without editing it (its README asks for that, so the diff against
upstream stays readable):

1. **Its instruction coverage is partial.** There is `ADD`, `AND`, `OR`, `MOV`,
   `CMP` and `RET` - and no `SUB`, `XOR`, `NOT`, `SHR`, `SAR`, `SETcc` or
   `MOVZX`, none of which the easy third can do without.
   `PSXEmu.Core/rec/x86_extras.h` has them, emitting through RecCore's own
   `Emitter`, so the two mix freely.
2. **The register file cannot live in RCX.** A variable shift needs its count
   in CL, which is the low byte of the register the Windows x64 convention
   delivers the argument in. A one-instruction prologue (`mov rdx, rcx`) moves
   it, which leaves ECX free to be both the second scratch and the shift count.

**Verified by a differential run**, which is the harness of the section below
in miniature: a block of 22 instruction forms is compiled, executed, and every
one of the 32 registers compared against an interpreter of the same subset
written separately from the instruction encoding rather than from the
compiler. Also: `r0` unwritable and reading zero, an uncompilable instruction
stopping the run at the right index and not executing what follows, `nop`
emitting no code at all, and every one of the 32 registers addressed at the
right offset - which is what catches an offset that is right for `r1` and
wrong for `r31`. `rec_test` was 81 checks at that point.

### Step 2: the block decoder

`PSXEmu.Core/rec/block_decoder.h` walks from an entry point to the end of a
block and records what is in it. No code is generated and nothing is executed:
this is the step where the boundary rules get debugged while a block is still
a vector anyone can print.

A block ends at, and includes, **a branch or jump and its delay slot** - the
slot runs whichever way the branch goes, so it belongs to the block that
contains the branch. It also ends at a syscall or break (execution leaves
through the exception vector, so what follows in memory is not what runs next),
at an `rfe` (what runs next is EPC), at unmapped memory, and at a length cap of
64 instructions so straight-line code does not compile into one enormous block.

Guest words arrive through a `FetchWord` callback, so the decoder has no
include from `psx/` and the tests feed it hand-assembled programs.

**The finding worth carrying forward: a block's cycle cost cannot be fully
static.** `mult` and `div` cost 6, 9, 13 or 36 cycles depending on the
magnitude of their operands (cpu_test's `muldelay` group, bug 43), which is not
knowable until the block runs. `DecodedBlock` therefore carries
`static_cycles` *and* a `dynamic_cost` flag, and the compiler will have to emit
runtime accounting for the blocks that set it. Charging a whole block at
compile time - the standard trick, and what the "Timing" section below
assumes - is only correct for blocks without one.

19 of `rec_test`'s 65 checks cover this: the cap and that the next block
resumes at exactly the right address, each jump form, the delay-slot marking,
syscall and rfe, an ordinary cop0 move *not* being mistaken for an rfe (bug 7
is what that one is defending against), loads and stores being told apart, the
edge of mapped memory, and entering at a delay slot - which is an ordinary
block start, since that is where an exception returns to.

### Step 1: the emitter

The library is vendored at `PSXEmu.Core/lib/reccore/` (upstream commit
`5f795ea`, its `Lib/` directory, with a README recording both), and
`rec_test` - 29 checks, all passing - proves the two things step 1 exists to
prove:

- **It emits x86-64 this process can call.** A block that returns 42, one that
  adds its two arguments, and one that reads and writes a struct through a
  pointer, all emitted, executed and checked.
- **The Windows x64 convention is what it looks like.** Arguments arrive in
  RCX/RDX, a 32-bit write zero-extends into the full register, and a 32-bit add
  wraps the way the guest's does.

Also written, and tested by the same harness: `PSXEmu.Core/rec/block_cache.h`,
which is step 5's invalidation machinery built first because it is the part
that goes wrong. It maps a normalised physical address to a block, knows which
4 KB pages blocks were compiled from, discards every block in a page on a store
into it, and throws everything away on the cache-control write. The three
KUSEG/KSEG0/KSEG1 views of an address are one block, and a block spanning two
pages belongs to both.

**None of it is wired into the emulator.** Nothing in `psx/` includes anything
in `rec/` or `lib/reccore/`, the solution does not build them, and only
`build_tools.bat` knows they exist. That is deliberate and worth keeping for as
long as possible: the recompiler should be switchable at the end, not gradually
entangled from the start.

What using the library taught us is in its README - it has two `struct EA`
definitions, one of them commented out, and the live one names memory operands
by strings out of a table it will happily read past the end of. Worth reading
before writing the first real block.

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

**4. Memory and branches.** *(Done - see the step 4 section above.)* Loads and
stores call the existing `Cpu::Load` / `Cpu::Store` as C functions first -
correctness before speed, and it keeps the region decode, the timing and the
store-invalidation check in one place. Branches end blocks; block linking
(patching a compiled block to jump straight to the next) comes after, and only
with a measurement behind it. The one thing this step found that the plan did
not anticipate: the load delay slot has to be resolved by the compiler, since
the emitted code has no pipeline to run.

**5. Invalidation and the cache-control write**, as described above. The cache
itself was built during step 1 (`rec/block_cache.h`); what is left is wiring it
to the emulator - which is also where the thunks onto `Cpu::Load` and
`Cpu::Store` get written, and where the exception question in step 4's section
has to be answered.

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
