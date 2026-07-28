#ifndef JZ_AEC_H_
#define JZ_AEC_H_

#include <cstdint>
#include "JZObject.h"
#include "JZByteArray.h"

// 回声消除(AEC)抽象基类: 定义与具体算法无关的通用接口。
// 设计目标: 作为所有 AEC 实现的基类, 例如:
//   - JZAecNLMS  : 时域 NLMS 自适应滤波(本文件内附带的参考实现)
//   - JZAecWebRtc: 封装 WebRTC AEC(频域, 生产级, 需外部依赖)
//   - JZAecFDAF  : 频域分块频域自适应滤波(无外部依赖的进阶实现)
//
// 双流接口(对齐 WebRTC AEC3 的 AnalyzeRender/AnalyzeCapture/ProcessCapture):
//   addOutput : 喂"扬声器实际播放出去的内容"(远端参考信号) + 播放调用时刻 time
//   addInput  : 喂麦克风采集到的信号 + 采集时刻 time -> 内部消除回声后 emit sigAecResult
//   sigAecResult : 发出消除回声后的近端信号
//
// 字节格式约定(所有子类必须遵循, 保证互通):
//   addInput/addOutput 入参与 sigAecResult 出参 均为 int16 单声道 PCM。
//   (需要 float/多声道的算法子类在内部自行转换。)
//
// 时间/延迟语义:
//   setOutputDelay + setInputDelay 合计 = 对 AEC 的"外部回声路径延迟估计"。
//   - NLMS 类: 回声延迟由滤波器自适应学习(只要 delay < 滤波器长度), 两偏移
//     对其可选(仅作初始对齐提示, 当前实现未强制使用); time 参数不参与对齐。
//   - WebRTC 类(内部自带延迟估计): 两偏移作为可选 hint, 对应其
//     SetAudioBufferDelay(); time 参数也可被忽略(WebRTC 靠调用顺序对齐)。
class JZAec : public JZObject
{
public:
    JZAec();
    virtual ~JZAec();

    // 通用配置(所有算法子类共享)
    void setOutputDelay(int64_t ms) { m_outputDelay = ms; }
    void setInputDelay(int64_t ms)  { m_inputDelay  = ms; }
    void setChannels(int ch)        { m_channels = ch; }

    int     sampleRate() const { return m_sampleRate; }
    int64_t outputDelay() const { return m_outputDelay; }
    int64_t inputDelay()  const { return m_inputDelay; }
    int     channels()   const { return m_channels; }

    // 初始化: 采样率 + 回声拖尾长度(ms, >= 房间回声路径时延) + 声道数。
    // 基类保存通用参数后调用子类 onInit 建具体资源。
    void init(int sampleRate, int tailMs = 256, int channels = 1);

    // 双流接口(算法无关, 由子类实现)
    virtual void addInput(int64_t time, const JZByteArray& buffer) = 0;
    virtual void addOutput(int64_t time, const JZByteArray& buffer) = 0;
    virtual void reset() = 0;

jz_signals:
    JZSigByteArray sigAecResult;   // 消除回声后的近端信号(int16 单声道 PCM)

protected:
    int     m_sampleRate  = 16000;
    int     m_tailMs      = 256;   // 回声拖尾长度(ms), 供子类换算为样本数/配置
    int     m_channels    = 1;     // 声道数(单声道算法可忽略)
    int64_t m_outputDelay = 0;     // 硬件播放延迟(ms)
    int64_t m_inputDelay  = 0;     // 采集/接收延迟(ms)

    // 子类真正初始化(建缓冲/系数等); base 已保存通用参数
    virtual void onInit(int sampleRate, int tailMs, int channels) = 0;
};

// ----------------------------------------------------------------------------
// 参考实现: 时域 NLMS 自适应滤波 AEC
// 约定: addInput/addOutput 的 JZByteArray 为 int16 单声道 PCM(单声道, 忽略 m_channels)。
// ----------------------------------------------------------------------------
class JZAecNLMS : public JZAec
{
public:
    JZAecNLMS();
    ~JZAecNLMS();

    void addInput(int64_t time, const JZByteArray& buffer) override;
    void addOutput(int64_t time, const JZByteArray& buffer) override;
    void reset() override;

protected:
    void onInit(int sampleRate, int tailMs, int channels) override;

private:
    // 取相对 ring 头(最新样本)回退 offset 个样本的参考值; offset>=0 表示更早的样本
    short ringSample(int offset) const;

    float   m_mu      = 0.5f;      // NLMS 步长(本类专属, 已从基类下沉)
    int     m_filterLen = 4096;    // 滤波器抽头数(样本)
    std::vector<float> m_w;        // 自适应滤波器系数
    float   m_eps     = 1e-3f;     // 归一化防除零

    // 参考(播放)样本环形缓冲: 头为最近样本, ringSample(j) 取第 j 个更早的样本
    std::vector<short> m_ring;
    int   m_ringCap   = 0;
    int   m_ringHead  = 0;        // 下一个写入位置

    std::mutex m_mutex;           // addInput(采集线程) / addOutput(渲染线程) 跨线程保护
};

#endif // !JZ_AEC_H_
