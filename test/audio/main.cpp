// JZAudioUtils::audioTrans 测试: 采样格式(uint8/int16/int32/float) / 采样率 / 声道 转换。
//
// 独立 exe (不并到 test_core): 转换矩阵 + 已知数值校验, 见 ctest 的 audio.test_audio。
#include "JZTest.h"
#include "JZApplication.h"
#include "JZAudioFormat.h"
#include "JZAudioUtils.h"
#include "JZByteArray.h"
#include "JZByteArrayView.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

namespace {

JZAudioFormat makeFormat(int channels, int rate, JZAudioFormat::SampleFormat sf)
{
    JZAudioFormat f;
    f.channelCount = channels;
    f.sampleRate = rate;
    f.sampleFormat = sf;
    return f;
}

int bytesOf(JZAudioFormat::SampleFormat sf)
{
    switch (sf)
    {
    case JZAudioFormat::UInt8: return 1;
    case JZAudioFormat::Int16: return 2;
    case JZAudioFormat::Int32:
    case JZAudioFormat::Float: return 4;
    default:                   return 0;
    }
}

// ---- 参照实现: 按"规格"写/读单个样本 (与被测实现独立, 用于构造输入与校验输出) ----
void writeSample(char *p, float v, JZAudioFormat::SampleFormat sf)
{
    const float c = std::max(-1.0f, std::min(1.0f, v));
    switch (sf)
    {
    case JZAudioFormat::UInt8:
    {
        int u = static_cast<int>(lrintf(c * 128.0f)) + 128;
        p[0] = static_cast<char>(static_cast<uint8_t>(std::max(0, std::min(255, u))));
        break;
    }
    case JZAudioFormat::Int16:
    {
        int s = static_cast<int>(lrintf(c * 32768.0f));
        s = std::max(-32768, std::min(32767, s));
        const int16_t x = static_cast<int16_t>(s);
        memcpy(p, &x, sizeof(x));
        break;
    }
    case JZAudioFormat::Int32:
    {
        const double d = static_cast<double>(c) * 2147483648.0;
        int32_t s = INT32_MAX;
        if (d <= -2147483648.0) s = INT32_MIN;
        else if (d < 2147483647.0) s = static_cast<int32_t>(d);
        memcpy(p, &s, sizeof(s));
        break;
    }
    case JZAudioFormat::Float:
    {
        const float f = v;
        memcpy(p, &f, sizeof(f));
        break;
    }
    default:
        break;
    }
}

float readSample(const char *p, JZAudioFormat::SampleFormat sf)
{
    switch (sf)
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
        return v;
    }
    default:
        return 0.0f;
    }
}

// 该格式的量化误差上限 (1.5 LSB, 留一点舍入余量)
double quantTol(JZAudioFormat::SampleFormat sf)
{
    switch (sf)
    {
    case JZAudioFormat::UInt8: return 1.5 / 128.0;
    case JZAudioFormat::Int16: return 1.5 / 32768.0;
    case JZAudioFormat::Int32: return 1.5 / 2147483648.0;
    default:                   return 1e-6;
    }
}

// 用 float 参考信号按某格式构造 PCM
JZByteArray buildPcm(const std::vector<float> &ref, JZAudioFormat::SampleFormat sf)
{
    const int bps = bytesOf(sf);
    JZByteArray out;
    out.resize(static_cast<int>(ref.size()) * bps);
    char *p = out.data();
    for (size_t i = 0; i < ref.size(); i++)
        writeSample(p + i * bps, ref[i], sf);
    return out;
}

int16_t i16At(const JZByteArray &bytes, int index)
{
    int16_t v = 0;
    memcpy(&v, bytes.constData() + index * 2, sizeof(v));
    return v;
}

float f32At(const JZByteArray &bytes, int index)
{
    float v = 0;
    memcpy(&v, bytes.constData() + index * 4, sizeof(v));
    return v;
}

// ==================== 用例 ====================

// 完全同格式: 位精确直通(不做浮点往返)
bool passthrough()
{
    const int16_t src[5] = { 100, -200, 3000, -32768, 32767 };
    JZByteArray in((const char *)src, sizeof(src));

    JZByteArray out = audioTrans(in, makeFormat(1, 16000, JZAudioFormat::Int16),
                                     makeFormat(1, 16000, JZAudioFormat::Int16));
    JZ_TEST_EQ(out.size(), in.size());
    JZ_TEST_TRUE(memcmp(out.constData(), in.constData(), in.size()) == 0);
    return true;
}

// 4x4 采样格式转换矩阵: 同一段参考信号, 任意 from -> to
bool format_matrix()
{
    static const JZAudioFormat::SampleFormat kFormats[] = {
        JZAudioFormat::UInt8, JZAudioFormat::Int16, JZAudioFormat::Int32, JZAudioFormat::Float
    };
    const std::vector<float> ref = { -1.0f, -0.5f, 0.0f, 0.25f, 1.0f };

    for (JZAudioFormat::SampleFormat from : kFormats)
    {
        JZByteArray in = buildPcm(ref, from);
        for (JZAudioFormat::SampleFormat to : kFormats)
        {
            JZByteArray out = audioTrans(in, makeFormat(1, 16000, from), makeFormat(1, 16000, to));

            const int bps = bytesOf(to);
            JZ_TEST_EQ(out.size(), static_cast<int>(ref.size()) * bps);

            // 误差上限取两端量化误差的较大者
            const double tol = std::max(quantTol(from), quantTol(to));
            for (size_t i = 0; i < ref.size(); i++)
            {
                const float got = readSample(out.constData() + i * bps, to);
                JZ_TEST_TRUE(std::fabs(got - ref[i]) <= tol);
            }
        }
    }
    return true;
}

// 已知数值: float -> int16 / int16 -> float / uint8 -> int16 / 削顶
bool known_values()
{
    // float -> int16 (乘 32768 后四舍五入; 超出 [-1,1] 削顶)
    const float sf[4] = { 0.25f, -1.0f, 2.0f, -2.0f };
    JZByteArray inF((const char *)sf, sizeof(sf));
    JZByteArray outI = audioTrans(inF, makeFormat(1, 16000, JZAudioFormat::Float),
                                       makeFormat(1, 16000, JZAudioFormat::Int16));
    JZ_TEST_EQ(outI.size(), 8);
    JZ_TEST_EQ(i16At(outI, 0), 8192);
    JZ_TEST_EQ(i16At(outI, 1), -32768);
    JZ_TEST_EQ(i16At(outI, 2), 32767);      // +2.0 削顶
    JZ_TEST_EQ(i16At(outI, 3), -32768);     // -2.0 削顶

    // int16 -> float (除以 32768)
    const int16_t s16[3] = { 0, 16384, -32768 };
    JZByteArray inI((const char *)s16, sizeof(s16));
    JZByteArray outF = audioTrans(inI, makeFormat(1, 16000, JZAudioFormat::Int16),
                                       makeFormat(1, 16000, JZAudioFormat::Float));
    JZ_TEST_EQ(outF.size(), 12);
    JZ_TEST_TRUE(std::fabs(f32At(outF, 0) - 0.0f) < 1e-6f);
    JZ_TEST_TRUE(std::fabs(f32At(outF, 1) - 0.5f) < 1e-6f);
    JZ_TEST_TRUE(std::fabs(f32At(outF, 2) + 1.0f) < 1e-6f);

    // uint8: 0 = -1.0, 128 = 0, 255 = +127/128
    const uint8_t s8[3] = { 0, 128, 255 };
    JZByteArray in8((const char *)s8, sizeof(s8));
    JZByteArray out8 = audioTrans(in8, makeFormat(1, 16000, JZAudioFormat::UInt8),
                                       makeFormat(1, 16000, JZAudioFormat::Int16));
    JZ_TEST_EQ(i16At(out8, 0), -32768);
    JZ_TEST_EQ(i16At(out8, 1), 0);
    JZ_TEST_EQ(i16At(out8, 2), 32512);      // 0.9921875 * 32768

    // float 输入里的 NaN 要变成 0 (不能污染后续样本)
    const float nanv = std::nanf("");
    JZByteArray inN((const char *)&nanv, sizeof(nanv));
    JZByteArray outN = audioTrans(inN, makeFormat(1, 16000, JZAudioFormat::Float),
                                       makeFormat(1, 16000, JZAudioFormat::Int16));
    JZ_TEST_EQ(i16At(outN, 0), 0);
    return true;
}

// int16 -> float -> int16 往返位精确 (编解码缩放系数对称)
bool roundtrip_exact()
{
    const int16_t src[6] = { -32768, -12345, -1, 0, 12345, 32767 };
    JZByteArray in((const char *)src, sizeof(src));

    JZByteArray f = audioTrans(in, makeFormat(1, 16000, JZAudioFormat::Int16),
                                    makeFormat(1, 16000, JZAudioFormat::Float));
    JZByteArray back = audioTrans(f, makeFormat(1, 16000, JZAudioFormat::Float),
                                     makeFormat(1, 16000, JZAudioFormat::Int16));

    JZ_TEST_EQ(back.size(), in.size());
    JZ_TEST_TRUE(memcmp(back.constData(), in.constData(), in.size()) == 0);
    return true;
}

bool resample()
{
    // 48k -> 16k: 480 帧 -> 160 帧; 常量信号插值后仍是常量
    std::vector<int16_t> src(480, 1000);
    JZByteArray in((const char *)src.data(), (int)(src.size() * 2));
    JZByteArray out = audioTrans(in, makeFormat(1, 48000, JZAudioFormat::Int16),
                                     makeFormat(1, 16000, JZAudioFormat::Int16));
    JZ_TEST_EQ(out.size(), 160 * 2);
    for (int i = 0; i < 160; i++)
        JZ_TEST_EQ(i16At(out, i), 1000);

    // 16k -> 48k: 160 帧 -> 480 帧
    JZByteArray out2 = audioTrans(out, makeFormat(1, 16000, JZAudioFormat::Int16),
                                       makeFormat(1, 48000, JZAudioFormat::Int16));
    JZ_TEST_EQ(out2.size(), 480 * 2);
    return true;
}

bool channels()
{
    const int16_t mono[2] = { 1000, -1000 };
    JZByteArray inM((const char *)mono, sizeof(mono));

    // 1ch -> 2ch: 左右相同(复制)
    JZByteArray up = audioTrans(inM, makeFormat(1, 16000, JZAudioFormat::Int16),
                                     makeFormat(2, 16000, JZAudioFormat::Int16));
    JZ_TEST_EQ(up.size(), 8);
    JZ_TEST_EQ(i16At(up, 0), 1000);
    JZ_TEST_EQ(i16At(up, 1), 1000);
    JZ_TEST_EQ(i16At(up, 2), -1000);
    JZ_TEST_EQ(i16At(up, 3), -1000);

    // 2ch -> 1ch: 取平均(真正的下混, 不再是"只取左声道")
    const int16_t st[4] = { 1000, 2000, -1000, -3000 };
    JZByteArray inS((const char *)st, sizeof(st));
    JZByteArray down = audioTrans(inS, makeFormat(2, 16000, JZAudioFormat::Int16),
                                       makeFormat(1, 16000, JZAudioFormat::Int16));
    JZ_TEST_EQ(down.size(), 4);
    JZ_TEST_EQ(i16At(down, 0), 1500);
    JZ_TEST_EQ(i16At(down, 1), -2000);
    return true;
}

bool invalid_params()
{
    JZByteArray in("12345678", 8);
    // 声道/采样率 <= 0: 返回空 (老实现会步长 0 死循环 / 除零)
    JZ_TEST_TRUE(audioTrans(in, makeFormat(0, 16000, JZAudioFormat::Int16),
                                makeFormat(1, 16000, JZAudioFormat::Int16)).isEmpty());
    JZ_TEST_TRUE(audioTrans(in, makeFormat(1, 0, JZAudioFormat::Int16),
                                makeFormat(1, 16000, JZAudioFormat::Int16)).isEmpty());
    // 未知采样格式
    JZ_TEST_TRUE(audioTrans(in, makeFormat(1, 16000, JZAudioFormat::UnknownSampleFormat),
                                makeFormat(1, 16000, JZAudioFormat::Int16)).isEmpty());
    // 空输入 / 不足一帧 (1ch int16 只给 1 字节)
    JZ_TEST_TRUE(audioTrans(JZByteArrayView(), makeFormat(1, 16000, JZAudioFormat::Int16),
                            makeFormat(1, 16000, JZAudioFormat::Int16)).isEmpty());
    JZ_TEST_TRUE(audioTrans(JZByteArrayView("x", 1), makeFormat(1, 16000, JZAudioFormat::Int16),
                            makeFormat(1, 16000, JZAudioFormat::Int16)).isEmpty());
    return true;
}

// 输入走 JZByteArrayView 的偏移切片: 只转换中间那段, 前后字节不受影响
bool view_slice()
{
    const int16_t all[6] = { 1111, 2222, 3333, 4444, 5555, 6666 };
    JZByteArray buf((const char *)all, sizeof(all));

    // 取 [1, 4) = 2222, 3333, 4444
    JZByteArrayView slice = JZByteArrayView(buf).mid(1 * 2, 3 * 2);
    JZ_TEST_EQ(slice.size(), 6);

    JZByteArray out = audioTrans(slice, makeFormat(1, 16000, JZAudioFormat::Int16),
                                        makeFormat(1, 16000, JZAudioFormat::Float));
    JZ_TEST_EQ(out.size(), 12);
    JZ_TEST_TRUE(std::fabs(f32At(out, 0) - 2222.0f / 32768.0f) < 1e-6f);
    JZ_TEST_TRUE(std::fabs(f32At(out, 1) - 3333.0f / 32768.0f) < 1e-6f);
    JZ_TEST_TRUE(std::fabs(f32At(out, 2) - 4444.0f / 32768.0f) < 1e-6f);
    return true;
}

} // namespace

int main(int argc, char *argv[])
{
    JZApplication app;
    JZTest test;

    auto suite = std::make_shared<JZTestSuite>("JZAudioUtils");
    test.addSuite(suite);
    suite->addTestCase("passthrough", passthrough);
    suite->addTestCase("format_matrix", format_matrix);
    suite->addTestCase("known_values", known_values);
    suite->addTestCase("roundtrip_exact", roundtrip_exact);
    suite->addTestCase("resample", resample);
    suite->addTestCase("channels", channels);
    suite->addTestCase("invalid_params", invalid_params);
    suite->addTestCase("view_slice", view_slice);

    return test.run(argc, argv);
}
