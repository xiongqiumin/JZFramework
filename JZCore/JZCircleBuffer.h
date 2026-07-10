#ifndef JZCIRCLEBUFFER_H
#define JZCIRCLEBUFFER_H

#include <vector>
#include "JZMutex.h"

class JZCircleBuffer
{
public:
    /**
     * @brief 构造环形缓冲
     * @param initCap 初始容量
     * @param enableDynamic true=动态扩容 false=固定容量
     */
    explicit JZCircleBuffer(int initCap, bool enableDynamic = false);

    // 禁止拷贝、移动（多线程缓冲区不允许复制）
    JZCircleBuffer(const JZCircleBuffer&) = delete;
    JZCircleBuffer& operator=(const JZCircleBuffer&) = delete;
    JZCircleBuffer(JZCircleBuffer&&) = delete;
    JZCircleBuffer& operator=(JZCircleBuffer&&) = delete;

    /**
     * @brief 写入指定长度数据，必须全部写入才返回true，不足则整体失败
     * @param data 源数据指针（只读）
     * @param len 待写入字节数
     * @return true 全部写入成功；false 参数非法/空间不足/扩容失败
     */
    bool write(const char* data, int len);

    /**
     * @brief 读取数据，最多读取len字节
     * @param data 输出缓冲区
     * @param len 期望读取长度
     * @return 实际读取字节数，0代表无数据
     */
    int read(char* data, int len);

    // 获取当前有效数据长度
    int getSize();

    // 获取缓冲区总容量
    int getCapacity();

    // 清空所有数据
    void clear();

private:
    /**
     * @brief 将环形缓冲区数据拷贝到线性连续内存（扩容专用）
     * @param dst 目标线性内存首地址（长度至少为 m_dataLen）
     */
    void copyCircleToLinear(char* dst);

private:
    JZMutex m_mutex;
    std::vector<char> m_buffer;
    int m_head;
    int m_tail;
    int m_dataLen;
    bool m_dynamicMode;
};

#endif // JZCIRCLEBUFFER_H