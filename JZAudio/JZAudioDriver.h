#ifndef JZ_AUDIO_DRIVER_H_
#define JZ_AUDIO_DRIVER_H_

#include <vector>
#include "JZGlobal.h"
#include "JZAudioDeviceInfo.h"
#include "detail/JZAudioInputImpl.h"
#include "detail/JZAudioOutputImpl.h"

class JZAudioDriver
{
public:
    static void setInstance(JZAudioDriver *inst);
    static JZAudioDriver *instance();

    virtual ~JZAudioDriver();
    virtual std::vector<JZAudioDeviceInfo> audioInputList() = 0;
    virtual std::vector<JZAudioDeviceInfo> audioOutputList() = 0;
    virtual JZAudioInputImpl *createInput(JZAudioInput* parent) = 0;
    virtual JZAudioOutputImpl *createOutput(JZAudioOutput* parent) = 0;

protected:
    static JZAudioDriver *m_inst;
};

#endif