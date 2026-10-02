#include <stdexcept>
#include <iostream>
#include <cstring>
#include "JZAudioOutputWindows.h"
#include "JZThread.h"
#include "JZDebug.h"

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
    auto format = m_device->format();
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
    // 必须先离开 m_queueMutex 作用域, 否则 waveOutReset/Close 等待 WOM_DONE 回调时,
    // 回调(waveOutProc)也要拿同一把锁 -> 死锁, 表现为卡在 waveOutReset。
    HWAVEOUT h = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        if (!m_isPlaying || m_hWaveOut == nullptr) {
            return;
        }
        m_isPlaying = false;
        h = m_hWaveOut;
        m_hWaveOut = nullptr;
    }

    // 不调用 waveOutReset: 它会同步等待设备停止/所有 WOM_DONE 回调,
    // 当设备被别的程序占用时可能长时间阻塞(卡在 wdmaud.drv)。
    // 改用 waveOutPause(立即返回) + waveOutClose(强制释放所有 pending 缓冲, 不等应用层确认)。
    waveOutPause(h);
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        cleanupBuffers();
    }
    waveOutClose(h);
}

void JZAudioOutputWindows::reset()
{
    HWAVEOUT h = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        if (m_hWaveOut == nullptr) {
            return;
        }
        h = m_hWaveOut;
    }

    // 同上: 用 Pause + (调用方负责后续 close) 替代会同步等待的 waveOutReset。
    waveOutPause(h);
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        cleanupBuffers();
    }
}

bool JZAudioOutputWindows::write(const JZByteArray& data)
{
    if (!m_isPlaying || m_hWaveOut == nullptr || data.isEmpty()) {
        return false;
    }

    if (m_bufferQueue.size() >= MAX_BUFFER_COUNT) {
        // 队列满: 直接失败, **不丢数据**(调用方稍后重试)。
        // ⚠ 这里以前会打一行 "Buffer queue full, drop data" —— 但那是正常流控(没丢),
        //   打成错误反而误导; 上层看返回值 false 就够了。
        return false;
    }

    WAVEHDR* pHeader = new WAVEHDR{ 0 };
    char* pBuffer = new char[data.size()];
    memcpy(pBuffer, data.data(), data.size());

    pHeader->lpData = pBuffer;
    pHeader->dwBufferLength = static_cast<DWORD>(data.size());
    pHeader->dwFlags = 0;

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        if (m_hWaveOut == nullptr) {
            delete[] pBuffer;
            delete pHeader;
            return false;
        }
        MMRESULT result = waveOutPrepareHeader(m_hWaveOut, pHeader, sizeof(WAVEHDR));
        if (result != MMSYSERR_NOERROR) {
            delete[] pBuffer;
            delete pHeader;
            std::cerr << "waveOutPrepareHeader failed: " << result << std::endl;
            return false;
        }
        // 注意: waveOutWrite 在锁外调用, 避免设备被别的程序占用时(wdmaud.drv 同步等待)
        // 阻塞本线程(通常是主线程/网络线程)导致界面卡死, 也避免与 waveOutProc 回调死锁。
        m_bufferQueue.push_back(pHeader);
    }
    // ⚠ 这儿以前有 `jzDebug() << "waveOutWrite";` —— 每喂一块就喷一行, 把脚本的 stdout 搅乱(实测)。
    MMRESULT result = waveOutWrite(m_hWaveOut, pHeader, sizeof(WAVEHDR));
    if (result != MMSYSERR_NOERROR) {
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            auto it = std::find(m_bufferQueue.begin(), m_bufferQueue.end(), pHeader);
            if (it != m_bufferQueue.end())
                m_bufferQueue.erase(it);
        }
        waveOutUnprepareHeader(m_hWaveOut, pHeader, sizeof(WAVEHDR));
        delete[] pBuffer;
        delete pHeader;
        std::cerr << "waveOutWrite failed: " << result << std::endl;
        return false;
    }

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
        auto output = pImpl->m_device;
        runOnObject(output, [output] { output->sigPlayFinish.emit(); });
    }
}
