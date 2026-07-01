#include "JZAudioDeviceImpl.h"
#include <alsa/asoundlib.h>
#include <iostream>
#include <cstring>

// 通用设备枚举方法（私有，供 audioInputList/audioOutputList 调用）
static std::vector<JZAudioDeviceInfo> enumerateAudioDevices(bool isLookingForInput)
{
    std::vector<JZAudioDeviceInfo> devices;
    snd_ctl_t *ctl_handle = nullptr;
    snd_pcm_info_t *pcm_info = nullptr;
    int card = -1;
    int err;

    // 初始化 PCM 信息结构体
    snd_pcm_info_alloca(&pcm_info);

    // 1. 遍历所有声卡（card）
    while (snd_card_next(&card) >= 0 && card >= 0)
    {
        // 构造声卡控制接口名称（例如 "hw:0"）
        char card_name[32];
        snprintf(card_name, sizeof(card_name), "hw:%d", card);

        // 2. 打开声卡控制接口
        err = snd_ctl_open(&ctl_handle, card_name, 0);
        if (err < 0)
        {
            std::cerr << "打开声卡 " << card << " 失败：" << snd_strerror(err) << std::endl;
            continue;
        }

        // 3. 遍历当前声卡下的所有 PCM 设备（device）
        int device = -1;
        while (snd_ctl_pcm_next_device(ctl_handle, &device) >= 0 && device >= 0)
        {
            // 4. 设置 PCM 信息参数（声卡、设备、流类型：输入/输出）
            snd_pcm_stream_t stream = isLookingForInput ? SND_PCM_STREAM_CAPTURE : SND_PCM_STREAM_PLAYBACK;
            snd_pcm_info_set_device(pcm_info, device);
            snd_pcm_info_set_subdevice(pcm_info, 0);
            snd_pcm_info_set_stream(pcm_info, stream);

            // 5. 获取 PCM 设备详细信息
            err = snd_ctl_pcm_info(ctl_handle, pcm_info);
            if (err < 0)
            {
                // 该设备不支持当前流类型（输入/输出），跳过
                continue;
            }

            // 6. 提取设备核心信息
            // 6.1 设备 ID（hw:card,device）
            char dev_id[32];
            snprintf(dev_id, sizeof(dev_id), "hw:%d,%d", card, device);

            // 6.2 设备友好名称（修复 snd_card_get_longname 调用，正确获取声卡名称）
            char* card_longname = nullptr;
            err = snd_card_get_longname(card, &card_longname); // 正确调用方式
            const char* pcm_name = snd_pcm_info_get_name(pcm_info);
            
            std::string device_name;
            if (err >= 0 && card_longname && pcm_name)
            {
                device_name = std::string(card_longname) + " - " + std::string(pcm_name);
                free(card_longname); // 释放 snd_card_get_longname 分配的内存
            }
            else if (pcm_name)
            {
                device_name = pcm_name;
            }
            else
            {
                device_name = dev_id;
            }

            // 7. 封装 JZAudioDeviceInfo 对象并加入列表（仅填充需要的 3 个字段）
            JZAudioDeviceInfo dev;
            dev.name = device_name;
            dev.id = dev_id;
            dev.isInput = isLookingForInput;
            devices.emplace_back(dev);
        }

        // 8. 关闭声卡控制接口（避免资源泄漏）
        snd_ctl_close(ctl_handle);
        ctl_handle = nullptr;
    }

    // 9. 清理 ALSA 相关资源
    snd_config_update_free_global();

    return devices;
}

std::vector<JZAudioDeviceInfo> JZAudioDeviceImpl::audioInputList()
{
    return enumerateAudioDevices(true);
}

std::vector<JZAudioDeviceInfo> JZAudioDeviceImpl::audioOutputList()
{
    return enumerateAudioDevices(false);
}