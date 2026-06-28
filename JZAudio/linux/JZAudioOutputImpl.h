#ifndef JZ_AUDIO_OUTPUT_IMPL_H_
#define JZ_AUDIO_OUTPUT_IMPL_H_

#include <alsa/asoundlib.h>
#include <list>
#include "JZAudioFormat.h"
#include "JZByteArray.h"
#include "JZAudioDevice.h"
#include "JZTimer.h"

class JZAudioOutput;
class JZAudioOutputImpl : public JZObject
{
public:
    explicit JZAudioOutputImpl(JZAudioOutput* parent);
    ~JZAudioOutputImpl();

    bool open(const JZAudioDevice& device);
    void close();
    void reset();
    bool write(const JZByteArray& data);

private:
    void onAudioTimerTimeout();

    // 辅助函数：将JZAudioFormat的采样格式映射为ALSA格式
    snd_pcm_format_t mapSampleFormat(JZAudioFormat::SampleFormat format);
    // 释放音频设备资源
    void playFinished();
    void releaseResources();
    int getBytesPerFrame() ;
    JZByteArray generateSilence(int frames);

    JZTimer* m_audioTimer;
    JZAudioOutput* m_parent;          // 父类指针，读取m_format
    snd_pcm_t* m_pcmHandle;           // ALSA音频输出设备句柄
    bool m_isOpened;                  // 设备是否已打开
    snd_pcm_uframes_t m_periodSize;   // ALSA周期大小（优化写入效率）
    bool m_isPlay;
    JZByteArray m_buffer;
    int m_silenceFilled;
};
#endif // !JZ_AUDIO_OUTPIT_IMPL_H_
