#include <stdexcept>
#include <iostream>
#include <cstring>
#include "JZAudioOutputWindows.h"
#include "JZThread.h"

JZAudioOutputWindows::JZAudioOutputWindows(JZAudioOutput* parent)
    : JZAudioOutputImpl(parent)
{
}

JZAudioOutputWindows::~JZAudioOutputWindows()
{
    close();
}

bool JZAudioOutputWindows::convertFormat(const JZAudioFormat& format, WAVEFORMATEX& waveFmt)
{
    if (format.channelCount != 1 && format.channelCount != 2) {
        std::cerr << "Unsupported channels: " << format.channelCount << " (only 1/2)" << std::endl;
        return false;
    }

    if (format.sampleFormat != JZAudioFormat::UInt8 && format.sampleFormat != JZAudioFormat::Int16) {
        std::cerr << "Unsupported sample format: " << format.sampleFormat << " (only UInt8/Int16)" << std::endl;
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

bool JZAudioOutputWindows::open(const JZAudioDeviceInfo& device)
{
    auto format = m_parent->format();
    if (m_isPlaying || m_hWaveOut != nullptr) {
        return false;
    }

    WAVEFORMATEX waveFmt = { 0 };
    if (!convertFormat(format, waveFmt)) {
        return false;
    }

    MMRESULT result = waveOutOpen(
        &m_hWaveOut,
        std::stoi(device.id),
        &waveFmt,
        reinterpret_cast<DWORD_PTR>(waveOutProc),
        reinterpret_cast<DWORD_PTR>(this),
        CALLBACK_FUNCTION
    );

    if (result != MMSYSERR_NOERROR) {
        std::cerr << "waveOutOpen failed, error: " << result << std::endl;
        return false;
    }

    m_isPlaying = true;
    return true;
}

void JZAudioOutputWindows::close()
{
    std::lock_guard<std::mutex> lock(m_queueMutex);

    if (!m_isPlaying || m_hWaveOut == nullptr) {
        return;
    }

    m_isPlaying = false;
    waveOutReset(m_hWaveOut);

    cleanupBuffers();

    waveOutClose(m_hWaveOut);
    m_hWaveOut = nullptr;
}

void JZAudioOutputWindows::reset()
{
    if (m_hWaveOut == nullptr) {
        return;
    }

    waveOutReset(m_hWaveOut);
    cleanupBuffers();
}

bool JZAudioOutputWindows::write(const JZByteArray& data)
{
    std::lock_guard<std::mutex> lock(m_queueMutex);

    if (!m_isPlaying || m_hWaveOut == nullptr || data.isEmpty()) {
        return false;
    }

    if (m_bufferQueue.size() >= MAX_BUFFER_COUNT) {
        std::cerr << "Buffer queue full, drop data (current: " << m_bufferQueue.size() << ")" << std::endl;
        return false;
    }

    WAVEHDR* pHeader = new WAVEHDR{ 0 };
    char* pBuffer = new char[data.size()];
    memcpy(pBuffer, data.data(), data.size());

    pHeader->lpData = pBuffer;
    pHeader->dwBufferLength = static_cast<DWORD>(data.size());
    pHeader->dwFlags = 0;

    MMRESULT result = waveOutPrepareHeader(m_hWaveOut, pHeader, sizeof(WAVEHDR));
    if (result != MMSYSERR_NOERROR) {
        delete[] pBuffer;
        delete pHeader;
        std::cerr << "waveOutPrepareHeader failed: " << result << std::endl;
        return false;
    }

    result = waveOutWrite(m_hWaveOut, pHeader, sizeof(WAVEHDR));
    if (result != MMSYSERR_NOERROR) {
        waveOutUnprepareHeader(m_hWaveOut, pHeader, sizeof(WAVEHDR));
        delete[] pBuffer;
        delete pHeader;
        std::cerr << "waveOutWrite failed: " << result << std::endl;
        return false;
    }

    m_bufferQueue.push_back(pHeader);
    return true;
}

void JZAudioOutputWindows::cleanupBuffers()
{
    if (m_hWaveOut == nullptr) {
        return;
    }

    for (auto pHeader : m_bufferQueue) {
        if (pHeader) {
            if (pHeader->dwFlags & WHDR_PREPARED) {
                waveOutUnprepareHeader(m_hWaveOut, pHeader, sizeof(WAVEHDR));
            }
            if (pHeader->lpData != nullptr) {
                delete[] pHeader->lpData;
            }
            delete pHeader;
        }
    }

    m_bufferQueue.clear();
}

void JZAudioOutputWindows::waveOutProc(HWAVEOUT hwo, UINT uMsg, DWORD_PTR dwInstance,
    DWORD_PTR dwParam1, DWORD_PTR dwParam2)
{
    if (uMsg != WOM_DONE) {
        return;
    }

    JZAudioOutputWindows* pImpl = reinterpret_cast<JZAudioOutputWindows*>(dwInstance);
    WAVEHDR* pHeader = reinterpret_cast<WAVEHDR*>(dwParam1);

    if (!pImpl || !pHeader) {
        return;
    }

    std::lock_guard<std::mutex> lock(pImpl->m_queueMutex);

    if (pHeader->dwFlags & WHDR_PREPARED) {
        waveOutUnprepareHeader(hwo, pHeader, sizeof(WAVEHDR));
    }
    if (pHeader->lpData != nullptr) {
        delete[] pHeader->lpData;
    }
    auto it = std::find(pImpl->m_bufferQueue.begin(), pImpl->m_bufferQueue.end(), pHeader);
    if (it != pImpl->m_bufferQueue.end()) {
        pImpl->m_bufferQueue.erase(it);
    }
    delete pHeader;

    if (pImpl->m_bufferQueue.empty()) {
        auto output = pImpl->m_parent;
        runOnObject(output, [output] { output->sigPlayFinish.emit(); });
    }
}
