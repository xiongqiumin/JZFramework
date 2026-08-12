#ifndef JZ_DATA_STREAM_H_
#define JZ_DATA_STREAM_H_

#include <string>
#include <stdint.h>
#include <type_traits>
#include <map>
#include <vector>
#include "JZByteArray.h"
#include "JZFile.h"
#include <cstring>

// ---- 编译期主机字节序(静态判断) ----
// 主机字节序在编译期即已知, 无需运行时探测; 交换与否据此静态确定。
#if defined(__BYTE_ORDER__)
  #if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    #define JZ_HOST_LITTLE_ENDIAN 0
  #else
    #define JZ_HOST_LITTLE_ENDIAN 1
  #endif
#elif defined(__BIG_ENDIAN__) || defined(_BIG_ENDIAN)
  #define JZ_HOST_LITTLE_ENDIAN 0
#else
  #define JZ_HOST_LITTLE_ENDIAN 1   // x86 / ARM(默认小端)
#endif

// 字节交换: 使用编译器内建指令, 开销极小(单条指令)
#if defined(_MSC_VER)
  #define JZ_BSWAP16(v) _byteswap_ushort(v)
  #define JZ_BSWAP32(v) _byteswap_ulong(v)
  #define JZ_BSWAP64(v) _byteswap_uint64(v)
#else
  #define JZ_BSWAP16(v) __builtin_bswap16(v)
  #define JZ_BSWAP32(v) __builtin_bswap32(v)
  #define JZ_BSWAP64(v) __builtin_bswap64(v)
#endif

inline uint16_t jzByteSwap(uint16_t v) { return JZ_BSWAP16(v); }
inline uint32_t jzByteSwap(uint32_t v) { return JZ_BSWAP32(v); }
inline uint64_t jzByteSwap(uint64_t v) { return JZ_BSWAP64(v); }
inline int16_t  jzByteSwap(int16_t  v) { return (int16_t)  JZ_BSWAP16((uint16_t)v); }
inline int32_t  jzByteSwap(int32_t  v) { return (int32_t)  JZ_BSWAP32((uint32_t)v); }
inline int64_t  jzByteSwap(int64_t  v) { return (int64_t)  JZ_BSWAP64((uint64_t)v); }
inline float    jzByteSwap(float    v) { uint32_t u; memcpy(&u, &v, 4); u = JZ_BSWAP32(u); memcpy(&v, &u, 4); return v; }
inline double   jzByteSwap(double   v) { uint64_t u; memcpy(&u, &v, 8); u = JZ_BSWAP64(u); memcpy(&v, &u, 8); return v; }
// 1 字节类型不需要字节交换, 直接返回(避免 writePad/readPad 模板中 bool/char 重载歧义)
inline bool          jzByteSwap(bool v)          { return v; }
inline char          jzByteSwap(char v)          { return v; }
inline signed char   jzByteSwap(signed char v)   { return v; }
inline unsigned char jzByteSwap(unsigned char v) { return v; }
#undef JZ_BSWAP16
#undef JZ_BSWAP32
#undef JZ_BSWAP64

class JZDataStreamImpl;
class JZDataStream;

// 前向声明: 供 JZDataStream 类内 friend 使用(使其能访问 protected 的 m_buffer/m_file/m_offset)
template<class T>
JZDataStream &operator<<(JZDataStream &s, const std::vector<T> &vec);

class JZDataStream
{
public:
    enum ByteOrder
    {
        LittleEndian,
        BigEndian,
    };

    // 编译期常量: 本机字节序。同序设备(如 x86/ARM 上配置小端)使用流时,
    // writePod/readPod 中的交换条件恒为假, 不执行任何 swap, 速度与原实现一致。
    static constexpr ByteOrder HostOrder =
        (JZ_HOST_LITTLE_ENDIAN ? LittleEndian : BigEndian);

    JZDataStream(JZFile *file);
    JZDataStream(const JZByteArray *buffer);
    JZDataStream(JZByteArray *buffer, JZ::OpenMode mode);    
    ~JZDataStream();

    int writeRawData(const char *s, int len);
    int readRawData(char *s, int len);
    void skipRawData(int offset);

    void setByteOrder(ByteOrder order);
    ByteOrder byteOrder();

    // vector 批量序列化友元: 跨端内存流需直接访问 m_buffer/m_offset 做原地字节交换,
    // 故将 operator<<(std::vector<T>) 主模板声明为友元(仅主模板, vector<bool> 特化无需访问)。
    template<class T>
    friend JZDataStream &operator<<(JZDataStream &s, const std::vector<T> &vec);

    JZDataStream &operator<<(bool i);
    JZDataStream &operator>>(bool &i);
    JZDataStream &operator<<(int8_t i);
    JZDataStream &operator>>(int8_t &i);
    JZDataStream &operator<<(uint8_t i);
    JZDataStream &operator>>(uint8_t &i);
    JZDataStream &operator<<(int16_t i);
    JZDataStream &operator>>(int16_t &i);
    JZDataStream &operator<<(uint16_t i);
    JZDataStream &operator>>(uint16_t &i);
    JZDataStream &operator<<(int32_t i);
    JZDataStream &operator>>(int32_t &i);
    JZDataStream &operator<<(uint32_t i);
    JZDataStream &operator>>(uint32_t &i);
    JZDataStream &operator<<(int64_t i);
    JZDataStream &operator>>(int64_t &i);
    JZDataStream &operator<<(uint64_t i);
    JZDataStream &operator>>(uint64_t &i);
    JZDataStream &operator<<(float i);
    JZDataStream &operator>>(float &i);
    JZDataStream &operator<<(double i);
    JZDataStream &operator>>(double &i);

public:
    // 错误状态: 一旦发生错误, 后续 read/write 操作都将直接跳过 (no-op)。
    bool isError() const { return m_error; }
    std::string errorString() const { return m_errorString; }
    void setError(const std::string& msg);
    void clearError();

    // 预判内存流是否还有 need 字节可读; 文件流不预判, 返回 true; 出错或容量不足返回 false。
    bool checkReadCapacity(int64_t need) const;

    // 写入上限: 设置后, 任何一次 write 超出该大小都会立即 setError (默认 -1 表示不限制)。
    void setMaxSize(int64_t size) { m_maxSize = size; }
    int64_t maxSize() const { return m_maxSize; }

protected:
    JZ_DISABLE_COPY(JZDataStream);

    template<class T>
    void writePod(T i)
    {
        if (m_error)
            return;
        // 静态判断: HostOrder 为编译期常量。仅当配置的字节序与主机字节序不同时才交换;
        // 同序设备(如 x86/ARM 上配置小端)此条件恒假, 不执行任何 swap, 速度与原实现一致。
        if (m_byteOrder != HostOrder && sizeof(T) > 1)
            i = jzByteSwap(i);

        if (m_file)
        {
            int w = m_file->write((const char*)&i, sizeof(i));
            if (w != (int)sizeof(i))
                setError("JZDataStream: write failed");
        }
        else
        {
            if (m_maxSize >= 0 && m_offset + (int)sizeof(i) > m_maxSize)
            {
                setError("JZDataStream: write exceeds max size");
                return;
            }
            if (m_offset + (int)sizeof(i) > m_buffer->size())
                m_buffer->resize(m_offset + sizeof(i));

            T *ptr = (T*)(m_buffer->data() + m_offset);
            *ptr = i;
            m_offset += sizeof(i);
        }
    }

    template<class T>
    void readPod(T &i)
    {
        if (m_error)
            return;
        if (m_file)
        {
            int r = m_file->read((char*)&i, sizeof(i));
            if (r != (int)sizeof(i))
                setError("JZDataStream: read failed");
        }
        else
        {
            if (m_offset + (int)sizeof(i) > m_buffer->size())
            {
                setError("JZDataStream: read out of range");
                return;
            }

            T *ptr = (T*)(m_buffer->data() + m_offset);
            i = *ptr;
            m_offset += sizeof(i);
        }

        // 读出的是按 m_byteOrder 存放的字节, 与主机序不同则交换为本地值
        if (m_byteOrder != HostOrder && sizeof(T) > 1)
            i = jzByteSwap(i);
    }

    int m_offset;
    ByteOrder m_byteOrder;      // 当前配置的字节序(运行期可设, 见 setByteOrder)
    JZByteArray *m_buffer;
    JZFile *m_file;
    bool m_error;
    std::string m_errorString;
    int64_t m_maxSize;   // 写入上限(字节), <0 表示不限制
};

JZDataStream &operator<<(JZDataStream &s, const std::string &str);
JZDataStream &operator>>(JZDataStream &s, std::string &str);
JZDataStream &operator<<(JZDataStream &s, const JZByteArray &buffer);
JZDataStream &operator>>(JZDataStream &s, JZByteArray &buffer);

// 容器长度字段: 统一以小端写入/读出, 与流当前配置的字节序无关。
// 这样 size 头部在大小端流下完全一致, 且便于跨端/抓包排查。
inline void jzWriteSize(JZDataStream &s, int32_t size)
{
    JZDataStream::ByteOrder saved = s.byteOrder();
    s.setByteOrder(JZDataStream::LittleEndian);
    s << size;
    s.setByteOrder(saved);
}

inline bool jzReadSize(JZDataStream &s, int32_t &size)
{
    JZDataStream::ByteOrder saved = s.byteOrder();
    s.setByteOrder(JZDataStream::LittleEndian);
    s >> size;
    s.setByteOrder(saved);
    return !s.isError();
}

// SFINAE 约束：仅枚举类型生效
template<typename T>
using EnableIfEnum = typename std::enable_if<std::is_enum<T>::value, int>::type;

// 预定义基础类型(仅这些类型允许走批量裸字节路径, 不含自定义结构体/类)。
// 覆盖: bool/char/有符号无符号整型(含定宽)/浮点。枚举由专门操作符处理, 不计入。
template<typename T> struct JZIsFundamental : std::false_type {};
template<> struct JZIsFundamental<bool>            : std::true_type {};
template<> struct JZIsFundamental<int8_t>          : std::true_type {};
template<> struct JZIsFundamental<uint8_t>         : std::true_type {};
template<> struct JZIsFundamental<int16_t>         : std::true_type {};
template<> struct JZIsFundamental<uint16_t>        : std::true_type {};
template<> struct JZIsFundamental<int32_t>         : std::true_type {};
template<> struct JZIsFundamental<uint32_t>        : std::true_type {};
template<> struct JZIsFundamental<int64_t>         : std::true_type {};
template<> struct JZIsFundamental<uint64_t>        : std::true_type {};
template<> struct JZIsFundamental<float>           : std::true_type {};
template<> struct JZIsFundamental<double>          : std::true_type {};

// 对 char* 指向的 size 字节序列原地字节交换(用于 vector 批量换序)。
// 用 memcpy 读写, 不要求地址对齐(规避 reinterpret_cast<T*> 解引用的未对齐 UB),
// 在严格对齐架构(如部分 ARM/嵌入式)上也能安全工作。
// 主模板为 no-op: 仅占位, 保证 C++14 下非基础类型(如 std::string)实例化该分支体时
// 调用点始终可编译(此时 sizeof(T) 非 2/4/8, 实际不会进入该逻辑分支)。
template<size_t N>
inline void jzByteSwapInPlace(char *) { }

template<>
inline void jzByteSwapInPlace<2>(char *e)
{
    char t = e[0]; e[0] = e[1]; e[1] = t;
}

template<>
inline void jzByteSwapInPlace<4>(char *e)
{
    char t;
    t = e[0]; e[0] = e[3]; e[3] = t;
    t = e[1]; e[1] = e[2]; e[2] = t;
}

template<>
inline void jzByteSwapInPlace<8>(char *e)
{
    char t;
    t = e[0]; e[0] = e[7]; e[7] = t;
    t = e[1]; e[1] = e[6]; e[6] = t;
    t = e[2]; e[2] = e[5]; e[5] = t;
    t = e[3]; e[3] = e[4]; e[4] = t;
}

// 枚举写入：自动使用枚举底层原生类型序列化
template<typename T, EnableIfEnum<T> = 0>
JZDataStream& operator<<(JZDataStream& s, T val)
{
    using UnderlyingT = typename std::underlying_type<T>::type;
    UnderlyingT raw = static_cast<UnderlyingT>(val);
    return s << raw;
}

// 枚举读取：按底层类型读出再转回枚举
template<typename T, EnableIfEnum<T> = 0>
JZDataStream& operator>>(JZDataStream& s, T& val)
{
    using UnderlyingT = typename std::underlying_type<T>::type;
    UnderlyingT raw{};
    s >> raw;
    val = static_cast<T>(raw);
    return s;
}

template<class T>
JZDataStream &operator<<(JZDataStream &s, const std::vector<T> &vec)
{
    if (s.isError())
        return s;
    jzWriteSize(s, (int32_t)vec.size());
    // 预定义基础类型:
    //  - 单字节类型 或 流字节序 == 主机序: 直接批量裸写(无需换序);
    //  - 跨端多字节基础类型: 内存流先按本机序批量裸写, 再原地 swapLastWritten 一次换序
    //    (避免逐元素 >> 的函数调用/错误检查开销); 文件流落盘后无法原地换序, 退化为逐元素 <<。
    // 非基础类型(含自定义结构/类)退化为逐元素 << 以走各自序列化逻辑。
    if (JZIsFundamental<T>::value)
    {
        int64_t need = (int64_t)vec.size() * (int64_t)sizeof(T);
        if (sizeof(T) == 1 || s.byteOrder() == JZDataStream::HostOrder)
        {
            // 单字节类型 或 流字节序 == 主机序: 直接批量裸写(无需换序)
            s.writeRawData((const char*)vec.data(), (int)need);
            if (s.isError())
                return s;
        }
        else if (!s.m_file)
        {
            // 跨端多字节基础类型 + 内存流: 先按本机序批量裸写, 再对刚写入区域原地换序
            // (一次遍历, 避免逐元素 << 的函数调用/错误检查开销)
            s.writeRawData((const char*)vec.data(), (int)need);
            if (s.isError())
                return s;   // 写入失败(如超出 maxSize): 不做原地换序, 直接返回
            
            char *base = s.m_buffer->data() + (s.m_offset - (int)need);
            int n = (int)(need / sizeof(T));
            for (int i = 0; i < n; i++)
                jzByteSwapInPlace<sizeof(T)>(base + i * (int)sizeof(T));
        }
        else
        {
            // 跨端多字节基础类型 + 文件流: 落盘后无法原地换序, 退化为逐元素 <<
            for (int i = 0; i < (int)vec.size(); i++)
                s << vec[i];
        }
    }
    else
    {
        for (int i = 0; i < (int)vec.size(); i++)
            s << vec[i];
    }

    return s;
}

// std::vector<bool> 是位压缩特化, 没有连续的 bool 存储, vec.data() 不可用,
// 必须逐元素序列化(单字节, 无字节序问题)。否则走主模板会触发 data() 类型错误。
template<>
inline JZDataStream &operator<<(JZDataStream &s, const std::vector<bool> &vec)
{
    if (s.isError())
        return s;
    jzWriteSize(s, (int32_t)vec.size());
    for (int i = 0; i < (int)vec.size(); i++)
        s << (bool)vec[i];
    return s;
}

template<class T>
JZDataStream &operator>>(JZDataStream &s, std::vector<T> &vec)
{
    if (s.isError())
        return s;
    int32_t size = 0;
    if (!jzReadSize(s, size))
        return s;
    if (size < 0)
    {
        s.setError("JZDataStream: invalid vector size");
        return s;
    }

    // 预定义基础类型一律先批量裸读, 再按需统一换序:
    //  - 多字节类型且流字节序 != 主机序时, 整块读入后对每元素批量 jzByteSwap(比逐元素 >> 少一层调用/校验);
    //  - 单字节类型无字节序问题, 直接批量裸读。
    // 非基础类型(含自定义结构/类)仍退化为逐元素 >> 以走各自的序列化逻辑。
    if (JZIsFundamental<T>::value && sizeof(T) > 1)
    {
        int64_t need = (int64_t)size * (int64_t)sizeof(T);
        if (!s.checkReadCapacity(need))
        {
            s.setError("JZDataStream: vector size exceeds available buffer");
            vec.clear();
            return s;
        }
        vec.resize(size);
        s.readRawData((char*)vec.data(), (int)need);
        if (s.isError())
        {
            vec.clear();
            return s;
        }
        // 流字节序与主机序不同 -> 批量字节交换为本地序
        if (s.byteOrder() != JZDataStream::HostOrder)
        {
            for (int i = 0; i < (int)vec.size(); i++)
                jzByteSwapInPlace<sizeof(T)>((char*)&vec[i]);
        }
    }
    else if (JZIsFundamental<T>::value && sizeof(T) == 1)
    {
        int64_t need = (int64_t)size * (int64_t)sizeof(T);
        if (!s.checkReadCapacity(need))
        {
            s.setError("JZDataStream: vector size exceeds available buffer");
            vec.clear();
            return s;
        }
        vec.resize(size);
        s.readRawData((char*)vec.data(), (int)need);
        if (s.isError())
            vec.clear();
    }
    else
    {
        for (int i = 0; i < size; i++)
        {
            T t;
            s >> t;
            if (s.isError())
            {
                vec.clear();
                break;
            }
            vec.push_back(t);
        }
    }

    return s;
}

// std::vector<bool> 读取特化: 逐元素读出(位压缩特化无连续存储)。
template<>
inline JZDataStream &operator>>(JZDataStream &s, std::vector<bool> &vec)
{
    if (s.isError())
        return s;
    int32_t size = 0;
    if (!jzReadSize(s, size))
        return s;
    if (size < 0)
    {
        s.setError("JZDataStream: invalid vector size");
        return s;
    }
    vec.clear();
    vec.reserve(size);
    for (int i = 0; i < size; i++)
    {
        bool t = false;
        s >> t;
        if (s.isError())
        {
            vec.clear();
            break;
        }
        vec.push_back(t);
    }
    return s;
}


template<class K, class V>
JZDataStream &operator<<(JZDataStream &s, const std::map<K, V> &m)
{
    if (s.isError())
        return s;
    s << (int32_t)m.size();
    auto it = m.begin();
    while (it != m.end())
    {
        s << it->first;
        s << it->second;
        it++;
    }

    return s;
}

template<class K, class V>
JZDataStream &operator >> (JZDataStream &s, std::map<K,V> &vec)
{
    if (s.isError())
        return s;
    int32_t size = 0;
    if (!jzReadSize(s, size))
        return s;
    if (size < 0)
    {
        s.setError("JZDataStream: invalid map size");
        return s;
    }
    // 注: map 不做预分配 resize, 循环内每次读取都会校验容量并在出错时 break,
    // 因此无需像 vector(string/JZByteArray) 那样预先 checkReadCapacity 防止巨量分配。

    for (int i = 0; i < size; i++)
    {
        K k;
        V v;
        s >> k;
        if (s.isError())
            break;
        s >> v;
        if (s.isError())
            break;
        vec[k] = v;
    }

    return s;
}

#endif // !JZ_DATA_STREAM_H_
