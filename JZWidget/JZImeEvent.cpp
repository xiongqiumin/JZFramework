#include "JZImeEvent.h"

JZImeEvent::JZImeEvent(int flags)
    : JZEvent(JZEvent_ime)
    , m_flags(flags)
{
}

bool JZImeEvent::isAccepted() const
{
    return m_accepted;
}

void JZImeEvent::accept()
{
    m_accepted = true;
}

void JZImeEvent::ignore()
{
    m_accepted = false;
}

// ==================== UTF-8 解码 / 索引换算 ====================
namespace {

// 读出一个码点并推进下标; 非法序列按 U+FFFD 处理并跳过, 保证不卡死
bool utf8Next(const std::string& s, size_t& i, char32_t& cp)
{
    if (i >= s.size())
        return false;

    unsigned char lead = static_cast<unsigned char>(s[i]);
    int extra = 0;
    char32_t c = 0;

    if ((lead & 0x80) == 0x00)        { extra = 0; c = lead; }
    else if ((lead & 0xE0) == 0xC0)   { extra = 1; c = lead & 0x1F; }
    else if ((lead & 0xF0) == 0xE0)   { extra = 2; c = lead & 0x0F; }
    else if ((lead & 0xF8) == 0xF0)   { extra = 3; c = lead & 0x07; }
    else { ++i; cp = 0xFFFD; return true; }

    if (i + static_cast<size_t>(extra) >= s.size()) {
        i = s.size();
        cp = 0xFFFD;
        return true;
    }

    for (int k = 1; k <= extra; ++k)
        c = (c << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);

    i += extra + 1;
    cp = c;
    return true;
}

} // namespace

int JZImeEvent::utf16Length(const std::string& utf8)
{
    int n = 0;
    size_t i = 0;
    char32_t cp = 0;
    while (utf8Next(utf8, i, cp))
        n += (cp > 0xFFFF) ? 2 : 1;   // 非 BMP 占一个代理对
    return n;
}

int JZImeEvent::utf16ToCodePointIndex(const std::string& utf8, int utf16Index)
{
    if (utf16Index <= 0)
        return 0;

    int u16 = 0, cpi = 0;
    size_t i = 0;
    char32_t cp = 0;
    while (utf8Next(utf8, i, cp)) {
        if (u16 >= utf16Index)
            break;
        u16 += (cp > 0xFFFF) ? 2 : 1;
        ++cpi;
    }
    return cpi;
}

int JZImeEvent::codePointToUtf16Index(const std::string& utf8, int cpIndex)
{
    int u16 = 0, cpi = 0;
    size_t i = 0;
    char32_t cp = 0;
    while (utf8Next(utf8, i, cp) && cpi < cpIndex) {
        u16 += (cp > 0xFFFF) ? 2 : 1;
        ++cpi;
    }
    return u16;
}
