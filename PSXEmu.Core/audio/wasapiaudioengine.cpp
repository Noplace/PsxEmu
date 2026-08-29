#include "wasapiaudioengine.h"
#include <stdexcept>
#include <iostream>
#include  <thread>
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "mmdevapi.lib")

// Helper
static void ThrowIfFailed(HRESULT hr) {
    if (FAILED(hr)) {
        throw std::runtime_error("WASAPI Error");
    }
}

WASAPIAudioEngine::WASAPIAudioEngine() : m_initialized(false) {
}

WASAPIAudioEngine::~WASAPIAudioEngine() {
    Shutdown();
}

bool WASAPIAudioEngine::Initialize(int sampleRate, int channels) {
    if (m_initialized) return true;

    m_sampleRate = sampleRate;
    m_channels = channels;

    try {
        // Don't assume we own COM on this thread. A Win32 host that never
        // initialises it gives S_OK and we clean up in Shutdown(); a WinUI host
        // already owns the apartment and gives RPC_E_CHANGED_MODE, which is not
        // an error - COM is initialised, just not as MTA, and the calls below
        // work either way. Treating it as fatal would mean no audio under WinUI.
        const HRESULT co_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (co_hr == S_OK) {
            m_com_initialized = true;
        } else if (co_hr != S_FALSE && co_hr != RPC_E_CHANGED_MODE) {
            ThrowIfFailed(co_hr);
        }

        ThrowIfFailed(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&m_enumerator));
        ThrowIfFailed(m_enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &m_device));
        ThrowIfFailed(m_device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&m_audioClient));

        WAVEFORMATEX waveFormat = {};
        waveFormat.wFormatTag = WAVE_FORMAT_PCM;
        waveFormat.nChannels = m_channels;
        waveFormat.nSamplesPerSec = m_sampleRate;
        waveFormat.wBitsPerSample = 16;
        waveFormat.nBlockAlign = (waveFormat.nChannels * waveFormat.wBitsPerSample) / 8;
        waveFormat.nAvgBytesPerSec = waveFormat.nSamplesPerSec * waveFormat.nBlockAlign;
        waveFormat.cbSize = 0;

        // Note: AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM and AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY 
        // allow WASAPI to resample the emulator's 44100Hz 16-bit output to the device's actual format.
        DWORD streamFlags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;

        // Request 50ms buffer
        REFERENCE_TIME bufferDuration = 500000; // 100ns units

        ThrowIfFailed(m_audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, streamFlags, bufferDuration, 0, &waveFormat, nullptr));
        ThrowIfFailed(m_audioClient->GetBufferSize(&m_bufferFrameCount));
        ThrowIfFailed(m_audioClient->GetService(__uuidof(IAudioRenderClient), (void**)&m_renderClient));

        m_initialized = true;
        return true;
    }
    catch (const std::exception&) {
        Shutdown();
        return false;
    }
}

void WASAPIAudioEngine::Shutdown() {
    if (!m_initialized) return;

    Pause();

    m_renderClient.Reset();
    m_audioClient.Reset();
    m_device.Reset();
    m_enumerator.Reset();

    // Only if we were the ones who initialised it - otherwise this would
    // decrement the host's reference count and tear down its apartment.
    if (m_com_initialized) {
        CoUninitialize();
        m_com_initialized = false;
    }

    m_initialized = false;
}

void WASAPIAudioEngine::Play() {
    if (m_initialized && !m_playing) {
        m_audioClient->Start();
        m_playing = true;
    }
}

void WASAPIAudioEngine::Pause() {
    if (m_initialized && m_playing) {
        m_audioClient->Stop();
        m_playing = false;
    }
}

void WASAPIAudioEngine::QueueAudio(const int16_t* samples, int sampleCount) {
    if (!m_initialized || !m_playing) return;

    // sampleCount represents total 16-bit ints. A frame is typically 2 channels (left/right).
    int frameCount = sampleCount / m_channels;

    UINT32 padding = 0;
    if (FAILED(m_audioClient->GetCurrentPadding(&padding))) return;

    UINT32 availableFrames = m_bufferFrameCount - padding;
    
    // Block and wait if buffer is full instead of dropping samples
    while (availableFrames < frameCount && m_playing) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (FAILED(m_audioClient->GetCurrentPadding(&padding))) return;
        availableFrames = m_bufferFrameCount - padding;
    }

    UINT32 framesToWrite = frameCount;

    BYTE* pData = nullptr;
    if (SUCCEEDED(m_renderClient->GetBuffer(framesToWrite, &pData))) {
        memcpy(pData, samples, framesToWrite * m_channels * sizeof(int16_t));
        m_renderClient->ReleaseBuffer(framesToWrite, 0);
    }
}

int WASAPIAudioEngine::GetQueuedSampleCount() const {
    if (!m_initialized) return 0;
    UINT32 padding = 0;
    m_audioClient->GetCurrentPadding(&padding);
    return padding * m_channels;
}
