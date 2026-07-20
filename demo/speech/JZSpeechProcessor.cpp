#include "JZSpeechProcessor.h"
#include <cstdint>
#include "JZDebug.h"

// ===================== JZSpeechVadStub =====================
// 基于帧能量的简单 VAD 占位实现。
// 说明: 仅作占位/演示用, 真实场景建议替换为 WebRTC VAD / SpeexDSP 等。
JZSpeechVadStub::JZSpeechVadStub()
{
    // 16bit PCM: 样本范围约 ±32768, 平方和门限取一个较小值以便普通说话即可触发。
    m_threshold = 200 * 200;
    m_hangover  = 8; // 约 80ms (@16k, 320样本/帧 时为 8 帧)
}

bool JZSpeechVadStub::process(const JZByteArray &pcmIn, JZByteArray &pcmOut)
{
    pcmOut.clear();

    int bytesPerSample = m_format.bytesPerSample();
    if (bytesPerSample <= 0 || pcmIn.size() < bytesPerSample)
        return false;

    // 计算帧能量(平方和)。目前仅支持 Int16 单/多通道。
    long long energy = 0;
    int sampleCount = (int)(pcmIn.size() / bytesPerSample);
    if (m_format.sampleFormat == JZAudioFormat::Int16)
    {
        const int16_t *p = reinterpret_cast<const int16_t *>(pcmIn.constData());
        for (int i = 0; i < sampleCount; i++)
            energy += (long long)p[i] * p[i];
    }
    else
    {
        // 其它格式暂按直通处理(不判定能量)
        pcmOut = pcmIn;
        return true;
    }

    bool speech = (energy / sampleCount) >= m_threshold;
    if (speech)
    {
        m_hangCount = m_hangover;
        pcmOut = pcmIn;
        return true;
    }

    if (m_hangCount > 0)
    {
        // 拖尾保持: 仍当作语音输出, 避免截断词尾
        m_hangCount--;
        pcmOut = pcmIn;
        return true;
    }

    return false;
}

// ===================== JZSpeechAecStub =====================
void JZSpeechAecStub::process(const JZByteArray &nearIn, const JZByteArray &farRef, JZByteArray &out)
{
    // TODO: 占位实现为直通。真实实现应基于 farRef 估计并减去回声。
    (void)farRef;
    out = nearIn;
}

// ===================== JZSpeechNsStub =====================
void JZSpeechNsStub::process(const JZByteArray &pcmIn, JZByteArray &pcmOut)
{
    // TODO: 占位实现为直通。真实实现应替换为 SpeexDSP/WebRTC NS。
    pcmOut = pcmIn;
}

// ===================== JZSpeechAgcStub =====================
void JZSpeechAgcStub::process(const JZByteArray &pcmIn, JZByteArray &pcmOut)
{
    // TODO: 占位实现为直通。真实实现应做音量归一化。
    pcmOut = pcmIn;
}

// ===================== JZSpeechPreprocessStub =====================
JZSpeechPreprocessStub::JZSpeechPreprocessStub()
{
    m_aec = JZCreateSpeechAec();
    m_ns  = JZCreateSpeechNs();
    m_agc = JZCreateSpeechAgc();
}

void JZSpeechPreprocessStub::setAec(std::unique_ptr<JZSpeechAec> v)
{
    if (v) m_aec = std::move(v);
}

void JZSpeechPreprocessStub::setNs(std::unique_ptr<JZSpeechNs> v)
{
    if (v) m_ns = std::move(v);
}

void JZSpeechPreprocessStub::setAgc(std::unique_ptr<JZSpeechAgc> v)
{
    if (v) m_agc = std::move(v);
}

void JZSpeechPreprocessStub::setFormat(const JZAudioFormat &format)
{
    m_format = format;
    m_aec->setFormat(format);
    m_ns->setFormat(format);
    m_agc->setFormat(format);
}

void JZSpeechPreprocessStub::process(const JZByteArray &nearIn, const JZByteArray &farRef, JZByteArray &out)
{
    // 内部串联: AEC -> NS -> AGC
    JZByteArray t1, t2;
    m_aec->process(nearIn, farRef, t1);
    m_ns->process(t1, t2);
    m_agc->process(t2, out);
}

// ===================== JZSpeechKwsStub =====================
bool JZSpeechKwsStub::process(const JZByteArray &pcmIn, std::string &kw)
{
    // TODO: 占位实现为直通 (始终不命中)。真实实现应替换为 Porcupine/Snowboy 等。
    (void)pcmIn;
    kw.clear();
    return false;
}

// ===================== 工厂 =====================
std::unique_ptr<JZSpeechVad> JZCreateSpeechVad()
{
    return std::make_unique<JZSpeechVadStub>();
}

std::unique_ptr<JZSpeechAec> JZCreateSpeechAec()
{
    return std::make_unique<JZSpeechAecStub>();
}

std::unique_ptr<JZSpeechNs> JZCreateSpeechNs()
{
    return std::make_unique<JZSpeechNsStub>();
}

std::unique_ptr<JZSpeechAgc> JZCreateSpeechAgc()
{
    return std::make_unique<JZSpeechAgcStub>();
}

std::unique_ptr<JZSpeechPreprocess> JZCreateSpeechPreprocess()
{
    return std::make_unique<JZSpeechPreprocessStub>();
}

std::unique_ptr<JZSpeechKws> JZCreateSpeechKws()
{
    return std::make_unique<JZSpeechKwsStub>();
}
