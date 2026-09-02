# Save states

## What a state has to contain

Everything the machine can be asked about that is not derivable. Surveyed from
the tree as it stands:

| Where | What | Size | Note |
|---|---|---|---|
| `io_.ram_buffer` | main RAM | 2 MB | heap, via `Buffer::Alloc` |
| `io_.scratchpad` | scratchpad | 1 KB | heap |
| `io_.parallel_port_buffer` | expansion | 64 KB | heap, almost always zero |
| `gpu_.vram_` | VRAM | 1 MB | heap, `uint16_t*` |
| `spu_.ram_` | sound RAM | 512 KB | heap, `uint8_t*` |
| `cpu_context_` | registers, pc, Cop0, cycles | small | plain struct |
| `gte_` | 32 data + 32 control registers | small | plain |
| `io_.io` | I_STAT, I_MASK, cache control | small | plain |
| `io_.rootcounter_[4]` | counters | small | plain |
| `io_.dma` | 7 channels, DPCR, DICR | small | plain |
| `io_.cdrom` | mode, lbas, sector buffer, FIFOs | ~2.5 KB | **three `std::deque`** |
| `io_.sio` | pad and card shift state | small | plain |
| `spu_` voices | 24 voices, ADSR, reverb | small | plain |
| `gpu_` | GPUSTAT, draw area, texpage, GP0 FIFO | small | plain |

Deliberately **not** in a state:

- `io_.bios_buffer` - 512 KB that comes from the user's dump. Store a hash of
  it instead and refuse to load a state made with a different BIOS, which is a
  real failure mode and an unhelpful one to debug from a black screen.
- `gpu_.framebuffer_` - derived from VRAM by `ResolveFramebuffer`.
- The disc - store the image path and the current LBA. A state is not a copy of
  the game.
- `mc_[2]` - the cards are files on disk with their own lifetime; see the
  memory card plan. Saving them into a state means loading one silently rewinds
  the player's saves.
- `state`, `thread`, `timer`, `timing_` - host-side, meaningless on reload.
- The statistics counters - `interrupts_taken_`, the kernel and GPU `Stats`.
  They are diagnostics, not machine state.

## The obstacle

Nothing in the core is serialisable today. `Buffer` holds three raw pointers
into one allocation, `gpu_.vram_` and `spu_.ram_` are bare `new[]`, the CD-ROM
FIFOs are `std::deque`, and `Disc` holds a `FILE*`, a `std::string` and two
`std::vector`s. A `memcpy` of `System` writes host pointers into the file and
loads them back into a different process.

So the work is mostly plumbing, and the plumbing is the part worth getting
right first.

## Order of work

### 1. A serialiser

One small class in `psx/state.h` with a single template method used for both
directions, so a field can never be saved and not loaded:

```cpp
class StateIO {
 public:
  bool saving() const;
  template <typename T> void Plain(T& value);      // trivially copyable
  void Bytes(void* data, size_t size);             // the big buffers
  template <typename T> void Deque(std::deque<T>&);
  void Str(std::string&);
};
```

Then `void Serialise(StateIO&)` on each component. One function per component,
listing its fields once. A field added later and not added to `Serialise` is
the failure mode to watch for - see the test below, which catches it.

### 2. Header, versioning and identity

```
magic     "PSXSTATE"
version   uint32, bumped whenever any Serialise changes
bios_hash 8 bytes, FNV-1a of the BIOS image
disc_path length-prefixed string, empty for no disc
payload   the components in a fixed order
```

Refuse to load on a magic, version or BIOS mismatch, and say which it was.
Versioning is cheap now and impossible to retrofit once states exist in the
wild.

### 3. Components, in dependency order

`Cpu` and `GTE` first (plain structs, nothing to trip over), then `IOInterface`
with its counters and DMA, then `Gpu` and `Spu` with their big buffers, then
`Cdrom` with its deques and its `Disc` reference by path.

`Disc` reopens from the stored path on load, seeks to the stored LBA, and
reports a clear error if the image has moved rather than running with a closed
file.

### 4. Front end

`File > Save state` and `Load state`, plus numbered slots on F1-F8 with F5/F9
for quick save and load, which is what people expect.

The location is already decided and the directory already exists:
`app.savestates_root` in `main.cpp` resolves to
`Documents\My Games\PSXEmu\savestates`, created at startup alongside
`memcards`, and is unused until this lands. States go in there as
`<disc identifier>.st<n>` - the same identifier `DiscIdentifier()` already
derives for a disc's memory card folder (the image's filename, directory and
extension stripped), so a state and a save are found under the same name.

The front end runs the machine on its own thread; a state must be taken between
frames, not part-way through one. Set a flag and act on it at the top of the
frame loop.

## How to know it works

The harness is the test, as it is for everything else:

- `boot_runner --frames 600 --save-state a.st --frames 900 --ppm a.ppm` against
  `boot_runner --frames 600 --load-state a.st --frames 300 --ppm b.ppm`. The
  two PPMs and the two framebuffer checksums must be identical. This is the
  whole test: if any field is missing from a `Serialise`, the run diverges and
  the checksum moves.
- Save, load, save again: the two state files must be byte-identical. Catches
  fields that are saved but not loaded, which the checksum test can miss when
  the field only matters later.
- A state from a different BIOS, and a state whose disc image has been moved,
  must both be refused with a message naming the reason.

## Traps

- **Do not save the framebuffer.** Rebuild it from VRAM on load, or a state
  taken mid-frame shows a torn picture that then corrects itself.
- **The CD-ROM's pending response queue holds cycle counts.** They are relative
  and must be saved as such.
- **`Buffer` must be serialised by its bytes, never by its struct** - the three
  pointers are aliases of one allocation.
- **Bump the version on every change to any `Serialise`.** A state that loads
  and is subtly wrong is worse than one that refuses.
