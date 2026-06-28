#include "JZAudioDeviceImpl.h"
#include "JZAudioInputImpl.h"
#include <Windows.h>
#include <mmsystem.h>
#include <iostream>

std::vector<JZAudioDevice> JZAudioDeviceImpl::audioInputList()
{
    std::vector<JZAudioDevice> devices;

    int deviceCount = waveInGetNumDevs();
    if (deviceCount == 0) {
        std::cerr << "没有可用的音频输入设备" << std::endl;
        return devices;
    }

    for (int i = 0; i < deviceCount; i++) {
        WAVEINCAPSA caps;
        MMRESULT result = waveInGetDevCapsA(i, &caps, sizeof(caps));

        JZAudioDevice device;
        int channel;
        std::vector<int> sampleRate;
        if (result == MMSYSERR_NOERROR)
        {
            device.id = std::to_string(i);
            device.name = str_local_to_utf8(caps.szPname);
            device.isInput = true;

            devices.push_back(device);
        }
    }

    return devices;
}

std::vector<JZAudioDevice> JZAudioDeviceImpl::audioOutputList()
{
    std::vector<JZAudioDevice> devices;

    int deviceCount = waveInGetNumDevs();
    if (deviceCount == 0) {
        std::cerr << "没有可用的音频输入设备" << std::endl;
        return devices;
    }

    for (int i = 0; i < deviceCount; i++) {
        WAVEOUTCAPSA caps;
        MMRESULT result = waveOutGetDevCapsA(i, &caps, sizeof(caps));
        
        JZAudioDevice device;
        int channel;
        std::vector<int> sampleRate;
        if (result == MMSYSERR_NOERROR) 
        {
            device.id = std::to_string(i);
            device.name = str_local_to_utf8(caps.szPname);
            device.isInput = true;
            
            devices.push_back(device);
        }
    }

    return devices;
}
