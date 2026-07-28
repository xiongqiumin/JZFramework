#ifndef JZ_SPEECH_H_
#define JZ_SPEECH_H_

#include <vector>
#include "JZObject.h"
#include "JZAudioInput.h"
#include "JZAudioOutput.h"
#include "JZAgent.h"
#include "JZAec.h"

class JZSpeech : public JZObject
{
public:
    JZSpeech();
    ~JZSpeech();

    bool init();

    void setAecEnabled(bool on) { m_aecEnabled = on; }
    bool aecEnabled() const { return m_aecEnabled; }

    // 硬件延迟校准(ms): 让 AEC 的时间戳对齐更准确
    void setOutputDelay(int64_t ms) { m_aec.setOutputDelay(ms); }
    void setInputDelay(int64_t ms)  { m_aec.setInputDelay(ms); }

protected:
    void onAudioInput(const JZByteArray& buffer);
    void onAecResult(const JZByteArray& out);
    void onPlayFinished();

    JZAudioInput m_input;
    JZAudioOutput m_output;
    JZAgent m_agent;

    JZAecNLMS m_aec;
    bool  m_aecEnabled = true;

    // 断环用: 保存"上一帧实际播放出去"的音频, 作为下一帧 AEC 的参考
    JZByteArray m_lastPlayed;
    int64_t     m_lastRenderTime = 0;
};


#endif