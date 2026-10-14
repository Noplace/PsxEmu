#include "dsoundaudioengine.h"
#include <avrt.h>
#include <cstring>
#pragma comment(lib, "dsound.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "avrt.lib")

// Windows 10 1803 and later; an older one refuses the flag and WaitForRoom
// falls back to Sleep. See platform/frame_limiter.h for why the plain timer is
// not good enough: its 15 ms granularity is half this engine's lead.
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

DirectSoundAudioEngine::DirectSoundAudioEngine() : m_initialized(false) {
}

DirectSoundAudioEngine::~DirectSoundAudioEngine() {
    Shutdown();
}

bool DirectSoundAudioEngine::Initialize(int sampleRate, int channels) {
    if (m_initialized) return true;

    m_sampleRate = sampleRate;
    m_channels = channels;

    HWND hwnd = GetForegroundWindow(); // Passable fallback if window handle isn't explicitly passed
    if (!hwnd) hwnd = GetDesktopWindow();

    if (FAILED(DirectSoundCreate8(nullptr, &m_dsound, nullptr))) {
        return false;
    }
    m_initialized = true;   // from here, Shutdown has things to release

    if (FAILED(m_dsound->SetCooperativeLevel(hwnd, DSSCL_PRIORITY))) {
        Shutdown();
        return false;
    }

    WAVEFORMATEX waveFormat = {};
    waveFormat.wFormatTag = WAVE_FORMAT_PCM;
    waveFormat.nChannels = m_channels;
    waveFormat.nSamplesPerSec = m_sampleRate;
    waveFormat.wBitsPerSample = 16;
    waveFormat.nBlockAlign = (waveFormat.nChannels * waveFormat.wBitsPerSample) / 8;
    waveFormat.nAvgBytesPerSec = waveFormat.nSamplesPerSec * waveFormat.nBlockAlign;

    // Set primary buffer format
    DSBUFFERDESC primaryDesc = {};
    primaryDesc.dwSize = sizeof(DSBUFFERDESC);
    primaryDesc.dwFlags = DSBCAPS_PRIMARYBUFFER;
    if (SUCCEEDED(m_dsound->CreateSoundBuffer(&primaryDesc, &m_primaryBuffer, nullptr)))
        m_primaryBuffer->SetFormat(&waveFormat);

    // A second, looping. Far more than is ever in flight, which is the point:
    // the play cursor is never anywhere near lapping the data.
    m_bufferSize = waveFormat.nAvgBytesPerSec;

    DSBUFFERDESC secondaryDesc = {};
    secondaryDesc.dwSize = sizeof(DSBUFFERDESC);
    secondaryDesc.dwFlags = DSBCAPS_GLOBALFOCUS | DSBCAPS_GETCURRENTPOSITION2;
    secondaryDesc.dwBufferBytes = m_bufferSize;
    secondaryDesc.lpwfxFormat = &waveFormat;

    IDirectSoundBuffer* tempBuffer = nullptr;
    if (FAILED(m_dsound->CreateSoundBuffer(&secondaryDesc, &tempBuffer, nullptr))) {
        Shutdown();
        return false;
    }
    tempBuffer->QueryInterface(IID_IDirectSoundBuffer8, (void**)&m_buffer);
    tempBuffer->Release();
    if (m_buffer == nullptr) {
        Shutdown();
        return false;
    }

    m_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                     TIMER_ALL_ACCESS);

    // The thread that opens the engine is the one that drives it (see
    // IAudioEngine), so it is the one to schedule as "Pro Audio".
    DWORD task_index = 0;
    m_mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index);

    m_writePos = 0;
    return true;
}

void DirectSoundAudioEngine::Shutdown() {
    if (!m_initialized) return;

    Pause();

    if (m_buffer) {
        m_buffer->Release();
        m_buffer = nullptr;
    }
    if (m_primaryBuffer) {
        m_primaryBuffer->Release();
        m_primaryBuffer = nullptr;
    }
    if (m_dsound) {
        m_dsound->Release();
        m_dsound = nullptr;
    }
    if (m_timer != nullptr) {
        CloseHandle(m_timer);
        m_timer = nullptr;
    }
    if (m_mmcss != nullptr) {
        AvRevertMmThreadCharacteristics(m_mmcss);
        m_mmcss = nullptr;
    }

    m_initialized = false;
}

void DirectSoundAudioEngine::Play() {
    if (!m_initialized || m_playing || m_buffer == nullptr) return;

    // Silence throughout before it starts, so the guard below holds from the
    // first write and nothing from a previous run can ever play.
    Fill(0, nullptr, m_bufferSize);
    m_buffer->SetCurrentPosition(0);
    m_buffer->Play(0, 0, DSBPLAY_LOOPING);
    m_playing = true;

    // Only once it is running does the write cursor mean anything, and it
    // keeps moving from here - so the write position is placed on the first
    // look at the cursors rather than now.
    m_restarting = true;
}

void DirectSoundAudioEngine::Pause() {
    if (m_initialized && m_playing && m_buffer != nullptr) {
        m_buffer->Stop();
        m_playing = false;
    }
}

// Whole frames only. A half-frame offset does not corrupt samples - it is still
// 16-bit aligned - but it swaps left and right for everything written after it.
DWORD DirectSoundAudioEngine::FrameBytes() const {
    return static_cast<DWORD>(m_channels * sizeof(int16_t));
}

// How far past the write cursor the data is kept: 30 ms. The thread tops it up
// every 5 ms, so it can be woken 25 ms late before anything is heard.
DWORD DirectSoundAudioEngine::LeadBytes() const {
    return static_cast<DWORD>(m_sampleRate * 3 / 100) * FrameBytes();
}

// Silence kept past the data: 100 ms, more than any stall the lead does not
// already cover.
DWORD DirectSoundAudioEngine::GuardBytes() const {
    return static_cast<DWORD>(m_sampleRate / 10) * FrameBytes();
}

DWORD DirectSoundAudioEngine::Distance(DWORD from, DWORD to) const {
    return (to + m_bufferSize - from) % m_bufferSize;
}

DWORD DirectSoundAudioEngine::AlignUp(DWORD offset) const {
    const DWORD frame = FrameBytes();
    return ((offset + frame - 1) / frame * frame) % m_bufferSize;
}

void DirectSoundAudioEngine::Fill(DWORD offset, const void* data, DWORD bytes) {
    if (bytes == 0)
        return;
    void* ptr1 = nullptr;
    void* ptr2 = nullptr;
    DWORD bytes1 = 0;
    DWORD bytes2 = 0;
    if (FAILED(m_buffer->Lock(offset % m_bufferSize, bytes, &ptr1, &bytes1, &ptr2, &bytes2, 0)))
        return;
    if (data != nullptr) {
        if (ptr1) memcpy(ptr1, data, bytes1);
        if (ptr2) memcpy(ptr2, static_cast<const uint8_t*>(data) + bytes1, bytes2);
    } else {
        if (ptr1) memset(ptr1, 0, bytes1);
        if (ptr2) memset(ptr2, 0, bytes2);
    }
    m_buffer->Unlock(ptr1, bytes1, ptr2, bytes2);
}

// Writing resumes just past the write cursor, over fresh silence.
void DirectSoundAudioEngine::RestartAtWriteCursor(DWORD writeCursor) {
    m_writePos = AlignUp(writeCursor);
    Fill(m_writePos, nullptr, GuardBytes());
}

void DirectSoundAudioEngine::WaitForRoom(int timeout_ms) {
    const int wait_ms = (timeout_ms < 5) ? timeout_ms : 5;
    if (m_timer != nullptr) {
        LARGE_INTEGER due;
        due.QuadPart = -static_cast<LONGLONG>(wait_ms) * 10000;   // relative, 100 ns units
        if (SetWaitableTimerEx(m_timer, &due, 0, nullptr, nullptr, nullptr, 0)) {
            WaitForSingleObject(m_timer, INFINITE);
            return;
        }
    }
    Sleep(static_cast<DWORD>(wait_ms));
}

int DirectSoundAudioEngine::WritableFrames() {
    if (!m_initialized || !m_playing) return 0;

    DWORD playCursor = 0;
    DWORD writeCursor = 0;
    if (FAILED(m_buffer->GetCurrentPosition(&playCursor, &writeCursor))) return 0;

    const DWORD committed = Distance(playCursor, writeCursor);
    DWORD ahead = Distance(playCursor, m_writePos);

    if (m_restarting) {
        // First look since Play: place the write position, and do not call it
        // an underrun - nothing has been written yet to fall behind.
        m_restarting = false;
        RestartAtWriteCursor(writeCursor);
        ahead = Distance(playCursor, m_writePos);
    } else if (ahead < committed || ahead > m_bufferSize / 2) {
        // The play cursor has caught up with the data: it is inside the
        // mixer's region, or - read the other way round the ring - behind the
        // play cursor altogether. Anything written there now is never heard.
        ++m_underruns;
        RestartAtWriteCursor(writeCursor);
        ahead = Distance(playCursor, m_writePos);
    }

    const DWORD target = committed + LeadBytes();
    if (ahead >= target)
        return 0;
    return static_cast<int>((target - ahead) / FrameBytes());
}

void DirectSoundAudioEngine::WriteFrames(const int16_t* samples, int frames) {
    if (!m_initialized || !m_playing || frames <= 0) return;

    const DWORD bytes = static_cast<DWORD>(frames) * FrameBytes();
    const DWORD at = m_writePos;
    Fill(at, samples, bytes);
    m_writePos = (at + bytes) % m_bufferSize;

    // Keep GuardBytes() of silence past the data. Up to now it lay at
    // [at, at + guard); the write has used `bytes` of it, so the same amount
    // has to be silenced at the far end - or all of it again, if this one
    // write was longer than the guard.
    if (bytes < GuardBytes())
        Fill((at + GuardBytes()) % m_bufferSize, nullptr, bytes);
    else
        Fill(m_writePos, nullptr, GuardBytes());
}

int DirectSoundAudioEngine::BufferedFrames() {
    if (!m_initialized || !m_playing) return 0;
    DWORD playCursor = 0;
    DWORD writeCursor = 0;
    if (FAILED(m_buffer->GetCurrentPosition(&playCursor, &writeCursor))) return 0;
    return static_cast<int>(Distance(playCursor, m_writePos) / FrameBytes());
}
