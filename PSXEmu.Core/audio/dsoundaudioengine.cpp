#include "dsoundaudioengine.h"
#include <stdexcept>
#include <iostream>
#include <thread>
#pragma comment(lib, "dsound.lib")
#pragma comment(lib, "dxguid.lib")

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

    if (FAILED(m_dsound->SetCooperativeLevel(hwnd, DSSCL_PRIORITY))) {
        Shutdown();
        return false;
    }

    // Set primary buffer format
    DSBUFFERDESC primaryDesc = {};
    primaryDesc.dwSize = sizeof(DSBUFFERDESC);
    primaryDesc.dwFlags = DSBCAPS_PRIMARYBUFFER;
    
    if (SUCCEEDED(m_dsound->CreateSoundBuffer(&primaryDesc, &m_primaryBuffer, nullptr))) {
        WAVEFORMATEX waveFormat = {};
        waveFormat.wFormatTag = WAVE_FORMAT_PCM;
        waveFormat.nChannels = m_channels;
        waveFormat.nSamplesPerSec = m_sampleRate;
        waveFormat.wBitsPerSample = 16;
        waveFormat.nBlockAlign = (waveFormat.nChannels * waveFormat.wBitsPerSample) / 8;
        waveFormat.nAvgBytesPerSec = waveFormat.nSamplesPerSec * waveFormat.nBlockAlign;
        m_primaryBuffer->SetFormat(&waveFormat);
    }

    // Create secondary buffer (1 second)
    WAVEFORMATEX waveFormat = {};
    waveFormat.wFormatTag = WAVE_FORMAT_PCM;
    waveFormat.nChannels = m_channels;
    waveFormat.nSamplesPerSec = m_sampleRate;
    waveFormat.wBitsPerSample = 16;
    waveFormat.nBlockAlign = (waveFormat.nChannels * waveFormat.wBitsPerSample) / 8;
    waveFormat.nAvgBytesPerSec = waveFormat.nSamplesPerSec * waveFormat.nBlockAlign;

    m_bufferSize = waveFormat.nAvgBytesPerSec; // 1 second buffer

    DSBUFFERDESC secondaryDesc = {};
    secondaryDesc.dwSize = sizeof(DSBUFFERDESC);
    secondaryDesc.dwFlags = DSBCAPS_GLOBALFOCUS | DSBCAPS_GETCURRENTPOSITION2 | DSBCAPS_CTRLPOSITIONNOTIFY;
    secondaryDesc.dwBufferBytes = m_bufferSize;
    secondaryDesc.lpwfxFormat = &waveFormat;

    IDirectSoundBuffer* tempBuffer = nullptr;
    if (FAILED(m_dsound->CreateSoundBuffer(&secondaryDesc, &tempBuffer, nullptr))) {
        Shutdown();
        return false;
    }

    tempBuffer->QueryInterface(IID_IDirectSoundBuffer8, (void**)&m_buffer);
    tempBuffer->Release();

    m_writeOffset = 0;
    m_initialized = true;
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

    m_initialized = false;
}

void DirectSoundAudioEngine::Play() {
    if (m_initialized && !m_playing) {
        // Clear buffer before playing
        void* ptr1 = nullptr;
        void* ptr2 = nullptr;
        DWORD bytes1 = 0;
        DWORD bytes2 = 0;
        if (SUCCEEDED(m_buffer->Lock(0, m_bufferSize, &ptr1, &bytes1, &ptr2, &bytes2, 0))) {
            if (ptr1) memset(ptr1, 0, bytes1);
            if (ptr2) memset(ptr2, 0, bytes2);
            m_buffer->Unlock(ptr1, bytes1, ptr2, bytes2);
        }
        
        m_buffer->SetCurrentPosition(0);
        m_buffer->Play(0, 0, DSBPLAY_LOOPING);
        m_playing = true;

        // Start with the target amount of silence already queued beyond the
        // write cursor, rather than nothing. The front end's rate control can
        // only trim by half a percent, so from empty it takes about five
        // seconds to build up to its target. The buffer was just zeroed, so
        // this is silence that really will be played before the first sample.
        // Placed after Play() because the write cursor only means anything
        // once the buffer is running.
        DWORD playCursor = 0;
        DWORD writeCursor = 0;
        m_buffer->GetCurrentPosition(&playCursor, &writeCursor);
        m_lastPlayCursor = playCursor;
        ResyncAfter(playCursor, writeCursor);
    }
}

// Carries on from a prime's worth beyond the write cursor, on a whole frame,
// over silence written there now, with m_queuedBytes set to the distance from
// the play cursor so the accounting stays in step with where the data is.
void DirectSoundAudioEngine::ResyncAfter(DWORD playCursor, DWORD writeCursor) {
    const DWORD aligned = writeCursor - (writeCursor % FrameBytes());
    m_writeOffset = (aligned + PrimeBytes()) % m_bufferSize;
    WriteSilence(aligned, PrimeBytes());
    m_queuedBytes = (m_writeOffset + m_bufferSize - playCursor) % m_bufferSize;
}

// All offsets in the buffer are whole frames. A half-frame offset does not
// corrupt samples - it is still 16-bit aligned - but it swaps left and right
// for everything written after it, which the old resync did by adding 4410
// bytes (1102.5 frames) to the play cursor.
DWORD DirectSoundAudioEngine::FrameBytes() const {
    return static_cast<DWORD>(m_channels * sizeof(int16_t));
}

int DirectSoundAudioEngine::TargetQueuedSamples() const {
    return (m_sampleRate / 20) * m_channels;   // 50 ms - see the header
}

// Start, and restart after an underrun, at the depth the rate control is going
// to hold anyway, so it has nothing to climb.
DWORD DirectSoundAudioEngine::PrimeBytes() const {
    return static_cast<DWORD>(TargetQueuedSamples() / m_channels) * FrameBytes();
}

// Silence written after the last real sample, and not counted as queued.
//
// This is what makes an underrun sound like WASAPI's rather than like
// DirectSound's. The secondary buffer loops, so when the play cursor overtakes
// the data it does not stop - it plays on into whatever the buffer held a
// second ago, which is a burst of the wrong waveform and the loudest kind of
// click. With this much silence always waiting after the data, running out
// plays a gap instead.
DWORD DirectSoundAudioEngine::GuardBytes() const {
    return static_cast<DWORD>(m_sampleRate / 10) * FrameBytes();   // 100 ms
}

void DirectSoundAudioEngine::WriteSilence(DWORD offset, DWORD bytes) {
    if (bytes == 0)
        return;
    void* ptr1 = nullptr;
    void* ptr2 = nullptr;
    DWORD bytes1 = 0;
    DWORD bytes2 = 0;
    if (SUCCEEDED(m_buffer->Lock(offset, bytes, &ptr1, &bytes1, &ptr2, &bytes2, 0))) {
        if (ptr1) memset(ptr1, 0, bytes1);
        if (ptr2) memset(ptr2, 0, bytes2);
        m_buffer->Unlock(ptr1, bytes1, ptr2, bytes2);
    }
}

void DirectSoundAudioEngine::Pause() {
    if (m_initialized && m_playing) {
        m_buffer->Stop();
        m_playing = false;
    }
}

int DirectSoundAudioEngine::QueueAudio(const int16_t* samples, int sampleCount) {
    if (!m_initialized || !m_playing) return 0;

    DWORD bytesToWrite = sampleCount * sizeof(int16_t);
    if (bytesToWrite == 0) return 0;

    DWORD playCursor = 0;
    DWORD writeCursor = 0;
    m_buffer->GetCurrentPosition(&playCursor, &writeCursor);

    DWORD playedSinceLast = 0;
    if (playCursor >= m_lastPlayCursor) {
        playedSinceLast = playCursor - m_lastPlayCursor;
    } else {
        playedSinceLast = m_bufferSize - m_lastPlayCursor + playCursor;
    }
    m_lastPlayCursor = playCursor;
    m_queuedBytes = (m_queuedBytes > playedSinceLast) ? m_queuedBytes - playedSinceLast : 0;

    // The data has run out when it no longer reaches past the *write* cursor,
    // not the play cursor. Everything between the two has already been handed
    // to the mixer - measured at 30 ms on the machine this was written on - and
    // a sample written in there is simply never played. Testing only against
    // the play cursor missed exactly that: the play cursor never overtook the
    // data, so nothing was counted, while every frame lost its first few
    // milliseconds into the committed region and clicked.
    const DWORD lead = (writeCursor + m_bufferSize - playCursor) % m_bufferSize;
    if (m_queuedBytes <= lead) {
        ++m_underruns;
        ResyncAfter(playCursor, writeCursor);
    }

    // Leave a small 10ms safety gap to avoid hitting the play cursor
    DWORD maxAllowedQueuedBytes = m_bufferSize - (m_sampleRate * m_channels * sizeof(int16_t) / 100);

    // Write what fits and say so, rather than sleeping until the buffer drains:
    // the caller is the thread running the machine, and it must not be stopped
    // by a sound card that is a few milliseconds behind. See IAudioEngine.
    if (m_queuedBytes >= maxAllowedQueuedBytes)
        return 0;
    const DWORD roomBytes = maxAllowedQueuedBytes - m_queuedBytes;
    if (bytesToWrite > roomBytes) {
        // Whole frames only - half a frame would swap the channels of every
        // sample after it.
        const DWORD frameBytes = m_channels * sizeof(int16_t);
        bytesToWrite = (roomBytes / frameBytes) * frameBytes;
        if (bytesToWrite == 0)
            return 0;
    }

    void* ptr1 = nullptr;
    void* ptr2 = nullptr;
    DWORD bytes1 = 0;
    DWORD bytes2 = 0;

    if (SUCCEEDED(m_buffer->Lock(m_writeOffset, bytesToWrite, &ptr1, &bytes1, &ptr2, &bytes2, 0))) {
        if (ptr1) {
            memcpy(ptr1, samples, bytes1);
        }
        if (ptr2) {
            memcpy(ptr2, (const uint8_t*)samples + bytes1, bytes2);
        }
        m_buffer->Unlock(ptr1, bytes1, ptr2, bytes2);
    }

    m_writeOffset = (m_writeOffset + bytesToWrite) % m_bufferSize;
    m_queuedBytes += bytesToWrite;

    // Keep silence waiting after the data - see GuardBytes. Never so much that
    // it would reach round the ring into data that is queued but not played.
    const DWORD free_bytes = m_bufferSize - m_queuedBytes;
    DWORD guard = GuardBytes();
    if (guard > free_bytes)
        guard = free_bytes - (free_bytes % FrameBytes());
    WriteSilence(m_writeOffset, guard);

    return static_cast<int>(bytesToWrite / sizeof(int16_t));
}

// How much the device really holds right now - live, from the play cursor.
//
// This used to return m_queuedBytes as the last QueueAudio left it, with a note
// that it was "purely informational". It stopped being that when the blocking
// wait went: the front end's rate control steers the buffer by this number, and
// reading it one frame stale made the controller hold about 8 ms where it was
// aiming for 25. That is small enough for the play cursor to catch up every
// couple of seconds - an underrun, and a click - which is what DirectSound
// sounded like until this read the cursor. WASAPI never had the problem because
// its count asks the device directly.
//
// And measured from the write cursor, not the play cursor: what lies between
// the two is already the mixer's, so the only audio still in hand is what was
// written beyond it. Holding *that* at the front end's target keeps every write
// clear of the committed region - the latency is the device's own lead plus
// the target, which is what it has to be for DirectSound to keep up at all.
int DirectSoundAudioEngine::GetQueuedSampleCount() const {
    if (!m_initialized || !m_playing) return 0;

    DWORD playCursor = 0;
    DWORD writeCursor = 0;
    m_buffer->GetCurrentPosition(&playCursor, &writeCursor);

    const DWORD played = (playCursor >= m_lastPlayCursor)
                             ? playCursor - m_lastPlayCursor
                             : m_bufferSize - m_lastPlayCursor + playCursor;
    const DWORD from_play = (m_queuedBytes > played) ? m_queuedBytes - played : 0;
    const DWORD lead = (writeCursor + m_bufferSize - playCursor) % m_bufferSize;
    const DWORD uncommitted = (from_play > lead) ? from_play - lead : 0;
    return static_cast<int>(uncommitted / sizeof(int16_t));
}
