#ifndef JZ_AUDIO_DEVICE_H_
#define JZ_AUDIO_DEVICE_H_
#include "JZGlobal.h"
#include <vector>
#include <string>

class JZFRAMEWORK_API JZAudioDeviceInfo
{
public:
    static std::vector<JZAudioDeviceInfo> inputList();
    static std::vector<JZAudioDeviceInfo> outputList();

    JZAudioDeviceInfo();
    virtual ~JZAudioDeviceInfo();

    std::string name;
    std::string id;
    bool isInput;
};

#endif // JZ_AUDIO_DEVICE_H_