# RecCore, vendored - and no longer built

A copy of `https://github.com/Noplace/RecCore` at commit
`5f795ead1bd2bcdc68e0ea19b6ff1c1c07ddd4ff` ("update to VS 2022", 2023-02-25),
taken 2026-09-16. The repository's `Lib/` directory, minus its own `.vcxproj`
and wizard `ReadMe.txt`.

**Nothing compiles this any more.** It is not in `PSXEmu.Core.vcxproj` and not
in `tools/build_tools.bat`; the recompiler uses `PSXEmu.Core/rec/emitter.h`
instead. The directory is kept because the findings below took real time to
establish and are worth having written down, and because the upstream commit is
the base for any future re-sync.

## Why it was replaced

It was vendored to supply an x86 emitter. In the end the emulator used six
functions from it - allocate a block of executable memory, free it, set a
cursor, emit a byte, a word, a dword - because every actual instruction
encoding was written by hand in `rec/x86_extras.h` against the instruction
reference. Five thousand six hundred lines of library for sixty lines of work,
with two defects in the sixty:

- **`destroy_block` never freed anything.** It called
  `VirtualFree(address, size, MEM_DECOMMIT|MEM_RELEASE)`. `MEM_RELEASE` requires
  a size of zero and cannot be combined with `MEM_DECOMMIT`, so the call failed
  with `ERROR_INVALID_PARAMETER` every time - and the return value was not
  checked, so it failed silently. Measured: 200 allocate/free cycles of 256 KB
  leaked all 51,200 KB. `rec_test` now has a check that watches the process's
  own committed memory, so this cannot come back unnoticed.
- **`emit8` was unchecked.** `*(ptr8bit + cursor++) = byte`, with nothing
  comparing the cursor against the block's size. Running off the end of an
  arena would have corrupted the block next to it silently.

Both are fixed in `rec/emitter.h`, which also keeps a note on why its pages are
RWX rather than W^X - block linking patches emitted code millions of times in a
long run, and flipping page protection around each patch would cost more than
linking saves.

## What is in here, if it is ever wanted again

An x86-64 instruction emitter, despite the `IA32` name: `intel.h` defines
`Reg64 = RAX..R15`, `ia32.h` carries `Reg64` overloads and an `emitREX`, and
there is VEX encoding for the XMM/YMM forms. Its instruction coverage is
partial - `ADD`, `AND`, `OR`, `MOV`, `CMP`, `SHL` and `RET`, with no `SUB`,
`XOR`, `NOT`, `SHR`, `SAR`, `NEG`, `SETcc`, `MOVZX`, `CALL`, `CMOVcc` or the
jumps - which is what made writing the encodings by hand the shorter path.

## Local changes

None. Nothing here was ever edited; what was missing went into
`rec/x86_extras.h` instead, so a diff against upstream is still just the
upstream.

## The rough edges found while using it

- **`EA::mode_string_to_vars` walks off the end of its table** if the form
  string is not in it: the loop is `for (auto ptr = &addressforms[0]; ptr !=
  nullptr; ++ptr)`, and that pointer is never null.
- **`addressing.h` has two `struct EA` definitions**, the first inside a comment
  block around line 258 and the live one at the bottom - a different type with
  different constructors. Reading the wrong one costs an afternoon.
- **`MemMgr::offset` is never initialised** (`mem_mgr.h`), so its `alloc` walks
  from a garbage offset. `create_block` does not use it.
- **`MOV(EA, void*)` truncates the pointer** to `int32_t` in `ia32_m.cpp` - fine
  on IA32, wrong on x64.
- **`execute_block` casts to `void(*)()`**, which is useless for a block that
  takes arguments.
