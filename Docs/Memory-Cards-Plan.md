# Memory cards: swapping, and an editor

Two related pieces of work. Swapping is small and unblocks real use; the editor
is bigger and builds on the same parsing.

## Where it stands

`psx/mc.h` already declares the whole on-card format, which is most of the hard
part of an editor:

- `MCBlock0` - the header frame, 15 `MCDirectoryFrame`s, the broken-sector
  table and its replacements.
- `MCDirectoryFrame` - `block_alloc_state`, `filesize`, `next_block`,
  `filename[21]`, checksum.
- `MCTitleFrame` - the Shift-JIS title, the icon display flag, the block count
  and a 16-entry icon palette.
- `MCBlock` - a title frame and 63 data frames.

`MC` itself loads and creates 128 KB files, reads and writes 128-byte sectors,
and carries the `flag_` byte the SIO layer reports. What it does not do is
understand any of it: nothing walks the directory, follows a block chain,
decodes a title or an icon, or computes a checksum.

Three things to fix while in here regardless of which feature comes first:

- **`WriteSector` opens, seeks, writes and closes the file for every 128-byte
  sector.** A game saving a block does that 64 times. It is slow, and a crash
  part-way through leaves a half-written card. Hold the card in memory, mark it
  dirty, and flush on a timer, on eject and on exit.
- **`CreateFile` writes 128 KB of zeroes.** That is not a formatted card - the
  BIOS will offer to format it, which works but is a poor first impression.
  Write a properly formatted empty card: `"MC"` and the checksum in the header
  frame, all fifteen directory frames marked free, `0xFFFF` next-block links.
- **`LoadFile` returning `S_FALSE` for anything not exactly 0x20000 bytes** is
  right, but the front end reports it as "must be 128KB" for a missing file too.
  Separate the two.

## Part one: swapping cards - default cards done, manual swapping still open

`main.cpp` now loads or creates a disc's cards automatically on every cold
boot - `LoadOrCreateMemoryCardsForDisc()`, called from `BootDiscFromFile` and
from the command-line disc argument, never from `Swap disc`. Point 4 below is
therefore done, and done slightly differently from how it reads: per-disc
rather than one shared pair, under `Documents\My Games\PSXEmu\memcards\` -
GBAEmu's own save-directory convention - rather than beside the executable.
`DiscIdentifier()` derives the folder name from the disc image's filename, not
from anything on the disc, so it works uniformly whether or not the disc
carries a SYSTEM.CNF.

That the mechanism is already there is still true for the rest of this
section - `flag_` bit 3 does exactly what a swap needs - and everything below
is still open:

1. **Eject.** There is no way to disconnect a card while the machine is
   running. Add `MC::Eject()` that flushes, frees `mcfile` and leaves
   `connected()` false, so the SIO layer reports no card in that slot. Games
   handle an empty slot; they do not handle a card that changes underneath
   them without the flag. `System::Deinitialize()` already does the
   equivalent on a cold boot - which is what makes the auto-load safe to call
   unconditionally today - but nothing does it for a card the player wants to
   change mid-session.
2. **Per-slot menu.** A `Memory Cards` submenu, per slot: Insert, Eject,
   Create, and a Recent list. Slot 1 and slot 2 independently. `Open Memory
   Card...` and `Create Memory Card...` already exist for a manual override of
   the automatic per-disc card; this is about exposing eject and a browsable
   history of the per-disc folders alongside them.
3. **Insert while running must go through eject.** Swapping in place without
   clearing `connected()` first leaves a game holding a directory that no
   longer describes the card. Eject, then insert, then set the flag.

Small, self-contained, and worth doing before the editor.

## Part two: the editor

A dialog listing what is on a card, and the operations people actually want.

### Reading a card

- **Directory.** Walk the fifteen frames. `block_alloc_state` says whether a
  frame starts a save (`0x51`), continues one (`0x52`, `0x53`), or is free
  (`0xA0` and friends). Follow `next_block` to get the block chain and so the
  real size of each save.
- **Name.** `filename[21]` is the region and product code plus the game's own
  identifier, e.g. `BASLUS-01013LOM`. Show it, but show the title too.
- **Title.** `MCTitleFrame::title_shift_jis[64]`, Shift-JIS, needs converting
  to UTF-16 for display. `MultiByteToWideChar` with code page 932 does it on
  Windows and is the right call here rather than a hand-rolled table.
- **Icon.** 16x16, 4 bits per pixel, through the 16-entry `icon_pallete` which
  is 15-bit BGR like VRAM. One to three frames, animated; `icon_display_flag`
  says how many. Static first frame is enough to be useful.
- **Free space.** Count free directory frames; a card holds fifteen blocks.

### Operations

In the order they earn their keep:

1. **List** - icon, title, filename, blocks used. Read-only, and already worth
   having.
2. **Delete** - mark the chain's frames free. The BIOS does this by setting the
   top nibble of `block_alloc_state`; the data stays, which is why undelete is
   possible.
3. **Undelete** - a deleted save whose blocks have not been reused can be put
   back. Cheap once delete exists, and people want it.
4. **Export / import a single save** - `.mcs` (the raw block chain with its
   directory frame) is the common interchange format. Import needs to find a
   free run of blocks and rebuild the chain.
5. **Copy between slots** - both cards open at once, drag or a button.
6. **Format** - write a clean formatted card.

### Checksums

Every frame's last byte is the XOR of the preceding 127. Nothing in the core
computes it today. Any write from the editor must recompute it for every frame
it touches, or the BIOS declares the card corrupt on the next boot. One
function, used everywhere:

```cpp
uint8_t FrameChecksum(const uint8_t* frame);   // XOR of bytes 0..126
```

### Where it lives

The parsing belongs in the core - `psx/mc_directory.h/.cpp`, operating on a
`MCFile` - so it can be tested headlessly and so a future front end on another
platform gets it for free. The dialog belongs in `PSXEmu.Win32`.

### How to know it works

A `mc_test` harness alongside the others:

- Format a card, and the checksums must match what the BIOS computes.
- Import a known `.mcs`, list it, and the title and block count must match.
- Delete then undelete must return the card byte-for-byte to its previous
  state.
- Round-trip: export every save from a card, format it, import them all back,
  and the directory must describe the same saves.
- The end-to-end one: boot a game, save in it, and the editor must list the
  save with the right title.

## Order

Swapping first - it is small, it unblocks playing with two cards, and the eject
and flush work it needs is a prerequisite for the editor writing to a card that
a game also has open. Then the read-only listing, which is most of the value
for a fraction of the risk. Then delete, export and import.

The one thing to decide early: **the editor and a running game must not both
write the same card.** Simplest answer is to eject the card while its editor is
open and re-insert on close, which is also what the hardware would make you do.
