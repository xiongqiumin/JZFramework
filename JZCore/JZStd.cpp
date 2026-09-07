#ifdef _WIN32
#include <Windows.h>
#endif

#include <string>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <cstdio>
#include <thread>
#include <stdarg.h>
#include <limits.h>
#include "JZStd.h"
#include "JZDebug.h"
#include "JZString.h"
#include "JZUtf8Iterator.h"

using namespace std;

string str_asprintf(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);

    // 第一次调用 vsnprintf 来获取所需缓冲区大小
    va_list args_copy;
    va_copy(args_copy, args);
    int len = vsnprintf(nullptr, 0, fmt, args_copy);
    va_end(args_copy);

    if (len < 0) {
        va_end(args);
        return ""; // 格式化失败
    }

    // 分配足够的内存（+1 是为了 '\0' 结束符）
    string result(len + 1, '\0');

    // 第二次调用 vsnprintf 来实际格式化字符串
    vsnprintf(&result[0], result.size(), fmt, args);

    va_end(args);

    // 去掉多余的 '\0'
    result.pop_back();
    return result;
}

// 可变参数版本
string str_vasprintf(const char *fmt, va_list ap)
{
    va_list ap_copy;
    va_copy(ap_copy, ap);

    // 第一次调用：获取所需缓冲区大小
    int len = vsnprintf(nullptr, 0, fmt, ap_copy);
    va_end(ap_copy);

    if (len < 0) 
        return ""; // 格式化失败

    // 分配足够空间（+1 用于 '\0' 结束符）
    string str(len, '\0');

    // 第二次调用：实际格式化到字符串
    vsnprintf(&str[0], len + 1, fmt, ap);

    return str;
}

bool str_startsWith(const string &str, const string &prefix, bool caseSensitive) {
    if (prefix.length() > str.length()) return false;
    if (caseSensitive) {
        return str.compare(0, prefix.length(), prefix) == 0;
    }
    else {
        string s = str_toLower(str);
        string p = str_toLower(prefix);
        return s.compare(0, p.length(), p) == 0;
    }
}

bool str_endsWith(const string &str, const string &suffix, bool caseSensitive) {
    if (suffix.length() > str.length()) return false;
    if (caseSensitive) {
        return str.compare(str.length() - suffix.length(), suffix.length(), suffix) == 0;
    }
    else {
        string s = str_toLower(str);
        string su = str_toLower(suffix);
        return s.compare(s.length() - su.length(), su.length(), su) == 0;
    }
}

bool str_contains(const string &str, const string &substr, bool caseSensitive) {
    if (caseSensitive) {
        return str.find(substr) != string::npos;
    }
    else {
        string s = str_toLower(str);
        string sub = str_toLower(substr);
        return s.find(sub) != string::npos;
    }
}

void str_chop(string &str, int n) {
    if (n <= 0) 
        return;
    
    size_t len = str.length();
    if ((size_t)n >= len)
        str.clear();
    else
        str = str.substr(0, len - n);
}

string str_mid(const string &str, size_t pos, int len) {
    if (pos >= str.length()) return "";
    if (len < 0) return str.substr(pos);
    return str.substr(pos, len);
}

// 取左侧n个字符
string str_left(const string &s, size_t n)
{
    if (n >= s.size())
        return s;
    return s.substr(0, n);
}

// 取右侧n个字符
string str_right(const string &s, size_t n)
{
    if (n >= s.size())
        return s;
    return s.substr(s.size() - n, n);
}

void str_replace(string &str, const string &oldstr, const string &newstr) {    
    size_t pos = 0;
    while ((pos = str.find(oldstr, pos)) != string::npos) {
        str.replace(pos, oldstr.length(), newstr);
        pos += newstr.length();
    }
}

void str_remove(string &str, const string &substr) {    
    return str_replace(str, substr, "");
}

void str_prepend(string &src, const string &prefix)
{
    src.insert(src.begin(), prefix.begin(), prefix.end());
}

void str_append(string& str, const string& other)
{
    str += other;
}

string str_toLower(const string &str) {
    JZString jz_str = JZString::fromStdString(str);
    return jz_str.toLower().toStdString();
}

string str_toUpper(const string &str) {
    JZString jz_str = JZString::fromStdString(str);
    return jz_str.toUpper().toStdString();
}

string str_trimmed(const string &str) {
    if (str.empty()) return "";
    size_t start = 0;
    size_t end = str.length();
    while (start < end && isspace((unsigned char)str[start])) start++;
    while (end > start && isspace((unsigned char)str[end - 1])) end--;
    return str.substr(start, end - start);
}

string str_simplified(const string &str) {
    std::string res;
    bool in_space = true;

    for (char ch : str)
    {
        unsigned char c = static_cast<unsigned char>(ch);
        if (std::isspace(c))
        {
            if (!in_space)
            {
                res += ' ';
                in_space = true;
            }
        }
        else
        {
            res += ch;
            in_space = false;
        }
    }
    // 去掉末尾可能多余的一个空格
    if (!res.empty() && res.back() == ' ')
        res.pop_back();
    return res;
}

vector<string> str_split(const string &str, const string &sep, JZ::SplitBehavior behavior)
{
    vector<string> result;
    if (sep.empty()) {
        // 如果分隔符为空，返回整个字符串作为一个元素
        result.push_back(str);
        return result;
    }

    size_t pos = 0;
    while (true) {
        size_t next = str.find(sep, pos);
        string token = str.substr(pos, next - pos);

        if (behavior == JZ::KeepEmptyParts || !token.empty()) {
            result.push_back(token);
        }

        if (next == string::npos) {
            break;
        }

        pos = next + sep.size();
    }

    return result;
}

string str_join(const vector<string> &v, const string &sep)
{
    if (v.empty()) return "";

    string result;

    // 预留空间，减少内存分配
    size_t total_len = 0;
    for (const auto &s : v) {
        total_len += s.size();
    }
    total_len += sep.size() * (v.size() - 1);
    result.reserve(total_len);

    // 拼接第一个字符串
    result += v[0];

    // 拼接剩余的字符串和分隔符
    for (size_t i = 1; i < v.size(); ++i) {
        result += sep;
        result += v[i];
    }

    return result;
}

int str_indexOf(const string &str, const string &substr, int pos, bool caseSensitive ) 
{
    if (pos < 0) pos = 0;
    size_t p = static_cast<size_t>(pos);

    if (p > str.size()) return -1;
    if (substr.empty()) return static_cast<int>(p);

    if (caseSensitive) {
        // 大小写敏感：直接查找
        size_t found = str.find(substr, p);
        return (found == string::npos) ? -1 : static_cast<int>(found);
    }
    else {
        // 忽略大小写：先转换为小写再查找
        string lower_str = str_toLower(str);
        string lower_sub = str_toLower(substr);

        size_t found = lower_str.find(lower_sub, p);
        return (found == string::npos) ? -1 : static_cast<int>(found);
    }
}

int str_lastIndexOf(const string &str, const string &substr, int pos, bool caseSensitive) 
{
    if (substr.empty()) {
        if (pos < 0 || static_cast<size_t>(pos) >= str.size())
            return static_cast<int>(str.size());
        return pos;
    }

    size_t p = (pos < 0) ? str.size() : static_cast<size_t>(pos + 1);
    if (p > str.size()) p = str.size();

    if (caseSensitive) {
        // 大小写敏感：直接反向查找
        size_t found = str.rfind(substr, p - 1);
        return (found == string::npos) ? -1 : static_cast<int>(found);
    }
    else {
        // 忽略大小写：先转换为小写再反向查找
        string lower_str = str_toLower(str);
        string lower_sub = str_toLower(substr);

        size_t found = lower_str.rfind(lower_sub, p - 1);
        return (found == string::npos) ? -1 : static_cast<int>(found);
    }
}

int str_toInt(const string& str, bool* ok, int base)
{
    // 初始化转换状态（仅当ok不为空时操作）
    if (ok != nullptr)
    {
        *ok = false;
    }

    // 空字符串直接返回失败
    if (str.empty())
    {
        return 0;
    }

    // 重置errno，避免之前的错误影响
    errno = 0;
    char* end_ptr = nullptr;

    // 核心转换逻辑（十进制）
    long int value = strtol(str.c_str(), &end_ptr, base);

    // 检查转换错误场景
    bool conversion_success = true;
    if (errno != 0 || end_ptr == str.c_str() || *end_ptr != '\0')
    {
        conversion_success = false;
    }
    // 检查long转int的溢出
    else if (value < INT_MIN || value > INT_MAX)
    {
        conversion_success = false;
    }

    // 转换失败则返回0
    if (!conversion_success)
    {
        return 0;
    }

    // 转换成功，更新ok状态（仅当ok不为空时）
    if (ok != nullptr)
    {
        *ok = true;
    }

    // 返回转换后的整数值
    return static_cast<int>(value);
}

double str_toDouble(const string& str, bool* ok)
{
    // 初始化转换状态
    if (ok != nullptr)
    {
        *ok = false;
    }

    // 空字符串直接返回失败
    if (str.empty())
    {
        return 0.0;
    }

    // 重置错误码
    errno = 0;
    char* end_ptr = nullptr;

    // 核心：字符串转 double（十进制）
    double value = strtod(str.c_str(), &end_ptr);

    bool conversion_success = true;

    // 浮点数转换错误检查
    if (errno != 0)          // 发生溢出/其他系统错误
    {
        conversion_success = false;
    }
    else if (end_ptr == str.c_str())  // 没有任何字符被转换
    {
        conversion_success = false;
    }
    else if (*end_ptr != '\0')       // 字符串末尾有无效字符（如 "123abc"）
    {
        conversion_success = false;
    }

    // 转换失败返回 0.0
    if (!conversion_success)
    {
        return 0.0;
    }

    // 转换成功，更新状态
    if (ok != nullptr)
    {
        *ok = true;
    }

    return value;
}

string str_escape(const string& src)
{
    string dst;
    dst.reserve(src.size() * 2);  // 预留空间，避免频繁重分配

    for (unsigned char ch : src)
    {
        switch (ch)
        {
        case '\\':  dst += "\\\\"; break;
        case '"':   dst += "\\\""; break;
        case '\'':  dst += "\\'";  break;
        case '\a':  dst += "\\a";  break;
        case '\b':  dst += "\\b";  break;
        case '\f':  dst += "\\f";  break;
        case '\n':  dst += "\\n";  break;
        case '\r':  dst += "\\r";  break;
        case '\t':  dst += "\\t";  break;
        case '\v':  dst += "\\v";  break;
        default:
            // ASCII不可打印控制字符(0~31、127)用十六进制转义
            if (ch < 0x20 || ch == 0x7F)
            {
                char buf[5];
                snprintf(buf, sizeof(buf), "\\x%02X", static_cast<uint8_t>(ch));
                dst += buf;
            }
            else
            {
                dst.push_back(static_cast<char>(ch));
            }
            break;
        }
    }
    return dst;
}

namespace {

// 十六进制字符转数值，非法返回 -1
int hex2val(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

// Unicode码点转UTF-8字节写入dst (非法码点替换为 U+FFFD; 编码统一走 JZUtf8Iterator)
void codepoint_to_utf8(string& dst, char32_t cp)
{
    if (cp > 0x10FFFF)
        cp = 0xFFFD;
    JZUtf8Iterator::encode(cp, dst);
}

// 读取固定N位十六进制，成功返回数值并更新 idx，失败返回 -1 且不修改 idx
int read_fixed_hex(const string& src, size_t& idx, size_t len)
{
    size_t tmp = idx;
    unsigned int val = 0;
    for (size_t k = 0; k < len; ++k)
    {
        if (tmp >= src.size())
            return -1;
        int v = hex2val(src[tmp]);
        if (v == -1)
            return -1;
        val = (val << 4) | v;
        ++tmp;
    }
    // 全部成功，更新外部索引
    idx = tmp;
    return static_cast<int>(val);
}

} // anonymous namespace

string str_unescape(const string& src)
{
    string dst;
    dst.reserve(src.size());
    const size_t len = src.size();
    size_t i = 0;

    while (i < len)
    {
        char ch = src[i++];
        if (ch != '\\')
        {
            dst.push_back(ch);
            continue;
        }

        // 末尾孤立反斜杠
        if (i >= len)
        {
            jzWarning("unescape warning: trailing backslash at index %zu", i - 1);
            dst.push_back('\\');
            continue;
        }

        const size_t esc_pos = i - 1;
        char esc_c = src[i];

        // 1. 八进制转义 \ddd 最多3位 0~7
        if (esc_c >= '0' && esc_c <= '7')
        {
            unsigned int val = esc_c - '0';
            ++i;
            int cnt = 1;
            while (i < len && cnt < 3)
            {
                char c = src[i];
                if (c >= '0' && c <= '7')
                {
                    val = val * 8 + (c - '0');
                    ++i;
                    cnt++;
                }
                else
                    break;
            }
            if (val > 0xFF)
            {
                jzWarning("unescape warning: octal escape \\%o overflow (val=%u), truncate to 0xFF at index %zu", val, val, esc_pos);
            }
            dst.push_back(static_cast<char>(static_cast<uint8_t>(val & 0xFF)));
            continue;
        }

        // 2. 十六进制 \x 任意位十六进制，单字节
        if (esc_c == 'x')
        {
            ++i;
            unsigned int val = 0;
            bool has_digit = false;
            while (i < len)
            {
                int v = hex2val(src[i]);
                if (v == -1)
                    break;
                val = (val << 4) | static_cast<unsigned int>(v);
                has_digit = true;
                ++i;
            }
            if (!has_digit)
            {
                jzWarning("unescape warning: incomplete hex escape \\x at index %zu", esc_pos);
                dst.push_back('\\');
                dst.push_back('x');
            }
            else
            {
                if (val > 0xFF)
                {
                    jzWarning("unescape warning: hex escape overflow 0x%X, truncate to 0xFF at index %zu", val, esc_pos);
                }
                dst.push_back(static_cast<char>(static_cast<uint8_t>(val & 0xFF)));
            }
            continue;
        }

        // 3. C++11 Unicode转义 \uXXXX(4位16进制) \UXXXXXXXX(8位16进制) → UTF-8
        if (esc_c == 'u')
        {
            ++i;
            int cp = read_fixed_hex(src, i, 4);
            if (cp == -1)
            {
                jzWarning("unescape warning: incomplete \\uXXXX escape at index %zu", esc_pos);
                dst.push_back('\\');
                dst.push_back('u');
                // 注意：此时 i 未被修改，已读的字符未被消耗，后续循环会正常输出它们
                continue;
            }
            codepoint_to_utf8(dst, static_cast<char32_t>(cp));
            continue;
        }
        if (esc_c == 'U')
        {
            ++i;
            int cp = read_fixed_hex(src, i, 8);
            if (cp == -1)
            {
                jzWarning("unescape warning: incomplete \\UXXXXXXXX escape at index %zu", esc_pos);
                dst.push_back('\\');
                dst.push_back('U');
                continue;
            }
            codepoint_to_utf8(dst, static_cast<char32_t>(static_cast<uint32_t>(cp)));
            continue;
        }

        // 4. C++标准单字符转义序列
        ++i;
        switch (esc_c)
        {
        case '\\': dst.push_back('\\'); break;
        case '\'': dst.push_back('\''); break;
        case '"':  dst.push_back('"');  break;
        case 'a':  dst.push_back('\a'); break;
        case 'b':  dst.push_back('\b'); break;
        case 'f':  dst.push_back('\f'); break;
        case 'n':  dst.push_back('\n'); break;
        case 'r':  dst.push_back('\r'); break;
        case 't':  dst.push_back('\t'); break;
        case 'v':  dst.push_back('\v'); break;
        default:
            jzWarning("unescape warning: invalid escape sequence \\%c at index %zu", esc_c, esc_pos);
            dst.push_back('\\');
            dst.push_back(esc_c);
            break;
        }
    }

    return dst;
}

#ifdef _WIN32

string str_local_to_utf8(const string &str)
{
    if (str.empty()) return "";

    // 1. 本地编码 -> UTF-16
    int wcs_len = MultiByteToWideChar(CP_ACP, 0, str.c_str(), -1, nullptr, 0);
    if (wcs_len <= 1) return ""; // 只有结尾的 \0

    wstring wstr(wcs_len - 1, L'\0');
    MultiByteToWideChar(CP_ACP, 0, str.c_str(), -1, &wstr[0], wcs_len);

    // 2. UTF-16 -> UTF-8
    int utf8_len = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (utf8_len <= 1) return "";

    string utf8_str(utf8_len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &utf8_str[0], utf8_len, nullptr, nullptr);

    return utf8_str;
}

string str_utf8_to_local(const string& utf8_str) {
    if (utf8_str.empty())
        return "";

    int wcs_len = MultiByteToWideChar(CP_UTF8, 0, utf8_str.c_str(), -1, nullptr, 0);
    if (wcs_len <= 1)
        return "";

    wstring wstr(wcs_len - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8_str.c_str(), -1, &wstr[0], wcs_len);
    int local_len = WideCharToMultiByte(CP_ACP, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (local_len <= 1)
        return "";

    string local_str(local_len - 1, '\0');
    WideCharToMultiByte(CP_ACP, 0, wstr.c_str(), -1, &local_str[0], local_len, nullptr, nullptr);
    return local_str;
}

wstring str_to_wstr(const string& str)
{
    if (str.empty()) 
        return L"";
    
    int len = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, nullptr, 0);
    
    wstring wstr(len - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, &wstr[0], len);
    
    return wstr;
}

string wstr_to_str(const wstring &wstr)
{
    if (wstr.empty())
        return "";

    // 获取需要的字节长度，转 UTF-8
    int byteLen = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);

    string str(byteLen - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &str[0], byteLen, nullptr, nullptr);

    return str;
}

#else
// 非 Windows 平台手写 UTF-8 <-> wstring 编解码，不依赖任何第三方库。
// wchar_t 宽度随平台/编译器而定：
//   - Linux / macOS（GCC、Clang）：wchar_t 为 32 位（UTF-32）
//   - ESP32（esp-idf / GCC xtensa）、Windows：wchar_t 为 16 位（UTF-16）
// 因此这里同时实现 UTF-32 与 UTF-16 两条路径，并在编译期按 sizeof(wchar_t) 分发。

// ---- 公共：UTF-8 解码为码点（uint32_t） ----
// 解码统一走 JZUtf8Iterator::decode, 这里保留对过长形式/越界的剔除
static bool jz_utf8_decode(const string& utf8, size_t& i, uint32_t& cp)
{
    char32_t c;
    int len;
    if (!JZUtf8Iterator::decode(utf8.data(), utf8.size(), i, c, len))
        return false;

    cp = static_cast<uint32_t>(c);

    // 剔除去重编码（过长的 UTF-8 形式）及越界
    if (cp > 0x10FFFF ||
        (len == 2 && cp < 0x80) ||
        (len == 3 && cp < 0x800) ||
        (len == 4 && cp < 0x10000)) {
        return false;
    }

    i += static_cast<size_t>(len);
    return true;
}

// ---- 公共：码点（uint32_t）编码为 UTF-8 ----
static void jz_utf8_encode(string& out, uint32_t cp)
{
    JZUtf8Iterator::encode(cp, out);
}

#if defined(__SIZEOF_WCHAR_T__) && (__SIZEOF_WCHAR_T__ == 4) || (defined(SIZEOF_WCHAR_T) && SIZEOF_WCHAR_T == 4) || (defined(_WCHAR_T_SIZE) && _WCHAR_T_SIZE == 4)

// ===== wchar_t 为 32 位（UTF-32）：Linux / macOS =====

static wstring jz_utf8_to_wide(const string& utf8)
{
    wstring out;
    out.reserve(utf8.size());

    size_t i = 0;
    uint32_t cp = 0;
    while (i < utf8.size()) {
        if (jz_utf8_decode(utf8, i, cp)) {
            out.push_back(static_cast<wchar_t>(cp));
        } else {
            // 跳过非法字节，输出替换字符
            ++i;
            out.push_back(static_cast<wchar_t>(0xFFFD));
        }
    }
    return out;
}

static string jz_wide_to_utf8(const wstring& wstr)
{
    string out;
    out.reserve(wstr.size());
    for (wchar_t wc : wstr) {
        uint32_t cp = static_cast<uint32_t>(wc);
        // UTF-32 下代理项区间非法
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            jz_utf8_encode(out, 0xFFFD);
            continue;
        }
        jz_utf8_encode(out, cp);
    }
    return out;
}

#else

// ===== wchar_t 为 16 位（UTF-16）：ESP32 / Windows（非 Windows 分支） =====

static wstring jz_utf8_to_wide(const string& utf8)
{
    wstring out;
    out.reserve(utf8.size());

    size_t i = 0;
    uint32_t cp = 0;
    while (i < utf8.size()) {
        if (!jz_utf8_decode(utf8, i, cp)) {
            ++i;
            out.push_back(static_cast<wchar_t>(0xFFFD));
            continue;
        }
        if (cp <= 0xFFFF) {
            out.push_back(static_cast<wchar_t>(cp));
        } else {
            // 辅助平面：编码为代理项对（surrogate pair）
            cp -= 0x10000;
            out.push_back(static_cast<wchar_t>(0xD800 | (cp >> 10)));
            out.push_back(static_cast<wchar_t>(0xDC00 | (cp & 0x3FF)));
        }
    }
    return out;
}

static string jz_wide_to_utf8(const wstring& wstr)
{
    string out;
    out.reserve(wstr.size());

    size_t i = 0;
    const size_t n = wstr.size();
    while (i < n) {
        uint32_t c = static_cast<uint16_t>(wstr[i]);
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < n) {
            uint32_t lo = static_cast<uint16_t>(wstr[i + 1]);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                uint32_t cp = 0x10000 + (((c - 0xD800) << 10) | (lo - 0xDC00));
                jz_utf8_encode(out, cp);
                i += 2;
                continue;
            }
        }
        if (c >= 0xD800 && c <= 0xDFFF) {
            // 孤立代理项，按替换字符处理
            jz_utf8_encode(out, 0xFFFD);
            ++i;
            continue;
        }
        jz_utf8_encode(out, c);
        ++i;
    }
    return out;
}

#endif

wstring str_to_wstr(const string& utf8)
{
    if (utf8.empty())
        return L"";
    return jz_utf8_to_wide(utf8);
}

string wstr_to_str(const wstring& wstr)
{
    if (wstr.empty())
        return "";
    return jz_wide_to_utf8(wstr);
}

// 非 Windows 环境下，本地编码即 UTF-8，二者等价（直接透传）。
// 框架在 Linux 下以 UTF-8 处理文件名/文本，故无需转换。
string str_local_to_utf8(const string &str)
{
    return str;
}

string str_utf8_to_local(const string& utf8_str)
{
    return utf8_str;
}

#endif