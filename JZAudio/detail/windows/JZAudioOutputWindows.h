#ifndef JZ_AUDIO_OUTPIT_IMPL_H_
#define JZ_AUDIO_OUTPIT_IMPL_H_

#include <Windows.h>
#include <mmsystem.h>
#include <atomic>
#include <vector>
#include <mutex>
#include <list>
#include "JZAudioOutput.h"
#include "detail/JZAudioOutputImpl.h"

class JZAudioOutputWindows : public JZAudioOutputImpl
{
public:
    explicit JZAudioOutputWindows(JZAudioOutput* parent);
    ~JZAudioOutputWindows();

    bool open(const JZAudioDeviceInfo& device);
    void close();
    void reset();
    bool write(const JZByteArray& data);

private:
    static void CALLBACK waveOutProc(HWAVEOUT hwo, UINT uMsg, DWORD_PTR dwInstance,
        DWORD_PTR dwParam1, DWORD_PTR dwParam2);

    bool convertFormat(const JZAudioFormat& format, WAVEFORMATEX& waveFmt);
    void cleanupBuffers();

    JZAudioOutput* m_parent = nullptr;
    HWAVEOUT m_hWaveOut = nullptr;
    std::atomic<bool> m_isPlaying = false;
    std::list<WAVEHDR*> m_bufferQueue;
    std::mutex m_queueMutex;
    static const int MAX_BUFFER_COUNT = 4;
};

#endif // !JZ_AUDIO_OUTPIT_IMPL_H_
