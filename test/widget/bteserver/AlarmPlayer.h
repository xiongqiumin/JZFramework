#ifndef BTE_ALARM_PLAYER_H_
#define BTE_ALARM_PLAYER_H_

// ---------------------------------------------------------------------------
// AlarmPlayer.h —— 对应原版 bteserver/BTEServer/AlarmPlayer.h(报警音)。
//
// 原版是 Windows 的 PlaySoundW(SND_LOOP | SND_ASYNC), 一个文件一个循环; 这里用引擎的
// JZWaveFile + JZAudioOutput 拼出同样的效果: 整段 PCM 读进来, 播完(sigPlayFinish)
// 再喂一遍 —— 就是"循环播放"; stop() = 关输出设备。
//
// 原版特意把它单独放一个 .cpp("避免 <windows.h> 的 min/max 宏和 Qt 头文件打架");
// 引擎侧没有这个问题, 但保持独立文件: 报警是"收到通道故障就响, 直到用户手动清除"。
// ---------------------------------------------------------------------------

#include "JZAudioFormat.h"
#include "JZAudioOutput.h"
#include "JZByteArray.h"
#include "JZWaveFile.h"

#include <string>

class AlarmPlayer
{
public:
    AlarmPlayer();
    ~AlarmPlayer();

    // 开始循环播放; 文件打不开/设备开不了返回 false
    bool start(const std::string& wavPath);
    void stop();

    bool isPlaying() const { return m_playing; }

private:
    JZWaveFile    m_wave;
    JZAudioOutput m_output;
    JZByteArray   m_pcm;        // 整段 PCM(报警音文件很小)
    bool          m_playing = false;
};

#endif // BTE_ALARM_PLAYER_H_
