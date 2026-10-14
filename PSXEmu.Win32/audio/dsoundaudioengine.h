#pragma once

#include "audio/iaudioengine.h"
#include <windows.h>
#include <dsound.h>

// DirectSound, driven as a pull device: a one-second looping buffer that the
// thread driving this tops up every 5 ms to a fixed distance past the write
// cursor.
//
// Two lessons from the push version are built in rather than patched on. The
// region between the play and write cursors already belongs to the mixer -
// measured at 30 ms here - so everything is written beyond the write cursor
// (bug 61). And the buffer loops, so whatever lies past the data is what plays
// if the data ever runs out: silence is always kept there, and a stall is a
// gap rather than the last second of sound again (bug 63).
class DirectSoundAudioEngine : public IAudioEngine {
public:
    DirectSoundAudioEngine();
    virtual ~DirectSoundAudioEngine() override;

    virtual bool Initialize(int sampleRate, int channels) override;
    virtual void Shutdown() override;

    virtual void Play() override;
    virtual void Pause() override;

    virtual void WaitForRoom(int timeout_ms) override;
    virtual int WritableFrames() override;
    virtual void WriteFrames(const int16_t* samples, int frames) override;
    virtual int BufferedFrames() override;

    // How many times the play cursor overtook the written data and writing had
    // to start again past the write cursor. Each one is an audible gap.
    int underruns() const { return m_underruns; }

private:
    DWORD FrameBytes() const;
    DWORD LeadBytes() const;
    DWORD GuardBytes() const;
    DWORD Distance(DWORD from, DWORD to) const;
    DWORD AlignUp(DWORD offset) const;
    void Fill(DWORD offset, const void* data, DWORD bytes);   // null data writes silence
    void RestartAtWriteCursor(DWORD writeCursor);

    bool m_initialized = false;
    bool m_playing = false;

    IDirectSound8* m_dsound = nullptr;
    IDirectSoundBuffer8* m_buffer = nullptr;
    IDirectSoundBuffer* m_primaryBuffer = nullptr;

    int m_sampleRate = 44100;
    int m_channels = 2;
    DWORD m_bufferSize = 0;

    // Where the next write goes. Everything from here to GuardBytes() past it
    // is silence - WriteFrames keeps that true.
    DWORD m_writePos = 0;

    HANDLE m_timer = nullptr;   // paces WaitForRoom
    HANDLE m_mmcss = nullptr;   // the driving thread's "Pro Audio" registration

    // Set by Play: the first look at the cursors places the write position,
    // rather than finding it stale and counting an underrun that never
    // happened. The cursors only settle once the buffer is really running.
    bool m_restarting = false;
    int m_underruns = 0;
};
