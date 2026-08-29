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

    virtual void QueueAudio(const int16_t* samples, int sampleCount) override;
    virtual int GetQueuedSampleCount() const override;

private:
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
};
