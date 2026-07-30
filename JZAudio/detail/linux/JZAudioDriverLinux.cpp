#include "JZAudioDriverLinux.h"
#include "JZAudioInputLinux.h"
#include "JZAudioOutputLinux.h"
#include "JZAudioDeviceImpl.h"

JZAudioDriverLinux::JZAudioDriverLinux()
{
}

std::vector<JZAudioDeviceInfo> JZAudioDriverLinux::audioInputList()
{
    return JZAudioDeviceImpl::audioInputList();
}

std::vector<JZAudioDeviceInfo> JZAudioDriverLinux::audioOutputList()
{
    return JZAudioDeviceImpl::audioOutputList();
}

JZAudioInputImpl *JZAudioDriverLinux::createInput(JZAudioInput* parent)
{
    return new JZAudioInputLinux(parent);
}

JZAudioOutputImpl *JZAudioDriverLinux::createOutput(JZAudioOutput* parent)
{
    return new JZAudioOutputLinux(parent);
}
