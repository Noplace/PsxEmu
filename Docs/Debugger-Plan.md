# A debugger

**Status: phases 0-2 done, 2026-09-19** (bugs 71-73).
- **Phase 0, the core:** `psx/debugger.h` - execute breakpoints, step
  into/over/out, run to, break, the machine halting mid-frame, `debug_test` and
  `boot_runner --break`. It passed the determinism test below.
- **Phase 1, the window:** Emulation > Debugger - disassembly, registers with
  the load delay shown, the breakpoint list, and the controls. The
  disassembler moved to `psx/disasm.h`.

- **Phase 2, memory:** a memory pane reading through a side-effect-free
  peek (`Debugger::PeekData`), writing RAM and the scratchpad, and editing
  registers while halted (bug 73).

Phases 3-4, watchpoints onward, are not started. The rest of this document is
the plan as proposed; where a phase came out differently, its bug entry says
how.

## The conclusion first

A debugger here is mostly plumbing that already exists, joined up. The machine
already runs one instruction at a time through `System::StepInstruction`, which
now has a pre-step hook for BIOS calls (bug 66) - a breakpoint check is the same
hook. The machine thread already pauses for reasons (`host::PauseReason`) and
answers requests while paused. `tools/disasm.h` already disassembles, the store
observer already hears every CPU store, and the DMA channels already report
every range they write (`Cpu::NoteBulkWrite`, for the recompiler). What is
missing is: stopping *mid-frame*, a place to keep breakpoints, and a window.

Three decisions shape everything else, and are the ones worth arguing with:

1. **The debugger never reads the machine directly.** The window is the UI
   thread's; `System` is the machine thread's (Threading-Plan.md, rule 1). The
   window is shown snapshots - registers, a page of disassembly, a page of
   memory - taken on the machine thread when it halts, and every action (step,
   continue, set a register, poke memory) is a request. Exactly how the memory
   card editor works (bug 69).
2. **While anything is armed, the machine runs interpreted.** The recompiler
   runs a chain of blocks per step and cannot stop between two instructions of
   it. DuckStation makes the same choice: breakpoints switch it to the
   interpreter. When the last breakpoint is cleared and nothing is stepping,
   compiled code comes back.
3. **A halt is a pause reason, and halting mid-frame is allowed.**
   `Machine::RunOneFrame` returns early when the debugger says stop; the half
   frame is not published, sound runs dry to silence (the audio thread already
   fills gaps), and video keeps showing the last whole frame. Continue resumes
   the same frame from the same instruction.

## What it is for

Everything in Bugs-Found.md was found with `boot_runner`'s `--trace`,
`--trace-at`, `--hot`, `--dis` and `--watch-ram`, one run at a time. That works
and should keep working. The debugger is for what a batch run is bad at: poking
at a game *as it runs*, from the front end, without guessing an instruction
count first - "stop when this address is written", "what called this", "step
over this function and show me what it returned".

## What it shows

**Phase 1 - the CPU:**

- **Disassembly** around the pc - a scrolling list, the current instruction
  marked, breakpoints marked, branch targets clickable. From `tools/disasm.h`,
  which moves into Core (`psx/disasm.h`) since two users now need it.
- **Registers** - the 32 general registers by name, HI/LO, the pc, and the
  Cop0 registers that matter (SR, Cause, EPC, BadVaddr). A load still in flight
  is shown as such, since the register it will write does not have its value
  yet - the load delay is the thing most likely to confuse anyone reading a
  register view of this CPU.
- **Controls** - Continue (F5), Break (Ctrl+Break), Step Into (F11), Step Over
  (F10), Step Out (Shift+F11), Run to Cursor.

**Phase 2 - memory:** a hex and ASCII view of any address - RAM, scratchpad,
BIOS, the hardware registers - with go-to, follow-a-pointer, and editing.
Hardware registers are read *without side effects*: a normal read of the CD-ROM
response FIFO pops it, so the view reads through a peek path, or shows the
register as "not safe to read" where no peek exists.

**Phase 3 - watchpoints:** stop on a read or write of an address range. CPU
accesses go through `Cpu::Load`/`Cpu::Store` already; DMA writes through
`NoteBulkWrite`. A watchpoint that DMA trips reports which channel wrote, which
is the question bug 24 and bug 55 both had to answer by hand.

**Phase 4 - the rest, each small once the above exists:**

- **BIOS call log** - `Kernel::Call` already counts every A0h/B0h/C0h call; the
  debugger can list the last few hundred with their arguments, and break on a
  particular one ("stop at the next `open`").
- **A call stack, approximate.** MIPS keeps no frame chain; what can be shown is
  the `jal`s taken and not yet returned from, recorded while stepping or while
  armed. Labelled as a guess, because a `jr ra` from a longjmp or a hand-written
  return confuses it.
- **Device panes** - GPU (VRAM viewer exists: View VRAM), CD-ROM (the
  command/response log `boot_runner` already prints), SPU voices, timers, DMA.
  Read-only snapshots, the same way.
- **Labels** - PSX discs carry no symbols. Allow naming an address by hand, and
  loading PsyQ `.SYM` files where a homebrew or a leaked build has one.

## How it works

### The core: `psx/debugger.h`

Lives in `System`, owned by the machine thread like every other component.

- **Breakpoints** - execute, read, write; each an address (or range), enabled or
  not, with a hit count. Execute breakpoints are checked in
  `System::StepInstruction` at the pre-step hook, *after* any interrupt has
  moved the pc - the same place and reason as the BIOS-call hook, so a
  breakpoint on an exception vector works. Read and write in `Cpu::Load`/`Store`
  and at the DMA write reports.
- **Step state** - into (stop after one instruction), over (a temporary execute
  breakpoint after a `jal`'s delay slot), out (stop at the next `jr ra` whose
  stack depth matches), to cursor (a temporary execute breakpoint).
- **The cost when idle has to be zero.** One `bool` - "is anything armed" -
  checked per instruction, predicted not-taken. Measured against the BIOS boot's
  wall-clock before and after; the baselines must not move at all, since an idle
  debugger changes nothing the machine computes.

**A known limit, stated up front:** this core runs a branch and its delay slot
as one step (`Cpu::Jump` executes the slot inside the branch). So a breakpoint
*on* a delay-slot instruction fires only when that instruction is reached some
other way, and Step Into over a branch lands after the slot. DuckStation steps
the slot separately; matching that means splitting `Jump`, which touches the
CPU's core timing path and is not worth doing for this. The window marks delay
slots so the limit is visible rather than surprising.

### Halting mid-frame

`Machine::RunOneFrame` loops `StepInstruction` until the GPU's frame counter
moves. It gains one more exit: the debugger asked to halt. `Run` then sets
`kPausedByDebugger`, does *not* publish the unfinished frame, and posts a
snapshot to the UI. Continue clears the reason, and the next `RunOneFrame`
carries on to the same frame boundary it was heading for.

The instruction-count guard (`kMaxInstructionsPerFrame`) has to count across a
halt, not restart, or stepping a frame one instruction at a time would reset it
eight million times.

### The recompiler

While any breakpoint is enabled or a step is in progress, `StepInstruction`
takes the interpreter path. The recompiler's code cache is kept, not freed - it
is still valid, since the interpreter keeps invalidating it as memory changes -
so clearing the last breakpoint returns to full speed at once.

### Snapshots and requests

After a halt, and on request while halted, the machine thread posts:

- the register file and the load in flight,
- 64 instructions of disassembly around the pc (and around wherever the window
  has scrolled to),
- the memory page the view is showing,
- the breakpoint list with hit counts.

Edits - a register, a memory byte, a breakpoint - are requests, applied between
instructions while halted. Writing memory goes through the same store path a
CPU store does, so the recompiler invalidates what it must.

While running, the window shows the last halt's state greyed, with a Break
button. A live-updating register view of a machine running at 60 frames a
second would be noise.

## How to know it is right

- **Determinism is the test that matters.** A run that halts at a breakpoint
  N times and continues each time must end on exactly the same instruction count
  and framebuffer checksum as a run that never halted - the BIOS boot's
  `c7c8db90c5984798` and the twelve-disc table. A debugger that changes what the
  machine computes is worse than none.
- **A `debug_test` harness**, headless, like the rest: breakpoints firing before
  the instruction runs, on an exception vector, disabled ones not firing; step
  into/over/out landing where they should, including across a `jal`, a `jalr`,
  a branch-likely-shaped loop, and an interrupt taken mid-step; read and write
  watchpoints from the CPU and from each DMA channel that writes RAM; hit
  counts.
- **`boot_runner --break <addr>`** printing the registers and a disassembly
  window at each hit, so the core half can be used and tested with no window at
  all - and so it is still useful in a batch run.
- **The window** checked the way the editor and the console were: driven by
  posted commands, read back from outside the process.

## Phases

| # | What | Size |
|---|---|---|
| 0 | Core: `psx/debugger.h`, execute breakpoints, halt mid-frame, step into/over/out, interpreter-while-armed; `debug_test`; `boot_runner --break` | **done** (bug 71) |
| 1 | Window: disassembly, registers, controls, breakpoint list | **done** (bug 72) |
| 2 | Memory view and editing, with side-effect-free peeks | **done** (bug 73) |
| 3 | Read/write watchpoints, CPU and DMA | half a day |
| 4 | BIOS call log, approximate call stack, labels, device panes | open-ended, each piece small |

Phase 0 alone is useful - `boot_runner --break` is a better `--trace-at` - and
nothing after it is started until the determinism test passes.

## What this will not do

- **No GDB remote protocol**, at least not first. It would give a real
  debugger's UI for free, but it is its own project and the in-app window is
  what is being asked for.
- **No rewind.** Save states already give a manual version of it.
- **No GPU command debugger** in the sense RenderDoc is one. The device panes
  show state; stepping through a display list is a later, separate question.
