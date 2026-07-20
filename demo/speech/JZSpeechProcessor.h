#ifndef JZ_SPEECH_PROCESSOR_H_
#define JZ_SPEECH_PROCESSOR_H_

#include <memory>
#include <string>
#include <functional>
#include "JZByteArray.h"
#include "JZAudioFormat.h"

// ============================================================================
//  语音处理链路占位接口 (demo/speech)
//  下列接口为音频双工链路中的可选处理节点, 目前提供占位实现,
//  可在不改动 demo/speech 主流程的前提下, 替换为真实算法 (如 WebRTC VAD /
//  SpeexDSP AEC / Opus 编解码等)。
// ============================================================================

// ===================== VAD: 语音活动检测 =====================
// 输入一帧原始 PCM, 判定该帧是否含有语音。
// 典型用途: 仅在检测到语音时才把音频上行到服务器, 减少带宽与误触发。
class JZSpeechVad
{
public:
    virtual ~JZSpeechVad() = default;

    // pcmIn : 采集到的一帧 PCM (格式由 setFormat 指定)
    // pcmOut: 判定为语音的输出帧 (静音段可被裁剪为空)
    // 返回 true 表示该帧包含语音
    virtual bool process(const JZByteArray &pcmIn, JZByteArray &pcmOut) = 0;

    virtual void setFormat(const JZAudioFormat &format) { m_format = format; }

protected:
    JZAudioFormat m_format;
};

// ===================== AEC: 回声消除 =====================
// 消除扬声器播放(被麦克风采集到的)回声。
class JZSpeechAec
{
public:
    virtual ~JZSpeechAec() = default;

    // near  : 麦克风近端信号 (含扬声器回声)
    // farRef: 远端参考信号 (即将/正在播放给扬声器的音频)
    // out   : 回声消除后的近端信号
    virtual void process(const JZByteArray &nearIn, const JZByteArray &farRef, JZByteArray &out) = 0;

    virtual void setFormat(const JZAudioFormat &format) { m_format = format; }

protected:
    JZAudioFormat m_format;
};

// ===================== NS: 噪声抑制 =====================
// 抑制稳态/环境噪声, 提升上行语音清晰度。通常放在 AEC 之后、VAD 之前。
class JZSpeechNs
{
public:
    virtual ~JZSpeechNs() = default;

    // pcmIn : AEC 之后的近端 PCM
    // pcmOut: 降噪后的 PCM (始终输出, 不会裁剪为静音)
    virtual void process(const JZByteArray &pcmIn, JZByteArray &pcmOut) = 0;

    virtual void setFormat(const JZAudioFormat &format) { m_format = format; }

protected:
    JZAudioFormat m_format;
};

// ===================== AGC: 自动增益控制 =====================
// 将人声音量归一化到稳定区间, 避免忽大忽小。通常放在 NS 之后、VAD 之前。
class JZSpeechAgc
{
public:
    virtual ~JZSpeechAgc() = default;

    // pcmIn : 降噪后的 PCM
    // pcmOut: 增益调整后的 PCM
    virtual void process(const JZByteArray &pcmIn, JZByteArray &pcmOut) = 0;

    virtual void setFormat(const JZAudioFormat &format) { m_format = format; }

protected:
    JZAudioFormat m_format;
};

// ===================== 上行预处理: AEC + NS + AGC 三合一 =====================
// 将回声消除、噪声抑制、自动增益合并为一个上行预处理节点。
// 内部按 AEC -> NS -> AGC 顺序串联, 仍可通过 setAec/setNs/setAgc 替换子节点。
class JZSpeechPreprocess
{
public:
    virtual ~JZSpeechPreprocess() = default;

    // near  : 麦克风近端信号(含扬声器回声)
    // farRef: 远端参考信号(正在播放的音频, 供 AEC 使用)
    // out   : 预处理后的上行 PCM
    virtual void process(const JZByteArray &nearIn, const JZByteArray &farRef, JZByteArray &out) = 0;

    virtual void setFormat(const JZAudioFormat &format) { m_format = format; }

protected:
    JZAudioFormat m_format;
};

// ===================== KWS: 唤醒词检测 =====================
// 在 VAD 之后判定是否命中唤醒词, 命中前可抑制后续上行, 命中后才进入 ASR。
class JZSpeechKws
{
public:
    virtual ~JZSpeechKws() = default;

    // pcmIn : 经过预处理/VAD 后的 PCM
    // kw    : 命中时输出唤醒词名, 未命中为空
    // 返回 true 表示命中唤醒词
    virtual bool process(const JZByteArray &pcmIn, std::string &kw) = 0;

    virtual void setFormat(const JZAudioFormat &format) { m_format = format; }

protected:
    JZAudioFormat m_format;
};

// ===================== ASR: 语音识别 =====================
// 由 JZSpeechClient(远端 ASR 服务) 或本地识别引擎实现。
class JZSpeechAsr
{
public:
    virtual ~JZSpeechAsr() = default;

    virtual void setFormat(const JZAudioFormat &format) { m_format = format; }

    // 喂入一帧 PCM, 触发增量识别
    virtual void feed(const JZByteArray &pcmIn) = 0;

    // 标记一句话结束 (请求返回整句结果)
    virtual void flush() = 0;

    // 识别结果回调 (增量或整句文本)
    void setResultCallback(std::function<void(const std::string &)> cb) { m_onResult = std::move(cb); }

protected:
    std::function<void(const std::string &)> m_onResult;
    JZAudioFormat m_format;
};

// ===================== TTS: 语音合成 =====================
// 由 JZSpeechClient(远端 TTS 服务) 或本地合成引擎实现。
class JZSpeechTts
{
public:
    virtual ~JZSpeechTts() = default;

    virtual void setFormat(const JZAudioFormat &format) { m_format = format; }

    // 喂入待合成文本 (可整句或分句)
    virtual void feed(const std::string &text) = 0;

    // 合成音频回调 (PCM 分片, 供播放)
    void setPcmCallback(std::function<void(const JZByteArray &)> cb) { m_onPcm = std::move(cb); }

protected:
    std::function<void(const JZByteArray &)> m_onPcm;
    JZAudioFormat m_format;
};

// ----------------------- 默认占位实现 -----------------------

// 占位 VAD: 基于帧能量的简单门限检测 (可被替换为 WebRTC/Speex VAD)。
// 实现可用, 但仅为占位, 真实场景建议替换。
class JZSpeechVadStub : public JZSpeechVad
{
public:
    JZSpeechVadStub();

    void setThreshold(int threshold) { m_threshold = threshold; }
    void setHangover(int frames) { m_hangover = frames; }

    bool process(const JZByteArray &pcmIn, JZByteArray &pcmOut) override;

private:
    int  m_threshold;   // 能量(平方和)门限
    int  m_hangover;    // 语音结束后的保持帧数, 避免截断词尾
    int  m_hangCount = 0;
};

// 占位 AEC: 直通 (不处理)。真实实现应减去 farRef 对应的回声。
class JZSpeechAecStub : public JZSpeechAec
{
public:
    void process(const JZByteArray &nearIn, const JZByteArray &farRef, JZByteArray &out) override;
};

// 占位 KWS: 直通 (始终不命中)。真实实现应替换为 Porcupine/Snowboy 等。
class JZSpeechKwsStub : public JZSpeechKws
{
public:
    bool process(const JZByteArray &pcmIn, std::string &kw) override;
};

// 占位 NS: 直通 (不处理)。真实实现应替换为 SpeexDSP/WebRTC NS。
class JZSpeechNsStub : public JZSpeechNs
{
public:
    void process(const JZByteArray &pcmIn, JZByteArray &pcmOut) override;
};

// 占位 AGC: 直通 (不处理)。真实实现应做音量归一化。
class JZSpeechAgcStub : public JZSpeechAgc
{
public:
    void process(const JZByteArray &pcmIn, JZByteArray &pcmOut) override;
};

// 占位上行预处理: 内部以 AEC -> NS -> AGC 直通链串接。
// 子节点默认使用对应占位实现, 可经 setAec/setNs/setAgc 单独替换。
class JZSpeechPreprocessStub : public JZSpeechPreprocess
{
public:
    JZSpeechPreprocessStub();

    // 可选: 替换内部子节点
    void setAec(std::unique_ptr<JZSpeechAec> v);
    void setNs(std::unique_ptr<JZSpeechNs> v);
    void setAgc(std::unique_ptr<JZSpeechAgc> v);

    void setFormat(const JZAudioFormat &format) override;
    void process(const JZByteArray &nearIn, const JZByteArray &farRef, JZByteArray &out) override;

private:
    std::unique_ptr<JZSpeechAec> m_aec;
    std::unique_ptr<JZSpeechNs>  m_ns;
    std::unique_ptr<JZSpeechAgc> m_agc;
};

// ----------------------- 工厂: 返回默认(占位)处理器 -----------------------
std::unique_ptr<JZSpeechVad>         JZCreateSpeechVad();
std::unique_ptr<JZSpeechAec>         JZCreateSpeechAec();
std::unique_ptr<JZSpeechNs>          JZCreateSpeechNs();
std::unique_ptr<JZSpeechAgc>         JZCreateSpeechAgc();
std::unique_ptr<JZSpeechPreprocess>  JZCreateSpeechPreprocess();
std::unique_ptr<JZSpeechKws>         JZCreateSpeechKws();

#endif // !JZ_SPEECH_PROCESSOR_H_
