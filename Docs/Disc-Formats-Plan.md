# Disc formats: raw images, and compressed ones

## First, a correction worth making before any work starts

**A bare `.img` does load.** Tested against `Legend of Mana [SLUS-01013].img`
with no cue sheet:

```
mounted   ...\Legend of Mana [SLUS-01013].img (1 track(s), 288163 sectors)
display   vram (0,0) 320x240, enabled
non-black 76800 of 76800
mdec      84 commands, 24600 macroblocks
```

It mounts, boots, and plays its film. `Disc::Open` sends anything that is not a
`.cue`, a drive letter or a device path to `OpenImage`, and the front end's file
dialog already lists `*.img`.

So before building anything, **find out what actually failed**. The plausible
causes, and how to tell them apart in one attempt each:

1. **A different image.** The one tested is a clean multiple of 2352. An image
   that is not a multiple of 2352, 2336 or 2048 falls into a guess. Check the
   file size against those three.
2. **A disc with audio tracks.** This is the real gap - see below.
3. **The front end rather than the core.** Try the same file through
   `boot_runner --disc`. If that works and Boot disc does not, the fault is in
   `main.cpp`, not `disc.cpp`.
4. **A CloneCD set.** The tested folder also contains a `.ccd` and a `.sub`.
   The `.ccd` holds the real track layout and nothing reads it.

Ask which file, get its size, and one of these will be it. Building a format
loader for a problem that turns out to be a front-end path is wasted work.

## The real gap: a bare image has no track layout

`OpenImage` makes one assumption and it is a big one:

```cpp
// A bare image is one data track covering the whole file.
```

For a single-track game that is right. For a disc with CD-DA - and a great many
PSX games put their music on audio tracks - it is wrong in a way that cannot be
recovered from: the audio tracks are read as data, `Getstat` reports the wrong
track, `GetTN`/`GetTD` give one track where there are twelve, and any game that
plays a music track gets noise or nothing.

A cue sheet supplies that layout, which is why `.cue` works and a bare image of
the same disc does not. That is the thing worth fixing, and it is worth fixing
before any compression work because a compressed multi-track disc has the same
problem plus a container.

### Step 1: find the sidecar

When given `game.img`, look for `game.cue` and then `game.ccd` beside it and use
whichever exists. This alone probably fixes the user's case and is perhaps
thirty lines.

### Step 2: read `.ccd`

CloneCD's descriptor is an INI file: a `[Disc]` section with `TocEntries`, then
`[Entry n]` blocks carrying `Session`, `Point`, `Control`, `TrackNo` and
`PLBA`. Points `0xA0`, `0xA1` and `0xA2` are the first track, last track and
lead-out; points 1 to 99 are the tracks themselves. `Control` bit 2 says data
or audio.

It is a smaller parser than the cue reader already written, and it maps onto
the same `Track` list.

### Step 3: guess a layout when there is no sidecar at all

Only if it is wanted. A single data track is the right guess for most PSX
images and is what happens today; the honest improvement is to say so, by
reporting the assumption in the mount line rather than letting it look like
knowledge.

## Compressed images

`miniz.c` and `miniz.h` sit in `GBAEmu/Solution/GBAEmu.Core/lib/`, already used
by that project's `kernel.cpp` through `mz_zip_reader_init_file` and
`mz_zip_reader_extract_to_mem`. Copying those two files in is easy.

**But the way GBAEmu uses it does not transfer.** A GBA ROM is at most 32 MB and
gets extracted whole into memory. A PSX disc is 600 to 700 MB. Extracting one
into RAM means a 700 MB allocation, is impossible in the 32-bit configurations
this project still builds, and makes the user wait through a full decompression
before anything appears.

The deeper problem is that deflate has no random access. The CD-ROM reads
sector 4, then 16, then 67535; a zip entry has to be decompressed from the
start to reach any of those. That is not a detail to work around, it is the
reason zip is the wrong container for this.

So there are three honest options, and they are not equivalent:

### Option A: extract to a temporary file on open

Simple, correct, and works with every zip. Open `game.zip`, find the largest
entry that looks like a disc image, decompress it to the temp directory, and
mount that. Costs disk space and a wait proportional to the disc.

Perhaps a hundred lines including progress reporting. The right first step, and
possibly the only one needed: it makes `.zip` work, which is what was asked
for, and everything after it is optimisation.

The things to get right: delete the temp file on eject and on exit, handle a
zip holding a `.cue` plus its `.bin` (extract both, mount the cue), and refuse
gracefully when there is no room.

### Option B: CHD

MAME's format, and the one built for exactly this problem: the disc is split
into hunks that are compressed independently, so any sector can be reached by
decompressing one hunk. Typically half the size of the raw image with no
practical seek cost.

It needs `libchdr`, which is a real dependency rather than two files, and it
brings its own zlib/lzma/flac codecs. It is the right answer for anyone with a
large collection, and the wrong first move given zip was what was asked for.

### Option C: PBP and ECM

`.pbp` is the PSP's EBOOT container and very common for PSX; it is hunk-indexed
like CHD and self-contained. `.ecm` only strips the ECC bytes and is sequential,
so it has option A's problem without option A's simplicity.

### Recommendation

Do option A. It answers the request, reuses miniz as suggested, and is a day
rather than a week. Treat CHD as a separate decision once there is a reason to
want it - and if it is ever done, note that a hunk cache in `Disc::ReadSector`
would serve CHD, PBP and a future format equally, which argues for putting the
seam there rather than in the zip reader.

## Where the seam goes

`Disc` already abstracts a `Source` with a file handle or a device handle and a
sector size. Everything above it - the CD-ROM, the filesystem reader, the boot
path - goes through `Disc::ReadSector` and knows nothing about files.

That is the right place for all of this. A compressed source becomes another
kind of `Source`; a sidecar becomes another way of filling the `Track` list.
Nothing outside `disc.cpp` should have to change, and if something does, that is
a sign the seam is being cut in the wrong place.

## How to know it works

`media_test` already covers sector layout detection and the 150-sector lead-in,
and it is where this belongs:

- A generated multi-track image with a cue, mounted from the cue and mounted
  from the bare image, must report the same track count and the same track
  types. This is the check that fails today.
- The same image zipped must give byte-identical sectors to the unzipped one,
  at the start, the middle and the end.
- A `.ccd` and its `.cue` describing the same disc must produce the same tracks.
- A truncated or corrupt archive must fail the mount with a message, not crash
  and not mount something half-formed.

The end-to-end check is the one that matters: `boot_runner --disc game.zip`
must reach the same framebuffer checksum as `--disc game.cue`. If it does not,
the container is changing what the machine sees, which is the whole thing this
must not do.
