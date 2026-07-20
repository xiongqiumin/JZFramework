#include <string.h>
#include "JZByteArray.h"
#include "JZDebug.h"
#include "libbase64.h"

JZByteArray JZByteArray::fromBase64(const JZByteArray &base64_in) {
    const JZByteVector &base64 = base64_in.m_vec;
    const size_t len = base64.size();

    if (len == 0)
        return JZByteArray();

    // 预分配输出上限 (4 字符 -> 最多 3 字节), 解码后按实际长度收缩。
    // 使用 libbase64 进行解析, 非法输入返回空。
    JZByteArray out;
    out.m_vec.resize((len / 4) * 3 + 2);

    size_t outlen = 0;
    int ret = base64_decode(base64.data(), len, out.m_vec.data(), &outlen, 0);
    if (ret != 1) {
        jzError() << "Base64 decode failed: invalid input";
        return JZByteArray();
    }
    out.m_vec.resize(outlen);
    return out;
}

JZByteArray::JZByteArray()
{
}

JZByteArray::JZByteArray(const char *buffer, int len)
    :m_vec(buffer, buffer + len)
{    
}

JZByteArray::~JZByteArray()
{

}

JZByteArray JZByteArray::toHex() const
{
    JZByteArray ret;
    const char hex[] = "0123456789abcdef";
    JZByteVector &vec = ret.m_vec;
    vec.resize(m_vec.size() * 2);

    for (size_t i = 0; i < m_vec.size(); i++)
    {
        uint8_t byte = static_cast<uint8_t>(m_vec[i]);
        vec[i * 2] = hex[byte >> 4];
        vec[i * 2 + 1] = hex[byte & 0x0F];
    }
    return ret;
}

JZByteArray JZByteArray::toBase64() const
{
    JZByteArray out;

    const JZByteVector &buffer = this->m_vec;
    size_t len = buffer.size();
    if (len == 0)
        return out;

    // base64 编码长度 = ceil(len/3)*4
    size_t encoded_len = ((len + 2) / 3) * 4;
    out.m_vec.resize(encoded_len);

    size_t outlen = 0;
    base64_encode(buffer.data(), len, out.m_vec.data(), &outlen, 0);
    out.m_vec.resize(outlen);
    return out;
}

void JZByteArray::swap(JZByteArray& other)
{
    m_vec.swap(other.m_vec);
}

bool JZByteArray::isEmpty() const
{
    return (m_vec.size() == 0);
}

void JZByteArray::resize(int size)
{
    m_vec.resize(size);
}

void JZByteArray::resize(int size, char value)
{
    m_vec.resize(size,value);
}

void JZByteArray::fill(char value)
{
    std::fill(m_vec.begin(), m_vec.end(), value);
}

void JZByteArray::reserve(int size)
{
    m_vec.reserve(size);
}

int JZByteArray::size() const
{
    return (int)m_vec.size();
}

char *JZByteArray::data()
{
    return m_vec.data();
}

const char *JZByteArray::data() const
{
    return m_vec.data();
}

const char *JZByteArray::constData() const
{
    return m_vec.data();
}

char *JZByteArray::begin()
{
    return m_vec.data();
}

char *JZByteArray::end()
{
    return m_vec.data() + m_vec.size();
}

const char *JZByteArray::begin() const
{
    return m_vec.data();
}

const char *JZByteArray::end() const
{
    return m_vec.data() + m_vec.size();
}

void JZByteArray::push_back(char ch)
{
    m_vec.push_back(ch);
}

void JZByteArray::append(const char *buffer, int len)
{
    int n = size();
    resize(n + len);
    memcpy(m_vec.data() + n, buffer, len);
}

void JZByteArray::append(const JZByteArray &buffer)
{
    return append(buffer.data(), buffer.size());
}

JZByteArray JZByteArray::left(int size) const
{
    return mid(0, size);
}

JZByteArray JZByteArray::mid(int start, int len) const
{
    if (len == -1)
        len = size() - start;
    if (start < 0 || start > size())
        return JZByteArray();
    if (start + len > size())
        len = size() - start;

    JZByteArray ret;
    ret.m_vec.resize(len);
    memcpy(ret.m_vec.data(), m_vec.data() + start, len);
    return ret;
}

void JZByteArray::clear()
{
    m_vec.clear();
}

JZByteArray str_toUtf8(const std::string &str)
{
    return JZByteArray(str.data(), (int)str.size());
}

std::string str_fromUtf8(const JZByteArray &buffer)
{
    return std::string(buffer.data(), buffer.size());
}

JZByteArray str_toLocal(const std::string &str)
{
    std::string local_str = str_utf8_to_local(str);
    return JZByteArray(local_str.data(), (int)local_str.size());
}

std::string str_fromLocal(const JZByteArray &buffer)
{
    std::string local_str(buffer.data(), buffer.size());
    return str_local_to_utf8(local_str);
}

JZDebug operator<<(JZDebug d, const JZByteArray &bytes)
{
    const char hex[] = "0123456789abcdef";
    const int MAX_PRINT = 30;  // 最多打印30个字节

    std::string line;
    line = "[";

    if (bytes.size() <= MAX_PRINT)
    {
        // 小于等于30个字节，全部打印
        for (int i = 0; i < bytes.size(); ++i)
        {
            unsigned char c = static_cast<unsigned char>(bytes[i]);
            line += "0x";
            line += hex[c >> 4];
            line += hex[c & 0x0F];
            if (i < bytes.size() - 1)
                line += " ";
        }
    }
    else
    {
        // 大于30个字节，打印前15个和后15个
        int half = MAX_PRINT / 2;  // 15

        // 打印前15个
        for (int i = 0; i < half; ++i)
        {
            unsigned char c = static_cast<unsigned char>(bytes[i]);
            line += hex[c >> 4];
            line += hex[c & 0x0F];
            line += ", ";
        }

        // 打印省略号
        line += "... ";

        // 打印后15个
        for (int i = bytes.size() - half; i < bytes.size(); ++i)
        {
            unsigned char c = static_cast<unsigned char>(bytes[i]);
            line += hex[c >> 4];
            line += hex[c & 0x0F];
            if (i < bytes.size() - 1)
                line += ", ";
        }
    }
    line += "]";
    d << line.c_str();

    return d;
}