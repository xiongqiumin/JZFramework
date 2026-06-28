#pragma once
#include "JZAudioInput.h"
#include <Windows.h>
#include <mmsystem.h>
#include <atomic>
#include <vector>

class JZAudioInput;
class JZAudioInputImpl
{
public:
    explicit JZAudioInputImpl(JZAudioInput* parent);
    ~JZAudioInputImpl();

    bool open(const JZAudioDevice& device);
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