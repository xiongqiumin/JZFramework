#include "JZAec.h"

#include <mutex>
#include <vector>

// ===================== JZAec 抽象基类 =====================
JZAec::JZAec()
    : sigAecResult(this)
{
}

JZAec::~JZAec()
{
}

void JZAec::init(int sampleRate, int tailMs, int channels)
{
    m_sampleRate = sampleRate;
    m_tailMs     = tailMs;
    m_channels   = channels;
    onInit(sampleRate, tailMs, channels);
}

// ===================== JZAecNLMS 参考实现 =====================
JZAecNLMS::JZAecNLMS()
{
    init(16000, 256);
}

JZAecNLMS::~JZAecNLMS()
{
}

void JZAecNLMS::onInit(int sampleRate, int tailMs, int channels)
{
    m_filterLen = (sampleRate * tailMs) / 1000;
    if (m_filterLen < 64) m_filterLen = 64;

    m_w.assign(m_filterLen, 0.0f);

    // 环形缓冲覆盖: 滤波器长度 + 余量, 取 2 秒足够(最近样本在 ring 头)
    m_ringCap  = sampleRate * 2;
    m_ring.assign(m_ringCap, 0);
    m_ringHead = 0;
}

void JZAecNLMS::reset()
{
    std::lock_guard<std::mutex> lk(m_mutex);
    m_w.assign(m_filterLen, 0.0f);
    m_ring.assign(m_ringCap, 0);
    m_ringHead = 0;
}

short JZAecNLMS::ringSample(int offset) const
{
    if (offset < 0) offset = 0;                 // 未来样本 -> 取最新
    if (offset >= m_ringCap) offset = m_ringCap - 1; // 超出历史 -> 取最旧
    int idx = m_ringHead - 1 - offset;
    if (idx < 0) idx += m_ringCap;
    return m_ring[idx];
}

void JZAecNLMS::addOutput(int64_t time, const JZByteArray& buffer)
{
    const short* p = reinterpret_cast<const short*>(buffer.constData());
    int n = buffer.size() / 2;
    if (n <= 0) return;

    std::lock_guard<std::mutex> lk(m_mutex);
    for (int i = 0; i < n; i++)
    {
        m_ring[m_ringHead] = p[i];
        m_ringHead = (m_ringHead + 1) % m_ringCap;
    }
    // time 仅作调用时刻记录, NLMS 不依赖时间戳对齐(延迟由滤波器自适应学习)
}

void JZAecNLMS::addInput(int64_t time, const JZByteArray& buffer)
{
    (void)time;   // NLMS 自适应学习回声延迟, 时间戳不直接参与对齐(避免参考窗抖动)
    const short* mic = reinterpret_cast<const short*>(buffer.constData());
    int n = buffer.size() / 2;
    if (n <= 0) return;

    std::vector<char> outBytes(n * 2);

    {
        std::lock_guard<std::mutex> lk(m_mutex);
        std::vector<float> x(m_filterLen);
        for (int i = 0; i < n; i++)
        {
            // 参考向量 = 最近 m_filterLen 个"实际播放"样本(由 addOutput 写入 ring)。
            // ring 头是最近样本, 越早越靠后: x[j] = ringSample(j)。
            // 回声延迟由 NLMS 在滤波器内自适应学习(只要 delay < m_filterLen 即可)。
            // 注: 不依赖时间戳算 base, 否则每帧对齐量抖动会导致 yhat 跳变->爆音。
            float yhat = 0.0f;
            float energy = 0.0f;
            for (int j = 0; j < m_filterLen; j++)
            {
                float xj = (float)ringSample(j);
                x[j] = xj;
                yhat   += m_w[j] * xj;
                energy += xj * xj;
            }

            float d = (float)mic[i];
            float e = d - yhat;                    // 残差 = 近端信号(去回声)

            float norm = energy + m_eps;           // NLMS 系数更新(归一化防除零)
            for (int j = 0; j < m_filterLen; j++)
                m_w[j] += m_mu * e * x[j] / norm;

            if (e >  32767.0f) e =  32767.0f;      // 限幅输出
            if (e < -32768.0f) e = -32768.0f;
            short ov = (short)e;
            outBytes[i * 2]     = (char)(ov & 0xff);
            outBytes[i * 2 + 1] = (char)((ov >> 8) & 0xff);
        }
    }

    JZByteArray result(outBytes.data(), n * 2);
    sigAecResult.emit(result);
}
