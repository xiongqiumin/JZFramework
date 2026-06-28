#ifndef JZ_AUDIO_DEVICE_IMPL_H_
#define JZ_AUDIO_DEVICE_IMPL_H_

#include <vector>
#include "JZAudioDevice.h"

class JZAudioDeviceImpl
{
public:
    static std::vector<JZAudioDevice> audioInputList();
    static std::vector<JZAudioDevice> audioOutputList();
};

#endif // JZ_AUDIO_DEVICE_IMPL_H_
