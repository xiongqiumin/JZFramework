#ifndef JZCIRCLEBUFFER_H
#define JZCIRCLEBUFFER_H
#include <vector>
#include "JZMutex.h"
#include "JZByteArray.h"

class JZFRAMEWORK_API JZCircleBuffer
{
public:
    /**
     * @brief 构造环形缓冲
     * @param initCap 初始容量(字节), 默认 1024; <=0 时不预分配
     * @param enableDynamic true=动态扩容 false=固定容量(默认)
     */
    explicit JZCircleBuffer(int initCap = 1024, bool enableDynamic = false);

    void setDynamic(bool enableDynamic);
    bool isDynamic() const;
    
    /**
     * @brief 调整缓冲容量(字节); 大于当前数据则扩容保留全部, 小于当前数据则缩小并丢弃最旧的多余数据(保留最新)
     * @param newCap 目标新容量(字节); <=0 时直接忽略
     */
    void resize(int newCap);
    void clear();

    /** 丢弃队首最旧的 len 字节; 若 len >= 数据量则清空 */
    void dropOldest(int len);

    int size() const;
    int capacity() const;

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

private:
    /**
     * @brief 将环形缓冲区数据拷贝到线性连续内存（扩容专用）
     * @param dst 目标线性内存首地址（长度至少为 m_dataLen）
     */
    void copyCircleToLinear(char* dst);

private:
    JZ_DISABLE_COPY(JZCircleBuffer)

    mutable JZMutex m_mutex;
    JZByteArray m_buffer;
    int m_head;
    int m_tail;
    int m_dataLen;
    bool m_dynamicMode;
};

#endif // JZCIRCLEBUFFER_H