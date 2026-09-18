#pragma once

#include "audio/iaudioengine.h"
#include <windows.h>
#include <dsound.h>

class DirectSoundAudioEngine : public IAudioEngine {
public:
    DirectSoundAudioEngine();
    virtual ~DirectSoundAudioEngine() override;

    virtual bool Initialize(int sampleRate, int channels) override;
    virtual void Shutdown() override;

    virtual void Play() override;
    virtual void Pause() override;

    virtual int QueueAudio(const int16_t* samples, int sampleCount) override;
    virtual int GetQueuedSampleCount() const override;

    // 50 ms beyond the write cursor, twice WASAPI's. Measured on the machine
    // this was written on: the play cursor moves in 20 ms steps and frames
    // arrive up to 30 ms apart, so 25 ms of uncommitted audio can be used up
    // by one late frame and one cursor step together. The write cursor's own
    // lead (30 ms) comes on top, so the total latency is about 80 ms - high for
    // an audio API, ordinary for DirectSound.
    virtual int TargetQueuedSamples() const override;

    // How many times the play cursor caught up with the written data. Each one
    // is an audible glitch, so this is the number that says whether the output
    // is healthy - it cannot be heard from a test, but it can be counted.
    int underruns() const { return m_underruns; }

private:
    DWORD FrameBytes() const;
    DWORD PrimeBytes() const;
    DWORD GuardBytes() const;
    void WriteSilence(DWORD offset, DWORD bytes);
    void ResyncAfter(DWORD playCursor, DWORD writeCursor);

    bool m_initialized = false;
    bool m_playing = false;

    IDirectSound8* m_dsound = nullptr;
    IDirectSoundBuffer8* m_buffer = nullptr;
    IDirectSoundBuffer* m_primaryBuffer = nullptr;

    int m_sampleRate = 44100;
    int m_channels = 2;
    DWORD m_bufferSize = 0;
    DWORD m_writeOffset = 0;
    DWORD m_lastPlayCursor = 0;
    DWORD m_queuedBytes = 0;
    int m_underruns = 0;
};
