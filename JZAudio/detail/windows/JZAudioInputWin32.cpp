#include <stdexcept>
#include <iostream>
#include "JZAudioInputImpl.h"
#include "JZAudioDeviceImpl.h"
#include "JZThread.h"

JZAudioInputImpl::JZAudioInputImpl(JZAudioInput* parent)
    : m_parent(parent)
{
    for (int i = 0; i < BUFFER_COUNT; i++) {
        m_waveHdr[i].lpData = nullptr;
        m_waveHdr[i].dwBufferLength = 0;
        m_waveHdr[i].dwFlags = 0;
    }
}

JZAudioInputImpl::~JZAudioInputImpl()
{
    close();
}

bool JZAudioInputImpl::convertFormat(const JZAudioFormat& format, WAVEFORMATEX& waveFmt)
{
    if (format.channelCount != 1 && format.channelCount != 2) {
        std::cerr << "Unsupported channel count: " << format.channelCount << ", only support 1/2" << std::endl;
        return false;
    }

    if (format.sampleFormat != JZAudioFormat::UInt8 && format.sampleFormat != JZAudioFormat::Int16) {
        std::cerr << "Unsupported sample format: " << format.sampleFormat << ", only support UInt8/Int16" << std::endl;
        return false;
    }

    waveFmt.wFormatTag = WAVE_FORMAT_PCM;
    waveFmt.nChannels = format.channelCount;
    waveFmt.nSamplesPerSec = format.sampleRate;
    waveFmt.wBitsPerSample = (format.sampleFormat == JZAudioFormat::UInt8) ? 8 : 16;
    waveFmt.nBlockAlign = waveFmt.nChannels * (waveFmt.wBitsPerSample / 8);
    waveFmt.nAvgBytesPerSec = waveFmt.nSamplesPerSec * waveFmt.nBlockAlign;
    waveFmt.cbSize = 0;

    return true;
}

bool JZAudioInputImpl::open(const JZAudioDeviceInfo& device)
{
    if (m_isCapturing || m_hWaveIn != nullptr) {
        return false;
    }

    const JZAudioFormat& format = m_parent->m_format;
    WAVEFORMATEX waveFmt = { 0 };
    if (!convertFormat(format, waveFmt)) {
        return false;
    }

    MMRESULT result = waveInOpen(
        &m_hWaveIn,                  
        std::stoi(device.id),         
        &waveFmt,                    
        reinterpret_cast<DWORD_PTR>(waveInProc), 
        reinterpret_cast<DWORD_PTR>(this),       
        CALLBACK_FUNCTION            
    );

    if (result != MMSYSERR_NOERROR) {
        std::cerr << "waveInOpen failed, error code: " << result << std::endl;
        return false;
    }

    const int BASE_BUFFER_SIZE = 4096;
    DWORD bufferSize = BASE_BUFFER_SIZE - (BASE_BUFFER_SIZE % waveFmt.nBlockAlign);
    if (bufferSize == 0) {
        waveInClose(m_hWaveIn);
        m_hWaveIn = nullptr;
        return false;
    }

    for (int i = 0; i < BUFFER_COUNT; i++) {
        m_waveHdr[i].lpData = new char[bufferSize];
        m_waveHdr[i].dwBufferLength = bufferSize;
        m_waveHdr[i].dwFlags = 0;

        result = waveInPrepareHeader(m_hWaveIn, &m_waveHdr[i], sizeof(WAVEHDR));
        if (result != MMSYSERR_NOERROR) {
            cleanupBuffers();
            waveInClose(m_hWaveIn);
            m_hWaveIn = nullptr;
            return false;
        }

        result = waveInAddBuffer(m_hWaveIn, &m_waveHdr[i], sizeof(WAVEHDR));
        if (result != MMSYSERR_NOERROR) {
            cleanupBuffers();
            waveInClose(m_hWaveIn);
            m_hWaveIn = nullptr;
            return false;
        }
    }

    result = waveInStart(m_hWaveIn);
    if (result != MMSYSERR_NOERROR) {
        cleanupBuffers();
        waveInClose(m_hWaveIn);
        m_hWaveIn = nullptr;
        return false;
    }

    m_isCapturing = true;
    return true;
}

void JZAudioInputImpl::close()
{
    if (!m_isCapturing || m_hWaveIn == nullptr) {
        return;
    }

    cleanupBuffers();

    waveInStop(m_hWaveIn);
    waveInClose(m_hWaveIn);
    m_hWaveIn = nullptr;
    m_isCapturing = false;
}

void JZAudioInputImpl::cleanupBuffers()
{
    if (m_hWaveIn == nullptr) {
        return;
    }

    for (int i = 0; i < BUFFER_COUNT; i++) {
        if (m_waveHdr[i].dwFlags & WHDR_PREPARED) {
            waveInUnprepareHeader(m_hWaveIn, &m_waveHdr[i], sizeof(WAVEHDR));
        }
        if (m_waveHdr[i].lpData != nullptr) {
            delete[] m_waveHdr[i].lpData;
            m_waveHdr[i].lpData = nullptr;
        }
        m_waveHdr[i].dwBufferLength = 0;
        m_waveHdr[i].dwFlags = 0;
    }
}

void JZAudioInputImpl::waveInProc(HWAVEIN hwi, UINT uMsg, DWORD_PTR dwInstance,
    DWORD_PTR dwParam1, DWORD_PTR dwParam2)
{
    if (uMsg != WIM_DATA) {
        return;
    }

    JZAudioInputImpl* pImpl = reinterpret_cast<JZAudioInputImpl*>(dwInstance);
    WAVEHDR* pHeader = reinterpret_cast<WAVEHDR*>(dwParam1);

    if (!pImpl || !pImpl->m_isCapturing || !pHeader || pHeader->dwBytesRecorded == 0) {
        waveInAddBuffer(hwi, pHeader, sizeof(WAVEHDR));
        return;
    }

    JZByteArray audioData((char*)pHeader->lpData, pHeader->dwBytesRecorded);
    JZAudioInput* input = pImpl->m_parent;
    runOnObject(input, [input, audioData] { input->sigReadyRead.emit(audioData); });

    waveInAddBuffer(hwi, pHeader, sizeof(WAVEHDR));
}
