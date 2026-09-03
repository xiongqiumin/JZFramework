#include "JZSpeechClient.h"

#include "JZWebSocket.h"
#include "JZByteArray.h"
#include "JZAudioOutput.h"
#include "JZAudioFormat.h"
#include "JZDebug.h"
#include "json/json.h"

#include <condition_variable>
#include <chrono>
#include <cstring>

static std::string jsonWrite(const Json::Value &v)
{
    Json::FastWriter w;
    return w.write(v);
}

// ---- WAV 封装 (16k / 单声道 / 16bit) ----
static JZByteArray makeWav(const JZByteArray &pcm, int sampleRate, int channels, int bits)
{
    int byteRate = sampleRate * channels * bits / 8;
    int blockAlign = channels * bits / 8;
    int dataSize = pcm.size();
    int chunkSize = 36 + dataSize;

    JZByteArray hdr;
    hdr.reserve(44);
    auto put32 = [&](uint32_t v)
    {
        hdr.push_back((char)(v & 0xFF));
        hdr.push_back((char)((v >> 8) & 0xFF));
        hdr.push_back((char)((v >> 16) & 0xFF));
        hdr.push_back((char)((v >> 24) & 0xFF));
    };
    auto put16 = [&](uint16_t v)
    {
        hdr.push_back((char)(v & 0xFF));
        hdr.push_back((char)((v >> 8) & 0xFF));
    };
    hdr.append("RIFF", 4);
    put32((uint32_t)chunkSize);
    hdr.append("WAVE", 4);
    hdr.append("fmt ", 4);
    put32(16);                 // fmt chunk size
    put16(1);                  // PCM
    put16((uint16_t)channels);
    put32((uint32_t)sampleRate);
    put32((uint32_t)byteRate);
    put16((uint16_t)blockAlign);
    put16((uint16_t)bits);
    hdr.append("data", 4);
    put32((uint32_t)dataSize);

    hdr.append(pcm);
    return hdr;
}

// 从 WAV 字节里抽出 fmt(格式) 与 data(PCM)。返回 PCM；若不是合法 WAV 返回空。
static JZByteArray extractPcmFromWav(const JZByteArray &wav, JZAudioFormat &fmt)
{
    fmt.sampleRate = 16000;
    fmt.channelCount = 1;
    fmt.sampleFormat = JZAudioFormat::Int16;

    if (wav.size() < 12 || std::memcmp(wav.data(), "RIFF", 4) != 0)
        return JZByteArray();

    int pos = 12;
    int n = wav.size();
    bool gotFmt = false;
    while (pos + 8 <= n)
    {
        char ckId[5] = {0};
        std::memcpy(ckId, wav.data() + pos, 4);
        uint32_t ckSize = (uint8_t)wav[pos + 4] | ((uint8_t)wav[pos + 5] << 8)
                        | ((uint8_t)wav[pos + 6] << 16) | ((uint32_t)(uint8_t)wav[pos + 7] << 24);
        int body = pos + 8;
        if (std::memcmp(ckId, "fmt ", 4) == 0 && body + 16 <= n)
        {
            uint16_t format = (uint8_t)wav[body] | ((uint8_t)wav[body + 1] << 8);
            uint16_t ch = (uint8_t)wav[body + 2] | ((uint8_t)wav[body + 3] << 8);
            uint32_t sr = (uint8_t)wav[body + 4] | ((uint8_t)wav[body + 5] << 8)
                        | ((uint8_t)wav[body + 6] << 16) | ((uint32_t)(uint8_t)wav[body + 7] << 24);
            uint16_t bits = (uint8_t)wav[body + 14] | ((uint8_t)wav[body + 15] << 8);
            if (format == 1)   // 仅支持 PCM
            {
                fmt.channelCount = ch;
                fmt.sampleRate = (int)sr;
                fmt.sampleFormat = (bits == 8) ? JZAudioFormat::UInt8
                                  : (bits == 32 || bits == 64) ? JZAudioFormat::Int32
                                  : JZAudioFormat::Int16;
                gotFmt = true;
            }
        }
        else if (std::memcmp(ckId, "data", 4) == 0)
        {
            int size = (int)ckSize;
            if (body + size > n) size = n - body;
            if (size > 0)
                return wav.mid(body, size);
        }
        pos = body + (int)ckSize + ((int)ckSize & 1);   // 对齐到偶边界
    }
    (void)gotFmt;
    return JZByteArray();
}

JZSpeechClient::JZSpeechClient(JZObject *parent)
    : JZObject(parent)
{
}

JZSpeechClient::~JZSpeechClient()
{
}

bool JZSpeechClient::connectServer(const std::string &wsUri)
{
    m_wsUri = wsUri;
    m_ws = new JZWebSocket(this);
    connect(&m_ws->sigTextMessageReceived, this, &JZSpeechClient::onMessage);
    m_ws->connectToHost(wsUri);
    return m_ws->waitForConnected(5000);
}

bool JZSpeechClient::isConnected() const
{
    return m_ws && m_ws->isConnected();
}

void JZSpeechClient::disconnect()
{
    if (m_ws)
    {
        m_ws->disconnectFromHost();
        delete m_ws;
        m_ws = nullptr;
    }
}

void JZSpeechClient::setLocalPlay(bool on)
{
    m_localPlay = on;
}

bool JZSpeechClient::localPlay() const
{
    return m_localPlay;
}

// 网络线程回调: 解析 ugreen 下发的 JSON, 按 command 填充当前事务结果并唤醒等待线程
void JZSpeechClient::onMessage(const std::string &msg)
{
    Json::Value root;
    Json::Reader reader;
    if (!reader.parse(msg, root))
        return;

    std::string command = root.get("command", "").asString();

    std::unique_lock<std::mutex> lk(m_mutex);
    if (command == "createStream")
    {
        if (root.get("code", 0).asInt() != 0)
        {
            m_txnOk = false;
            m_err = root.get("message", "createStream failed").asString();
        }
        else
        {
            m_streamId = root.get("id", -1).asInt();
            m_txnOk = true;
        }
        m_txnDone = true;
    }
    else if (command == "asrResult")
    {
        m_asrText += root.get("text", "").asString();
        if (root.get("isFinish", false).asBool())
        {
            m_txnOk = true;
            m_txnDone = true;
        }
    }
    else if (command == "generate")
    {
        if (root.get("code", 0).asInt() != 0)
        {
            m_txnOk = false;
            m_err = root.get("message", "generate failed").asString();
        }
        else
        {
            m_ttsAudio = root.get("audioData", "").asString();
            m_txnOk = true;
        }
        m_txnDone = true;
    }
    else if (command == "error")
    {
        m_txnOk = false;
        m_err = root.get("message", "speech error").asString();
        m_txnDone = true;
    }
    // destroyStream / inputFinish / 其它命令: 忽略
    lk.unlock();
    m_cv.notify_all();
}

bool JZSpeechClient::asr(const std::string &pcmBase64, std::string &outText, int timeoutMs)
{
    if (!isConnected())
        return false;

    std::unique_lock<std::mutex> lk(m_mutex);
    m_streamId = -1;
    m_asrText.clear();
    m_ttsAudio.clear();
    m_err.clear();
    m_txnOk = false;
    m_txnDone = false;

    // 1) 建流
    Json::Value req;
    req["command"] = "createStream";
    req["task"] = "asr";
    req["params"]["lang"] = "zh";
    m_ws->sendTextMessage(jsonWrite(req));
    if (!m_cv.wait_for(lk, std::chrono::milliseconds(timeoutMs), [this] { return m_txnDone; }))
    {
        jzWarning() << "speech asr: createStream timeout";
        return false;
    }
    if (!m_txnOk || m_streamId < 0)
    {
        jzWarning() << "speech asr: createStream failed: " << m_err;
        return false;
    }
    int sid = m_streamId;

    // 2) 送音频 + 结束输入
    m_txnDone = false;
    m_txnOk = false;
    m_asrText.clear();

    Json::Value r2;
    r2["id"] = sid;
    r2["command"] = "recognize";
    r2["params"]["audioData"] = pcmBase64;
    m_ws->sendTextMessage(jsonWrite(r2));

    Json::Value r3;
    r3["id"] = sid;
    r3["command"] = "inputFinish";
    m_ws->sendTextMessage(jsonWrite(r3));

    if (!m_cv.wait_for(lk, std::chrono::milliseconds(timeoutMs), [this] { return m_txnDone; }))
    {
        jzWarning() << "speech asr: recognize timeout";
        return false;
    }

    // 3) 销毁流 (best-effort, 不等响应)
    Json::Value r4;
    r4["id"] = sid;
    r4["command"] = "destroyStream";
    m_ws->sendTextMessage(jsonWrite(r4));

    if (!m_txnOk)
    {
        jzWarning() << "speech asr failed: " << m_err;
        return false;
    }
    outText = m_asrText;
    return true;
}

bool JZSpeechClient::tts(const std::string &text, std::string &outWavBase64, int timeoutMs)
{
    if (!isConnected())
        return false;

    std::unique_lock<std::mutex> lk(m_mutex);
    m_streamId = -1;
    m_asrText.clear();
    m_ttsAudio.clear();
    m_err.clear();
    m_txnOk = false;
    m_txnDone = false;

    // 1) 建流
    Json::Value req;
    req["command"] = "createStream";
    req["task"] = "tts";
    m_ws->sendTextMessage(jsonWrite(req));
    if (!m_cv.wait_for(lk, std::chrono::milliseconds(timeoutMs), [this] { return m_txnDone; }))
    {
        jzWarning() << "speech tts: createStream timeout";
        return false;
    }
    if (!m_txnOk || m_streamId < 0)
    {
        jzWarning() << "speech tts: createStream failed: " << m_err;
        return false;
    }
    int sid = m_streamId;

    // 2) 合成
    m_txnDone = false;
    m_txnOk = false;
    m_ttsAudio.clear();

    Json::Value r2;
    r2["id"] = sid;
    r2["command"] = "generate";
    r2["params"]["text"] = text;
    r2["params"]["lang"] = "zh";
    m_ws->sendTextMessage(jsonWrite(r2));

    if (!m_cv.wait_for(lk, std::chrono::milliseconds(timeoutMs), [this] { return m_txnDone; }))
    {
        jzWarning() << "speech tts: generate timeout";
        return false;
    }

    // 3) 销毁流 (best-effort)
    Json::Value r4;
    r4["id"] = sid;
    r4["command"] = "destroyStream";
    m_ws->sendTextMessage(jsonWrite(r4));

    if (!m_txnOk)
    {
        jzWarning() << "speech tts failed: " << m_err;
        return false;
    }

    if (m_localPlay)
        playLocal(m_ttsAudio);

    outWavBase64 = toWavBase64(m_ttsAudio);
    return true;
}

std::string JZSpeechClient::toWavBase64(const std::string &rawBase64)
{
    JZByteArray raw = JZByteArray::fromBase64(JZByteArray(rawBase64.data(), (int)rawBase64.size()));
    if (raw.size() >= 4 && std::memcmp(raw.data(), "RIFF", 4) == 0)
        return str_fromUtf8(raw.toBase64());   // 服务端已返回 wav, 原样透传

    // 默认按 16k/单声道/16bit PCM 封装成 wav
    JZByteArray wav = makeWav(raw, 16000, 1, 16);
    return str_fromUtf8(wav.toBase64());
}

void JZSpeechClient::playLocal(const std::string &rawBase64)
{
    JZByteArray raw = JZByteArray::fromBase64(JZByteArray(rawBase64.data(), (int)rawBase64.size()));
    if (raw.size() < 4)
        return;

    JZAudioFormat fmt;
    fmt.sampleRate = 16000;
    fmt.channelCount = 1;
    fmt.sampleFormat = JZAudioFormat::Int16;
    JZByteArray pcm = raw;
    if (std::memcmp(raw.data(), "RIFF", 4) == 0)
    {
        JZByteArray extracted = extractPcmFromWav(raw, fmt);
        if (!extracted.isEmpty())
            pcm = extracted;
    }

    if (!m_audio)
    {
        m_audio = new JZAudioOutput(this);
        m_audio->setFormat(fmt);
        if (!m_audio->open())
        {
            jzWarning() << "speech: 本地音频输出打开失败 (无音频设备?)";
            delete m_audio;
            m_audio = nullptr;
            return;
        }
    }
    m_audio->write(pcm);
}
