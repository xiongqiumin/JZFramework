#ifndef JZ_AUDIO_DEVICE_IMPL_H_
#define JZ_AUDIO_DEVICE_IMPL_H_
#include <vector>
#include "JZAudioDeviceInfo.h"

class JZFRAMEWORK_API JZAudioDeviceImpl
{
public:
    static std::vector<JZAudioDeviceInfo> audioInputList();
    static std::vector<JZAudioDeviceInfo> audioOutputList();
};

#endif // JZ_AUDIO_DEVICE_IMPL_H_
