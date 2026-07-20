#include "JZSpeechClient.h"
#include "JZDebug.h"
#include "JZAudioDeviceInfo.h"

JZ_OBJECT_IMPL(JZSpeechClient, JZObject)

JZSpeechClient::JZSpeechClient(JZObject *parent)
    : JZObject(parent)
    , sigConnected(this), sigDisconnected(this), sigError(this)
{
    // 默认使用占位处理器
    m_vad   = JZCreateSpeechVad();
    m_pre   = JZCreateSpeechPreprocess();

    m_format.channelCount = 1;
    m_format.sampleRate   = 16000;
    m_format.sampleFormat = JZAudioFormat::Int16;
}

JZSpeechClient::~JZSpeechClient()
{
    stop();
}

void JZSpeechClient::setVad(std::unique_ptr<JZSpeechVad> vad)
{
    if (vad)
    {
        vad->setFormat(m_format);
        m_vad = std::move(vad);
    }
}

void JZSpeechClient::setPre(std::unique_ptr<JZSpeechPreprocess> pre)
{
    if (pre)
    {
        pre->setFormat(m_format);
        m_pre = std::move(pre);
    }
}

bool JZSpeechClient::start(const std::string &wsUrl,
                           const std::string &inDeviceId,
                           const std::string &outDeviceId)
{
    // 处理器初始化
    m_vad->setFormat(m_format);
    m_pre->setFormat(m_format);

    // ---- 音频设备 ----
    m_input = new JZAudioInput(this);
    m_input->setFormat(m_format);

    m_output = new JZAudioOutput(this);
    m_output->setFormat(m_format);

    JZAudioDeviceInfo inDev, outDev;
    if (!inDeviceId.empty())
    {
        for (auto &d : JZAudioDeviceInfo::inputList())
        {
            if (d.id == inDeviceId) { inDev = d; break; }
        }
    }
    if (!outDeviceId.empty())
    {
        for (auto &d : JZAudioDeviceInfo::outputList())
        {
            if (d.id == outDeviceId) { outDev = d; break; }
        }
    }

    bool inOk  = inDev.id.empty()  ? m_input->open()  : m_input->open(inDev);
    bool outOk = outDev.id.empty() ? m_output->open() : m_output->open(outDev);
    if (!inOk)
    {
        jzDebug() << "[speech] 打开音频输入设备失败";
        return false;
    }
    if (!outOk)
    {
        jzDebug() << "[speech] 打开音频输出设备失败";
        return false;
    }

    connect(&m_input->sigReadyRead, this, &JZSpeechClient::onAudioInputReady);

    // ---- WebSocket ----
    m_ws = new JZWebSocket(this);
    connect(&m_ws->sigConnected,           this, &JZSpeechClient::onWsConnected);
    connect(&m_ws->sigDisconnected,        this, &JZSpeechClient::onWsDisconnected);
    connect(&m_ws->sigError,               this, &JZSpeechClient::onWsError);
    connect(&m_ws->sigBinaryMessageReceived, this, &JZSpeechClient::onWsBinaryReceived);

    jzDebug() << "[speech] 连接 WebSocket:" << wsUrl.c_str();
    m_ws->connectToHost(JZNetworkRequest(wsUrl));
    return true;
}

// 仅建立网络连接, 不打开音频设备 (音频由 JZSpeechDemo 管理)
bool JZSpeechClient::connectServer(const std::string &wsUrl)
{
    m_ws = new JZWebSocket(this);
    connect(&m_ws->sigConnected,           this, &JZSpeechClient::onWsConnected);
    connect(&m_ws->sigDisconnected,        this, &JZSpeechClient::onWsDisconnected);
    connect(&m_ws->sigError,               this, &JZSpeechClient::onWsError);
    connect(&m_ws->sigBinaryMessageReceived, this, &JZSpeechClient::onWsBinaryReceived);

    jzDebug() << "[speech] 连接 WebSocket:" << wsUrl.c_str();
    m_ws->connectToHost(JZNetworkRequest(wsUrl));
    return true;
}

void JZSpeechClient::stop()
{
    if (m_ws)
    {
        m_ws->disconnectFromHost();
        m_ws = nullptr;
    }
    if (m_input)
    {
        m_input->close();
        delete m_input;
        m_input = nullptr;
    }
    if (m_output)
    {
        m_output->close();
        delete m_output;
        m_output = nullptr;
    }
}

void JZSpeechClient::onWsConnected()
{
    jzDebug() << "[speech] WebSocket 已连接, 开始音频双工";
    sigConnected.emit();
}

void JZSpeechClient::onWsDisconnected()
{
    jzDebug() << "[speech] WebSocket 已断开";
    sigDisconnected.emit();
}

void JZSpeechClient::onWsError()
{
    jzDebug() << "[speech] WebSocket 错误";
    sigError.emit();
}

// 下行: 服务器音频 -> 解码 -> 扬声器
void JZSpeechClient::onWsBinaryReceived(const JZByteArray &data)
{
    m_recvBytes += data.size();

    JZByteArray pcm = data;
    if (pcm.isEmpty())
        return;

    // 记录最近播放音频, 作为 AEC 远端参考(占位)
    m_farRef = pcm;

    if (m_output)
        m_output->write(pcm);
}

// 上行: 麦克风 -> Preprocess(AEC+NS+AGC) -> VAD -> 编码 -> 服务器
void JZSpeechClient::onAudioInputReady(const JZByteArray &pcm)
{
    if (!m_ws || !m_ws->isConnect())
        return;

    // 1) 上行预处理: AEC -> NS -> AGC
    JZByteArray preOut;
    m_pre->process(pcm, m_farRef, preOut);

    // 2) VAD: 仅在有语音时上行
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

    // 3) 直接发送 (原始 PCM, 未压缩)
    m_sendBytes += vadOut.size();
    m_ws->sendBinaryMessage(vadOut);
}

// ===================== ASR / TTS 节点适配器 =====================
// 说明: 以下为接口骨架。具体的网络流式协议(将 PCM 编码后通过 WebSocket 发送、
// 并将服务端返回的识别文本 / 合成音频回填到回调)在后续实现。
JZSpeechAsr* JZSpeechClient::getAsrInterface()
{
    if (!m_asrNode)
        m_asrNode = std::make_unique<JZSpeechClientAsr>(this);
    return m_asrNode.get();
}

JZSpeechTts* JZSpeechClient::getTtsInterface()
{
    if (!m_ttsNode)
        m_ttsNode = std::make_unique<JZSpeechClientTts>(this);
    return m_ttsNode.get();
}

void JZSpeechClientAsr::feed(const JZByteArray &pcmIn)
{
    // TODO: 编码并发送至 ASR 服务, 服务端文本结果经 setResultCallback 回调。
    (void)pcmIn;
}

void JZSpeechClientAsr::flush()
{
    // TODO: 标记一句话结束, 请求返回整句识别结果。
}

void JZSpeechClientTts::feed(const std::string &text)
{
    // TODO: 将文本发送至 TTS 服务, 合成的 PCM 分片经 setPcmCallback 回调。
    (void)text;
}
