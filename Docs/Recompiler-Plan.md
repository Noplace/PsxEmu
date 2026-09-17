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

## Started, 2026-09-16: steps 1 to 5 are done

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
