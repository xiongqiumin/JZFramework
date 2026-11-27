#ifndef JZ_BYTE_ARRAY_VIEW_H_
#define JZ_BYTE_ARRAY_VIEW_H_
#include <cstring>
#include <string>
#include <algorithm>
#include "JZGlobal.h"
#include "JZByteArray.h"

// JZByteArray 的**只读视图** (类似 QByteArrayView / std::span<const char>)。
//
// 设计意图 —— 只用来解决"看见一块字节, 但不想拥有它":
//   - 只持有 {指针, 长度}, 构造/拷贝/切片都不拷贝字节 (纯值类型, 16 字节);
//   - 只读: 类型上根本没有 append/resize/非 const data(), 写操作不可达;
//   - 从 JZByteArray 隐式构造, 所以"只读传参"的既有调用点可以逐个替换成 view,
//     而 JZByteArray::fromRawData() 的借用对象也能零拷贝地传进同一套 API;
//   - 生命周期由调用方保证: 视图存活期间被引用的内存必须有效。
//     ⚠ 因此不要把 view 存进成员/容器、不要跨线程或异步回调持有;
//       需要长期持有就 JZByteArray(bytes) 拷一份, 或 toOwned()。
//
// 与 JZByteArray 的分工(约定):
//   - 只读传参: 优先 const JZByteArray& (借用对象也能传), 需要精确表达"我只借来看"时才用 view;
//   - 返回/持有: 一律 JZByteArray (拥有数据), 不要返回 view。
class JZFRAMEWORK_API JZByteArrayView
{
public:
    JZByteArrayView() {}
    JZByteArrayView(const char *data, int size) { init(data, size); }
    // 不拷贝: 直接指向 JZByteArray 的缓冲 (自有态与借用态都适用)
    JZByteArrayView(const JZByteArray &bytes) { init(bytes.constData(), bytes.size()); }
    JZByteArrayView(const std::string &str) { init(str.data(), (int)str.size()); }

    const char *data() const { return m_data; }
    const char *constData() const { return m_data; }
    int size() const { return m_size; }
    bool isEmpty() const { return m_size <= 0; }

    const char *begin() const { return m_data; }
    const char *end() const { return m_data ? m_data + m_size : nullptr; }

    const char &operator[](int index) const { return m_data[index]; }

    // 切片: 返回**新视图**(零拷贝), 不是副本; 越界自动裁剪到有效范围
    JZByteArrayView mid(int start, int len = -1) const
    {
        if (start < 0 || start > m_size)
            return JZByteArrayView();
        if (len < 0 || start + len > m_size)
            len = m_size - start;
        return JZByteArrayView(m_data + start, len);
    }
    JZByteArrayView left(int size) const { return mid(0, size); }

    // 拷一份**自有副本**(唯一"付钱"的出口): 需要长期持有/要写/要跨作用域时用它。
    JZByteArray toByteArray() const
    {
        if (m_size <= 0)
            return JZByteArray();
        return JZByteArray(m_data, m_size);
    }

    // 按字节比较 (长度不同即不等)
    bool operator==(const JZByteArrayView &other) const
    {
        if (m_size != other.m_size)
            return false;
        if (m_size <= 0)
            return true;
        return std::memcmp(m_data, other.m_data, (size_t)m_size) == 0;
    }
    bool operator!=(const JZByteArrayView &other) const { return !(*this == other); }

private:
    void init(const char *data, int size)
    {
        const bool valid = (data != nullptr && size > 0);
        m_data = valid ? data : nullptr;
        m_size = valid ? size : 0;
    }

    const char *m_data = nullptr;
    int m_size = 0;
};

#endif // ! JZ_BYTE_ARRAY_VIEW_H_
