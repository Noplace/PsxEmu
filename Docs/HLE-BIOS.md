# HLE BIOS

**Status: working, 2026-10-09.** **Settings > Emulation > HLE BIOS** boots the machine with no BIOS
dump: the kernel is emulated in `PSXEmu.Core/bios/hle/hle_bios*.cpp`, a disc starts straight away with
no intro, and with nothing to boot it runs a shell of its own (`hle_shell.cpp`: a memory card manager
and a CD player). Every kernel function takes the time the SCPH-1001 kernel's takes, measured. With
no dump in the folder the machine boots it whatever the switch says.
`boot_runner --hle` is the same from the command line. Off by default; the real BIOS stays the
thing every baseline is measured against.

What it boots, at 2000 frames (`boot_runner --hle`): Air Combat, Metal Gear Solid, Breath of Fire
III, Ridge Racer, Wild Arms, Legend of Mana, Final Fantasy VII, Tekken 3, Crash Bandicoot 2, Vandal
Hearts, Ace Combat 3, Silent Hill and Castlevania: SotN all reach their intros or title screens,
reading the disc as they do on the real BIOS. amidog's `psxtest_cpu` gives the same picture
(`457d7a8a6e18d4b6`) as on SCPH1001. Pads work through the kernel's own pad driver (Air Combat's
Start), the recompiler runs it (same sectors, same pictures but for Ridge Racer's timing, as on a
dump), and a save state round-trips exactly (Crash 2: frame 1000 saved, loaded, run to 1800, equal
to a straight run).

## Can it be the BIOS "1:1"?

Not literally, and nothing published is. There is no public decompilation of Sony's BIOS, and one
would be Sony's code anyway. The nearest thing is **PCSX-Redux's OpenBIOS** (MIT, in the
`pcsx-redux/nugget` repository): a from-scratch rewrite of the SCPH-7001 kernel that keeps its
behaviour and its bugs, built as a ROM image with a MIPS toolchain. It needs no code here at all -
an `openbios.bin` dropped in the BIOS folder boots like any dump.

What *can* be 1:1 is everything software can see, and that is the design:

- **The kernel's RAM is the SCPH-1001 kernel's**, address for address: the vectors at 0/80h/A0h/B0h/
  C0h, the table of tables at 100h, the A0h table at 200h, the dispatchers at 5C4h/5E0h/600h, the
  B0h/C0h tables at 874h/674h, the exception handler at C80h (with the hook slots at +70h..+AFh and the
  prologue games verify and patch), the exception stack and exit structure at 6CF0h/6CF4h/75D0h, the
  DCBs at 6EE0h, the FCBs at 8648h, and the ExCBs, EvCBs, PCB and TCBs allocated from E000h to the same
  addresses (E004h, E028h, E1ECh, E1F4h). Checked against the real kernel's RAM with
  `boot_runner bios/SCPH1001.BIN --frames 120 --dis 80000C80:200`.
- **The behaviour follows psx-spx down to the bugs** - memcmp's off-by-one, memmove's extra byte,
  strstr not stepping back, strtok's skipping, the default interrupt handler's timer-2-as-timer-1
  event, OpenThread not setting SR, the syscall handler stepping EPC even in a delay slot.
- **The time things take is the kernel's**, measured - see Timing.
- **What is not 1:1**: code games reach into *inside* functions (see How it works), the boot (instant,
  by design), and the shell, which is PSXEmu's own.

## Timing

`boot_runner bios/SCPH1001.BIN --disc <game> --time-calls <csv>` records every kernel call's
arguments and the cycles from the call to the return to its caller. Ten games, 3000 frames each, gave
19 million calls (2026-10-09); `hle_bios_timing.cpp` is the table made from them - the median, or the
least where interrupts taken during a call swell the median, and for the functions whose time grows
with an argument a line through the fastest call at each size: memcpy 17 + 10.7 a byte, memset
4.3, bzero 4.0, strlen 30 a character, printf about 1,500 and then a putchar's 404 for every
character. A CD-ROM file read is 540,000 cycles to seek and settle and 225,792 a sector (double
speed); a memory card sector two frames, as the kernel's driver moves them; `_bu_init`'s two reads
and a sector through B(4Eh)/B(4Fh) take a vblank to start and another to finish.

A call costing more than 20,000 cycles is not charged to one step: once it has done its work it is held
at the `kDelay` trap until its time has passed, a 1,024-cycle slice a step, with interrupts taken
between slices, as they are while the kernel's own code runs - a file that takes a console 22 ms to
load takes 22 ms here, and the vblanks during it are counted. The boot alone is untimed.

Checked by running the same measurement under `--hle`: on Breath of Fire III, Vandal Hearts, Wild Arms
and Metal Gear Solid every function with a fixed cost lands within a cycle of the console's, and
`_bu_init` at 2.44 million cycles against 2.77.

## The shell

With no disc, or one that is not a game, the HLE BIOS runs `hle_shell.cpp`: PSXEmu's own, not a
copy of Sony's. A three-second intro (any button skips it), then a menu:

- **Memory Card** - both cards side by side, each save with its animated icon, its title (the
  filename for a Japanese one the font cannot show), its size; copy to the other card, delete, format,
  each confirmed. Changes go through the card like any write, so they reach the card's file.
- **CD Player** - the tracks of an audio CD with their lengths; play, pause, stop, previous and next,
  the track and time playing. It sends the CD-ROM controller its commands (Demute, Play, Pause, Stop,
  GetlocP) and takes its interrupts by polling, with CD audio turned up in the SPU.
- A PlayStation disc put in boots a second later, as on a console.

It draws by building a display list in RAM and sending it to the GPU by DMA, double-buffered at
VRAM y 0 and 256, with its font (5x7 glyphs of its own, and the four face buttons) and the save icons
in a texture page at x 640. It reads the pads directly, runs with interrupts off and polls for vblank,
and keeps all its state in RAM at 2000h, so a save state taken in it resumes in it.

## How it works

`bios/hle/hle_bios.h` has the design in full. In short:

- **Kernel functions are traps.** Each has an entry point in ROM at `BFC10000h + n*10h`
  (`HleBios::TrapAddress`), and `System::StepImpl` hands a step whose pc lands on one to
  `HleBios::Trap` instead of executing the word there. The tables, the exit structure, the ExCB
  elements and the DCBs all hold those addresses, so a game that reads, calls or replaces an entry
  gets what it would on a console. The recompiler never compiles the trap region.
- **What has to be code is code**: the vectors, the dispatchers and the exception handler are MIPS
  written out by `bios/mips_assembler.h`, because games read, verify and patch them.
- **All kernel state is in guest RAM** - the control blocks where the kernel keeps them, the rest in
  `KernelVars` at 1000h. A save state holds it with no format change.
- **Calling guest code from the kernel** (an event's callback, a handler in a chain) does not nest a
  CPU loop: the host function pushes what it still has to do on a continuation stack in kernel RAM,
  points `ra` at the `kResume` trap and jumps; the trap pops it and carries on (`CallGuest`,
  `Resume`, `Then`).
- **Waiting** (WaitEvent, wait_card_status, `_bu_init`'s reads) is staying on the trap: the pc does
  not move, time passes, interrupts are taken, and the next step looks again.
- **The BIOS chip is chosen at the first instruction**, not at `Initialize`: a disc's own settings
  are applied after Initialize. `System::SelectBios` / `SettleBios`.
- **Game patches** (psx-spx "BIOS Patches"): the cop0r13 and early-card patches hit the exception
  handler, laid out as the 1995 kernel's so they are harmless or work; patches inside B(5Bh)'s pad
  and card driver land in an 8 KB ROM region of their own, where stores are discarded, and the three
  entry points games fetch from it by offset (+7A0h SetPadOutput, +884h/+894h the pad enable flag)
  are traps at those offsets. The bus tells the HLE BIOS of every store to ROM (`HleBios::RomWrite`):
  zeroes over B(5Bh)+62Ch-64Fh - patch_no_pad_card_auto_ack, Resident Evil 2 and Sporting Clays -
  stop its pad handler acknowledging vblank, as the patch stops the kernel's, leaving the interrupt
  to the game's own handler; the other documented targets (+3D8h-4FFh, +594h-5BFh, +9C4h-9E3h,
  +1988h) are for code this kernel does not have, and anything else is reported as an unknown
  patch. A(9Dh)'s first two instruction words encode where the configuration lives, for Spec Ops'
  set_conf_without_realloc.
- **The character sets** are where the SCPH-1001 keeps them - charset 2 (524 symbols, kana and
  alphabets, 16x15) at BFC66000h, charset 3 (the 2,965 level-1 kanji) at BFC69D68h, charset 4 (8x15
  ASCII) at BFC7F8DEh - drawn from the Shinonome fonts (public domain, The Electronic Font Open
  Laboratory) by `tools/make_hle_font.cpp` into `hle_font_data.cpp`. Krom2RawAdd and Krom2Offset
  number them as the real kernel does, measured by calling its for every code from 8140h to 9872h:
  all 3,489 that have a glyph get the same address from both; a code with none gets -1 here, as
  psx-spx documents, where the real one returns an address of no glyph in particular.
- **What it could not do** - a function it does not have, a system error, an unknown patch - goes to
  `boot_runner`'s output, and in the emulator to the BIOS Console window as `[HLE BIOS] ...` and, the
  first three of a boot, a notification: on a console those would just be a game hanging.

## Debugging a game

    boot_runner --hle --disc <cue> --frames 2000 --call-trace calls.txt
    boot_runner bios/SCPH1001.BIN --disc <cue> --frames 2000 --call-trace real.txt

`--call-trace` writes every kernel call with its arguments and caller - from the traps under HLE,
from the A0h/B0h/C0h vectors on a dump - in the same format, so the two can be lined up. That is how
Metal Gear Solid was fixed: on the real BIOS every low-level card completion also calls a
bu_callback (A(A7h)-A(AAh)) that delivers an F4000001h event, and `_bu_init` reads sector 0 of both
slots before returning; the game's card state machine waited for both. And Wild Arms' input: it
clears the pad enable flag (through the entry point it fetched from B(5Bh)+894h), stops the pads,
`LoadAndExecute`s its next executable, which calls B(15h) again - and psx-spx's InitPad "does set the
pad_enable_flag", which the HLE one did not, so the second executable never saw a button. Under HLE, boot_runner prints
every kernel function hit and the HLE BIOS's own messages ("not implemented", system errors) at the
end of a run.

## Tests

`hle_test` (built by `build_tools.bat`) assembles small PS-EXEs with `bios/mips_assembler.h` that
call kernel functions through A0h/B0h/C0h the way a game does, boots them on the HLE BIOS and checks
what they stored: the kernel's RAM layout against SCPH-1001's, strings and memory with psx-spx's
bugs, the heap and rand, events (ready and callback), a thread switch, qsort/lsearch/bsearch through
a callback, memory card files (create, write, read back, already-exists, firstfile, delete), and
printf, Resident Evil 2's pad driver patch run as the game writes it, Krom2RawAdd's addresses and a glyph
at one, and the shell reaching its menu with no disc and no dump. **81 checks, 0 failures** on
2026-10-09.

Discs, `boot_runner --hle --frames 2000` on 2026-10-09, all reaching the disc and drawing, no HLE
message on any: Air Combat (intro), Metal Gear Solid (opening), Breath of Fire III (title), Ridge
Racer (demo), Wild Arms (intro), Legend of Mana (intro), Final Fantasy VII (intro), Tekken 3 (intro),
Crash Bandicoot 2 (menu), Vandal Hearts (title), Ace Combat 3 (intro), Silent Hill (intro FMV),
Castlevania: SotN (title). These are not baselines against the real BIOS - the HLE BIOS skips the
intro, so every game starts seconds earlier and no frame lines up - but a baseline of their own: a
change to the HLE BIOS should not move them without a reason.

## Not done

- **The character sets** are Shinonome's glyphs, not Sony's: the same characters at the same
  addresses, drawn differently. Charset 1 (accented letters, JIS X 0212, which Shinonome lacks) and
  charsets 5 and 6 are empty; no BIOS function reaches them. No game run so far has called
  Krom2RawAdd, so it is checked against the real kernel's answers, not yet on a game's screen.
- **The shell's** CD player has no shuffle or repeat, and its time is the track's, not the disc's.
- **CdAsyncReadSector** with whole-sector modes reads the 800h data only.
- **The devices' own entry points** (A(5Fh)-A(6Fh), called directly rather than through a file
  function) report themselves and return 0.
- **Timing** of the functions no measured game called is a flat 100 cycles; DeliverEvent's does not
  grow with the number of events, as the kernel's does; and the CD-ROM's seek is a flat figure, not
  the distance the head travels.
- **More tests.** `psxtest_cpu` under `--hle` against `457d7a8a6e18d4b6` is checked by hand, not by a
  harness; the low-level card driver (B(4Eh)/B(4Fh) at vblank, the bu_callbacks) only by Metal Gear
  Solid booting.

## Files

| File | What |
|---|---|
| `bios/hle/hle_bios.h` | The class, the RAM layout, the design |
| `bios/hle/hle_bios.cpp` | ROM, boot, vectors and exception handler, chains, events, threads, the A/B/C tables |
| `bios/hle/hle_bios_libc.cpp` | Strings, memory, numbers, heap, printf |
| `bios/hle/hle_bios_io.cpp` | Files (cdrom:, bu, tty), CD-ROM functions, memory card sectors, pads |
| `bios/hle/hle_bios_timing.cpp` | What each function costs, measured |
| `bios/hle/hle_shell.cpp` | The shell: intro, menu, memory card manager, CD player, its font |
| `bios/hle/hle_font_data.cpp`, `tools/make_hle_font.cpp` | The character sets, and the generator that made them from Shinonome |
| `bios/mips_assembler.h` | The few instructions the kernel's RAM code needs |
| `Sio::ExchangeDirect`, `Iso9660::List`, `Cpu::SettleLoads` | What the kernel needed from the devices |
