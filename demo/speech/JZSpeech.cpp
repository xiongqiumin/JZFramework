#include "JZSpeech.h"
#include "JZPlatform.h"

JZSpeech::JZSpeech()
{
    connect(&m_input.sigReadyRead, this, &JZSpeech::onAudioInput);
    connect(&m_output.sigPlayFinish, this, &JZSpeech::onPlayFinished);
    connect(&m_aec.sigAecResult, this, &JZSpeech::onAecResult);
}

JZSpeech::~JZSpeech()
{
} 

bool JZSpeech::init()
{
    // AEC 基于 int16 处理, 强制输入输出统一格式, 避免回声失真/速率异常
    JZAudioFormat fmt;
    fmt.sampleRate   = 16000;
    fmt.channelCount = 1;
    fmt.sampleFormat = JZAudioFormat::Int16;
    m_input.setFormat(fmt);
    m_output.setFormat(fmt);

    if(!m_input.open())
        return false;

    if(!m_output.open())
        return false;

    // AEC 滤波器长度覆盖回声路径时延(默认 256ms @16k = 4096 抽头)
    m_aec.init(16000, 256);

    return true;
}

void JZSpeech::onAudioInput(const JZByteArray &buffer)
{
    int n = buffer.size() / 2;
    if (n <= 0)
        return;

    int64_t now  = JZPlatform::getTickCountMs();
    int64_t bufMs = (int64_t)((double)n / 16000.0 * 1000.0 + 0.5);
    int64_t capTime = now - bufMs;   // 本帧采集起始时刻

    if (!m_aecEnabled)
    {
        // 原始回声测试: 麦克风直接回放, 并把播放内容当参考(保持 AEC 参考连续)
        m_output.write(buffer);
        m_aec.addOutput(now, buffer);
        return;
    }

    // 断环: 用"上一帧实际播放出去"的音频做参考, 而非原始 mic,
    // 否则扬声器放的是 AEC 结果而非 mic, 参考会错位导致消不干净。
    if (!m_lastPlayed.isEmpty())
        m_aec.addOutput(m_lastRenderTime, m_lastPlayed);

    // 喂采集流 -> AEC 内部对齐参考并 emit sigAecResult
    m_aec.addInput(capTime, buffer);
}

void JZSpeech::onAecResult(const JZByteArray& out)
{
    // AEC 结果即"去回声后的近端语音", 写回扬声器, 并记为下一帧的播放参考
    m_output.write(out);
    m_lastPlayed     = out;
    m_lastRenderTime = JZPlatform::getTickCountMs();
}

void JZSpeech::onPlayFinished()
{
    // sigPlayFinish 仅在输出队列完全排空(发生间隙/欠载)时触发。
    // 此时扬声器已无声, 若继续用间隙之前的旧样本做回声参考会错位,
    // 因此重置 AEC(内部同步清空参考环形缓冲与滤波系数), 让回路重新收敛。
    m_aec.reset();
}