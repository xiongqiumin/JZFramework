#include "JZSpeechCore.h"
#include "JZDebug.h"

JZ_OBJECT_IMPL(JZSpeechCore, JZObject)

JZSpeechCore::JZSpeechCore(JZObject *parent)
    : JZObject(parent)
    , sigError(this)
{
    // 默认使用占位处理器
    m_pre = JZCreateSpeechPreprocess();
    m_vad = JZCreateSpeechVad();
    m_kws = JZCreateSpeechKws();

    m_format.channelCount = 1;
    m_format.sampleRate   = 16000;
    m_format.sampleFormat = JZAudioFormat::Int16;

    // 衔接 LLM 节点 (信号常驻)
    connect(&m_llm.sigReply,    this, &JZSpeechCore::onLlmReply);
    connect(&m_llm.sigFinished, this, &JZSpeechCore::onLlmFinished);

    m_running = true;
}

JZSpeechCore::~JZSpeechCore()
{
}

void JZSpeechCore::setAudioOutputSink(std::function<void(const JZByteArray &)> sink)
{
    m_audioOutput = std::move(sink);
}

void JZSpeechCore::setPreprocess(std::unique_ptr<JZSpeechPreprocess> pre)
{
    if (pre) { pre->setFormat(m_format); m_pre = std::move(pre); }
}

void JZSpeechCore::setVad(std::unique_ptr<JZSpeechVad> vad)
{
    if (vad) { vad->setFormat(m_format); m_vad = std::move(vad); }
}

void JZSpeechCore::setKws(std::unique_ptr<JZSpeechKws> kws)
{
    if (kws) { kws->setFormat(m_format); m_kws = std::move(kws); }
}

void JZSpeechCore::setAsr(JZSpeechAsr *asr)
{
    m_asr = asr;
    if (m_asr)
    {
        m_asr->setFormat(m_format);
        m_asr->setResultCallback([this](const std::string &text) { onAsrResult(text); });
    }
}

void JZSpeechCore::setTts(JZSpeechTts *tts)
{
    m_tts = tts;
    if (m_tts)
    {
        m_tts->setFormat(m_format);
        m_tts->setPcmCallback([this](const JZByteArray &pcm) { onTtsPcm(pcm); });
    }
}

void JZSpeechCore::reset()
{
    // 结束当前 ASR/TTS 会话
    if (m_asr)
        m_asr->flush();

    // 清空远端参考与计数
    m_farRef.clear();
    m_tick = 0;
}

// 音频输入入口: 上行链路处理
void JZSpeechCore::audioInput(const JZByteArray &pcm)
{
    // 自动累加经过时间(按帧长计算, 用于静音超时 / 时序逻辑)
    int bps = m_format.bytesPerSample();
    if (bps > 0 && m_format.channelCount > 0 && m_format.sampleRate > 0)
        m_tick += static_cast<int64_t>(pcm.size()) * 1000 /
                  (bps * m_format.channelCount * m_format.sampleRate);

    // 1) 上行预处理: AEC -> NS -> AGC (farRef 为最近播放音频)
    JZByteArray preOut;
    m_pre->process(pcm, m_farRef, preOut);

    // 2) VAD: 仅在有语音时继续
    JZByteArray vadOut;
    if (m_vadEnabled)
    {
        if (!m_vad->process(preOut, vadOut))
            return;
    }
    else
    {
        vadOut = preOut;
    }

    // 3) KWS: 未命中唤醒词则抑制上行
    if (m_kwsEnabled && m_kws)
    {
        std::string kw;
        if (!m_kws->process(vadOut, kw))
            return;
    }

    // 4) ASR: 喂入音频帧
    if (m_asr)
        m_asr->feed(vadOut);
}

void JZSpeechCore::onAsrResult(const std::string &text)
{
    if (text.empty())
        return;
    jzDebug() << "[speech-core] ASR:" << text.c_str();
    // 识别文本送入 LLM
    m_llm.request(text);
}

void JZSpeechCore::onLlmReply(const std::string &text)
{
    jzDebug() << "[speech-core] LLM:" << text.c_str();
    // LLM 回复送入 TTS 合成
    if (m_tts)
        m_tts->feed(text);
}

void JZSpeechCore::onLlmFinished()
{
    // 一句话结束: 可触发 ASR flush / 复位状态等
    if (m_asr)
        m_asr->flush();
}

void JZSpeechCore::onTtsPcm(const JZByteArray &pcm)
{
    if (pcm.isEmpty())
        return;
    // 更新 AEC 远端参考, 并经输出汇播放
    m_farRef = pcm;
    if (m_audioOutput)
        m_audioOutput(pcm);
}
