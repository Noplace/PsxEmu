# Implementing the MDEC

The motion decoder is the last large component the core is missing. Everything
else in the chain works: a disc boots, a game runs, and it streams its movie off
the disc at the right rate. What it cannot do is turn those sectors into
pictures, so a game that opens with a full-motion video shows a black screen
while behaving perfectly otherwise.

Evidence it is wanted, from a 1800-frame run of Legend of Mana:

```
1F801820     0 reads   235 writes     command/parameter port
1F801824   235 reads     2 writes     status/control port
dma channel 0                          never ran a transfer
dma channel 1                          never ran a transfer
```

`grep -rn "mdec\|1F801820" PSXEmu.Core/psx` finds nothing at all.

## What it is

A fixed-function JPEG-ish decoder. Compressed macroblocks go in through DMA
channel 0, 16-bit or 24-bit pixels come out through DMA channel 1, and the game
uploads those to VRAM itself. It has no memory of a frame: it is a pipe that
decodes one macroblock at a time and knows nothing about video.

Two registers:

- `1F801820h` - write: command and parameter FIFO. Read: the data output
  response, though almost everything uses DMA1 instead.
- `1F801824h` - write: control and reset (bit 31 resets, bit 30 enables the
  DMA0 request, bit 29 enables DMA1). Read: status, and the bits that matter
  are 31 (data-out FIFO empty), 30 (data-in FIFO full), 29 (command busy), 27
  (data-out request), 26 (data-in request), 25-23 (output depth), and 15-0 (the
  number of parameter words still wanted, minus one).

Three commands, in the top byte of the first word written:

- `1` - decode macroblock. The parameter count in the low 16 bits says how many
  words of compressed data follow.
- `2` - set the quant tables. 16 words for luminance only, 32 for luminance and
  colour.
- `3` - set the IDCT table: 32 words, the 64 signed 16-bit cosine coefficients.

## The order to build it in

Each step is worth doing on its own and can be checked before the next.

### 1. The registers and the status word

`psx/mdec.h` and `psx/mdec.cpp`, a `Component` like the others, wired into
`IOInterface::Read32`/`Write32` at `0x1F801820`-`0x1F801827`. Command decode,
the parameter countdown in status bits 15-0, reset on control bit 31.

Nothing decodes yet - the data-out FIFO stays empty. This alone is worth
landing, because software polls the status word and the shape of that polling
is the first thing to get right.

**Check:** a new `mdec_test` harness. Write command 2 with 32 words and confirm
the parameter count counts down to zero and the busy bit clears.

### 2. The tables

Command 2 fills the two 64-entry quant tables; command 3 fills the 64-entry
IDCT table. Both are plain stores with a defined word order - the quant tables
arrive as bytes packed four to a word, the IDCT table as signed halfwords two
to a word.

**Check:** write the tables the BIOS uses and compare against the known values.

### 3. The bitstream decoder

The compressed data is a run-length and variable-length coded stream, MPEG-1
style. Per block: a 10-bit quantisation factor and a 10-bit signed DC
coefficient, then variable-length codes each giving a zero run and a level,
terminated by an end-of-block code. Coefficients land in zigzag order and are
scaled by the quant table entry and the block's quant factor.

This is the fiddly part and the part most worth testing in isolation, because
every later stage is downstream of it.

**Check:** feed one macroblock captured from a real stream and compare the 64
dequantised coefficients against a reference decode.

### 4. The inverse DCT

A separable 8x8 IDCT using the coefficient table from command 3. The hardware's
is not exact and games do not care, but it must saturate to signed 9-bit on
output or edges bloom.

**Check:** a DC-only block must come out flat, and a known coefficient set must
match a reference within a pixel.

### 5. Colour conversion and output

A macroblock is six blocks: Cr, Cb, then four luminance blocks making a 16x16
tile. Convert to RGB with the standard YCbCr matrix, saturate to 0-255, and
pack to whatever output depth the command asked for - 4-bit, 8-bit, 16-bit
(with the bit-15 mask flag from the command) or 24-bit.

**Check:** a grey macroblock must come out grey; a saturated Cr block must come
out red.

### 6. DMA channels 0 and 1

Channel 0 feeds the command FIFO from RAM, channel 1 drains the output to RAM.
Both are block mode, and both must respect the sync mode and the DPCR enable -
`TransferWords` and `ShouldStart` in `dma.cpp` already exist for this, so the
two new channels should go through them rather than growing their own copies.

**Check:** the end-to-end one. `boot_runner --disc <a game with an opening
movie> --frames 1800 --ppm` should stop being black.

## What to be careful of

- **The output FIFO is 32 words.** Software that reads it a word at a time
  expects the status bits to track it. Handing over an unbounded buffer works
  until a game polls bit 31 to pace itself.
- **Decode is not instant.** A game that starts a decode and immediately
  expects the busy bit set will spin forever if the whole macroblock is decoded
  inside the register write. Model it as taking time, the way the CD-ROM
  responses do.
- **Do not decode straight into VRAM.** MDEC output goes to RAM; the game
  uploads it with GP0(A0) or DMA channel 2 afterwards, and often does something
  in between.
- **The data-in FIFO can be written before the command.** Some code writes the
  parameters and then the command word.

## Where it fits

MDEC blocks full-motion video and nothing else - a game without an opening
movie should be playable without it. That makes the order of work a real
choice: the next most valuable thing is probably to run several games without
movies and fix what they trip over, then come back to this with a clearer idea
of how much else is missing.
