# Air Combat: the intro film decodes one cycle and then never again

`Air Combat [SLUS-00001]` boots, reads its disc, and issues real GTE and MDEC
work - this is not a crash, not an unimplemented path, not a black screen from
a disabled display. The machine runs for as long as it is given (tested to
2000 frames, about 33 seconds) with the CD-ROM still reading sectors the whole
time, and nothing on screen ever changes again after roughly frame 620.

## What is already known

Measured with `boot_runner --frame-log 100` over 2000 frames:

```
frame 600   fa600874f5eb9c10   25758 non-black  640x478  mdec 0      cd 121   gp0 61222
frame 620   aedac3154f8a0383       0 non-black  256x240  mdec 12484  cd 163   gp0 61228
frame 630   aedac3154f8a0383       0 non-black  256x240  mdec 12484  cd 182   gp0 118908
frame 700   aedac3154f8a0383       0 non-black  256x240  mdec 12484  cd 338   gp0 118908
frame 2000  aedac3154f8a0383       0 non-black  256x240  mdec 12484  cd 3244  gp0 118908
```

The resolution switch to 256x240 at frame 620 is the film starting. Between
frame 620 and 630 the GPU receives one enormous burst (61228 -> 118908 words)
and MDEC decodes 12484 macroblocks - then both counts freeze **for the rest of
the run**, 1370 frames and counting, while the CD-ROM keeps delivering sectors
(`cd` climbs from 182 to 3244) and 1884 interrupts are taken. The machine is
not stopped. It is doing something, endlessly, that produces no further
output.

`checksum aedac3154f8a0383` and `non-black 0` from frame 620 onward: the
display is not disabled (`GPUSTAT.display_disable` was checked directly), the
frame is genuinely rendered and genuinely black.

## Ruled out

**Not the runaway MDEC transfer.** `dma channel 0` fired exactly three times
in the whole run:

```
chcr 01000201  bcr 00010020  madr 80039110      32 words   pc 80024D44
chcr 01000201  bcr 00010020  madr 80039194      32 words   pc 80024D44
chcr 01000201  bcr 00000020  madr 800751BC 2097152 words   pc 80024D44
```

The third transfer's BCR has block-size=32, block-count=0 - and 0 in the
block-count field is documented (nocash, and independently DuckStation's
`GetBlockCount()`) to mean 65536, not zero. `TransferWords()` in this codebase
already applies that convention correctly for sync-mode-1 transfers; it is not
a bug in the multiply. Traced live (`--trace-at 80024AA0`), the word count
really is computed as 0 by the game's own code - a 32-bit flag word at RAM
address `800751B8` is read-modify-written twice for unrelated status bits
(0x08000000, 0x02000000), and its low 16 bits, which double as this transfer's
word count, are zero going into that dance and stay zero coming out. This
looks exactly like it produces MDEC's 12484-macroblock count too: the 2 MB
transfer re-reads a buffer that still holds **stale, previously-valid**
compressed data from an earlier real decode, so a long prefix of the 2M words
decodes as genuine macroblocks before running into actual garbage RAM and
flooding the MDEC's command stream with ~1.66M "unknown command" hits
(`mdec` stats: 1662219 commands, 1662049 unknown).

This is a real, measurable defect worth fixing eventually (a hardware-accurate
DMA does not blast an entire miscomputed transfer through in one synchronous
burst; it is request-paced, and a stalled request just leaves the channel
pending) - but clamping the transfer to something sane (patched locally as an
experiment, not committed) left the freeze at exactly the same GPU word count,
on the same frame, with `mdec macroblocks` now 0 instead of 12484. **The
runaway transfer is a symptom, not the blocker.** Something downstream of it
is stuck regardless of what MDEC receives.

**Not a stalled GPU DMA channel either - this was a genuine dead end in the
investigation itself.** `dma channel 2` never appears in `boot_runner`'s
transfer report (no entries, all run), which looks exactly like "the GPU DMA
channel never fires." It is not what it looks like. `Dma2()`'s linked-list
path (sync mode 2, which is what this game's GPU submissions use - CHCR
`01000401` decodes to bit24 start set, sync=2) returns before calling
`NoteTransfer()`, so nothing in that path is counted by the very statistics
this investigation had been reading. Instrumented directly (a temporary,
reverted diagnostic counting `ShouldStart()` rejections): zero rejections.
Every one of the 624 CHCR writes to channel 2 passes `ShouldStart()` and
walks a real linked list. The channel works. It is walking a list that
contains nothing new to draw, which is a different, real finding, not the
same one restated - see below.

**Not "waiting for a button to skip a broken video."** Pressing Start and
Cross at frames 800 and 850 (`--press start@800+10 --press cross@850+10`)
changed nothing - same checksum, same frozen GP0 count, all the way to frame
1400.

**Not an interrupt storm or a dead CPU.** 1884 interrupts taken over the run,
665 of them vblank, 702 CD-ROM, 518 DMA. The CPU is executing normally -
`--hot` shows real work happening throughout, none of it looping in a way
consistent with "nothing runs."

## What is actually happening: one produce/consume cycle, then silence

Watching RAM address `800A2BB8` (a 16-bit status field, one element of what
turned out to be a single-slot structure - the selector at `800B4034` never
takes any value but 0 in the whole run) shows its entire lifecycle:

```
pc BFC0D864 / BFC03D1C   zeroed twice during BIOS/game init
pc 80029320              zeroed again (game's own init) - state 0, "free"
pc 80029100              set to 2                        - state 2, "ready"
pc 800293A8              set to 4                         - state 4, "consumed"
```

Five writes total, over 900 frames. `800293A8`'s surrounding code
(`80029380`-`800293E8`) is a clean read-transition-and-hand-off: if the slot
reads 2, it sets it to 4 and hands back two computed pointers into what is
presumably the decoded frame data. That ran exactly once.

The hot loop that dominates CPU time from frame 620 onward (confirmed by
`--trace-skip` deep into the frozen tail, at instruction 260,000,000 of
264,070,800 total) is a bounded poll:

```
80024C8:  jal   PollSlot           ; checks the same slot for state 1, then state 2
80024D0:  beq   v0, zero, done     ; if the poll says "found it", stop
80024D4:  addiu s0, s0, -1         ; else: countdown
80024D8:  bne   s0, zero, loop     ; keep polling until the countdown reaches 0
```

`PollSlot` reads the one slot's status, checks it against 1, then against 2,
and returns non-zero either way when it is neither - which it is not, because
it is sitting at 4, already fully consumed. The countdown (`s0`, in the
low millions when sampled) never appears to reach zero within the run; either
it is a very long timeout, or something about it never actually decrements to
completion.

**The question this leaves is who is supposed to move the slot back to 0
(free) and eventually to 2 (ready) a second time, and why that never runs.**
CD sectors keep arriving the entire time (`cd` climbing steadily to 3244+
sectors by frame 2000) - the data for a second frame is presumably on the
disc and being read - but nothing converts it into a fresh decode. Whether
that is a missing interrupt, a DMA channel that should have re-armed and
did not, or a second, not-yet-found MDEC/DMA trigger site that this
investigation has not located, is where the next session should pick up.

## One buffer, two DMA channels, and why the picture goes black

DMA channel 2's MADR is written from a single, fixed PC (`80027684`) with the
identical value every time, all 624 writes: `8009D1B8`. That is not a
coincidence - it is the *same address* DMA channel 1 (MDEC-out) has been
writing decoded pixels to the whole run (all 20 of DMA1's transfers land
there too). This is a real, working technique: MDEC decodes straight into a
buffer shaped like GPU packets, and the GPU's own linked-list DMA re-reads
that same buffer to push the picture into VRAM. Nothing here is broken by
itself.

Watching the buffer's first word directly (`8009D1B8`) shows the first few
of DMA1's 20 writes landing as real pixel data (`C210C210`, repeated - a
plausible packed-pixel pattern) and every write after that landing as
`00000000`. MDEC producing zeroes on `ReadWord()` once it has nothing valid
queued is exactly what its implementation does (`mdec.cpp`'s `ReadWord()`
returns 0 past the end of what it decoded) - so this is consistent with, not
independent of, the earlier finding: MDEC decoded once, got corrupted or ran
dry, and everything downstream faithfully propagates zeroes from there. The
GPU DMA channel is not stalled; it is correctly uploading an all-black frame
it was handed, 624 times.

This closes the loop back to the same open question the first pass of this
investigation reached: **something has to feed MDEC a second, genuine chunk
of compressed data, and nothing does.**

## The wait-and-consume function works correctly - it is not the bug

This needed correcting mid-investigation, so the wrong turn is recorded
rather than erased. The hot loop at `800242C8`-`800242D8` (the one
`--trace-skip` found spinning at instruction 260,000,000, deep in the
frozen tail) calls a single function at `8002933C` that reads the slot's
status, and - contrary to an earlier reading in this same investigation
- does not just poll it. Read closely, `8002933C` through `800293E8` is
*one* function with no prologue/epilogue boundary in the middle: it checks
status against 1, then against 2, and if it finds 2, consumes it right
there (writes 4, computes two output pointers, returns 0 for success) all
inside the same call. It is a `TryConsume()`, not a `Poll()`.

Live-traced from its first entry (`--trace-at 800242C8`), that is exactly
what it does the first time: status is 2, it consumes it, writes state 4 at
instruction 175,857,486 - 23 instructions after the outer loop started -
returns 0, and the outer loop exits *immediately* via `beq v0, zero, done`.
What runs right after is not more waiting - it is real validation code
(`800242F0`-`80024434`) comparing the consumed pointers and values against
expected constants, which is what a working pipeline's post-processing
looks like. **The first cycle is not almost-working. It completes
correctly, top to bottom, exactly as designed.**

Which means the instance found stuck at instruction 260,000,000 - same
function, same loop, status still 4, never finding 1 or 2 - is a *second*,
later call to this exact sequence, entered from somewhere this
investigation has not located. `8002933C` and `800242B4` each have exactly
one static call site in the object code (confirmed by search over the
0x80020000-0x80060000 range), so that second call is not a different
place in the source calling the same function - it is the *same* call site,
executed a second time. Something invokes `800241F4` (the function
containing the `800242B4` call) more than once over the course of playback,
and this investigation has only found where the *first* of those calls
comes from - the one-shot setup chain traced earlier
(`80023B00` -> `80023BD8` -> `80023C84`). Confirmed by `--trace-at 80023B00`,
that chain runs once, at instruction 175,597,937 (proportionally around
frame 465, before the freeze). The recurring caller - whatever asks for a
*second* frame - is a different, not-yet-found call site.

## Found: the per-frame driver, and the root cause

The recurring caller from the previous section exists and was found. Traced
via `--trace-at 800242C8 --trace 20000`, once execution moves past the
one-shot init chain, a completely different call sequence repeats with
*exactly* 414 instructions between each occurrence - as regular as a
per-frame hook:

```
80024170 -> 80024B1C -> 80024D60 -> 80024E88 -> (a jalr through a
dispatch table) -> 800240AC -> 80025FA0 -> ... -> 80027B1C -> 8002C54C
-> ... -> 80027468 -> 800283BC -> 8002CF90 -> 80027BEC -> 8002C54C
```

`80024D60` is the important one - it is called every frame, and its body,
read in full, is:

```
80024D60:
  jal 80024E88            ; wait-for-DMA1-then-continue (below)
  ...
  ; --- after the call returns, unconditionally: ---
  srl s0, s0, 5            ; block count = count >> 5      (same BCR shape
  sll s0, s0, 16           ;                                as DMA0's, seen
  ori s0, s0, 0x0020        ;   earlier - block size fixed at 32)
  sw s1, 0(MADR1)           ; source address = s1 (the caller's buffer)
  sw s0, 0(BCR1)
  sw (0x01000200|...), 0(CHCR1)   ; TRIGGER DMA1 - MDEC-out, every frame
```

And `80024E88`, called first, is a bounded wait:

```
80024E88:
  v0 = *CHCR1 & 0x01000000       ; is DMA1's busy/start bit set?
  if (v0 == 0) return              ; not busy - skip the wait, skip the
                                    ;   "handle a completed transfer" call
  loop up to 0x100000 times, re-reading CHCR1, until the bit clears
  call 80024F20                    ; only reached if the bit WAS set on entry
```

`80024D60`'s own DMA1 trigger at the bottom is unconditional - it runs every
frame regardless of what `80024E88` decided. This explains why DMA1 does
fire repeatedly (20 times across the run, not once): the *output* side is
re-armed every single frame, faithfully, exactly as designed.

**DMA channel 0 - MDEC's input - is never touched by any of this.** Searched
the entire 20,000-instruction per-frame trace (dozens of repetitions of the
loop above, well past frame 620) for any reference to CHCR0 (`0x1F801088`)
or to `80024AA0`/`80023C84` (the DMA0-triggering chain identified earlier in
this document). Both appear exactly once, at instruction 175,863,859-876 -
the same single occurrence from the one-shot init chain, caught only
because this trace window happened to start early enough to still include
the tail of setup. **Not one of the dozens of per-frame cycles that follow
ever re-triggers DMA0.** The recurring driver re-arms the *output* path
(push whatever MDEC has decoded to VRAM) every frame, and never once
re-arms the *input* path (feed MDEC the next chunk of compressed data) after
the first, one-time setup call.

### Why: a busy bit that can never be observed as busy

`80024E88`'s design assumes a real, hardware pattern: trigger a DMA, and on
a later check, sometimes find it still busy - because on real hardware a
transfer of any real size takes measurable time. This codebase's DMA
completes entirely within the single `Write()` call that triggers it -
`Dma::RunChannel()` runs the whole transfer synchronously and clears the
channel's busy bit (`chcr &= 0xfeffffff`) before that same `sw` instruction's
side effects are even visible to the next instruction. No code, anywhere,
running after a transfer starts, can ever observe `CHCR & 0x01000000` as
still set - it is already clear by the time control returns.

This is consistent with, and now gives a mechanism for, the earlier finding
that clamping DMA0's runaway word count changed nothing: the freeze was
never about *how much* DMA0 transferred. It is that whatever decides
"re-arm DMA0 for the next chunk" almost certainly follows the same
trigger-then-check-busy shape `80024E88` uses for DMA1, sees the bit already
clear the instant it looks, and takes the same "nothing to do" early exit -
every single frame, forever. The exact instruction sequence that does this
for DMA0 has not been located inside the wider per-frame call tree (the
dispatch-table `jalr` calls make a flat address search unreliable - the
targets are loaded at runtime, not fixed in the object code), but the shape
of the bug no longer needs it to be found to be understood: **this project's
DMA already completes in zero observable time (bug 33 added cycle billing,
which affects pacing, but not whether the busy bit is ever seen set by
anything checking it afterward), and Air Combat's own code depends on
seeing it busy to know when to feed the decoder again.**

## What an actual fix needs

Not a targeted patch to this one game - a DMA channel needs to stay
observably busy (`CHCR` bit 24 set, reads of it reflecting that) for some
non-zero span of real, other-code-executing time after being triggered,
rather than clearing within the same synchronous call. That is a genuine
architecture change to how `Dma::RunChannel()` works, touching every
channel this project has already built extensive regression coverage
against (Legend of Mana and Wild Arms both stream FMV through MDEC/DMA0/1;
the BIOS shell itself depends on DMA2 for its textures). It needs its own
careful, incremental pass - implemented and verified channel by channel,
against the full regression suite after each one - not a quick change
folded into this investigation. The shape of that work: give each DMA
channel a "busy until tick N" state instead of completing inline, and have
`IOInterface::Tick` advance and complete transfers as cycles pass, the way
real Sync Mode 1 (block/request) transfers are paced block-by-block rather
than run to completion in one call - which several points earlier in this
investigation independently arrived at from different angles (DuckStation's
own DMA scheduler works exactly this way) before this session found the
concrete, in-game mechanism that depends on it.

## Not yet tried

- Locate the exact per-frame instruction sequence that is meant to
  re-trigger DMA0, mirroring `80024D60`'s DMA1 trigger. Not required to
  understand or fix the bug (above), but would let the eventual DMA-pacing
  fix be verified against this exact game with a concrete "did it actually
  re-arm this frame" check, rather than only against the regression suite.
- Once DMA pacing exists for even one channel, re-run this exact scenario
  and confirm the freeze actually clears - the reasoning above is strong,
  but nothing in this project ships on reasoning alone. Bugs 16 and 33 are
  the precedent: measure before and after, not just once.

## The DMA-pacing fix was built and shipped - and did not fix this

The architecture change described above ("What an actual fix needs") was
implemented: see [bug 38](Bugs-Found.md). A DMA channel's busy bit and
completion interrupt now defer to `Dma::Tick`, observable for real across
subsequent instructions, instead of clearing within the triggering write.
It is real, it is regression-verified (719 checks across every harness, the
BIOS shell checksum unchanged, Legend of Mana/Wild Arms/Ridge Racer's CD
player all identical to before), and it is a genuine improvement to the
emulator's DMA model on its own merits.

Re-testing Air Combat against it produced an **identical freeze** - same
frame range, same frozen GP0 word count. This was a real, measured result,
not an assumption: total instruction count over the same 700 frames did
change (264,070,800 before, 202,671,262 after), proving some execution path
genuinely took a different route under the new timing - but re-tracing the
stuck loop's entry point and countdown value showed it is still the same
fundamental mechanism (the wait-and-consume function at `8002933C`, called
through `800242B4`) spinning on the same never-arriving signal, just reached
by a different call than the one this investigation had built the fix
around.

**The "per-frame `80024170`" pattern, re-examined, was not per-frame.**
Section "Why: a busy bit that can never be observed as busy" above
identified `80024170` -> `80024B1C` -> `80024D60` -> `80024E88` recurring
roughly every 414 instructions and read that as a sustained per-frame
heartbeat - the natural reading, since 414 instructions is the right order
of magnitude for one frame's driver work. Deeper live tracing this session
found that reading wrong: it is a *tight burst* of several back-to-back
wait-and-consume calls, all occurring shortly after the game's first
successful decode, not a cadence that continues once play is underway. A
`--trace-skip` deep into the frozen tail (well past frame 620) shows **zero**
occurrences of `80024D60`, `80024170` or `80024B1C` there - the recurring
driver this document describes has already stopped running entirely by the
time the freeze is showing on screen; whatever is actually spinning at that
point is the single stuck call at the end of that early burst, not a
periodic retry.

That means the fix's underlying hypothesis - a per-frame retrigger gated on
DMA1's busy-bit observability - was not the actual mechanism keeping this
game stuck, even though the busy-bit gap it closes is real and was worth
closing regardless. What Air Combat is actually waiting for is whatever
should produce the *next* item at the single-slot "frame ready" flag at RAM
address `0x800A2BB8` for that specific call in the early burst to consume -
and why nothing ever does. That producer has not been identified. Locating
it - likely by tracing forward from the last successful write to
`0x800A2BB8` rather than backward from the spin loop, which is where this
session's tracing stopped - is the next concrete step, not a repeat of the
DMA-pacing work.

## The producer identified: it is the CD DMA completion handler

The previous section left "what should produce the next frame" open. It has
now been found, and most of the machinery around it mapped. Everything below
was measured against the current build, not inferred.

**The disc** lives at
`\\superserverx\D\Games\Sony\PSX\ISO\Air Combat\Air Combat [SLUS-00001].cue`
(there is no local copy), recorded here because finding it again cost a
session's worth of searching.

**The ring is 32 slots, not one.** `80029320` - read earlier as "the game's
own init zeroing the slot" - is an init *loop* that zeroes 32 entries of 32
bytes each, starting at `800A2BB8`, called from `8002901C`. The consumer
index at `800B4034` and the producer indices at `800B402C`/`800B4030` all
stay 0 for the whole run, so only slot 0 is ever used. It is a ring that
never turns, not a single slot.

**The producer at `80029100` is the DMA channel 3 (CD-ROM) completion
handler.** The game runs its own interrupt system rather than the BIOS's:

- Master dispatcher `8002C6B0`. Reads I_STAT (`1F801070`) and I_MASK, masks
  with its own enable word at `8004B718` (= `0000000D`: vblank, cdrom, dma),
  then walks 11 sources. It acknowledges each with a **16-bit `sh` of
  `~(1 << source)`** to I_STAT - our `Write16` implements that as an AND, so
  the acknowledge is correct.
- Source table at `8004B6EC`: `[0]` vblank = `8002CEE0`, `[1]` gpu = null,
  `[2]` cdrom = `8002C34C`, `[3]` dma = `8002CB98`.
- The DMA handler (`8002CB98`, body at `8002CBC0`) reads DICR's completion
  flags (bits 24-30), and for each set flag acknowledges it write-one-to-clear
  and calls a per-channel routine from a second table at `8004C790`. Our DICR
  write-one-to-clear is correct.
- Traced live, when the producer runs `s1 = 8004C79C` - **table[3]**. So
  `80029100` is what the game does when a CD-ROM DMA finishes: mark the frame
  ready, then write `0x80` to `1F801803` ("want data") to ask the drive for
  the next sector.

**The CD interrupt path works and its callbacks are installed.** `8002C34C`
fetches the pending CD interrupt through `8002AA9C` (which reads the flag at
index 1, drains the response FIFO, acknowledges with `7` and re-enables with
`7`), then dispatches to `*(800B50A8)` = `80029410` or `*(800B5004)` =
`80028F54`. Both pointers are non-null.

`8002AA9C` does not return the raw interrupt number. It switches on it through
a jump table at `8002359C`; **INT1 (data ready) lands at `8002AD84`**, which
stores a status code at `8004B688`, copies the eight response bytes to
`800B4054`, and returns **4**. So the caller's `s0 & 4` test selects
`80029410` - **`80029410` is the data-ready callback**, not the
command-acknowledge one as a first reading of the bit tests suggests.

### What the data-ready callback actually does - and does not do

Exact execution counts for the whole run (`--hot 30000`):

```
8002C6B0  master interrupt dispatcher   285
8002C34C  cd-rom handler                187
8002AA9C  fetch one pending cd irq      553
80029410  data-ready callback           179
80029498    ... its normal path         179     <- every single time
80029454    ... its "defer" path          0     <- never
8002CB98  dma handler                    21
8002CC50    ... calls a channel handler  21
80029100  the producer (dma ch3)          1
```

`80029410` reads **DMA channel 1's CHCR** (`1F801098`, reached through the
register-pointer table entry at `8004B2F8`) and tests `& 0x01000000`. That is
a *defer* check: if MDEC-out is still busy it bumps a dropped-sector counter
at `800B4024`, sets flags at `800B46D0`/`8004B330`, and returns without taking
the sector. It never takes that path - MDEC-out is never busy when asked - so
all 179 times it takes the normal path, which sets CD index 0, writes **0** to
the request register `1F801803` (clearing "want data"), and calls `8002A128`
to send the next CD command.

**It never touches a DMA register.** Traced over 1800 instructions from its
151st invocation, deep inside the freeze: zero references to `1F8010B0/B4/B8`.
The register-access totals say the same thing for the whole run - DMA3's MADR
and BCR are each written exactly **161** times, matching the 161 transfers,
every one of them during loading and none during the FMV. DMA1's CHCR is
*read* 199 times, which is this busy check and nothing more.

It is also not an error path: the drive's status byte during the stream is
`0x22` (motor on, reading), and the callback's own `& 0x04` seek-error test is
false.

So the game's FMV data-ready handler behaves as though **the sector is already
in RAM by the time it is told one is ready** - it acknowledges, releases the
buffer, and asks for the next. Nothing in the emulated run ever put it there.

### What the measurements say is actually missing

- The FMV stream starts correctly: right after the single producer call the
  game issues **`ReadS` at lba 32501** (the only ReadS in the run) with mode
  `C0` - double speed, XA-ADPCM on.
- Sectors then arrive continuously to the end of the run: **341 INT1
  data-ready** plus 25 XA audio sectors decoded, still climbing at frame 700.
  The drive is not stalled and interrupts are not lost - 596 CD interrupts are
  taken and acknowledged.
- **DMA channel 3 runs 161 times, every one of them before the FMV, and never
  again.** So the sectors arrive and are never fetched into RAM.
- The BIOS itself says so: the console log ends with **`MDEC_vlec: invalid VLC
  ID`** twice. The decoder is being handed a buffer that was never refilled.

So the loop that should sustain playback - sector arrives, DMA3 moves it,
DMA3's completion runs the producer, the producer asks for the next sector -
turns exactly once and then stops at the "DMA3 moves it" step.

### Ruled out on the emulator side

Each of these was checked directly this session and is **not** the cause:

- **DICR acknowledge.** Write-one-to-clear on bits 24-30 is implemented
  correctly, so the game's per-channel ack works.
- **I_STAT acknowledge.** The game acks with a halfword `sh`; `Write16`
  ANDs rather than assigns, which is right. (Bug 24 was this class of fault
  for the DMA registers; it is not present here.)
- **CD interrupt gating.** `DeliverPending` correctly refuses to deliver a
  queued response while one is unacknowledged, so interrupts are not lost or
  doubled.
- **The data FIFO arming.** `data_fifo_loaded_` is reset on every newly
  arriving sector, so a repeated "want data" with no intervening clear still
  re-arms for a fresh sector.
- **DMA start rejection.** A temporary diagnostic (added, measured, reverted)
  counted every CHCR write on every channel against `ShouldStart`. **Zero
  rejections anywhere in the run.** Channel 3's 289 CHCR writes are 161 real
  starts and 128 benign clears with no start bit. The emulator is not
  refusing a transfer the game asked for - the game stops asking.

## There is no FMV DMA setup - and the one completion that fires is a ghost

Logging every write to channel 3's registers, DPCR and DICR with the PC that
made it (a temporary hook, since reverted) settles what the film does with the
CD DMA: **nothing.** The last channel-3 setup in the entire run is the file
loader, walking sectors into RAM:

```
CHCR3 <- 00000000   pc 80016E8C
MADR3 <- 8004D000   pc 80016E9C
BCR3  <- 00010200   pc 80016EB0
CHCR3 <- 11000000   pc 80016EC0     <- the last channel-3 transfer, ever
```

After that the module changes and the film begins, and channel 3's MADR and
BCR are never written again. So the producer cannot have been run by a real
CD DMA completion. It was run by a stale flag.

### The ghost, step by step

The complete DICR history from the film's start:

```
DICR <- 00000000   pc 8002CB20      clear/disable everything
DICR <- 00900000   pc 8002CD90      enable ch4 + master
DICR <- 00920000   pc 8002CD90      + ch1
DICR <- 009A0000   pc 8002CD90      + ch3
DICR <- 049A0000   pc 8002CC20      ack ch2   <- ch2 is not even enabled
DICR <- 089A0000   pc 8002CC20      ack ch3   <- the single producer call
DICR <- 029A0000   pc 8002CC20      ack ch1   x20, matching DMA1's 20 transfers
```

DICR's bits 24-30 are write-one-to-clear, so `DICR <- 00000000` does **not**
clear them. Measured over the run, channel 2 and channel 3 completions latch
during BIOS-era file loading (`DICR was 8C8C0000` - flags for ch2 and ch3 both
set, from PCs inside the BIOS like `BFC07E80` and `00001ECC`) and are never
acknowledged, because that loader polls rather than using interrupts.

Those flags survive the film's `DICR <- 00000000`. The moment the film writes
`009A0000` and enables channel 3, `enabled & flagged` becomes non-zero for the
first time, the master flag in bit 31 rises, and a DMA interrupt is delivered.
The game's dispatcher walks the flags, finds ch2 and ch3 set, acknowledges
both, and calls `table[3]` - **the producer** - once, for a transfer that
never happened.

That is the single producer call. It marks slot 0 of the frame ring "ready"
with nothing behind it; the main thread consumes it, validates garbage, and
the pipeline is desynchronised from then on.

### What this is not

Interrupt delivery itself is healthy - **537 DMA interrupts reach the CPU and
only 147 are swallowed** by the rising-edge rule in `UpdateMasterFlag`. During
the film the game's DMA dispatcher runs 21 times, which is exactly DMA1's 20
completions plus this one ghost. Nothing is being lost.

### The question this leaves

Every mechanism above is, as far as psx-spx describes it, faithful: flags
latch only when their channel's enable bit is set, writing 0 does not clear
them, and the CPU interrupt is edge-triggered on bit 31. So a real console
would appear to inherit the same stale flags. Either that reading is wrong
somewhere, or the flags should never have latched during loading - which would
mean the game does not leave ch2/ch3 enabled across BIOS file reads on real
hardware, and something earlier in our run sets DICR's enable bits when it
should not. **That is the next thing to establish: who writes DICR's enable
bits before and during loading, and whether the film's `DICR <- 00000000` is
preceded on hardware by an acknowledge our run never makes.**

## Where to pick up

The freeze now has a named first domino: **a phantom channel-3 DMA completion
at the start of the film, produced by DICR flags left latched during BIOS file
loading.** Remove it and the game's frame ring is no longer poisoned before the
film has drawn a thing.

The next step is to find out where those flags come from, which is a narrow
question with a bounded answer:

1. **Log every DICR write from the start of the run**, not just the last 600
   events, along with the PC. The one measured here begins mid-load; what
   matters is who first sets the ch2/ch3 enable bits, and whether anything
   ever acknowledges a ch3 completion before the film.
2. **Then decide which side is wrong.** If the game really does enable those
   channels and leave completions unacknowledged across BIOS reads, a real
   console inherits the same stale flags and the ghost is faithful - in which
   case the fault is downstream, in what the producer does with a completion
   it should have ignored. If instead our DICR enable bits are set when
   hardware's would not be, that is a straightforward emulator bug with a
   clear test.

Request-paced ("DREQ") DMA is no longer the leading theory for this game - the
film never arms channel 3 at all, so there is nothing for a data request to
drive. It remains a real gap in the DMA model ([Gaps.md](Gaps.md)), just not
this game's gap.

Harness note: `--trace-at` now accepts `<hex>[:<n>]` to skip the first *n*
visits, which is what made the late-invocation traces above possible; the
first call of any of these routines is always the one that worked.

## Status

One real fix landed as a result of this investigation - [bug 38](Bugs-Found.md),
the DMA busy-bit/completion pacing change - and is a permanent, verified part
of the tree. It did not resolve Air Combat's freeze; see above. The DMA0
clamp mentioned earlier in this document was a separate, local, uncommitted
experiment used to rule out a hypothesis, not a fix, and was reverted. This
is a picture-only bug - it was checked against bug 36 (the CD/XA volume
mixing fix) on the chance the two were related, and they are not: this
game's silence is in MDEC's video output, not the SPU's audio mix. The
freeze itself remains open.
