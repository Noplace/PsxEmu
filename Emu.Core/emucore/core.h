// What the machine's thread asks of a console (Docs/Shared-Libraries-Plan.md, phase 4).
//
// host::Machine runs a console a frame at a time on a thread of its own - requests between
// frames, input in, the picture and the sound out, pacing, run-ahead, rewind, the debugger's
// halts - and knows nothing of which console it is. This is all it knows: each console has one
// implementation (PSXEmu: psx::PsxCore, in PSXEmu.Core's emu/psx_core.h), which is the only thing
// that calls into the console for it. Every call is on the machine's thread.
//
// Whatever a console has beyond this - a rasteriser to choose, textures to replace, a debugger's
// breakpoints - its front end asks of its own implementation directly, in a request
// (Machine::Post), which runs on the machine's thread between frames.
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace emulation {

namespace host {
struct MachineReport;
struct VideoFrame;
}  // namespace host

// A state kept in memory, for run-ahead and rewind. Made by the core (ICore::NewMemoryState),
// which keeps in it whatever it needs beside the payload; taking another into the same one
// allocates nothing.
class MemoryState {
 public:
  virtual ~MemoryState() = default;
  // The machine's state, serialised: what rewind's store keeps (state::RewindBuffer), and fills
  // again to step back to.
  virtual std::vector<uint8_t>* payload() = 0;
};

class ICore {
 public:
  virtual ~ICore() = default;

  // ---- Running -------------------------------------------------------------------------------

  // Runs until the console's frame ends: one picture's worth, which is when its picture is
  // finished. `steps` is how many steps this frame has run so far - kept by the caller across a
  // halt - and running stops once it reaches `max_steps`, since a console whose display is off
  // never ends a frame. False if the debugger halted it first, mid-frame, before a step it did
  // not run: the next call carries on towards the same frame's end.
  virtual bool RunFrame(uint64_t* steps, uint64_t max_steps) = 0;

  // The debugger: whether it may stop the machine at all - while it may, run-ahead and rewind
  // are off, since a halt could fall in a frame that never happens - and whether it has.
  virtual bool debugger_armed() const = 0;
  virtual bool halted() const = 0;

  // Whether frames run ahead fit in a frame's time (PSXEmu: only with the recompiler).
  virtual bool RunAheadFits() const = 0;

  // The emulated display's rate, now; and which picture it is on, counting from 1 - for NVIDIA
  // Reflex's markers (host/latency_markers.h).
  virtual double refresh_hz() const = 0;
  virtual uint32_t picture_number() const = 0;

  // ---- Out -----------------------------------------------------------------------------------

  // The frame just finished: its picture in `pixels`, or left on the graphics card in `shared`,
  // or, with `raw_view`, the console's video memory as it holds it in `raw` - and its width and
  // height. The machine numbers it and hands it over.
  virtual void FillFrame(host::VideoFrame* frame, bool raw_view) = 0;

  // Sound, interleaved stereo at sample_rate() frames a second of emulated time: up to
  // `max_frames` of what has been made, and how many. 0 once there is none.
  virtual int sample_rate() const = 0;
  virtual int ReadSamples(int16_t* stereo, int max_frames) = 0;

  // What the console adds to the machine's report, about once a second (the rasteriser, its
  // textures). Nothing by default.
  virtual void FillReport(host::MachineReport* report) const { (void)report; }

  // ---- States in memory ----------------------------------------------------------------------

  virtual std::unique_ptr<MemoryState> NewMemoryState() = 0;

  // Run-ahead: the real frame kept before the frames ahead run - with whatever the console
  // saves to, since a save the frames ahead make is on input not given yet - and loaded back
  // after them, carrying on from what was last drawn. False if the load could not be made.
  virtual void SaveBeforeRunAhead(MemoryState* state) = 0;
  virtual bool LoadAfterRunAhead(const MemoryState& state) = 0;

  // Rewind: a state to step back to, without what the console saves to, which a step back
  // leaves as it is - as loading a save state does. False if the load could not be made.
  virtual void SaveForRewind(MemoryState* state) = 0;
  virtual bool LoadForRewind(const MemoryState& state) = 0;

  // ---- Saves ---------------------------------------------------------------------------------

  // The end of every real frame - after run-ahead's load back, so never with a frame ahead's
  // write in: what a console writes behind (memory cards, cartridge RAM) goes out when it is due.
  virtual void OnFrameEnd() = 0;
  // Pausing: anything written behind goes out now, not a second of running later.
  virtual void FlushSaves() = 0;
};

}  // namespace emulation
