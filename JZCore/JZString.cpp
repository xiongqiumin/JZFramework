#include <stdexcept>
#include <cstddef>
#include <sstream>
#include <algorithm>
#include <cctype>
#include "JZString.h"
#include "JZDebug.h"
#include "JZStd.h"
#include "JZUtf8Iterator.h"

JZChar::JZChar()
{
    data = 0;
}

JZChar::JZChar(char c)
{
    data = c;
}

JZChar::JZChar(char32_t ucs)
{
    data = ucs;
}

char JZChar::toChar() const
{
    if (!isAscii())
    {
        jzWarning() << data << "is not ascii";
        return '\0';
    }

    return static_cast<char>(data);
}

bool JZChar::isUpper() const
{
    if (!isAscii())
        return false;

    return ::isupper(toChar());
}

bool JZChar::isLower() const
{
    if (!isAscii())
        return false;

    return ::islower(toChar());
}

JZChar JZChar::toUpper() const
{
    if (isAscii())
    {
        unsigned char c = static_cast<unsigned char>(data);
        return ::toupper(c);
    }
    return *this;
}

JZChar JZChar::toLower() const
{
    if (isAscii())
    {
        unsigned char c = static_cast<unsigned char>(data);
        return ::tolower(c);
    }
    return *this;
}

char32_t JZChar::toUnicode() const
{
    return data;
}

bool JZChar::isAscii() const
{
    return data < 0x80;  // 0~127 = ASCII
}

bool JZChar::isDigit() const
{
    if (isAscii())
        return ::isdigit(static_cast<char>(data));
    else
        return false;
}

bool JZChar::isSpace() const
{
    if (isAscii())
        return ::isspace(static_cast<char>(data));
    else
        return false;
}

bool JZChar::operator==(const JZChar& other) const
{
    return data == other.data;
}

bool JZChar::operator!=(const JZChar& other) const
{
    return data != other.data;
}

bool JZChar::operator<(const JZChar& other) const {
    return data < other.data;
}

bool JZChar::operator<=(const JZChar& other) const { return *this < other || *this == other; }

bool JZChar::operator>(const JZChar& other) const { return !(*this <= other); }

bool JZChar::operator>=(const JZChar& other) const { return !(*this < other); }

JZDebug operator<<(JZDebug d, const JZChar& c)
{ 
    std::string raw = JZString(c).toStdString();
    std::string esc = raw;
    const std::string str_null("\0", 1);

    if (raw == str_null)
        esc = "\\0";
    else if (raw == "'")
        esc = "\\'";
    else if (raw == "\\")
        esc = "\\\\";
    else if (raw == "\n")
        esc = "\\n";
    else if (raw == "\t")
        esc = "\\t";
    else if (raw == "\r")
        esc = "\\r";
    else if (raw == "\b")
        esc = "\\b";
    else if (raw == "\f")
        esc = "\\f";

    std::string str = "'" + esc + "'";
    d << str.c_str();
    return d;
}

// JZString
JZString JZString::fromStdString(const std::string& stdStr)
{
    JZString jzStr;
    auto& dest = jzStr.m_chars;
    size_t len = stdStr.size();
    size_t idx = 0;

    dest.clear();
    while (idx < len)
    {
        char32_t cp = 0;
        int byteCnt = 0;

        // 解码统一走 JZUtf8Iterator (非法首字节/续字节/截断均返回 false)
        if (!JZUtf8Iterator::decode(stdStr.data(), len, idx, cp, byteCnt))
        {
            jzWarning() << "Invalid UTF-8 encoding: bad byte sequence at index " << idx;
            return JZString();
        }

        // 校验超长编码、代理对、合法Unicode范围
        bool invalid = false;
        switch (byteCnt)
        {
        case 2:
            if (cp < 0x80) invalid = true;
            break;
        case 3:
            if (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF)) invalid = true;
            break;
        case 4:
            if (cp < 0x10000 || cp > 0x10FFFF) invalid = true;
            break;
        }
        if (invalid)
        {
            jzWarning() << "Invalid UTF-8 encoding: overlong or surrogate code point at index " << idx;
            return JZString();
        }

        dest.push_back(JZChar(cp));
        idx += static_cast<size_t>(byteCnt);
    }

    // 保证最后一个是 0
    dest.push_back(JZChar());
    return jzStr;
}

JZString JZString::fromU32String(const std::u32string& stdStr)
{
    JZString jzStr;
    jzStr.m_chars.clear();
    for (char32_t c : stdStr)
        jzStr.m_chars.push_back(JZChar(c));

    // 保证最后一个是 0
    jzStr.m_chars.push_back({});
    return jzStr;
}

JZString JZString::number(int n)
{
    std::ostringstream oss;
    oss << n;
    return fromStdString(oss.str());
}

JZString JZString::number(double n, char format, int precision)
{
    std::ostringstream oss;
    oss.precision(precision);

    switch (format)
    {
        case 'f':
            oss << std::fixed << n;
            break;
        case 'e':
            oss << std::scientific << n;
            break;
        case 'g':
        default:
            oss << n;
            break;
    }

    return fromStdString(oss.str());
}

JZString::JZString()
{
    // 默认构造就带一个 0 结尾
    m_chars.push_back({});
}

JZString::JZString(const char* str)
{
    if (str)
    {
        *this = fromStdString(std::string(str));
    }
    else
    {
        m_chars.push_back({});
    }
}

JZString::JZString(JZChar c)
{
    m_chars.push_back(c);
    m_chars.push_back({});  // 保证 0 结尾
}

bool JZString::operator==(const JZString& other) const
{
    // 比较时排除最后一个 0
    auto mySize = m_chars.empty() ? 0 : m_chars.size() - 1;
    auto otherSize = other.m_chars.empty() ? 0 : other.m_chars.size() - 1;

    if (mySize != otherSize)
        return false;

    for (std::size_t i = 0; i < mySize; ++i)
    {
        if (m_chars[i] != other.m_chars[i])
            return false;
    }
    return true;
}

bool JZString::operator!=(const JZString& other) const
{
    return !(*this == other);
}

bool JZString::operator<(const JZString& other) const
{
    std::size_t mySize = m_chars.empty() ? 0 : m_chars.size() - 1;
    std::size_t otherSize = other.m_chars.empty() ? 0 : other.m_chars.size() - 1;

    std::size_t minSize = std::min(mySize, otherSize);

    for (std::size_t i = 0; i < minSize; ++i)
    {
        if (m_chars[i] < other.m_chars[i])
        {
            return true;
        }
        if (m_chars[i] > other.m_chars[i])
        {
            return false;
        }
    }

    return mySize < otherSize;
}

bool JZString::operator<=(const JZString& other) const
{
    return *this < other || *this == other;
}

bool JZString::operator>(const JZString& other) const
{
    return !(*this <= other);
}

bool JZString::operator>=(const JZString& other) const
{
    return !(*this < other);
}

JZString& JZString::operator+=(const JZString& other)
{
    if (m_chars.empty())
        m_chars.push_back({});

    // 先移除自己的 0
    m_chars.pop_back();

    // 追加内容（不含对方的 0）
    auto otherSize = other.m_chars.empty() ? 0 : other.m_chars.size() - 1;
    m_chars.insert(m_chars.end(), other.m_chars.begin(), other.m_chars.begin() + otherSize);

    // 重新加 0
    m_chars.push_back({});
    return *this;
}

JZString& JZString::operator+=(const char* str)
{
    if (str)
    {
        *this += fromStdString(std::string(str));
    }
    return *this;
}

JZString JZString::operator+(const JZString& other) const
{
    JZString result(*this);
    result += other;
    return result;
}

JZString JZString::operator+(const char* str) const
{
    JZString result(*this);
    result += str;
    return result;
}

std::string JZString::toStdString() const
{
    std::string stdStr;

    // 输出时排除最后一个 0
    auto realSize = m_chars.empty() ? 0 : m_chars.size() - 1;
    for (std::size_t i = 0; i < realSize; ++i)
    {
        char32_t cp = m_chars[i].data;

        if (cp > 0x10FFFF)
        {
            // 防御性处理：正常情况下不会进入，因为 fromStdString 已保证码点合法
            jzWarning().nospace() << "JZString::toStdString: invalid code point "
                        << str_asprintf("%x",(uint32_t)cp).c_str() << " at index" << i
                        << ", replacing with U+FFFD";

            // 输出替换字符 U+FFFD 的 UTF-8 编码：EF BF BD
            stdStr += "\xEF\xBF\xBD";
            continue;
        }

        JZUtf8Iterator::encode(cp, stdStr);
    }

    return stdStr;
}

std::u32string JZString::toU32String() const
{
    std::u32string stdStr;

    auto realSize = m_chars.empty() ? 0 : m_chars.size() - 1;
    for (std::size_t i = 0; i < realSize; ++i)
    {
        stdStr.push_back(m_chars[i].toUnicode());
    }

    return stdStr;
}

int JZString::toInt(bool* ok) const
{
    std::string str = toStdString();
    return str_toInt(str, ok);
}

double JZString::toDouble(bool* ok) const
{
    std::string str = toStdString();
    return str_toDouble(str, ok);
}

void JZString::resize(int size, const JZChar& c)
{
    if (size < 0)
        return;

    std::size_t newSize = static_cast<std::size_t>(size);

    // 先移除 0
    if (!m_chars.empty())
        m_chars.pop_back();

    if (newSize > m_chars.size())
    {
        m_chars.resize(newSize, c);
    }
    else if (newSize < m_chars.size())
    {
        m_chars.resize(newSize);
    }

    // 重新加 0
    m_chars.push_back({});
}

int JZString::size() const
{
    // 真实长度 = 总大小 - 1
    return static_cast<int>(m_chars.empty() ? 0 : m_chars.size() - 1);
}

int JZString::length() const
{
    return size();
}

int JZString::count() const
{
    return size();
}

bool JZString::isEmpty() const
{
    return size() == 0;
}

void JZString::clear()
{
    m_chars.clear();
    m_chars.push_back({});  // 清空后保留 0
}

JZChar& JZString::operator[](int index)
{
    return m_chars[index];
}

const JZChar& JZString::operator[](int index) const
{
    return m_chars[index];
}

const JZChar& JZString::at(int index) const
{
    return m_chars.at(index);
}

JZChar* JZString::data()
{
    return m_chars.data();
}

const JZChar* JZString::data() const
{
    return m_chars.data();
}

// 普通迭代器
std::vector<JZChar>::iterator JZString::begin()
{
    return m_chars.begin();
}

std::vector<JZChar>::iterator JZString::end()
{
    // 迭代器不包含最后的 0
    return m_chars.end() - 1;
}

// const 迭代器（只读）
std::vector<JZChar>::const_iterator JZString::begin() const
{
    return m_chars.begin();
}

std::vector<JZChar>::const_iterator JZString::end() const
{
    return m_chars.end() - 1;
}

void JZString::insert(int position, const JZString& str)
{
    if (position < 0 || position > size())
    {
        return;
    }

    // 移除 0
    m_chars.pop_back();

    // 插入（不含对方的 0）
    auto strSize = str.m_chars.empty() ? 0 : str.m_chars.size() - 1;
    m_chars.insert(m_chars.begin() + position, str.m_chars.begin(), str.m_chars.begin() + strSize);

    // 恢复 0
    m_chars.push_back({});
}

void JZString::remove(int position, int length)
{
    if (position < 0 || length <= 0)
    {
        return;
    }

    std::size_t startPos = static_cast<std::size_t>(position);
    std::size_t removeLength = static_cast<std::size_t>(length);
    int realSize = size();

    if (startPos >= static_cast<std::size_t>(realSize))
    {
        return;
    }

    if (startPos + removeLength > static_cast<std::size_t>(realSize))
    {
        removeLength = static_cast<std::size_t>(realSize) - startPos;
    }

    // 移除 0
    m_chars.pop_back();

    // 删除
    m_chars.erase(m_chars.begin() + startPos, m_chars.begin() + startPos + removeLength);

    // 恢复 0
    m_chars.push_back({});
}

void JZString::replace(int position, int length, const JZString& after)
{
    if (position < 0 || length < 0)
    {
        return;
    }

    remove(position, length);
    insert(position, after);
}

void JZString::replace(const JZString& before, const JZString& after)
{
    int pos = indexOf(before);
    while (pos >= 0)
    {
        replace(pos, before.size(), after);
        pos = indexOf(before, pos + after.size());
    }
}

int JZString::indexOf(const JZString& str, int from) const
{
    if (str.isEmpty() || from < 0)
    {
        return -1;
    }

    int srcLen = size();
    int subLen = str.size();

    int startPos = from;
    if (startPos >= srcLen)
    {
        return -1;
    }

    if (subLen > srcLen)
    {
        return -1;
    }

    int maxI = srcLen - subLen;
    for (int i = startPos; i <= maxI; ++i)
    {
        bool match = true;
        for (int j = 0; j < subLen; ++j)
        {
            if (m_chars[i + j] != str.m_chars[j])
            {
                match = false;
                break;
            }
        }
        if (match)
        {
            return i;
        }
    }

    return -1;
}

int JZString::lastIndexOf(const JZString& str, int from) const
{
    if (str.isEmpty())
    {
        return -1;
    }

    std::size_t strSize = str.size();
    std::size_t thisSize = size();
    if (strSize > thisSize)
    {
        return -1;
    }

    int searchStart = from;
    if (searchStart < 0 || static_cast<std::size_t>(searchStart) >= thisSize)
    {
        searchStart = static_cast<int>(thisSize - 1);
    }

    int minStart = static_cast<int>(thisSize - strSize);
    if (minStart < 0)
    {
        return -1;
    }

    searchStart = std::min(searchStart, minStart);
    for (int i = searchStart; i >= 0; --i)
    {
        bool match = true;
        for (std::size_t j = 0; j < strSize; ++j)
        {
            if (static_cast<std::size_t>(i + j) >= thisSize || m_chars[i + j] != str.m_chars[j])
            {
                match = false;
                break;
            }
        }
        if (match)
        {
            return i;
        }
    }

    return -1;
}

bool JZString::contains(const JZString& str) const
{
    return indexOf(str) >= 0;
}

bool JZString::startsWith(const JZString& str) const
{
    if (str.size() > size())
    {
        return false;
    }

    for (std::size_t i = 0; i < static_cast<std::size_t>(str.size()); ++i)
    {
        if (m_chars[i] != str.m_chars[i])
        {
            return false;
        }
    }

    return true;
}

bool JZString::endsWith(const JZString& str) const
{
    int strSize = str.size();
    int thisSize = size();

    if (strSize > thisSize)
    {
        return false;
    }

    std::size_t offset = static_cast<std::size_t>(thisSize - strSize);
    for (int i = 0; i < strSize; ++i)
    {
        if (m_chars[offset + i] != str.m_chars[i])
        {
            return false;
        }
    }

    return true;
}

int JZString::indexOf(JZChar c, int from) const
{
    std::size_t len = size();
    if (len == 0)
        return -1;

    int start = from;
    if (start < 0)
        start = 0;
    std::size_t startIdx = static_cast<std::size_t>(start);
    if (startIdx >= len)
        return -1;

    for (std::size_t i = startIdx; i < len; ++i)
    {
        if (m_chars[i] == c)
        {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int JZString::lastIndexOf(JZChar c, int from) const
{
    std::size_t len = size();
    if (len == 0)
        return -1;

    int start = from;
    if (start < 0 || static_cast<std::size_t>(start) >= len)
    {
        start = static_cast<int>(len - 1);
    }

    for (int i = start; i >= 0; --i)
    {
        if (m_chars[static_cast<std::size_t>(i)] == c)
        {
            return i;
        }
    }
    return -1;
}

const JZChar& JZString::front() const
{
    return m_chars.front();
}

const JZChar& JZString::back() const
{
    // 有效最后一个字符，不是 0
    return m_chars[size() - 1];
}

void JZString::push_back(const JZChar& other)
{
    if (m_chars.empty())
    {
        m_chars.push_back(other);
        m_chars.push_back({});
        return;
    }

    // 移除 0
    m_chars.pop_back();
    m_chars.push_back(other);
    // 恢复 0
    m_chars.push_back({});
}

void JZString::pop_back()
{
    if (isEmpty())
        return;

    // 移除 0
    m_chars.pop_back();
    // 移除最后一个有效字符
    m_chars.pop_back();
    // 恢复 0
    m_chars.push_back({});
}

void JZString::append(const JZString& other)
{
    *this += other;
}

void JZString::chop(int n)
{
    if (n <= 0)
    {
        return;
    }
    int realSize = size();
    size_t remove_count = static_cast<size_t>(n);

    if (remove_count >= static_cast<size_t>(realSize))
    {
        clear();
        return;
    }

    // 移除 0
    m_chars.pop_back();
    // 删除尾部
    m_chars.erase(m_chars.end() - remove_count, m_chars.end());
    // 恢复 0
    m_chars.push_back({});
}

JZString JZString::mid(int position, int length) const
{
    int realSize = size();
    
    // 处理负数的起始位置
    if (position < 0)
    {
        position = 0;
    }
    
    // 起始位置超出范围，返回空字符串
    if (position >= realSize)
    {
        return JZString();
    }
    
    // 计算实际可用的长度
    int maxAvailable = realSize - position;
    int actualLength = length;
    
    // 负数长度表示取到末尾
    if (actualLength < 0 || actualLength > maxAvailable)
    {
        actualLength = maxAvailable;
    }
    
    // 构建结果字符串
    JZString result;
    result.m_chars.clear();
    result.m_chars.insert(result.m_chars.end(),
                          m_chars.begin() + position,
                          m_chars.begin() + position + actualLength);
    result.m_chars.push_back({});  // 添加结尾的 null 字符
    
    return result;
}

JZString JZString::left(int length) const
{
    if (length < 0)
    {
        return JZString();
    }

    return mid(0, length);
}

JZString JZString::right(int length) const
{
    if (length < 0)
    {
        return JZString();
    }

    int position = size() - length;
    if (position < 0)
    {
        position = 0;
    }

    return mid(position);
}

JZString JZString::toUpper() const
{
    JZString result;
    result.m_chars = m_chars;

    int realSize = size();
    for (int i = 0; i < realSize; ++i)
    {
        if (result.m_chars[i].isAscii())
            result.m_chars[i] = result.m_chars[i].toUpper();
    }

    return result;
}

JZString JZString::toLower() const
{
    JZString result;
    result.m_chars = m_chars;

    int realSize = size();
    for (int i = 0; i < realSize; ++i)
    {
        if (result.m_chars[i].isAscii())
            result.m_chars[i] = result.m_chars[i].toLower();
    }

    return result;
}

JZString JZString::trimmed() const
{
    if (isEmpty())
    {
        return JZString();
    }
    
    int start = 0;
    int end = size() - 1;
    
    // 从左向右跳过空白字符
    while (start <= end)
    {
        const JZChar& ch = m_chars[start];
        if (ch.isSpace())
        {
            ++start;
        }
        else
        {
            break;
        }
    }
    
    // 如果全是空白字符
    if (start > end)
    {
        return JZString();
    }
    
    // 从右向左跳过空白字符
    while (end >= start)
    {
        const JZChar& ch = m_chars[end];
        if (ch.isSpace())
        {
            --end;
        }
        else
        {
            break;
        }
    }
    
    // 直接提取子串，避免多次修改原字符串
    return mid(start, end - start + 1);
}

JZString JZString::simplified() const
{
    JZString res;
    auto& dest = res.m_chars;
    dest.clear();

    bool lastWasSpace = true;
    for (const JZChar& ch : this->m_chars)
    {
        if (ch.isSpace())
        {
            if (!lastWasSpace)
            {
                dest.push_back(ch);
                lastWasSpace = true;
            }
        }
        else
        {
            dest.push_back(ch);
            lastWasSpace = false;
        }
    }

    // 移除末尾多余空格
    if (!dest.empty() && dest.back().isSpace())
    {
        dest.pop_back();
    }

    // 尾部追加字符串结束符
    dest.push_back(JZChar());
    return res;
}

std::vector<JZString> JZString::split(const JZString& sep, JZ::SplitBehavior behavior) const
{
    std::string str = toStdString();
    auto list = str_split(str, sep.toStdString(), behavior);
    std::vector<JZString> ret;
    for (auto& n : list)
        ret.push_back(JZString::fromStdString(n));
    return ret;
}

JZString& JZString::fill(const JZChar& c)
{
    int realSize = size();
    for (int i = 0; i < realSize; ++i)
    {
        auto& jzChar = m_chars[i];
        for (int i = 0; i < 4; ++i)
        {
            jzChar.data = c.data;
        }
    }
    return *this;
}

JZString operator+(const char* str1, const JZString& str2)
{
    return JZString(str1) + str2;
}

JZDebug operator<<(JZDebug d, const JZString& str)
{
    d << str.toStdString();
    return d;
}

//
JZString jzstr_join(const std::vector<JZString>& list, const JZString& sep)
{
    if (list.empty())
        return "";

    JZString result;

    // 拼接第一个字符串
    result += list[0];

    // 拼接剩余的字符串和分隔符
    for (size_t i = 1; i < list.size(); ++i)
    {
        result += sep;
        result += list[i];
    }

    return result;
}