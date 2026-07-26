#include "JZCircleBuffer.h"
#include "JZDebug.h"
#include <cstring>      // std::memcpy
#include <algorithm>    // std::min / std::max

JZCircleBuffer::JZCircleBuffer(int initCap, bool enableDynamic)
    : m_head(0)
    , m_tail(0)
    , m_dataLen(0)
    , m_dynamicMode(enableDynamic)
{
    if (initCap > 0)
        m_buffer.resize(static_cast<size_t>(initCap));
}

void JZCircleBuffer::setDynamic(bool enableDynamic)
{
    m_dynamicMode = enableDynamic;
}

bool JZCircleBuffer::isDynamic() const
{
    JZMutexLocker locker(&m_mutex);
    return m_dynamicMode;
}

void JZCircleBuffer::resize(int newCap)
{
    if (newCap <= 0)
        return;

    JZByteArray newBuf;
    newBuf.resize(newCap);
    if (m_dataLen > 0)
    {
        // 允许缩小: 至多保留 newCap 字节(从队首 head 拷贝), 多余数据直接丢弃
        int keep = std::min(m_dataLen, newCap);
        const int cap = static_cast<int>(m_buffer.size());
        int part1 = std::min(keep, cap - m_head);
        std::memcpy(newBuf.data(), m_buffer.data() + m_head, part1);
        if (keep > part1)
            std::memcpy(newBuf.data() + part1, m_buffer.data(), keep - part1);
        m_dataLen = keep;
    }
    else
    {
        m_dataLen = 0;
    }

    m_buffer.swap(newBuf);

    // ★ 线性化后数据落在 [0, m_dataLen), 必须重置头尾索引
    m_head = 0;
    m_tail = m_dataLen;
}

bool JZCircleBuffer::write(const char* data, int len)
{
    if (!data || len <= 0)
        return false;

    JZMutexLocker locker(&m_mutex);

    const int cap = static_cast<int>(m_buffer.size());
    int freeSpace = cap - m_dataLen;

    // 动态扩容模式：空间不足则扩容至满足写入需求, 2倍倍增(至少容纳本次写入)
    if (m_dynamicMode && freeSpace < len)
    {
        int newCap = std::max(cap * 2, cap + len);
        JZByteArray newBuf;
        newBuf.resize(newCap);
        // 将环形数据拷贝为连续线性(空缓冲时无数据可拷, copyCircleToLinear 已处理)
        copyCircleToLinear(newBuf.data());
        m_buffer.swap(newBuf);
        m_head = 0;
        m_tail = m_dataLen;
    }
    // 固定容量模式：空间不足则丢弃旧数据，保留最新
    else if (!m_dynamicMode && freeSpace < len)
    {
        jzWarning("JZCircleBuffer: buffer full, dropping oldest data "
                  "(cap=%d, used=%d, need=%d)", cap, m_dataLen, len);

        if (len > cap) {
            // 单次写入超过总容量，只保留尾部最新数据
            std::memcpy(m_buffer.data(), data + len - cap, cap);
            m_head = 0;
            m_tail = cap;
            m_dataLen = cap;
            return true;
        }

        // 丢弃队首最旧数据，多丢一些：至少丢弃一半容量，给后续写入留余量
        int drop = std::max(len - freeSpace, cap / 2);
        if (drop >= m_dataLen) {
            clear();
        } else {
            m_head = (m_head + drop) % cap;
            m_dataLen -= drop;
        }
    }

    // 分段写入环形区
    const int currCap = static_cast<int>(m_buffer.size());
    int writeSeg1 = std::min(len, currCap - m_tail);
    std::memcpy(m_buffer.data() + m_tail, data, writeSeg1);

    int writeSeg2 = len - writeSeg1;
    if (writeSeg2 > 0)
    {
        std::memcpy(m_buffer.data(), data + writeSeg1, writeSeg2);
    }

    m_tail = (m_tail + len) % currCap;
    m_dataLen += len;
    return true;
}

int JZCircleBuffer::read(char* data, int len)
{
    if (!data || len <= 0)
        return 0;

    JZMutexLocker locker(&m_mutex);
    if (m_dataLen == 0)
        return 0;

    int readTotal = std::min(len, m_dataLen);
    const int currCap = static_cast<int>(m_buffer.size());

    int readSeg1 = std::min(readTotal, currCap - m_head);
    std::memcpy(data, m_buffer.data() + m_head, readSeg1);

    int readSeg2 = readTotal - readSeg1;
    if (readSeg2 > 0)
    {
        std::memcpy(data + readSeg1, m_buffer.data(), readSeg2);
    }

    m_head = (m_head + readTotal) % currCap;
    m_dataLen -= readTotal;
    return readTotal;
}

int JZCircleBuffer::size() const
{
    JZMutexLocker locker(&m_mutex);
    return m_dataLen;
}

int JZCircleBuffer::capacity() const
{
    JZMutexLocker locker(&m_mutex);
    return static_cast<int>(m_buffer.size());
}

void JZCircleBuffer::clear()
{
    JZMutexLocker locker(&m_mutex);
    m_head = 0;
    m_tail = 0;
    m_dataLen = 0;
}

void JZCircleBuffer::dropOldest(int bytes)
{
    if (bytes <= 0)
        return;

    JZMutexLocker locker(&m_mutex);
    if (bytes >= m_dataLen) {
        clear();
        return;
    }
    const int cap = static_cast<int>(m_buffer.size());
    m_head = (m_head + bytes) % cap;
    m_dataLen -= bytes;
}

void JZCircleBuffer::copyCircleToLinear(char* dst)
{
    // ★ 修复：空缓冲区时无数据可拷贝
    if (m_dataLen == 0)
        return;

    const int cap = static_cast<int>(m_buffer.size());
    const char* src = m_buffer.data();
    if (m_head < m_tail)
    {
        // 数据单段连续
        std::memcpy(dst, src + m_head, m_dataLen);
    }
    else
    {
        // 两段：head到末尾、0到tail
        int part1 = cap - m_head;
        std::memcpy(dst, src + m_head, part1);
        std::memcpy(dst + part1, src, m_tail);
    }
}