#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "JZAudioUtils.h"

using namespace std;

namespace {

// 采样值域统一到 float [-1,1]:
//   UInt8 : 0..255   (中值 128)
//   Int16 : -32768..32767
//   Int32 : -2^31..2^31-1
//   Float : 直接当 [-1,1] 用
// 统一用 memcpy 读写, 避免未对齐指针强转 (ARM/严格对齐平台会挂)。
// 字节序按主机序 (PCM/WAV 在 x86 与 ARM-LE 上都是小端)。

float decodeSample(const char *p, JZAudioFormat::SampleFormat fmt)
{
    switch (fmt)
    {
    case JZAudioFormat::UInt8:
        return (static_cast<int>(static_cast<uint8_t>(p[0])) - 128) / 128.0f;
    case JZAudioFormat::Int16:
    {
        int16_t v = 0;
        memcpy(&v, p, sizeof(v));
        return v / 32768.0f;
    }
    case JZAudioFormat::Int32:
    {
        int32_t v = 0;
        memcpy(&v, p, sizeof(v));
        return static_cast<float>(v / 2147483648.0);
    }
    case JZAudioFormat::Float:
    {
        float v = 0;
        memcpy(&v, p, sizeof(v));
        return (v == v) ? v : 0.0f;              // NaN -> 0
    }
    default:
        return 0.0f;
    }
}

void encodeSample(char *p, float v, JZAudioFormat::SampleFormat fmt)
{
    if (!(v == v) || v > 1e30f || v < -1e30f)    // NaN / Inf -> 0
        v = 0.0f;

    switch (fmt)
    {
    // 编码用的缩放系数与上面 decode 完全对称 (128 / 32768 / 2^31),
    // 这样 int16->float->int16 这类往返是**位精确**的 (用 32767 会在满幅丢 1 LSB)。
    case JZAudioFormat::UInt8:
    {
        const float c = std::max(-1.0f, std::min(1.0f, v));
        int u = static_cast<int>(lrintf(c * 128.0f)) + 128;
        u = std::max(0, std::min(255, u));
        p[0] = static_cast<char>(static_cast<uint8_t>(u));
        break;
    }
    case JZAudioFormat::Int16:
    {
        const float c = std::max(-1.0f, std::min(1.0f, v));
        int s = static_cast<int>(lrintf(c * 32768.0f));
        s = std::max(-32768, std::min(32767, s));
        const int16_t v16 = static_cast<int16_t>(s);
        memcpy(p, &v16, sizeof(v16));
        break;
    }
    case JZAudioFormat::Int32:
    {
        const float c = std::max(-1.0f, std::min(1.0f, v));
        const double d = static_cast<double>(c) * 2147483648.0;
        int32_t s;
        if (d >= 2147483647.0)       s = INT32_MAX;
        else if (d <= -2147483648.0) s = INT32_MIN;
        else                         s = static_cast<int32_t>(d);
        memcpy(p, &s, sizeof(s));
        break;
    }
    case JZAudioFormat::Float:
    {
        // float 输出只清 NaN/Inf, 不做 [-1,1] 裁剪 (调用方可能有意超出)
        const float f = v;
        memcpy(p, &f, sizeof(f));
        break;
    }
    default:
        break;
    }
}

} // namespace

JZByteArray audioTrans(const JZByteArrayView &input,
                       JZAudioFormat input_format, JZAudioFormat output_format)
{
    const int inBps = input_format.bytesPerSample();
    const int outBps = output_format.bytesPerSample();
    const int inCh = input_format.channelCount;
    const int outCh = output_format.channelCount;
    const int inRate = input_format.sampleRate;
    const int outRate = output_format.sampleRate;

    // 参数校验: 非法直接返回空
    // (老实现里 channels<=0 会让步长变 0 而**死循环**, rate<=0 会除零)
    if (input.size() <= 0 || inBps <= 0 || outBps <= 0
        || inCh <= 0 || outCh <= 0 || inRate <= 0 || outRate <= 0)
        return JZByteArray();

    const int inFrameBytes = inBps * inCh;
    const int inFrames = input.size() / inFrameBytes;    // 尾部不足一帧的残余丢弃
    if (inFrames <= 0)
        return JZByteArray();

    // 完全同格式: 位精确直通 (省掉浮点往返, 也保证 no-op 调用不改数据)
    if (inBps == outBps && input_format.sampleFormat == output_format.sampleFormat
        && inCh == outCh && inRate == outRate)
        return JZByteArray(input.constData(), input.size());

    // ---- 1) 解码成 float (帧交织: [frame][channel]) ----
    std::vector<float> src(static_cast<size_t>(inFrames) * inCh);
    for (int f = 0; f < inFrames; f++)
    {
        const char *p = input.constData() + static_cast<size_t>(f) * inFrameBytes;
        float *dst = &src[static_cast<size_t>(f) * inCh];
        for (int c = 0; c < inCh; c++)
            dst[c] = decodeSample(p + c * inBps, input_format.sampleFormat);
    }

    // ---- 2) 声道映射: 输出声道 -> 输入声道索引, -1 = 取输入各声道平均值(下混) ----
    //   in==1  : 所有输出声道都取 ch0 (单声道复制)
    //   out==1 : 取所有输入声道的平均 (真正的下混, 不再像老实现只取左声道)
    //   其余   : 前 min(in,out) 个直通, 多出来的输出声道用平均值填充 (不是静音)
    std::vector<int> chMap(static_cast<size_t>(outCh), 0);
    if (inCh == 1)
    {
        for (int oc = 0; oc < outCh; oc++)
            chMap[oc] = 0;
    }
    else if (outCh == 1)
    {
        chMap[0] = -1;
    }
    else
    {
        for (int oc = 0; oc < outCh; oc++)
            chMap[oc] = (oc < inCh) ? oc : -1;
    }

    auto pick = [&src, inCh](int frame, int mappedCh) -> float
    {
        const float *row = &src[static_cast<size_t>(frame) * inCh];
        if (mappedCh >= 0)
            return row[mappedCh];
        float sum = 0.0f;
        for (int c = 0; c < inCh; c++)
            sum += row[c];
        return sum / static_cast<float>(inCh);
    };

    // ---- 3) 重采样(线性插值) + 声道映射 ----
    const long long outFramesLL = llround(static_cast<double>(inFrames) * outRate / inRate);
    const int outFrames = static_cast<int>(std::max(0LL, outFramesLL));
    if (outFrames <= 0)
        return JZByteArray();

    // 输出字节数上限保护 (JZByteArray 的长度是 int)
    const long long outBytesLL = static_cast<long long>(outFrames) * outBps * outCh;
    if (outBytesLL > INT_MAX)
        return JZByteArray();

    std::vector<float> dst(static_cast<size_t>(outFrames) * outCh);
    if (inRate == outRate)
    {
        // 采样率相同: 直接逐帧取, 不做插值 (保证位级一致, 也更快)
        for (int f = 0; f < outFrames; f++)
        {
            const int sf = std::min(f, inFrames - 1);
            float *d = &dst[static_cast<size_t>(f) * outCh];
            for (int oc = 0; oc < outCh; oc++)
                d[oc] = pick(sf, chMap[oc]);
        }
    }
    else
    {
        const double ratio = static_cast<double>(inRate) / outRate;   // 输入位置步进
        for (int f = 0; f < outFrames; f++)
        {
            const double pos = f * ratio;
            int i0 = static_cast<int>(pos);
            if (i0 < 0) i0 = 0;
            if (i0 >= inFrames) i0 = inFrames - 1;
            const int i1 = std::min(i0 + 1, inFrames - 1);
            const float frac = static_cast<float>(pos - i0);

            float *d = &dst[static_cast<size_t>(f) * outCh];
            for (int oc = 0; oc < outCh; oc++)
            {
                const float a = pick(i0, chMap[oc]);
                const float b = pick(i1, chMap[oc]);
                d[oc] = a + (b - a) * frac;
            }
        }
    }

    // ---- 4) 编码回目标格式 ----
    const int outFrameBytes = outBps * outCh;
    JZByteArray out;
    out.resize(static_cast<int>(outBytesLL));
    char *q = out.data();
    for (int f = 0; f < outFrames; f++)
    {
        const float *s = &dst[static_cast<size_t>(f) * outCh];
        char *p = q + static_cast<size_t>(f) * outFrameBytes;
        for (int oc = 0; oc < outCh; oc++)
            encodeSample(p + oc * outBps, s[oc], output_format.sampleFormat);
    }
    return out;
}
