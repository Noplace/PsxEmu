#pragma once

#include <cstdint>

// A sound device, pulled from rather than pushed to.
//
// One thread drives an engine for its whole life - host::AudioOutput's own
// (Docs/Threading-Plan.md, phase 3) - and that thread is the only caller of
// every method here, Initialize included: COM, the thread's scheduling class
// and everything the engine creates belong to the thread that opened it.
//
// The loop that drives it is always the same: WaitForRoom, ask WritableFrames,
// write exactly that many. The device's own clock paces the loop, which is why
// this replaced QueueAudio: pushing from the machine's thread meant the machine
// had to guess how much the device had room for, once a frame, and every wrong
// guess was a click (bugs 61 and 63). Pulled, the device asks for what it
// needs when it needs it, and the thread driving it fills any shortfall with
// silence - so running short is a gap, never a replay of whatever the buffer
// held last.
class IAudioEngine {
public:
    virtual ~IAudioEngine() = default;

    // Lifecycle. False if the device could not be opened.
    virtual bool Initialize(int sampleRate, int channels) = 0;
    virtual void Shutdown() = 0;

    // Playback control. Play primes the device with silence so the first real
    // samples do not arrive at an empty one.
    virtual void Play() = 0;
    virtual void Pause() = 0;

    // Blocks until the device is likely to want more, or for at most
    // `timeout_ms`. WASAPI waits on the event it signals each device period;
    // DirectSound has nothing worth waiting on, so it sleeps a few
    // milliseconds, which is its period in effect.
    virtual void WaitForRoom(int timeout_ms) = 0;

    // How many frames (stereo pairs) the device will take right now.
    virtual int WritableFrames() = 0;

    // Writes exactly `frames` frames. Never more than WritableFrames last
    // returned.
    virtual void WriteFrames(const int16_t* samples, int frames) = 0;

    // Frames written but not yet played - the device's own share of the
    // latency, for the timings readout.
    virtual int BufferedFrames() = 0;
};
