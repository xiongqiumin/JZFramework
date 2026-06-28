#include <string.h>
#include "JZByteArray.h"
#include "JZDebug.h"

static const char base64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz"
    "0123456789+/";


static int base64_index(char c) {
    if (c == '=') return 0;
    const char* pos = strchr(base64_table, c);
    return pos ? int(pos - base64_table) : -1;
}

JZByteArray JZByteArray::fromBase64(const JZByteArray &base64_in) {
    JZByteArray out;
    const std::vector<char> &base64 = base64_in.m_vec;
    size_t len = base64.size();
    if (len == 0)
        return JZByteArray();

    if (len % 4 != 0)
    {
        jzError() << "Base64 decode failed: invalid length (" << len << "), must be multiple of 4";
        return JZByteArray();
    }

    // 步骤2：计算总 padding 数（仅最后一个 4 字节块可能有 padding）
    size_t padding = 0;
    if (len >= 1 && base64[len - 1] == '=') {
        padding++;
    }
    if (len >= 2 && base64[len - 2] == '=') {
        padding++;
    }
    // 校验 padding 合法性（最多 2 个 =）
    if (padding > 2) {
        jzError() << "Base64 decode failed: invalid padding (" << padding << "), max 2";
        return JZByteArray();
    }    

    std::vector<char> &result = out.m_vec;    
    // 步骤3：逐 4 字节块解码
    for (size_t i = 0; i < len; i += 4) {        
        // 提取 4 个 Base64 字符的索引
        int idx1 = base64_index(base64[i]);
        int idx2 = base64_index(base64[i + 1]);
        int idx3 = base64_index(base64[i + 2]);
        int idx4 = base64_index(base64[i + 3]);

        // 运行时校验：非法字符（替代断言）
        if (idx1 == -1 || idx2 == -1) {
            jzError() << "Base64 decode failed: invalid character at pos " << i << " / " << i + 1;
            return JZByteArray();
        }
        // padding 位置的字符允许为 =（idx3/idx4 可能为 -1）
        if ((base64[i + 2] != '=' && idx3 == -1) ||
            (base64[i + 3] != '=' && idx4 == -1)) {
            jzError() << "Base64 decode failed: invalid non-padding character at block " << (i / 4);
            return JZByteArray();
        }

        // 处理 padding：= 对应的索引视为 0
        if (idx3 == -1) idx3 = 0;
        if (idx4 == -1) idx4 = 0;

        // 4 个 6 位 Base64 数拼接为 24 位（3 字节）
        uint32_t combined = (static_cast<uint32_t>(idx1) << 18) |
            (static_cast<uint32_t>(idx2) << 12) |
            (static_cast<uint32_t>(idx3) << 6) |
            static_cast<uint32_t>(idx4);

        // 提取字节（根据 padding 跳过末尾字节）
        result.push_back(static_cast<char>((combined >> 16) & 0xFF));
        // 仅当不是最后一个块，或 padding < 2 时，提取第二个字节
        if (i + 4 >= len ? (padding < 2) : true) {
            result.push_back(static_cast<char>((combined >> 8) & 0xFF));
        }
        // 仅当不是最后一个块，或 padding < 1 时，提取第三个字节
        if (i + 4 >= len ? (padding < 1) : true) {
            result.push_back(static_cast<char>(combined & 0xFF));
        }
    }

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
    std::vector<char> &vec = ret.m_vec;
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

    const std::vector<char> &buffer = this->m_vec;
    std::vector<char> &result = out.m_vec;
    size_t len = buffer.size();
    if (len == 0)
        return out;

    size_t padding = (3 - (len % 3)) % 3;
    size_t encoded_len = ((len + padding) / 3) * 4;
    result.reserve(encoded_len); 

    for (size_t i = 0; i < len; i += 3) {
        uint32_t byte1 = static_cast<uint8_t>(buffer[i]);
        uint32_t byte2 = (i + 1 < len) ? static_cast<uint8_t>(buffer[i + 1]) : 0;
        uint32_t byte3 = (i + 2 < len) ? static_cast<uint8_t>(buffer[i + 2]) : 0;

        uint32_t combined = (byte1 << 16) | (byte2 << 8) | byte3;
        int idx1 = (combined >> 18) & 0x3F;
        int idx2 = (combined >> 12) & 0x3F;
        int idx3 = (combined >> 6) & 0x3F;
        int idx4 = combined & 0x3F;

        result.push_back(base64_table[idx1]);
        result.push_back(base64_table[idx2]);
        result.push_back((i + 1 < len) ? base64_table[idx3] : '=');
        result.push_back((i + 2 < len) ? base64_table[idx4] : '=');
    }

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