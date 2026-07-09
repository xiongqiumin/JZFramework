#include <Windows.h>
#include <mmsystem.h>
#include <iostream>
#include "JZAudioDriverWindows.h"
#include "JZAudioInputWindows.h"
#include "JZAudioOutputWindows.h"
#include "JZStd.h"

JZAudioDriverWindows::JZAudioDriverWindows()
{
    setInstance(this);
}

std::vector<JZAudioDeviceInfo> JZAudioDriverWindows::audioInputList()
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

std::vector<JZAudioDeviceInfo> JZAudioDriverWindows::audioOutputList()
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

JZAudioInputImpl *JZAudioDriverWindows::createInput(JZAudioInput* parent)
{
    return new JZAudioInputWindows(parent);
}

JZAudioOutputImpl *JZAudioDriverWindows::createOutput(JZAudioOutput* parent)
{
    return new JZAudioOutputWindows(parent);
}

JZAudioDriverWindows g_winDriver;