#ifndef JZ_AUDIO_DRIVER_LINUX_H_
#define JZ_AUDIO_DRIVER_LINUX_H_
#include <vector>
#include "JZAudioDriver.h"

class JZFRAMEWORK_API JZAudioDriverLinux : public JZAudioDriver
{
public:
    JZAudioDriverLinux();

    virtual std::vector<JZAudioDeviceInfo> audioInputList() override;
    virtual std::vector<JZAudioDeviceInfo> audioOutputList() override;
    virtual JZAudioInputImpl *createInput(JZAudioInput* parent) override;
    virtual JZAudioOutputImpl *createOutput(JZAudioOutput* parent) override;
};

#endif // JZ_AUDIO_DRIVER_LINUX_H_
