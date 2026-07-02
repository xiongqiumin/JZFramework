#include "JZAudioDriverWin32.h"
#include <Windows.h>
#include <mmsystem.h>
#include <iostream>

JZAudioDriverWin32::JZAudioDriverWin32()
{
    setInstance(this);
}

std::vector<JZAudioDeviceInfo> JZAudioDriverWin32::audioInputList()
{
    std::vector<JZAudioDeviceInfo> devices;

    int deviceCount = waveInGetNumDevs();
    if (deviceCount == 0) {
        std::cerr << "没有可用的音频输入设备" << std::endl;
        return devices;
    }

    for (int i = 0; i < deviceCount; i++) {
        WAVEINCAPSA caps;
        MMRESULT result = waveInGetDevCapsA(i, &caps, sizeof(caps));

        JZAudioDeviceInfo device;
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

std::vector<JZAudioDeviceInfo> JZAudioDriverWin32::audioOutputList()
{
    std::vector<JZAudioDeviceInfo> devices;

    int deviceCount = waveInGetNumDevs();
    if (deviceCount == 0) {
        std::cerr << "没有可用的音频输入设备" << std::endl;
        return devices;
    }

    for (int i = 0; i < deviceCount; i++) {
        WAVEOUTCAPSA caps;
        MMRESULT result = waveOutGetDevCapsA(i, &caps, sizeof(caps));
        
        JZAudioDeviceInfo device;
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

JZAudioInputImpl *JZAudioDriverWin32::createInput(JZAudioInput* parent)
{
    return new JZAudioInputImpl(parent);
}

JZAudioOutputImpl *JZAudioDriverWin32::createOutput(JZAudioOutput* parent)
{
    return new JZAudioOutputImpl(parent);
}

JZAudioDriverWin32 g_winDriver;