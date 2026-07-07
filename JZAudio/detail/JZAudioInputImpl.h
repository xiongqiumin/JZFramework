#pragma once

#include "JZAudioDeviceInfo.h"

class JZAudioInput;
class JZAudioInputImpl
{
public:
    explicit JZAudioInputImpl(JZAudioInput* parent);
    virtual ~JZAudioInputImpl();

    virtual bool open(const JZAudioDeviceInfo& device) = 0;
    virtual void close() = 0;

protected:
    JZAudioInput *m_device;
};