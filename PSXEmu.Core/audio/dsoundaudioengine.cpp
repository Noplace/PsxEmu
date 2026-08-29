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
        m_writeOffset = 0;
        m_buffer->Play(0, 0, DSBPLAY_LOOPING);
        m_playing = true;
    }
}

void DirectSoundAudioEngine::Pause() {
    if (m_initialized && m_playing) {
        m_buffer->Stop();
        m_playing = false;
    }
}

void DirectSoundAudioEngine::QueueAudio(const int16_t* samples, int sampleCount) {
    if (!m_initialized || !m_playing) return;

    DWORD bytesToWrite = sampleCount * sizeof(int16_t);
    if (bytesToWrite == 0) return;

    DWORD playCursor = 0;
    m_buffer->GetCurrentPosition(&playCursor, NULL);

    DWORD playedSinceLast = 0;
    if (playCursor >= m_lastPlayCursor) {
        playedSinceLast = playCursor - m_lastPlayCursor;
    } else {
        playedSinceLast = m_bufferSize - m_lastPlayCursor + playCursor;
    }
    m_lastPlayCursor = playCursor;

    if (m_queuedBytes >= playedSinceLast) {
        m_queuedBytes -= playedSinceLast;
    } else {
        // Underrun detected!
        m_queuedBytes = 0;
        // Resync write offset with a small safety margin to prevent immediate re-underrun
        m_writeOffset = (playCursor + 4410) % m_bufferSize; 
    }

    // Leave a small 10ms safety gap to avoid hitting the play cursor
    DWORD maxAllowedQueuedBytes = m_bufferSize - (m_sampleRate * m_channels * sizeof(int16_t) / 100); 

    while (m_queuedBytes + bytesToWrite > maxAllowedQueuedBytes && m_playing) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        
        m_buffer->GetCurrentPosition(&playCursor, NULL);
        if (playCursor >= m_lastPlayCursor) {
            playedSinceLast = playCursor - m_lastPlayCursor;
        } else {
            playedSinceLast = m_bufferSize - m_lastPlayCursor + playCursor;
        }
        m_lastPlayCursor = playCursor;

        if (m_queuedBytes >= playedSinceLast) {
            m_queuedBytes -= playedSinceLast;
        } else {
            m_queuedBytes = 0;
            m_writeOffset = (playCursor + 4410) % m_bufferSize; 
            break; // Exit loop, we have plenty of space now
        }
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
}

int DirectSoundAudioEngine::GetQueuedSampleCount() const {
    if (!m_initialized || !m_playing) return 0;
    
    // Update m_queuedBytes
    DWORD playCursor = 0;
    m_buffer->GetCurrentPosition(&playCursor, NULL);
    
    // We can't update m_lastPlayCursor here easily because this is const
    // But this function is purely informational (used in UI if at all)
    
    return m_queuedBytes / sizeof(int16_t);
}
