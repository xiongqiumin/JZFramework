#include "JZCircleBuffer.h"
#include <cstring>      // std::memcpy
#include <algorithm>    // std::min

// 假设 JZMutex 和 JZMutexLocker 的实现已包含，此处仅作声明引用
// 若外部库需要链接，请确保正确包含实现文件

JZCircleBuffer::JZCircleBuffer(int initCap, bool enableDynamic)
    : m_head(0)
    , m_tail(0)
    , m_dataLen(0)
    , m_dynamicMode(enableDynamic)
{
    m_buffer.resize(initCap);
}

bool JZCircleBuffer::write(const char* data, int len)
{
    if (!data || len <= 0)
        return false;

    JZMutexLocker locker(&m_mutex);

    const int cap = static_cast<int>(m_buffer.size());
    int freeSpace = cap - m_dataLen;

    // 固定容量模式：空闲不足直接失败
    if (!m_dynamicMode && freeSpace < len)
        return false;

    // 动态扩容模式：空间不足则扩容至满足写入需求，2倍倍增
    if (freeSpace < len)
    {
        int newCap = cap;
        while ((newCap - m_dataLen) < len)
            newCap *= 2;

        std::vector<char> newBuf(newCap);
        // 将环形数据拷贝为连续线性（修复空缓冲区 bug）
        copyCircleToLinear(newBuf.data());
        m_buffer.swap(newBuf);
        m_head = 0;
        m_tail = m_dataLen;
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

int JZCircleBuffer::getSize()
{
    JZMutexLocker locker(&m_mutex);
    return m_dataLen;
}

int JZCircleBuffer::getCapacity()
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