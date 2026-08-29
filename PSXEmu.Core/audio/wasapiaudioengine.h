#pragma once

#include "audio/iaudioengine.h"
#include <windows.h>
#include <mmdeviceapi.h>
#include <Audioclient.h>
#include <wrl/client.h>

class WASAPIAudioEngine : public IAudioEngine {
public:
    WASAPIAudioEngine();
    virtual ~WASAPIAudioEngine() override;

    virtual bool Initialize(int sampleRate, int channels) override;
    virtual void Shutdown() override;

    virtual void Play() override;
    virtual void Pause() override;

    virtual void QueueAudio(const int16_t* samples, int sampleCount) override;
    virtual int GetQueuedSampleCount() const override;

private:
    bool m_initialized = false;
    bool m_playing = false;
    // True only when this engine is the one that initialised COM on its thread,
    // so Shutdown() knows whether it is entitled to call CoUninitialize().
    bool m_com_initialized = false;

    int m_channels = 2;
    int m_sampleRate = 44100;

    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> m_enumerator;
    Microsoft::WRL::ComPtr<IMMDevice> m_device;
    Microsoft::WRL::ComPtr<IAudioClient> m_audioClient;
    Microsoft::WRL::ComPtr<IAudioRenderClient> m_renderClient;

    UINT32 m_bufferFrameCount = 0;
};
