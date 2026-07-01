#pragma once
#include <vector>
#include "JZAudioDriver.h"

class JZAudioDriverWin32 : public JZAudioDriver
{
public:
    JZAudioDriverWin32();

    virtual std::vector<JZAudioDeviceInfo> audioInputList() override;
    virtual std::vector<JZAudioDeviceInfo> audioOutputList() override;
    virtual JZAudioInputImpl *createInput(JZAudioInput* parent) = override;
    virtual JZAudioOutputImpl *createOutput(JZAudioOutput* parent) = override;
};
