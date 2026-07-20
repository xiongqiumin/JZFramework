#ifndef JZ_SPEECH_CORE_H_
#define JZ_SPEECH_CORE_H_

#include <memory>
#include <string>
#include <functional>
#include "JZObject.h"
#include "JZAgent.h"
#include "JZByteArray.h"
#include "JZAudioFormat.h"
#include "JZSpeechProcessor.h"

// ============================================================================
//  JZSpeechCore: 语音处理管线引擎
//
//  上行链路(音频 -> 文本 -> 回复 -> 音频):
//      AudioInput
//        -> Preprocess(AEC+NS+AGC)
//        -> VAD
//        -> KWS (唤醒词)
//        -> ASR (JZSpeechClient 暴露的 ASR 节点)  --feed(pcm)-->
//        -> LLM (JZAgent)                         --request(text)-->
//        -> TTS (JZSpeechClient 暴露的 TTS 节点)  --feed(text)-->
//        -> AudioOutput (PCM)
//
//  各节点均通过 setXxx 注入, 可替换为真实算法 / 远端服务。
//  ASR / TTS 节点由 JZSpeechClient 提供 (getAsrInterface / getTtsInterface)。
//  LLM 由 JZAgent 提供。
//
//  本文件为接口/接线骨架: 节点内部算法与网络协议在后续实现。
// ============================================================================
class JZSpeechCore : public JZObject
{
    JZ_OBJECT_DECLARE(JZSpeechCore)

public:
    JZSpeechCore(JZObject *parent = nullptr);
    ~JZSpeechCore();

    // 音频输出汇(由 JZSpeechDemo 提供, TTS 合成的 PCM 经此播放)
    void setAudioOutputSink(std::function<void(const JZByteArray &)> sink);

    // 注入可替换节点 (ASR / TTS 来自 JZSpeechClient, LLM 见 llm())
    void setPreprocess(std::unique_ptr<JZSpeechPreprocess> pre);
    void setVad(std::unique_ptr<JZSpeechVad> vad);
    void setKws(std::unique_ptr<JZSpeechKws> kws);
    void setAsr(JZSpeechAsr *asr);
    void setTts(JZSpeechTts *tts);

    void setFormat(const JZAudioFormat &format) { m_format = format; }
    const JZAudioFormat &format() const { return m_format; }

    void setVadEnabled(bool on) { m_vadEnabled = on; }
    void setKwsEnabled(bool on) { m_kwsEnabled = on; }

    // 直接访问 LLM 节点以进行配置 (init / setSystemPrompt 等)
    JZAgent *llm() { return &m_llm; }

    bool running() const { return m_running; }

    // 累计经过时间(ms, 由 audioInput 按帧长自动累加, 用于静音超时 / 时序逻辑)
    int64_t tick() const { return m_tick; }

    // 复位当前会话: flush ASR/TTS/LLM, 清空远端参考与计数
    void reset();

    // 音频输入入口 (由 JZSpeechDemo 的采集回调调用), 自动累加经过时间
    void audioInput(const JZByteArray &pcm);

jz_signals:
    JZSigVoid sigError;

private:
    // 内部链路衔接
    void onAsrResult(const std::string &text);   // ASR 文本 -> LLM
    void onLlmReply(const std::string &text);    // LLM 回复 -> TTS
    void onLlmFinished();
    void onTtsPcm(const JZByteArray &pcm);       // TTS 音频 -> 播放

    JZAudioFormat  m_format;
    std::unique_ptr<JZSpeechPreprocess> m_pre;
    std::unique_ptr<JZSpeechVad>       m_vad;
    std::unique_ptr<JZSpeechKws>       m_kws;
    JZSpeechAsr  *m_asr = nullptr;   // 通常为 JZSpeechClient 的 ASR 接口
    JZSpeechTts  *m_tts = nullptr;   // 通常为 JZSpeechClient 的 TTS 接口
    JZAgent       m_llm{this};       // 大语言模型节点

    JZByteArray   m_farRef;          // 最近播放音频, 作为 AEC 远端参考
    bool          m_vadEnabled = true;
    bool          m_kwsEnabled = true;
    bool          m_running = false;
    int64_t       m_tick = 0;          // 累计处理节拍

    std::function<void(const JZByteArray &)> m_audioOutput;
};

#endif // !JZ_SPEECH_CORE_H_
