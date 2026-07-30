#ifndef JZ_AUDIO_INPUT_LINUX_H_
#define JZ_AUDIO_INPUT_LINUX_H_

#include <alsa/asoundlib.h>
#include "JZAudioFormat.h"
#include "JZByteArray.h"
#include "JZTimer.h"
#include "JZAudioDeviceInfo.h"
#include "detail/JZAudioInputImpl.h"

class JZAudioInput;
class JZAudioInputLinux : public JZAudioInputImpl
{
public:
    explicit JZAudioInputLinux(JZAudioInput* parent);
    ~JZAudioInputLinux();

    bool open(const JZAudioDeviceInfo &device) override;
    void close() override;

private:
    // 实际处理音频数据的函数
    void onAudioTimer();
    // 释放资源
    void releaseResources();
    // 核心：将JZAudioFormat映射为ALSA采样格式
    snd_pcm_format_t mapSampleFormat(JZAudioFormat::SampleFormat format);

    JZAudioInput* m_parent;          // 父类指针，触发sigDataReady
    snd_pcm_t* m_pcmHandle;          // ALSA设备句柄
    bool m_isOpened;                 // 设备是否打开
    char* m_buffer;                  // 动态音频缓冲区（根据格式调整）
    int m_bufferFrames;              // 缓冲区帧数（用于读取数据）
    JZByteArray m_audioBuffer;
    JZTimer *m_audioTimer;
};

#endif
