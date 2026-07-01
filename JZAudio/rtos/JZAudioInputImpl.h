#pragma once

#include "JZAudioDeviceInfo.h"

class JZAudioInput;
class JZAudioInputImpl
{
public:
    explicit JZAudioInputImpl(JZAudioInput* parent);
    virtual ~JZAudioInputImpl();

    virtual bool open(const JZAudioDeviceInfo& device);
    virtual void close();

private:
    JZAudioInput *m_device;
};