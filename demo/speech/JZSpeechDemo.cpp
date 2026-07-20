#include "JZSpeechDemo.h"
#include "JZDebug.h"

JZ_OBJECT_IMPL(JZSpeechDemo, JZObject)

JZSpeechDemo::JZSpeechDemo(JZObject *parent)
    : JZObject(parent)
    , sigStarted(this), sigStopped(this), sigError(this)
{
    m_format.channelCount = 1;
    m_format.sampleRate   = 16000;
    m_format.sampleFormat = JZAudioFormat::Int16;
}

JZSpeechDemo::~JZSpeechDemo()
{
    close();
}

bool JZSpeechDemo::open(const std::string &inDeviceId, const std::string &outDeviceId)
{
    m_input = new JZAudioInput(this);
    m_input->setFormat(m_format);

    m_output = new JZAudioOutput(this);
    m_output->setFormat(m_format);

    JZAudioDeviceInfo inDev, outDev;
    if (!inDeviceId.empty())
    {
        for (auto &d : JZAudioDeviceInfo::inputList())
            if (d.id == inDeviceId) { inDev = d; break; }
    }
    if (!outDeviceId.empty())
    {
        for (auto &d : JZAudioDeviceInfo::outputList())
            if (d.id == outDeviceId) { outDev = d; break; }
    }

    bool inOk  = inDev.id.empty()  ? m_input->open()  : m_input->open(inDev);
    bool outOk = outDev.id.empty() ? m_output->open() : m_output->open(outDev);
    if (!inOk)
    {
        jzDebug() << "[speech-demo] 打开音频输入设备失败";
        return false;
    }
    if (!outOk)
    {
        jzDebug() << "[speech-demo] 打开音频输出设备失败";
        return false;
    }

    connect(&m_input->sigReadyRead, this, &JZSpeechDemo::onAudioInputReady);
    sigStarted.emit();
    return true;
}

void JZSpeechDemo::close()
{
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
    sigStopped.emit();
}

void JZSpeechDemo::setFormat(const JZAudioFormat &format)
{
    m_format = format;
}

void JZSpeechDemo::setAudioInputCallback(std::function<void(const JZByteArray &)> cb)
{
    m_onInput = std::move(cb);
}

void JZSpeechDemo::play(const JZByteArray &pcm)
{
    if (pcm.isEmpty() || !m_output)
        return;
    m_played += pcm.size();
    m_output->write(pcm);
}

void JZSpeechDemo::onAudioInputReady(const JZByteArray &pcm)
{
    m_captured += pcm.size();
    if (m_onInput)
        m_onInput(pcm);
}
