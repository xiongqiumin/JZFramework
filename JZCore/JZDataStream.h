#ifndef JZ_DATA_STREAM_H_
#define JZ_DATA_STREAM_H_

#include <string>
#include <stdint.h>
#include <type_traits>
#include <map>
#include "JZByteArray.h"
#include "JZFile.h"

class JZDataStreamImpl;
class JZDataStream
{
public:
    enum ByteOrder
    {
        LittleEndian,
        BigEndian,
    };

    JZDataStream(JZFile *file);
    JZDataStream(const JZByteArray *buffer);
    JZDataStream(JZByteArray *buffer, JZ::OpenMode mode);    
    ~JZDataStream();

    int writeRawData(const char *s, int len);
    int readRawData(char *s, int len);
    void skipRawData(int offset);

    void setByteOrder(ByteOrder order);
    ByteOrder byteOrder();

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
    }

    int m_offset;
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

// SFINAE 约束：仅枚举类型生效
template<typename T>
using EnableIfEnum = typename std::enable_if<std::is_enum<T>::value, int>::type;

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
    s << (int32_t)vec.size();
    if (std::is_trivially_copyable<T>::value)
    {
        s.writeRawData((const char*)vec.data(),int(vec.size() * sizeof(T)));
    }
    else
    {
        for (int i = 0; i < (int)vec.size(); i++)
            s << vec[i];
    }

    return s;
}

template<class T>
JZDataStream &operator>>(JZDataStream &s, std::vector<T> &vec)
{
    if (s.isError())
        return s;
    int32_t size = 0;
    s >> size;
    if (s.isError())
        return s;
    if (size < 0)
    {
        s.setError("JZDataStream: invalid vector size");
        return s;
    }

    if (std::is_trivially_copyable<T>::value)
    {
        int64_t need = (int64_t)size * (int64_t)sizeof(T);
        if (!s.checkReadCapacity(need))
        {
            s.setError("JZDataStream: vector size exceeds available buffer");
            return s;
        }
        vec.resize(size);
        s.readRawData((char*)vec.data(),(int)vec.size() * (int)sizeof(T));
    }
    else
    {
        for (int i = 0; i < size; i++)
        {
            T t;
            s >> t;
            if (s.isError())
                break;
            vec.push_back(t);
        }
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
    s >> size;
    if (s.isError())
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
