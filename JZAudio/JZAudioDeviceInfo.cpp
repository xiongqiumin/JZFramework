#include "JZAudioDeviceInfo.h"
#include "detail/JZAudioDriver.h"

std::vector<JZAudioDeviceInfo> JZAudioDeviceInfo::inputList()
{
	return JZAudioDriver::instance()->audioInputList();
}

std::vector<JZAudioDeviceInfo> JZAudioDeviceInfo::outputList()
{
	return JZAudioDriver::instance()->audioOutputList();
}

JZAudioDeviceInfo::JZAudioDeviceInfo()
{
    isInput = false;
}

JZAudioDeviceInfo::~JZAudioDeviceInfo()
{
}
