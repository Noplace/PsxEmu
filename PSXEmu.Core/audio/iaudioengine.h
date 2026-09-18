#pragma once

#include <cstdint>

class IAudioEngine {
public:
    virtual ~IAudioEngine() = default;

    // Lifecycle
    virtual bool Initialize(int sampleRate, int channels) = 0;
    virtual void Shutdown() = 0;

    // Playback control
    virtual void Play() = 0;
    virtual void Pause() = 0;

    // Buffer management.
    //
    // Queues what fits and returns how many of the samples it took, which is
    // zero when the device has no room. It must never wait for room: the
    // caller is the thread running the machine, and a sound card that is a few
    // milliseconds behind must not be allowed to stop emulation - the frame
    // limiter paces the machine, not the audio device (bug 49). What the
    // caller does with the remainder is its business; App::PumpAudio keeps it
    // and offers it again next frame.
    virtual int QueueAudio(const int16_t* samples, int sampleCount) = 0;
    virtual int GetQueuedSampleCount() const = 0;

    // How much the front end should keep queued, in the same units as
    // GetQueuedSampleCount - int16 samples across both channels.
    //
    // A property of the output, not one number for all of them. The frame loop
    // delivers audio in bursts - measured at up to 30 ms apart on a machine
    // whose Sleep(1) really takes 15 - so the queue has to outlast the longest
    // gap plus however coarsely the device moves. WASAPI moves in fine steps
    // and is happy with 25 ms; DirectSound does not, and overrides this.
    virtual int TargetQueuedSamples() const { return 44100 / 40 * 2; }   // 25 ms
};
