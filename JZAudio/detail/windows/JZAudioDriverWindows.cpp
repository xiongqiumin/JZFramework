#include <Windows.h>
#include <mmsystem.h>
#include <iostream>
#include "JZAudioDriverWindows.h"
#include "JZAudioInputWindows.h"
#include "JZAudioOutputWindows.h"
#include "JZStd.h"

JZAudioDriverWindows::JZAudioDriverWindows()
{
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

    // ⚠ 这里以前拷的是输入那段的 waveInGetNumDevs()/isInput=true —— 输出列表要按**输出**数枚举,
    //   并且 isInput 必须是 false(否则 JS 侧 devices() 里输出设备看着像输入)。
    int deviceCount = waveOutGetNumDevs();
    if (deviceCount == 0) {
        std::cerr << "没有可用的音频输出设备" << std::endl;
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
            device.isInput = false;

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