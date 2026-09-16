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
};
