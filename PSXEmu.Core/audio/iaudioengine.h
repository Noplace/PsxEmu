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

    // Buffer management
    virtual void QueueAudio(const int16_t* samples, int sampleCount) = 0;
    virtual int GetQueuedSampleCount() const = 0;
};
