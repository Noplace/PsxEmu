# RecCore, vendored

A copy of `https://github.com/Noplace/RecCore` at commit
`5f795ead1bd2bcdc68e0ea19b6ff1c1c07ddd4ff` ("update to VS 2022", 2023-02-25),
taken 2026-09-16. The repository's `Lib/` directory, minus its own `.vcxproj`
and wizard `ReadMe.txt`.

**Copied, not depended on.** No submodule and no include path pointing outside
this tree - an include path that no longer resolved is what made the old Game
Boy recompiler unbuildable, and it is why `Docs/Recompiler-Plan.md` spent a
section wondering whether the library still existed. To re-sync, clone the
repository again and diff `Lib/` against this directory; the commit above is
the base.

## What it is

An x86 instruction emitter. `reccore::Emitter` owns a block of
`PAGE_EXECUTE_READWRITE` memory and an emit cursor; `reccore::intel::IA32` is
a method per mnemonic that encodes into it.

Despite the name, **it emits x86-64**: `intel.h` defines
`Reg64 = RAX..RDI, R8..R15` beside `Reg32`, `ia32.h` carries `Reg64` overloads
and an `emitREX` that encodes the high register bits into the prefix, and
there is VEX encoding for the XMM/YMM forms.

## Local changes

None, and the rule held through step 4 of the plan: what was missing went into
`PSXEmu.Core/rec/x86_extras.h` instead. Keep it that way where possible - the
diff against upstream stays readable - and if a change here ever becomes
unavoidable, note it in this list with the reason.

**Its instruction coverage is partial**, which is the thing to know before
planning work around it. Present: `ADD`, `AND`, `OR`, `MOV`, `CMP`, `SHL`
(one form), `RET`. Absent, and needed even for simple integer code: `SUB`,
`XOR`, `NOT`, `SHR`, `SAR`, `NEG`, `SETcc`, `MOVZX` - and, once a block can
call out and branch, `CALL`, `CMOVcc`, `PUSH`/`POP` and `MOVSX`. Those are in
`rec/x86_extras.h`, emitting through this library's own `Emitter`, so the two
mix freely in one block.

## How to use it, since the API has two of everything

`addressing.h` contains **two** `struct EA` definitions. The first, around line
258, is inside a comment block; the live one is at the bottom of the file and
is a different type with different constructors. Reading the wrong one costs an
afternoon:

```cpp
Emitter emitter;
CodeBlock* block = emitter.create_block(64);
emitter.set_block(block);
IA32 assembler(&emitter);

assembler.MOV(EAX, 42u);                          // register, immediate
assembler.ADD(EAX, EA(static_cast<uint8_t>(EDX))); // register, register
assembler.MOV(EAX, EA("[RCX]"));                   // register, memory
assembler.RET();
reinterpret_cast<int(*)()>(block->address)();
```

- **A register operand becomes an `EA`** through its one-byte constructor,
  which is the register-direct form (mod=3). Passing a register enum on its own
  where an `EA` is wanted is ambiguous - the enum converts to both a register
  and an immediate - so the cast is what says which was meant.
- **A memory operand is named by a string** out of the `addressforms` table:
  `"[RCX]"`, `"[RAX+RCX*4]"` and so on. There is no form carrying a
  displacement, so build one by hand: take `EA("[RCX]")`, set `mod` to 1 (an
  8-bit displacement follows) or 2 (32-bit), and set `displacement`.
- **x64 register operands need the REX-aware overloads** (`Reg64`), which the
  library supplies; the 32-bit forms zero-extend into the full register the way
  the hardware does.

## Known rough edges

Found while proving it out in `rec_test`, and worth knowing before trusting a
part of it that nothing has exercised yet:

- **`MemMgr::offset` is never initialised** (`mem_mgr.h`), so its `alloc` walks
  from a garbage offset. `Emitter::create_block` does not use it - it calls
  `VirtualAlloc` per block - so nothing here touches it, but do not reach for
  `MemMgr` without fixing that first.
- **`Emitter::execute_block` casts to `void(*)()`**, which is fine for a
  smoke test and useless for a block that takes arguments. `rec_test` casts the
  block's address itself instead, which is what a recompiler will do anyway.
- **Pages are allocated RWX.** That is the simple thing and it works; a later
  pass may want W^X (write, then `VirtualProtect` to execute) if only to stop
  the emulator tripping exploit-protection policies on someone's machine.
- **`EA::mode_string_to_vars` walks off the end of its table** if the form
  string is not in it: the loop is `for (auto ptr = &addressforms[0]; ptr !=
  nullptr; ++ptr)`, and that pointer is never null, so a typo in a form string
  reads past the array until something compares equal. Use only strings that
  exist in `addressforms`, and treat a mistyped one as undefined behaviour
  rather than an error you will be told about.
- **`MOV(EA, void*)` truncates the pointer** - `ia32_m.cpp` casts it to
  `int32_t` (warning C4311 when built here). Fine on IA32, wrong on x64: load a
  64-bit address through `MOV(Reg64, uint64_t)` instead.
