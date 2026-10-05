#include "JZKeySequence.h"

#include <cctype>
#include <cstdio>
#include <vector>

namespace {

// 主键名 -> JZKey(名字全部大写, 比较前把输入也转大写)
struct KeyName
{
    const char* name;
    uint32_t    key;
};

const KeyName kKeyNames[] = {
    { "BACKSPACE",  Key_Backspace },
    { "TAB",        Key_Tab },
    { "BACKTAB",    Key_Tab },          // 没有 Shift+Tab 的独立键码, 归一到 Tab
    { "ENTER",      Key_Return },
    { "RETURN",     Key_Return },
    { "PAUSE",      Key_Pause },
    { "CAPSLOCK",   Key_CapsLock },
    { "ESC",        Key_Escape },
    { "ESCAPE",     Key_Escape },
    { "SPACE",      Key_Space },
    { "PAGEUP",     Key_PageUp },
    { "PGUP",       Key_PageUp },
    { "PAGEDOWN",   Key_PageDown },
    { "PGDN",       Key_PageDown },
    { "PAGEDN",     Key_PageDown },
    { "END",        Key_End },
    { "HOME",       Key_Home },
    { "LEFT",       Key_Left },
    { "UP",         Key_Up },
    { "RIGHT",      Key_Right },
    { "DOWN",       Key_Down },
    { "INSERT",     Key_Insert },
    { "INS",        Key_Insert },
    { "DELETE",     Key_Delete },
    { "DEL",        Key_Delete },
    { "F1",  Key_F1 },  { "F2",  Key_F2 },  { "F3",  Key_F3 },  { "F4",  Key_F4 },
    { "F5",  Key_F5 },  { "F6",  Key_F6 },  { "F7",  Key_F7 },  { "F8",  Key_F8 },
    { "F9",  Key_F9 },  { "F10", Key_F10 }, { "F11", Key_F11 }, { "F12", Key_F12 },
};

std::string upper(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i)
        out.push_back((char)std::toupper((unsigned char)s[i]));
    return out;
}

void trim(std::string& s)
{
    size_t b = 0;
    while (b < s.size() && std::isspace((unsigned char)s[b]))
        ++b;
    size_t e = s.size();
    while (e > b && std::isspace((unsigned char)s[e - 1]))
        --e;
    s = s.substr(b, e - b);
}

// 单个词 -> 主键码; 认不出返回 0
uint32_t keyOfName(const std::string& token)
{
    if (token.empty())
        return 0;

    // 单字符: 字母/数字直接用它的 ASCII 大写码(与 Win32 VK 一致, 见 JZKeyEvent.h 的说明)
    if (token.size() == 1)
    {
        const char c = token[0];
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
            return (uint32_t)c;
        return 0;
    }

    const int n = (int)(sizeof(kKeyNames) / sizeof(kKeyNames[0]));
    for (int i = 0; i < n; ++i)
    {
        if (token == kKeyNames[i].name)
            return kKeyNames[i].key;
    }
    return 0;
}

std::string nameOfKey(uint32_t key)
{
    if ((key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9'))
        return std::string(1, (char)key);

    const int n = (int)(sizeof(kKeyNames) / sizeof(kKeyNames[0]));
    for (int i = 0; i < n; ++i)
    {
        if (key == kKeyNames[i].key)
            return kKeyNames[i].name;
    }

    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%X", (unsigned)key);
    return buf;
}

const uint32_t kModMask = JZKeyEvent::ShiftModifier | JZKeyEvent::CtrlModifier
                        | JZKeyEvent::AltModifier;

} // namespace

bool JZKeySequence::parse(const std::string& text, uint32_t& key, uint32_t& modifiers)
{
    key = 0;
    modifiers = 0;

    const std::string s = upper(text);

    // 按 '+' 切开逐段看: 修饰键累加, 最后一段当主键(与 Qt 一致: 主键在最右)
    uint32_t foundKey = 0;
    size_t pos = 0;
    while (pos <= s.size())
    {
        const size_t plus = s.find('+', pos);
        std::string tok = (plus == std::string::npos) ? s.substr(pos) : s.substr(pos, plus - pos);
        trim(tok);

        if (!tok.empty())
        {
            if (tok == "CTRL" || tok == "CONTROL")
                modifiers |= JZKeyEvent::CtrlModifier;
            else if (tok == "SHIFT")
                modifiers |= JZKeyEvent::ShiftModifier;
            else if (tok == "ALT")
                modifiers |= JZKeyEvent::AltModifier;
            else if (tok == "META" || tok == "WIN" || tok == "SUPER")
            {
                // 引擎没有 Win 键语义: 忽略这一段(不当作主键, 也不报错)
            }
            else if (uint32_t k = keyOfName(tok))
                foundKey = k;          // 多个候选时最后一个赢(与 Qt 的"末段是主键"同口径)
        }

        if (plus == std::string::npos)
            break;
        pos = plus + 1;
    }

    key = foundKey;
    return foundKey != 0;
}

std::string JZKeySequence::text(uint32_t key, uint32_t modifiers)
{
    std::string out;
    if (modifiers & JZKeyEvent::CtrlModifier)
        out += "Ctrl+";
    if (modifiers & JZKeyEvent::ShiftModifier)
        out += "Shift+";
    if (modifiers & JZKeyEvent::AltModifier)
        out += "Alt+";
    out += nameOfKey(key);
    return out;
}

bool JZKeySequence::matches(const std::string& shortcutText, const JZKeyEvent* event)
{
    if (!event || !event->isPress())
        return false;

    uint32_t key = 0, modifiers = 0;
    if (!parse(shortcutText, key, modifiers))
        return false;

    if (event->key() != key)
        return false;

    return (event->modifiers() & kModMask) == (modifiers & kModMask);
}
