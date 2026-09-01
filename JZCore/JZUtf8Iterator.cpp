#include "JZUtf8Iterator.h"

JZUtf8Iterator::JZUtf8Iterator(const std::string& str)
    : m_data(str.data())
    , m_size(str.size())
    , m_pos(0)
{
}

bool JZUtf8Iterator::isValid() const
{
    return m_pos < m_size;
}

JZUtf8Iterator::operator bool() const
{
    return isValid();
}

char32_t JZUtf8Iterator::codepoint() const
{
    char32_t cp;
    int len;
    decode(m_data, m_size, m_pos, cp, len);
    return cp;
}

int JZUtf8Iterator::byteIndex() const
{
    return static_cast<int>(m_pos);
}

int JZUtf8Iterator::byteLength() const
{
    char32_t cp;
    int len;
    decode(m_data, m_size, m_pos, cp, len);
    return len;
}

int JZUtf8Iterator::codepointCount() const
{
    return m_count;
}

JZUtf8Iterator& JZUtf8Iterator::operator++()
{
    char32_t cp;
    int len;
    if (decode(m_data, m_size, m_pos, cp, len))
        m_pos += static_cast<size_t>(len);
    else
        m_pos = m_size; // 遇到非法序列，跳到末尾避免死循环
    ++m_count;
    return *this;
}

void JZUtf8Iterator::seek(int byteOffset)
{
    m_pos = (byteOffset < 0) ? 0
        : (static_cast<size_t>(byteOffset) > m_size ? m_size : static_cast<size_t>(byteOffset));
}

int JZUtf8Iterator::size() const
{
    return static_cast<int>(m_size);
}

bool JZUtf8Iterator::decode(const char* data, size_t size, size_t pos, char32_t& cp, int& len)
{
    if (pos >= size)
    {
        cp = 0xFFFD;
        len = 1;
        return false;
    }

    unsigned char c = static_cast<unsigned char>(data[pos]);
    if (c < 0x80)
    {
        cp = c;
        len = 1;
        return true;
    }
    else if ((c & 0xE0) == 0xC0)            // 110xxxxx -> 2 字节
    {
        if (pos + 1 >= size) { cp = 0xFFFD; len = 1; return false; }
        unsigned char c1 = static_cast<unsigned char>(data[pos + 1]);
        if ((c1 & 0xC0) != 0x80) { cp = 0xFFFD; len = 1; return false; }
        cp = ((char32_t)(c & 0x1F) << 6) | (c1 & 0x3F);
        len = 2;
        return true;
    }
    else if ((c & 0xF0) == 0xE0)            // 1110xxxx -> 3 字节
    {
        if (pos + 2 >= size) { cp = 0xFFFD; len = 1; return false; }
        unsigned char c1 = static_cast<unsigned char>(data[pos + 1]);
        unsigned char c2 = static_cast<unsigned char>(data[pos + 2]);
        if ((c1 & 0xC0) != 0x80 || (c2 & 0xC0) != 0x80) { cp = 0xFFFD; len = 1; return false; }
        cp = ((char32_t)(c & 0x0F) << 12) | ((char32_t)(c1 & 0x3F) << 6) | (c2 & 0x3F);
        len = 3;
        return true;
    }
    else if ((c & 0xF8) == 0xF0)            // 11110xxx -> 4 字节
    {
        if (pos + 3 >= size) { cp = 0xFFFD; len = 1; return false; }
        unsigned char c1 = static_cast<unsigned char>(data[pos + 1]);
        unsigned char c2 = static_cast<unsigned char>(data[pos + 2]);
        unsigned char c3 = static_cast<unsigned char>(data[pos + 3]);
        if ((c1 & 0xC0) != 0x80 || (c2 & 0xC0) != 0x80 || (c3 & 0xC0) != 0x80) { cp = 0xFFFD; len = 1; return false; }
        cp = ((char32_t)(c & 0x07) << 18) | ((char32_t)(c1 & 0x3F) << 12)
           | ((char32_t)(c2 & 0x3F) << 6) | (c3 & 0x3F);
        len = 4;
        return true;
    }

    // 非法首字节 (0x80~0xBF 续字节 / 0xF8~0xFF)
    cp = 0xFFFD;
    len = 1;
    return false;
}

void JZUtf8Iterator::encode(char32_t cp, std::string& out)
{
    if (cp < 0x80)
    {
        out.push_back(static_cast<char>(cp));
    }
    else if (cp < 0x800)
    {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    else if (cp < 0x10000)
    {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    else
    {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

int JZUtf8Iterator::countCodepoints(const std::string& str)
{
    int n = 0;
    JZUtf8Iterator it(str);
    while (it.isValid())
    {
        ++n;
        ++it;
    }
    return n;
}
