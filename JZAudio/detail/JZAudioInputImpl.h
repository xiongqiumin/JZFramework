#ifndef JZ_AUDIO_INPUT_IMPL_H_
#define JZ_AUDIO_INPUT_IMPL_H_

#include "JZAudioDeviceInfo.h"
#include "JZObject.h"

class JZAudioInput;
class JZAudioInputImpl : public JZObject
{
public:
    explicit JZAudioInputImpl(JZAudioInput* parent);
    virtual ~JZAudioInputImpl();

    virtual bool open(const JZAudioDeviceInfo& device) = 0;
    virtual void close() = 0;

protected:
    JZAudioInput *m_device;
};

#endif