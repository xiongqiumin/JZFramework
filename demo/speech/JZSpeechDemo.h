#ifndef JZ_SPEECH_DEMO_H_
#define JZ_SPEECH_DEMO_H_

#include <memory>
#include <string>
#include <functional>
#include "JZObject.h"
#include "JZByteArray.h"
#include "JZAudioFormat.h"
#include "JZAudioInput.h"
#include "JZAudioOutput.h"
#include "JZAudioDeviceInfo.h"

// ============================================================================
//  JZSpeechDemo: 音频输入/输出管理
//
//  负责打开麦克风/扬声器设备, 将采集到的 PCM 通过回调转交 JZSpeechCore,
//  并将 JZSpeechCore / TTS 合成的 PCM 播放到扬声器。
//
//  语音处理管线本身在 JZSpeechCore 中实现, 本类只管音频设备。
// ============================================================================
class JZSpeechDemo : public JZObject
{
    JZ_OBJECT_DECLARE(JZSpeechDemo)

public:
    JZSpeechDemo(JZObject *parent = nullptr);
    ~JZSpeechDemo();

    // 打开默认 (或指定 id) 的音频输入/输出设备
    bool open(const std::string &inDeviceId = "", const std::string &outDeviceId = "");
    void close();

    void setFormat(const JZAudioFormat &format);
    const JZAudioFormat &format() const { return m_format; }

    // 采集到一帧 PCM 时回调 (连接到 JZSpeechCore::onAudioInput)
    void setAudioInputCallback(std::function<void(const JZByteArray &)> cb);

    // 播放一帧 PCM (供 TTS 合成音频回放)
    void play(const JZByteArray &pcm);

    uint64_t capturedBytes() const { return m_captured; }
    uint64_t playedBytes()   const { return m_played; }

jz_signals:
    JZSigVoid sigStarted;
    JZSigVoid sigStopped;
    JZSigVoid sigError;

private:
    void onAudioInputReady(const JZByteArray &pcm);

    JZAudioFormat  m_format;
    JZAudioInput  *m_input  = nullptr;
    JZAudioOutput *m_output = nullptr;
    std::function<void(const JZByteArray &)> m_onInput;
    uint64_t m_captured = 0;
    uint64_t m_played   = 0;
};

#endif // !JZ_SPEECH_DEMO_H_
