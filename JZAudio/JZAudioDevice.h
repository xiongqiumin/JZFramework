#ifndef JZ_AUDIO_DEVICE_H_
#define JZ_AUDIO_DEVICE_H_

#include <vector>
#include <string>

class JZAudioDevice 
{
public:
    static std::vector<JZAudioDevice> audioInputList();
    static std::vector<JZAudioDevice> audioOutputList();

    JZAudioDevice();
    virtual ~JZAudioDevice();

    std::string name;
    std::string id;
    bool isInput;
};

#endif // JZ_AUDIO_DEVICE_H_