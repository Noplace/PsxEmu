#pragma once

#include "audio/iaudioengine.h"
#include <windows.h>
#include <mmdeviceapi.h>
#include <Audioclient.h>
#include <wrl/client.h>

// WASAPI in shared, event-driven mode: the device signals an event each period
// (10 ms on most hardware) and the thread driving this fills whatever room the
// buffer has. That is the device's own clock pacing the writes, which is what
// the pull model is for - see IAudioEngine.
class WASAPIAudioEngine : public IAudioEngine {
public:
    WASAPIAudioEngine();
    virtual ~WASAPIAudioEngine() override;

    virtual bool Initialize(int sampleRate, int channels) override;
    virtual void Shutdown() override;

    virtual void Play() override;
    virtual void Pause() override;

    virtual void WaitForRoom(int timeout_ms) override;
    virtual int WritableFrames() override;
    virtual void WriteFrames(const int16_t* samples, int frames) override;
    virtual int BufferedFrames() override;

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

    // Signalled by the device each period. Waiting on it is what paces the
    // thread that drives this engine.
    HANDLE m_bufferEvent = nullptr;

    // The Multimedia Class Scheduler registration for the thread that opened
    // the engine - see Initialize.
    HANDLE m_mmcss = nullptr;
};
