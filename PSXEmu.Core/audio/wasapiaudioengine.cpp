#include "wasapiaudioengine.h"
#include <avrt.h>
#include <stdexcept>
#include <vector>
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "mmdevapi.lib")
#pragma comment(lib, "avrt.lib")

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

        // AUTOCONVERTPCM and SRC_DEFAULT_QUALITY let WASAPI resample the
        // emulator's 44,100 Hz 16-bit output to whatever the device really
        // runs at. EVENTCALLBACK is the pull model: the device signals
        // m_bufferEvent each period, and the thread driving this engine fills
        // the room it has.
        const DWORD streamFlags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                  AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY |
                                  AUDCLNT_STREAMFLAGS_EVENTCALLBACK;

        // 20 ms, two periods on most hardware. The slack for the machine's
        // once-a-frame bursts lives in host::SampleRing now, not here, so this
        // only has to cover the audio thread being woken a period late -
        // which is what the Multimedia Class Scheduler registration below is
        // there to prevent.
        const REFERENCE_TIME bufferDuration = 200000;   // 100 ns units

        ThrowIfFailed(m_audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, streamFlags, bufferDuration, 0, &waveFormat, nullptr));
        ThrowIfFailed(m_audioClient->GetBufferSize(&m_bufferFrameCount));

        m_bufferEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (m_bufferEvent == nullptr)
            ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()));
        ThrowIfFailed(m_audioClient->SetEventHandle(m_bufferEvent));
        ThrowIfFailed(m_audioClient->GetService(__uuidof(IAudioRenderClient), (void**)&m_renderClient));

        // This is the thread that will drive the engine (see IAudioEngine), so
        // this is the one to give "Pro Audio" scheduling - the class render
        // threads are expected to run in, which a busy machine thread cannot
        // starve. Not having it is not a reason to have no sound.
        DWORD task_index = 0;
        m_mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index);

        m_initialized = true;
        return true;
    }
    catch (const std::exception&) {
        m_initialized = true;   // so Shutdown releases what was created
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

    if (m_bufferEvent != nullptr) {
        CloseHandle(m_bufferEvent);
        m_bufferEvent = nullptr;
    }
    if (m_mmcss != nullptr) {
        AvRevertMmThreadCharacteristics(m_mmcss);
        m_mmcss = nullptr;
    }

    // Only if we were the ones who initialised it - otherwise this would
    // decrement the host's reference count and tear down its apartment.
    if (m_com_initialized) {
        CoUninitialize();
        m_com_initialized = false;
    }

    m_initialized = false;
}

void WASAPIAudioEngine::Play() {
    if (!m_initialized || m_playing || m_audioClient == nullptr) return;

    // Fill the buffer with silence before starting, as the WASAPI samples do:
    // a stream started empty is an underrun on its very first period.
    UINT32 padding = 0;
    if (SUCCEEDED(m_audioClient->GetCurrentPadding(&padding)) && padding < m_bufferFrameCount) {
        const UINT32 frames = m_bufferFrameCount - padding;
        BYTE* data = nullptr;
        if (SUCCEEDED(m_renderClient->GetBuffer(frames, &data)))
            m_renderClient->ReleaseBuffer(frames, AUDCLNT_BUFFERFLAGS_SILENT);
    }
    m_audioClient->Start();
    m_playing = true;
}

void WASAPIAudioEngine::Pause() {
    if (m_initialized && m_playing && m_audioClient != nullptr) {
        m_audioClient->Stop();
        m_playing = false;
    }
}

void WASAPIAudioEngine::WaitForRoom(int timeout_ms) {
    if (m_bufferEvent != nullptr && m_playing)
        WaitForSingleObject(m_bufferEvent, static_cast<DWORD>(timeout_ms));
    else
        Sleep(static_cast<DWORD>(timeout_ms));
}

int WASAPIAudioEngine::WritableFrames() {
    if (!m_initialized || !m_playing) return 0;
    UINT32 padding = 0;
    if (FAILED(m_audioClient->GetCurrentPadding(&padding))) return 0;
    return static_cast<int>(m_bufferFrameCount - padding);
}

void WASAPIAudioEngine::WriteFrames(const int16_t* samples, int frames) {
    if (!m_initialized || !m_playing || frames <= 0) return;
    BYTE* data = nullptr;
    if (FAILED(m_renderClient->GetBuffer(static_cast<UINT32>(frames), &data))) return;
    memcpy(data, samples, static_cast<size_t>(frames) * m_channels * sizeof(int16_t));
    m_renderClient->ReleaseBuffer(static_cast<UINT32>(frames), 0);
}

int WASAPIAudioEngine::BufferedFrames() {
    if (!m_initialized) return 0;
    UINT32 padding = 0;
    if (FAILED(m_audioClient->GetCurrentPadding(&padding))) return 0;
    return static_cast<int>(padding);
}
