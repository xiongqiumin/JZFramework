#pragma once
#include <vector>
#include "JZAudioDevice.h"

class JZAudioDeviceImpl
{
public:
    static std::vector<JZAudioDevice> audioInputList();
    static std::vector<JZAudioDevice> audioOutputList();
};
