#ifndef JZ_SPEECH_CLIENT_H_
#define JZ_SPEECH_CLIENT_H_

#include <string>
#include <mutex>
#include <condition_variable>
#include "JZObject.h"

class JZWebSocket;
class JZAudioOutput;

// 语音客户端: 通过 WebSocket 对接 ugreen-server (aispeech_engine) 的 ASR / TTS 服务,
// 协议见 ugreen-server/http_server/proto.md。
//   - ASR: 16k 单声道 16bit PCM (base64) -> 文本
//   - TTS: 文本 -> 音频 (base64, 默认 16k 单声道 16bit PCM, 若服务端返回 wav 则原样透传)
// 内部把 ugreen 的 WebSocket 当作"请求/响应"用: 主线程阻塞等结果, 网络线程收消息填坑,
// 二者分属不同线程 (JZNetThread), 不会死锁 HTTP 主循环。
class JZSpeechClient : public JZObject
{
public:
    JZSpeechClient(JZObject *parent = nullptr);
    ~JZSpeechClient();

    // 连接到 ugreen-server 的 WebSocket 地址 (如 ws://127.0.0.1:8000)
    bool connectServer(const std::string &wsUri);
    void disconnect();
    bool isConnected() const;

    // 识别一段语音, pcmBase64 为 16k/单声道/16bit PCM 的 base64, 结果写入 outText
    bool asr(const std::string &pcmBase64, std::string &outText, int timeoutMs = 15000);
    // 合成语音, 返回 16k/单声道/16bit PCM 封装的 WAV (base64), 可直接给浏览器 <audio> 播放
    bool tts(const std::string &text, std::string &outWavBase64, int timeoutMs = 20000);

    // 用 JZAudio 在本地 (服务所在机器) 直接播放 TTS 结果
    void setLocalPlay(bool on);
    bool localPlay() const;

private:
    void onMessage(const std::string &msg);

    // 把 ugreen 返回的音频 base64 (可能已是 wav, 也可能是裸 pcm) 规范成 wav 的 base64
    static std::string toWavBase64(const std::string &rawBase64);
    // 本地播放: 解码 base64, 剥 wav 头取 pcm, 经 JZAudioOutput 写出
    void playLocal(const std::string &rawBase64);

    JZWebSocket *m_ws = nullptr;
    std::string m_wsUri;
    bool m_localPlay = false;
    JZAudioOutput *m_audio = nullptr;

    std::mutex m_mutex;
    std::condition_variable m_cv;

    // 串行事务状态 (同一时刻只有一个 asr/tts 在跑, 由 m_mutex 保护)
    enum { PH_Idle } m_phase = PH_Idle;
    int m_streamId = -1;
    std::string m_asrText;
    std::string m_ttsAudio;   // 服务端原始音频 base64 (pcm 或 wav)
    bool m_txnDone = false;
    bool m_txnOk = false;
    std::string m_err;
};

#endif // !JZ_SPEECH_CLIENT_H_
