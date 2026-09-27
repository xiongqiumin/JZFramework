#pragma once
#include <vector>
#include "JZAudioDriver.h"

class JZFRAMEWORK_API JZAudioDriverWindows : public JZAudioDriver
{
public:
    JZAudioDriverWindows();

    virtual std::vector<JZAudioDeviceInfo> audioInputList() override;
    virtual std::vector<JZAudioDeviceInfo> audioOutputList() override;
    virtual JZAudioInputImpl *createInput(JZAudioInput* parent) override;
    virtual JZAudioOutputImpl *createOutput(JZAudioOutput* parent) override;
};
