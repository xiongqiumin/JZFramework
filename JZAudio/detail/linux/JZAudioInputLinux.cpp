 // 包含JZAudioFormat的定义
#include <alsa/asoundlib.h>
#include <iostream>
#include <cstring>
#include <stdexcept>
#include "JZAudioInputImpl.h"
#include "JZAudioInput.h"
#include "JZAudioFormat.h"
#include "JZByteArray.h"
#include "JZElapsedTimer.h"
#include "JZDebug.h"
#include "JZAudioDeviceImpl.h"

JZAudioInputImpl::JZAudioInputImpl(JZAudioInput* parent)
    : m_parent(parent)
    , m_pcmHandle(nullptr)
    , m_isOpened(false)
    , m_buffer(nullptr)
    , m_bufferFrames(0)
{
    m_audioTimer = new JZTimer(this);
    connect(&m_audioTimer->sigTimeout,this, &JZAudioInputImpl::onAudioTimer);
}

JZAudioInputImpl::~JZAudioInputImpl() {
    close();
}

// 核心：将JZAudioFormat的采样格式映射为ALSA格式
snd_pcm_format_t JZAudioInputImpl::mapSampleFormat(JZAudioFormat::SampleFormat format) {
    switch (format) {
        case JZAudioFormat::UInt8:
            return SND_PCM_FORMAT_U8;      // 8位无符号整型
        case JZAudioFormat::Int16:
            return SND_PCM_FORMAT_S16_LE;  // 16位有符号整型（小端）
        case JZAudioFormat::Int32:
            return SND_PCM_FORMAT_S32_LE;  // 32位有符号整型（小端）
        case JZAudioFormat::Float:
            return SND_PCM_FORMAT_FLOAT_LE;// 32位浮点（小端）
        case JZAudioFormat::UnknownSampleFormat:
        default:
            std::cerr << "不支持的采样格式，默认使用Int16" << std::endl;
            return SND_PCM_FORMAT_S16_LE;
    }
}

bool JZAudioInputImpl::open(const JZAudioDeviceInfo &device)
{
    if (m_isOpened) return true;
    if (!m_parent) {
        std::cerr << "父类指针为空，无法初始化音频格式" << std::endl;
        return false;
    }

    JZAudioFormat audioFormat = m_parent->m_format;

    // 2. 打开ALSA音频采集设备（使用指定的设备ID）
    int ret = snd_pcm_open(&m_pcmHandle, device.id.c_str(), SND_PCM_STREAM_CAPTURE, 0);
    if (ret < 0) {
        std::cerr << "打开音频设备失败: " << snd_strerror(ret) << std::endl;
        return false;
    }

    // 3. 配置音频参数（基于parent->m_audioFormat）
    snd_pcm_hw_params_t* params = nullptr;
    snd_pcm_hw_params_alloca(&params);
    snd_pcm_hw_params_any(m_pcmHandle, params);

    // 3.1 设置访问模式（交错模式，通用）
    ret = snd_pcm_hw_params_set_access(m_pcmHandle, params, SND_PCM_ACCESS_RW_INTERLEAVED);
    if (ret < 0) {
        std::cerr << "设置访问模式失败: " << snd_strerror(ret) << std::endl;
        releaseResources();
        return false;
    }

    // 3.2 设置采样格式（从JZAudioFormat映射）
    snd_pcm_format_t alsaFormat = mapSampleFormat(audioFormat.sampleFormat);
    ret = snd_pcm_hw_params_set_format(m_pcmHandle, params, alsaFormat);
    if (ret < 0) {
        std::cerr << "设置采样格式失败: " << snd_strerror(ret) << std::endl;
        releaseResources();
        return false;
    }

    // 3.3 设置声道数（从JZAudioFormat读取）
    ret = snd_pcm_hw_params_set_channels(m_pcmHandle, params, audioFormat.channelCount);
    if (ret < 0) {
        std::cerr << "设置声道数失败: " << snd_strerror(ret) << std::endl;
        releaseResources();
        return false;
    }

    // 3.4 设置采样率（从JZAudioFormat读取，自动适配最接近的支持值）
    unsigned int sampleRate = audioFormat.sampleRate;
    ret = snd_pcm_hw_params_set_rate(m_pcmHandle, params, sampleRate, 0);
    if (ret < 0) {
        std::cerr << "设置采样率失败: " << snd_strerror(ret) << std::endl;
        releaseResources();
        return false;
    }
    // 同步实际采样率回上层（如果系统不支持指定值，更新到实际值）
    if (sampleRate != audioFormat.sampleRate) {
        std::cout << "采样率 " << audioFormat.sampleRate << " 不支持，使用 " << sampleRate << std::endl;
        audioFormat.sampleRate = sampleRate;
    }

    // 3.5 应用所有参数
    ret = snd_pcm_hw_params(m_pcmHandle, params);
    if (ret < 0) {
        std::cerr << "应用音频参数失败: " << snd_strerror(ret) << std::endl;
        releaseResources();
        return false;
    }

    // 4. 初始化动态缓冲区（根据采样格式和声道数计算）
    m_bufferFrames = 4096 / snd_pcm_format_width(alsaFormat) / audioFormat.channelCount;
    int bufferSize = m_bufferFrames * snd_pcm_format_width(alsaFormat) / 8 * audioFormat.channelCount;
    m_buffer = new char[bufferSize];
    if (!m_buffer) {
        std::cerr << "分配音频缓冲区失败" << std::endl;
        releaseResources();
        return false;
    }
    memset(m_buffer, 0, bufferSize);

    // 5. 注册ALSA异步回调
    // 初始化PCM参数时配置周期大小（核心：调整这个值控制回调频率）
    snd_pcm_uframes_t period_size = 2048; // 周期越大，回调频率越低
    snd_pcm_uframes_t buffer_size = period_size * 4; // 缓冲区大小建议是周期的2-8倍

    // 设置周期大小（会自动适配系统支持的值）
    ret = snd_pcm_hw_params_set_period_size_near(m_pcmHandle, params, &period_size, nullptr);
    if (ret < 0) {
        std::cerr << "设置周期大小失败: " << snd_strerror(ret) << std::endl;
        releaseResources();
        return false;
    }

    // 6. 启动音频采集
    ret = snd_pcm_prepare(m_pcmHandle);
    if (ret < 0) {
        std::cerr << "准备音频设备失败: " << snd_strerror(ret) << std::endl;
        releaseResources();
        return false;
    }

    ret = snd_pcm_start(m_pcmHandle);
    if (ret < 0) {
        std::cerr << "启动音频设备失败: " << snd_strerror(ret) << std::endl;
        releaseResources();
        return false;
    }

    m_audioTimer->start(50);
    m_isOpened = true;
    std::cout << "麦克风初始化成功：" 
              << "声道数=" << audioFormat.channelCount 
              << " 采样率=" << audioFormat.sampleRate 
              << " 采样格式=" << audioFormat.sampleFormat << std::endl;
    return true;
}

void JZAudioInputImpl::close() {
    if (!m_isOpened) 
        return;

    // 2. 释放缓冲区
    if (m_buffer) {
        delete[] m_buffer;
        m_buffer = nullptr;
        m_bufferFrames = 0;
    }

    // 3. 关闭PCM设备
    releaseResources();

    m_isOpened = false;
}

void JZAudioInputImpl::releaseResources() {
    if (m_pcmHandle) {
        snd_pcm_close(m_pcmHandle);
        m_pcmHandle = nullptr;
    }
    m_audioTimer->stop();
}


// 实际处理音频数据（基于动态配置的格式）
void JZAudioInputImpl::onAudioTimer() 
{
    if (!m_pcmHandle || !m_isOpened || !m_parent || !m_buffer) 
        return;

    snd_pcm_sframes_t avail_frames = snd_pcm_avail_update(m_pcmHandle);
    if (avail_frames < 0) {
        // 处理查询错误（如下溢）
        int ret = snd_pcm_recover(m_pcmHandle, avail_frames, 1);
        if (ret < 0) {
            std::cerr << "查询可用帧数失败: " << snd_strerror(avail_frames) << std::endl;
            return;
        }
    }

    snd_pcm_format_t alsaFormat = mapSampleFormat(m_parent->m_format.sampleFormat);
    // ========== 修正多通道处理核心 ==========
    // 1. 获取音频通道数（需确保m_parent->m_format.channelCount已正确初始化）
    unsigned int channel_count = m_parent->m_format.channelCount;
    // 2. 正确计算每帧字节数：每采样字节数 × 通道数
    int frame_size = (snd_pcm_format_width(alsaFormat) / 8) * channel_count;
    // =======================================

    snd_pcm_sframes_t remaining_frames = avail_frames; // 剩余待读取帧数
    while (remaining_frames > 0) {
        // 确定本次读取帧数：剩余帧≥m_bufferFrames则读m_bufferFrames，否则读剩余帧
        int read_frames = std::min((int)remaining_frames, m_bufferFrames);

        // 步骤3：读取数据到临时缓冲区m_buffer
        int frames = snd_pcm_readi(m_pcmHandle, m_buffer, read_frames);
        if (frames < 0) {
            // 处理读取错误（如下溢）
            int ret = snd_pcm_recover(m_pcmHandle, frames, 1);
            if (ret < 0) {
                std::cerr << "读取音频数据失败: " << snd_strerror(frames) << std::endl;
                break; // 无法恢复，退出循环
            }
            continue; // 恢复后重试本次读取
        } else if (frames == 0) {
            std::cout << "本次未读取到任何帧，终止循环" << std::endl;
            break;
        }
        remaining_frames -= frames;

        // 步骤4：将m_buffer中的数据追加到m_audioBuffer（加锁保证线程安全）
        m_audioBuffer.append(JZByteArray(m_buffer, frames * frame_size));
    }

    int frame_min_size = 2048 * channel_count;
    if(m_audioBuffer.size() > frame_min_size)
    {
        int len = m_audioBuffer.size()/frame_min_size * frame_min_size;
        m_parent->sigReadyRead.emit(m_audioBuffer.mid(0,len));
        m_audioBuffer = m_audioBuffer.mid(len);
    }
}