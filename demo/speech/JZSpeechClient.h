#ifndef JZ_SPEECH_CLIENT_H_
#define JZ_SPEECH_CLIENT_H_

#include <memory>
#include <cstdint>
#include <string>
#include "JZObject.h"
#include "JZWebSocket.h"
#include "JZAudioInput.h"
#include "JZAudioOutput.h"
#include "JZAudioDeviceInfo.h"
#include "JZAudioFormat.h"
#include "JZSpeechProcessor.h"

// ============================================================================
//  JZSpeechClient: 通过 WebSocket 实现音频双工
//
//  上行(麦克风 -> 服务器):
//      JZAudioInput --sigReadyRead-->
//          Preprocess(AEC+NS+AGC) -> VAD -> PCM Codec(encode) -> WebSocket(binary)
//  下行(服务器 -> 扬声器):
//      WebSocket(binary) --sigBinaryMessageReceived-->
//          PCM Codec(decode) -> JZAudioOutput
//
//  说明: 本 demo 仅基于 TCP (WebSocket over TCP) 传输, 链路保序且可靠,
//  故不引入 JitterBuffer / 丢包补偿; 若改用 UDP/QUIC 需另行补充。
//
//  Preprocess(AEC/NS/AGC) / VAD / PCM 编解码 均为可替换的占位实现,
//  通过 setVad / setPre 注入真实算法。
//
//  作为语音管线节点: JZSpeechClient 同时通过 getAsrInterface() / getTtsInterface()
//  暴露为 JZSpeechCore 的 ASR / TTS 节点 (详见下方适配器)。
// ============================================================================

// ---- JZSpeechClient 作为 ASR / TTS 节点的内部适配器 ----
// 由 JZSpeechClient 持有, 通过 getAsrInterface() / getTtsInterface() 暴露给
// JZSpeechCore 接入。具体网络流式协议(编码后发送 / 接收回调)在后续实现,
// 此处仅搭建接口骨架。
class JZSpeechClient;
class JZSpeechClientAsr : public JZSpeechAsr
{
public:
    explicit JZSpeechClientAsr(JZSpeechClient *client) : m_client(client) {}
    void feed(const JZByteArray &pcmIn) override;   // TODO: 编码并发送至 ASR 服务
    void flush() override;                          // TODO: 请求返回整句结果
private:
    JZSpeechClient *m_client;
};

class JZSpeechClientTts : public JZSpeechTts
{
public:
    explicit JZSpeechClientTts(JZSpeechClient *client) : m_client(client) {}
    void feed(const std::string &text) override;    // TODO: 发送至 TTS 服务, 结果经 setPcmCallback 回传
private:
    JZSpeechClient *m_client;
};

class JZSpeechClient : public JZObject
{
    JZ_OBJECT_DECLARE(JZSpeechClient)

public:
    JZSpeechClient(JZObject *parent = nullptr);
    ~JZSpeechClient();

    // 启动双工: 连接 wsUrl, 打开音频设备
    // inDeviceId / outDeviceId 为空时自动选择默认设备
    bool start(const std::string &wsUrl,
               const std::string &inDeviceId = "",
               const std::string &outDeviceId = "");
    void stop();

    // 仅建立网络连接 (作为 ASR / TTS 节点时, 音频 I/O 由 JZSpeechDemo 管理)
    bool connectServer(const std::string &wsUrl);

    // 注入自定义(真实)处理器, 替换默认占位实现
    void setVad(std::unique_ptr<JZSpeechVad> vad);
    void setPre(std::unique_ptr<JZSpeechPreprocess> pre);

    // 暴露为语音管线节点: 供 JZSpeechCore 作为 ASR / TTS 节点接入
    JZSpeechAsr* getAsrInterface();
    JZSpeechTts* getTtsInterface();

    void setFormat(const JZAudioFormat &format) { m_format = format; }
    const JZAudioFormat &format() const { return m_format; }

    void setVadEnabled(bool on) { m_vadEnabled = on; }

    uint64_t sendBytes() const { return m_sendBytes; }
    uint64_t recvBytes() const { return m_recvBytes; }

jz_signals:
    JZSigVoid sigConnected;
    JZSigVoid sigDisconnected;
    JZSigVoid sigError;

private:
    void onWsConnected();
    void onWsDisconnected();
    void onWsError();
    void onWsBinaryReceived(const JZByteArray &data);
    void onAudioInputReady(const JZByteArray &pcm);

    JZAudioFormat  m_format;
    JZWebSocket   *m_ws = nullptr;
    JZAudioInput  *m_input = nullptr;
    JZAudioOutput *m_output = nullptr;

    std::unique_ptr<JZSpeechVad>         m_vad;
    std::unique_ptr<JZSpeechPreprocess>  m_pre;

    // 作为 ASR / TTS 节点的内部适配器
    std::unique_ptr<JZSpeechClientAsr>   m_asrNode;
    std::unique_ptr<JZSpeechClientTts>   m_ttsNode;

    JZByteArray m_farRef;      // 最近播放的音频, 作为 AEC 远端参考(占位, 未做精确时间对齐)
    bool        m_vadEnabled = true;
    uint64_t    m_sendBytes = 0;
    uint64_t    m_recvBytes = 0;
};

#endif // !JZ_SPEECH_CLIENT_H_
