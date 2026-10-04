#include "AlarmPlayer.h"

AlarmPlayer::AlarmPlayer()
{
    // 播完一批缓冲就再喂一次 —— 报警音是"一直响到用户清除"
    connect(&m_output.sigPlayFinish, [this]() {
        if (m_playing && !m_pcm.isEmpty())
            m_output.write(m_pcm);
    });
}

AlarmPlayer::~AlarmPlayer()
{
    stop();
}

bool AlarmPlayer::start(const std::string& wavPath)
{
    if (m_playing)
        return true;

    if (!m_wave.open(wavPath))
        return false;

    m_pcm = m_wave.readAll();
    if (m_pcm.isEmpty())
    {
        m_wave.close();
        return false;
    }

    m_output.setFormat(m_wave.format());
    if (!m_output.open())
    {
        m_wave.close();
        return false;
    }

    m_playing = m_output.write(m_pcm);
    if (!m_playing)
    {
        m_output.close();
        m_wave.close();
    }
    return m_playing;
}

void AlarmPlayer::stop()
{
    if (!m_playing)
        return;

    m_playing = false;
    m_output.close();
    m_wave.close();
}
