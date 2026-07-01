#include <alsa/asoundlib.h>
#include <iostream>
#include <cstring>
#include "JZAudioOutputImpl.h"
#include "JZAudioOutput.h"
#include "JZAudioFormat.h"
#include "JZByteArray.h"
#include "JZAudioDeviceImpl.h"
#include "JZDebug.h"

// 构造函数
JZAudioOutputImpl::JZAudioOutputImpl(JZAudioOutput* parent)
    : m_parent(parent)
    , m_pcmHandle(nullptr)
    , m_isOpened(false)
    , m_periodSize(0)
{
    m_isPlay = false;
    m_silenceFilled = 0;

    m_audioTimer = new JZTimer(this);
    connect(&m_audioTimer->sigTimeout,this, &JZAudioOutputImpl::onAudioTimerTimeout);
}

// 析构函数
JZAudioOutputImpl::~JZAudioOutputImpl() {
    close(); // 析构时确保关闭设备
}

// 采样格式映射（和输入类保持一致）
snd_pcm_format_t JZAudioOutputImpl::mapSampleFormat(JZAudioFormat::SampleFormat format) {
    switch (format) {
        case JZAudioFormat::UInt8:
            return SND_PCM_FORMAT_U8;
        case JZAudioFormat::Int16:
            return SND_PCM_FORMAT_S16_LE;
        case JZAudioFormat::Int32:
            return SND_PCM_FORMAT_S32_LE;
        case JZAudioFormat::Float:
            return SND_PCM_FORMAT_FLOAT_LE;
        case JZAudioFormat::UnknownSampleFormat:
        default:
            std::cerr << "未知采样格式，默认使用Int16" << std::endl;
            return SND_PCM_FORMAT_S16_LE;
    }
}

// 打开指定的音频输出设备
bool JZAudioOutputImpl::open(const JZAudioDeviceInfo& device) {
    if (m_isOpened) {
        std::cerr << "音频输出设备已打开，无需重复初始化" << std::endl;
        return true;
    }

    const JZAudioFormat& parentFormat = m_parent->m_format;

    // 2. 参数合法性检查
    if (parentFormat.channelCount <= 0 || parentFormat.sampleRate <= 0) {
        std::cerr << "父类m_format参数无效：声道数=" << parentFormat.channelCount 
                  << " 采样率=" << parentFormat.sampleRate << std::endl;
        return false;
    }

    // 4. 打开ALSA音频输出设备（使用指定的设备ID）
    int ret = snd_pcm_open(&m_pcmHandle, device.id.c_str(), SND_PCM_STREAM_PLAYBACK, 0);
    if (ret < 0) {
        std::cerr << "打开音频输出设备失败: " << snd_strerror(ret) << std::endl;
        return false;
    }

    // 5. 配置音频参数（基于父类m_format）
    snd_pcm_hw_params_t* params = nullptr;
    snd_pcm_hw_params_alloca(&params);
    snd_pcm_hw_params_any(m_pcmHandle, params);

    // 5.1 设置访问模式（交错模式，通用）
    ret = snd_pcm_hw_params_set_access(m_pcmHandle, params, SND_PCM_ACCESS_RW_INTERLEAVED);
    if (ret < 0) {
        std::cerr << "设置访问模式失败: " << snd_strerror(ret) << std::endl;
        releaseResources();
        return false;
    }

    // 5.2 设置采样格式（映射自父类m_format）
    auto audio_format = m_parent->m_format;
    snd_pcm_format_t alsaFormat = mapSampleFormat(audio_format.sampleFormat);
    ret = snd_pcm_hw_params_set_format(m_pcmHandle, params, alsaFormat);
    if (ret < 0) {
        std::cerr << "设置采样格式失败: " << snd_strerror(ret) << std::endl;
        releaseResources();
        return false;
    }

    // 5.3 设置声道数（来自父类m_format）
    ret = snd_pcm_hw_params_set_channels(m_pcmHandle, params, audio_format.channelCount);
    if (ret < 0) {
        std::cerr << "设置声道数失败: " << snd_strerror(ret) << std::endl;
        releaseResources();
        return false;
    }

    // 5.4 设置采样率（关键缺失步骤）
    unsigned int sampleRate = audio_format.sampleRate;
    ret = snd_pcm_hw_params_set_rate(m_pcmHandle, params, sampleRate, 0);
    if (ret < 0) {
        jzDebug() << "错误：设置采样率失败: rate=" << sampleRate << ", error=" << snd_strerror(ret);
        releaseResources();
        return false;
    }

    // 5.5 设置周期大小（优化播放流畅度）
    snd_pcm_uframes_t periodSize = 2048;
    ret = snd_pcm_hw_params_set_period_size_near(m_pcmHandle, params, &periodSize, nullptr);
    if (ret < 0) {
        std::cerr << "设置周期大小失败: " << snd_strerror(ret) << std::endl;
        releaseResources();
        return false;
    }
    m_periodSize = periodSize;

    // 5.6 应用所有参数
    ret = snd_pcm_hw_params(m_pcmHandle, params);
    if (ret < 0) {
        std::cerr << "应用音频参数失败: " << snd_strerror(ret) << std::endl;
        releaseResources();
        return false;
    }

    // 6. 准备设备，进入播放状态
    ret = snd_pcm_prepare(m_pcmHandle);
    if (ret < 0) {
        std::cerr << "准备音频输出设备失败: " << snd_strerror(ret) << std::endl;
        releaseResources();
        return false;
    }

    m_isOpened = true;
    m_silenceFilled = 0;
    std::cout << "音频输出设备初始化成功（基于父类m_format）："
              << "声道数=" << audio_format.channelCount
              << " 采样率=" << audio_format.sampleRate
              << " 采样格式=" << audio_format.sampleFormat << std::endl;
    return true;
}

// 关闭音频输出设备
void JZAudioOutputImpl::close() {
    if (!m_isOpened) 
        return;

    if(m_isPlay){
        playFinished();
    }

    // 2. 释放设备资源
    releaseResources();

    // 3. 停止定时器
    m_audioTimer->stop();
    m_buffer.clear();

    m_isOpened = false;
    m_isPlay = false;
    m_periodSize = 0;
}

void JZAudioOutputImpl::reset()
{
    m_buffer.clear();
    if (m_pcmHandle) {
        snd_pcm_drop(m_pcmHandle);
        snd_pcm_prepare(m_pcmHandle);
    }
}

void JZAudioOutputImpl::playFinished()
{
    m_audioTimer->stop();
    m_buffer.clear();

    if(m_isPlay){
        m_isPlay = false;
        m_parent->sigPlayFinish.emit();
    }
}

// 释放ALSA设备资源
void JZAudioOutputImpl::releaseResources() {
    if (m_pcmHandle) {
        snd_pcm_close(m_pcmHandle);
        m_pcmHandle = nullptr;
    }
}

// 写入音频数据并播放
// 写入数据（仅追加到缓存，不变）
bool JZAudioOutputImpl::write(const JZByteArray& data) {
    if (!m_isOpened || data.isEmpty()) {
        std::cerr << "无法写入音频数据：设备未打开或数据为空" << std::endl;
        return false;
    }

    // 加锁保证线程安全
    if(!m_isPlay)
    {
        snd_pcm_drop(m_pcmHandle);
        
        int ret = snd_pcm_prepare(m_pcmHandle);
        if (ret < 0) {
            std::cerr << "ALSA 设备重新准备失败：" << snd_strerror(ret) << std::endl;
            return false;
        }

        m_isPlay = true;
        m_audioTimer->start(20);
    }

    m_buffer.append(data);
    m_silenceFilled = 0;
    onAudioTimerTimeout();
    return true;
}

int JZAudioOutputImpl::getBytesPerFrame() 
{
    snd_pcm_format_t alsaFormat = mapSampleFormat(m_parent->m_format.sampleFormat);
    int sampleWidth = snd_pcm_format_width(alsaFormat) / 8;
    return sampleWidth * m_parent->m_format.channelCount;
}

JZByteArray JZAudioOutputImpl::generateSilence(int frames) {
    int bytesPerFrame = getBytesPerFrame();
    JZByteArray silence;
    silence.resize(frames * bytesPerFrame, 0);
    return silence;
}

// 统一定时器回调：整合缓存填充 + sigPlayFinish 触发逻辑
void JZAudioOutputImpl::onAudioTimerTimeout() {
    if (!m_isOpened || !m_pcmHandle || !m_isPlay)
        return;

    int bytesPerFrame = getBytesPerFrame();
    if (bytesPerFrame <= 0) {
        jzDebug() << "获取每帧字节数失败，直接返回";
        return;
    }

    // 计算100ms对应的帧数
    int targetFrames100ms = (m_parent->m_format.sampleRate * 100) / 1000;
    
    // 1. 查询可用帧数
    snd_pcm_sframes_t availFrames = snd_pcm_avail_update(m_pcmHandle);
    if (availFrames < 0) {
        if (availFrames == -EPIPE) {
            int recoverRet = snd_pcm_recover(m_pcmHandle, availFrames, 0);
            if (recoverRet < 0) {
                std::cerr << "underrun恢复失败: " << snd_strerror(recoverRet) << std::endl;
                playFinished();
                return;
            }
            availFrames = snd_pcm_avail_update(m_pcmHandle);
            if (availFrames < 0) {
                playFinished();
                return;
            }
        } else {
            std::cerr << "查询剩余帧数失败: " << snd_strerror(availFrames) << std::endl;
            return;
        }
    }

    // 2. 填充数据：优先真实数据，不够用静音补足到100ms
    if (availFrames > 0) {
        snd_pcm_sframes_t currentDelay = 0;
        snd_pcm_delay(m_pcmHandle, &currentDelay);
        
        // 计算需要填充多少帧才能让总缓冲达到100ms
        snd_pcm_uframes_t totalNeeded = 0;
        if (currentDelay < targetFrames100ms) {
            totalNeeded = targetFrames100ms - currentDelay;
            totalNeeded = std::min(totalNeeded, static_cast<snd_pcm_uframes_t>(availFrames));
        }
        
        if (totalNeeded > 0) {
            // 第一步：填充真实数据
            int realFramesWritten = 0;
            if (!m_buffer.isEmpty()) {
                int maxRealFrames = m_buffer.size() / bytesPerFrame;
                snd_pcm_uframes_t realToWrite = std::min(
                    static_cast<snd_pcm_uframes_t>(maxRealFrames),
                    totalNeeded
                );
                
                if (realToWrite > 0) {
                    int bytesToWrite = realToWrite * bytesPerFrame;
                    JZByteArray writeData = m_buffer.mid(0, bytesToWrite);
                    
                    const char* rawData = reinterpret_cast<const char*>(writeData.data());
                    int writeRet = snd_pcm_writei(m_pcmHandle, rawData, realToWrite);
                    
                    if (writeRet < 0) {
                        int writeRecoverRet = snd_pcm_recover(m_pcmHandle, writeRet, 0);
                        if (writeRecoverRet < 0) {
                            std::cerr << "写入真实数据失败: " << snd_strerror(writeRet) << std::endl;
                        }
                    } else {
                        m_buffer = m_buffer.mid(writeRet * bytesPerFrame);
                        realFramesWritten = writeRet;
                    }
                }
            }
            
            // 如果填充了真实数据，重置静音计数
            if (realFramesWritten > 0) {
                m_silenceFilled = 0;
            }

            // 第二步：如果真实数据不够，用静音补足到100ms
            snd_pcm_uframes_t silenceNeeded = totalNeeded - realFramesWritten;
            
            // 限制静音填充不超过100ms
            if (silenceNeeded > static_cast<snd_pcm_uframes_t>(targetFrames100ms)) {
                silenceNeeded = targetFrames100ms;
            }
            
            // 累加的静音不能超过100ms
            if (m_silenceFilled + silenceNeeded > static_cast<snd_pcm_uframes_t>(targetFrames100ms)) {
                silenceNeeded = targetFrames100ms - m_silenceFilled;
            }
            
            if (silenceNeeded > 0) {
                JZByteArray silence = generateSilence(silenceNeeded);
                if (!silence.isEmpty()) {
                    const char* rawSilence = reinterpret_cast<const char*>(silence.data());
                    int writeRet = snd_pcm_writei(m_pcmHandle, rawSilence, silenceNeeded);
                    
                    if (writeRet < 0) {
                        int recoverRet = snd_pcm_recover(m_pcmHandle, writeRet, 0);
                        if (recoverRet < 0) {
                            std::cerr << "写入静音失败: " << snd_strerror(writeRet) << std::endl;
                        }
                    } else {
                        m_silenceFilled += writeRet;
                    }
                }
            }
        }
    }

    // 3. 检查播放状态
    snd_pcm_state_t state = snd_pcm_state(m_pcmHandle);
    
    if (state == SND_PCM_STATE_RUNNING) {
        snd_pcm_sframes_t delayFrames = 0;
        int ret = snd_pcm_delay(m_pcmHandle, &delayFrames);
        
        if (ret == 0) 
        {
            bool is_buffer_empty = m_buffer.isEmpty();
            if (is_buffer_empty && delayFrames <= m_silenceFilled) {
                snd_pcm_drop(m_pcmHandle);
                playFinished();
                return; 
            }
        }
    } else if (state == SND_PCM_STATE_XRUN) {
        jzDebug() << "检测到XRUN";
        int recoverRet = snd_pcm_recover(m_pcmHandle, -EPIPE, 0);
        if (recoverRet < 0) {
            playFinished();
        } else {
            // 恢复后重新填充
            m_silenceFilled = 0;
        }
    } else{
        playFinished();
    }
}