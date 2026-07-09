#pragma once
#include "JZAudioInput.h"
#include <Windows.h>
#include <mmsystem.h>
#include <atomic>
#include <vector>
#include "detail/JZAudioInputImpl.h"

class JZAudioInputWindows : public JZAudioInputImpl
{
public:
    explicit JZAudioInputWindows(JZAudioInput* parent);
    ~JZAudioInputWindows();

    bool open(const JZAudioDeviceInfo& device);
    void close();

private:
    static void CALLBACK waveInProc(HWAVEIN hwi, UINT uMsg, DWORD_PTR dwInstance,
        DWORD_PTR dwParam1, DWORD_PTR dwParam2);

    bool convertFormat(const JZAudioFormat& format, WAVEFORMATEX& waveFmt);
    void cleanupBuffers();

    JZAudioInput* m_parent = nullptr;
    HWAVEIN m_hWaveIn = nullptr;
    std::atomic<bool> m_isCapturing = false;


    static const int BUFFER_COUNT = 3; 
    WAVEHDR m_waveHdr[BUFFER_COUNT] = { 0 };
};