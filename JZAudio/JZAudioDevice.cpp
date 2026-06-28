#include "JZAudioDevice.h"
#include "JZAudioDeviceImpl.h"

std::vector<JZAudioDevice> JZAudioDevice::audioInputList()
{
	return JZAudioDeviceImpl::audioInputList();
}

std::vector<JZAudioDevice> JZAudioDevice::audioOutputList()
{
	return JZAudioDeviceImpl::audioOutputList();
}

JZAudioDevice::JZAudioDevice()
{
    isInput = false;
}

JZAudioDevice::~JZAudioDevice()
{
}
