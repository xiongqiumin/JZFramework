#include "JZImeEvent.h"
#include "JZUtf8Iterator.h"

JZImeEvent::JZImeEvent(int flags)
    : JZEvent(JZEvent_ime)
    , m_flags(flags)
{
    ignore();   // 与键盘一致: 默认未接收, handler accept() 表示已消费
}

// ==================== UTF-16 下标 <-> 码点下标 换算 ====================
// 解码统一走 JZUtf8Iterator; 非 BMP 码点 (cp > 0xFFFF) 在 UTF-16 里占一个代理对

int JZImeEvent::utf16Length(const std::string& utf8)
{
    int n = 0;
    JZUtf8Iterator it(utf8);
    while (it.isValid()) {
        n += (it.codepoint() > 0xFFFF) ? 2 : 1;
        ++it;
    }
    return n;
}

int JZImeEvent::utf16ToCodePointIndex(const std::string& utf8, int utf16Index)
{
    if (utf16Index <= 0)
        return 0;

    int u16 = 0, cpi = 0;
    JZUtf8Iterator it(utf8);
    while (it.isValid()) {
        if (u16 >= utf16Index)
            break;
        u16 += (it.codepoint() > 0xFFFF) ? 2 : 1;
        ++cpi;
        ++it;
    }
    return cpi;
}

int JZImeEvent::codePointToUtf16Index(const std::string& utf8, int cpIndex)
{
    int u16 = 0, cpi = 0;
    JZUtf8Iterator it(utf8);
    while (it.isValid() && cpi < cpIndex) {
        u16 += (it.codepoint() > 0xFFFF) ? 2 : 1;
        ++cpi;
        ++it;
    }
    return u16;
}
