#include <string.h>
#include "JZByteArray.h"
#include "JZDebug.h"

static const char base64_table[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz"
    "0123456789+/";

// 查表：仅 Base64 字符集，非法字符返回 -1，'=' 仍保持 -1（作为特殊标记）
static const int8_t base64_decode_table[128] = {
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,  // 0-15
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,  // 16-31
    -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,  // 32-47: '+'=62, '/'=63
    52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,  // 48-63: '0'-'9'=52-61
    -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,  // 64-79: 'A'-'P'
    15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,  // 80-95: 'Q'-'Z'
    -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,  // 96-111: 'a'-'p'
    41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,  // 112-127: 'q'-'z'
};

JZByteArray JZByteArray::fromBase64(const JZByteArray &base64_in) {
    const JZByteVector &base64 = base64_in.m_vec;
    const size_t len = base64.size();

    if (len == 0)
        return JZByteArray();

    if (len % 4 != 0) {
        jzError() << "Base64 decode failed: invalid length (" << len << "), must be multiple of 4";
        return JZByteArray();
    }

    // 1. 计算末尾填充数（'=' 只能出现在最后，最多两个）
    size_t padding = 0;
    if (len >= 1 && base64[len - 1] == '=') padding++;
    if (len >= 2 && base64[len - 2] == '=') padding++;
    if (padding > 2) {
        jzError() << "Base64 decode failed: invalid padding (" << padding << "), max 2";
        return JZByteArray();
    }

    // 2. 预分配输出缓冲区
    JZByteArray out;
    const size_t out_len = (len / 4) * 3 - padding;
    out.m_vec.reserve(out_len);   // 也可以用 resize + 下标赋值，这里 reserve + push_back 更安全

    const size_t full_blocks = len / 4;               // 总 4 字符块数
    const size_t last_block_idx = full_blocks - 1;   // 最后一个块的索引（从 0 开始）

    // 3. 逐块解码
    for (size_t block = 0; block < full_blocks; ++block) {
        const char* p = base64.data() + block * 4;
        unsigned char c0 = p[0], c1 = p[1], c2 = p[2], c3 = p[3];

        // 快速查表（内联）
        if (c0 >= 128 || c1 >= 128) goto decode_error;
        int v0 = base64_decode_table[c0];
        int v1 = base64_decode_table[c1];
        if (v0 == -1 || v1 == -1) goto decode_error;

        // 对于最后一块，需要特殊处理可能的 '='
        int v2, v3;
        if (block == last_block_idx) {
            // 最后一块：按 padding 数量判断有效字符
            bool pad2 = (padding == 2);
            bool pad1 = (padding == 1);

            if (pad2) {
                // 格式：xx==   c2='=', c3='='
                if (c2 != '=' || c3 != '=') goto decode_error;
                v2 = v3 = 0;   // 填充位任意，最后会被忽略
            } else if (pad1) {
                // 格式：xxx=   c2 有效, c3='='
                if (c3 != '=') goto decode_error;
                if (c2 >= 128) goto decode_error;
                v2 = base64_decode_table[c2];
                if (v2 == -1) goto decode_error;
                v3 = 0;
            } else {
                // 无填充，正常 3 字节
                if (c2 >= 128 || c3 >= 128) goto decode_error;
                v2 = base64_decode_table[c2];
                v3 = base64_decode_table[c3];
                if (v2 == -1 || v3 == -1) goto decode_error;
            }
        } else {
            // 非最后一块：四个字符都必须有效
            if (c2 >= 128 || c3 >= 128) goto decode_error;
            v2 = base64_decode_table[c2];
            v3 = base64_decode_table[c3];
            if (v2 == -1 || v3 == -1) goto decode_error;
        }

        // 4. 拼接 24 位，拆分输出
        uint32_t combined = (static_cast<uint32_t>(v0) << 18) |
                            (static_cast<uint32_t>(v1) << 12) |
                            (static_cast<uint32_t>(v2) << 6)  |
                            static_cast<uint32_t>(v3);

        out.m_vec.push_back(static_cast<char>((combined >> 16) & 0xFF));

        // 对于最后一块，根据 padding 决定是否输出后续字节
        if (block == last_block_idx) {
            if (padding < 2) {
                out.m_vec.push_back(static_cast<char>((combined >> 8) & 0xFF));
            }
            if (padding < 1) {
                out.m_vec.push_back(static_cast<char>(combined & 0xFF));
            }
        } else {
            out.m_vec.push_back(static_cast<char>((combined >> 8) & 0xFF));
            out.m_vec.push_back(static_cast<char>(combined & 0xFF));
        }
    }

    return out;

decode_error:
    jzError() << "Base64 decode failed: invalid character at block " << (out.m_vec.size() / 3);
    return JZByteArray();
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
    JZByteVector &result = out.m_vec;
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