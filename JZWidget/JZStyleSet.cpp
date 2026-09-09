#include "JZStyleSet.h"

#include <ctype.h>
#include <stdlib.h>

namespace
{

std::string trimStr(const std::string& s)
{
    size_t b = 0, e = s.size();
    while (b < e && (unsigned char)s[b] <= ' ')
        b++;
    while (e > b && (unsigned char)s[e - 1] <= ' ')
        e--;
    return s.substr(b, e - b);
}

std::string lowerStr(const std::string& s)
{
    std::string r = s;
    for (size_t i = 0; i < r.size(); i++)
    {
        if (r[i] >= 'A' && r[i] <= 'Z')
            r[i] = (char)(r[i] - 'A' + 'a');
    }
    return r;
}

std::vector<std::string> splitTokens(const std::string& s)
{
    std::vector<std::string> out;
    std::string cur;
    for (size_t i = 0; i < s.size(); i++)
    {
        char c = s[i];
        if ((unsigned char)c <= ' ')
        {
            if (!cur.empty())
            {
                out.push_back(cur);
                cur.clear();
            }
            continue;
        }
        cur += c;
    }
    if (!cur.empty())
        out.push_back(cur);
    return out;
}

bool hexDigit(char c, int& v)
{
    if (c >= '0' && c <= '9')
    {
        v = c - '0';
        return true;
    }
    c = (char)tolower((unsigned char)c);
    if (c >= 'a' && c <= 'f')
    {
        v = c - 'a' + 10;
        return true;
    }
    return false;
}

bool parseHexColor(const std::string& s, JZColor& c)
{
    if (s.size() < 1 || s[0] != '#')
        return false;
    std::string h = s.substr(1);
    if (h.size() == 3)
    {
        int v[3];
        for (int i = 0; i < 3; i++)
        {
            if (!hexDigit(h[i], v[i]))
                return false;
            v[i] = v[i] * 16 + v[i];
        }
        c = JZColor((uint8_t)v[0], (uint8_t)v[1], (uint8_t)v[2], 255);
        return true;
    }
    if (h.size() == 6 || h.size() == 8)
    {
        std::vector<int> v;
        for (size_t i = 0; i < h.size(); i++)
        {
            int d;
            if (!hexDigit(h[i], d))
                return false;
            v.push_back(d);
        }
        auto at = [&](size_t i) { return (uint8_t)(v[i] * 16 + v[i + 1]); };
        if (h.size() == 6)
            c = JZColor(at(0), at(2), at(4), 255);
        else
            c = JZColor(at(2), at(4), at(6), at(0));   // #AARRGGBB
        return true;
    }
    return false;
}

// rgb(r,g,b) / rgba(r,g,b,a) / rgb(r g b / a) —— a 支持 0..1 小数与 0..255
bool parseRgbFunc(const std::string& s, JZColor& c)
{
    std::string t = lowerStr(s);
    bool hasAlpha = false;
    size_t lp = t.find('(');
    if (lp == std::string::npos || t[t.size() - 1] != ')')
        return false;
    if (t.compare(0, 4, "rgba") == 0)
        hasAlpha = true;
    else if (t.compare(0, 3, "rgb") != 0)
        return false;

    std::string inner = t.substr(lp + 1, t.size() - lp - 2);
    for (size_t i = 0; i < inner.size(); i++)
        if (inner[i] == ',' || inner[i] == '/')
            inner[i] = ' ';
    std::vector<std::string> tk = splitTokens(inner);
    if (tk.size() < 3)
        return false;

    int vals[4] = { 0, 0, 0, 255 };
    for (int i = 0; i < 3; i++)
        vals[i] = atoi(tk[i].c_str());
    if (tk.size() >= 4)
    {
        if (tk[3].find('.') != std::string::npos)
            vals[3] = (int)(atof(tk[3].c_str()) * 255.0 + 0.5);
        else
            vals[3] = atoi(tk[3].c_str());
    }
    (void)hasAlpha;
    c = JZColor((uint8_t)vals[0], (uint8_t)vals[1], (uint8_t)vals[2], (uint8_t)vals[3]);
    return true;
}

bool parseColorValue(const std::string& raw, JZColor& c)
{
    std::string s = lowerStr(trimStr(raw));
    if (s.empty())
        return false;
    if (s == "transparent" || s == "none")
    {
        c = JZColor(0, 0, 0, 0);
        return true;
    }
    if (parseHexColor(s, c))
        return true;
    if (parseRgbFunc(s, c))
        return true;

    // 少量常用命名色 (JZColor::ColorEnum 的子集)
    if (s == "white")     { c = JZColor(0xff, 0xff, 0xff); return true; }
    if (s == "black")     { c = JZColor(0, 0, 0); return true; }
    if (s == "red")       { c = JZColor(0xff, 0, 0); return true; }
    if (s == "green")     { c = JZColor(0, 0x80, 0); return true; }
    if (s == "blue")      { c = JZColor(0, 0, 0xff); return true; }
    if (s == "yellow")    { c = JZColor(0xff, 0xff, 0); return true; }
    if (s == "cyan")      { c = JZColor(0, 0xff, 0xff); return true; }
    if (s == "magenta")   { c = JZColor(0xff, 0, 0xff); return true; }
    if (s == "gray")      { c = JZColor(0x80, 0x80, 0x80); return true; }
    if (s == "lightgray") { c = JZColor(0xd3, 0xd3, 0xd3); return true; }
    if (s == "darkgray")  { c = JZColor(0xa9, 0xa9, 0xa9); return true; }
    if (s == "orange")    { c = JZColor(0xff, 0xa5, 0); return true; }
    return false;
}

bool parseLength(const std::string& raw, int& px)
{
    std::string s = trimStr(raw);
    if (s.empty())
        return false;
    if (s.size() > 2 && s.compare(s.size() - 2, 2, "px") == 0)
        s = s.substr(0, s.size() - 2);
    if (s.empty())
        return false;
    for (size_t i = 0; i < s.size(); i++)
    {
        if (!isdigit((unsigned char)s[i]) && s[i] != '.' && s[i] != '-')
            return false;
    }
    px = (int)(atof(s.c_str()) + 0.5);
    return true;
}

} // namespace

const std::vector<JZStylePropInfo>& JZStyleSet::propertyTable()
{
    using K = JZStyleValueKind;
    static const std::vector<JZStylePropInfo> table = {
        // ---- 已生效 ----
        { "background-color", K::Color,           true },
        { "background",       K::Color,           true },
        { "color",            K::Color,           true },
        { "border",           K::BorderShorthand, true },
        { "border-color",     K::Color,           true },
        { "border-width",     K::Length,          true },
        { "border-radius",    K::Length,          true },
        { "padding",          K::LengthBox,       true },
        { "padding-left",     K::Length,          true },
        { "padding-top",      K::Length,          true },
        { "padding-right",    K::Length,          true },
        { "padding-bottom",   K::Length,          true },

        // ---- 规划中: 能被解析但当前不生效 ----
        // 登记出来是为了让"能力查询 / 校验"能明确告诉使用者"先别写",
        // 而不是让使用者靠试错发现。
        { "border-left",   K::BorderShorthand, false },
        { "border-right",  K::BorderShorthand, false },
        { "border-top",    K::BorderShorthand, false },
        { "border-bottom", K::BorderShorthand, false },
        { "margin",        K::LengthBox,        false },
        { "margin-left",   K::Length,           false },
        { "margin-top",    K::Length,           false },
        { "margin-right",  K::Length,           false },
        { "margin-bottom", K::Length,           false },
        { "font-size",     K::Length,           false },
        { "font-weight",   K::Keyword,          false },
        { "font-family",   K::String,           false },
        { "text-align",    K::Keyword,          false },
        { "opacity",       K::Number,           false },
        { "line-height",   K::Length,           false },
    };
    return table;
}

const JZStylePropInfo* JZStyleSet::findProperty(const std::string& name)
{
    const std::vector<JZStylePropInfo>& t = propertyTable();
    for (size_t i = 0; i < t.size(); i++)
    {
        if (t[i].name == name)
            return &t[i];
    }
    return nullptr;
}

bool JZStyleSet::applyDecl(const std::string& property, const std::string& value)
{
    const JZStylePropInfo* info = findProperty(property);
    if (!info)            return false;   // 未知属性
    if (!info->implemented) return false;  // 已知但未实现

    std::string v = trimStr(value);

    switch (info->kind)
    {
    case JZStyleValueKind::Color:
    {
        JZColor c;
        if (!parseColorValue(v, c))
            return false;
        if (property == "color")
            color = JZOptional<JZColor>::Some(c);
        else if (property == "border-color")
            borderColor = JZOptional<JZColor>::Some(c);
        else
            background = JZOptional<JZColor>::Some(c);   // background / background-color
        return true;
    }
    case JZStyleValueKind::Length:
    {
        int n;
        if (!parseLength(v, n))
            return false;
        if (property == "border-width")        borderWidth = n;
        else if (property == "border-radius")  radius = n;
        else if (property == "padding-left")   paddingLeft = n;
        else if (property == "padding-top")    paddingTop = n;
        else if (property == "padding-right")  paddingRight = n;
        else if (property == "padding-bottom") paddingBottom = n;
        else
            return false;
        return true;
    }
    case JZStyleValueKind::LengthBox:
    {
        // 当前只有 padding 属于这一类
        if (property != "padding")
            return false;
        std::vector<std::string> tk = splitTokens(v);
        int n[4] = { 0, 0, 0, 0 };
        if (tk.size() < 1 || tk.size() > 4)
            return false;
        for (size_t i = 0; i < tk.size(); i++)
        {
            if (!parseLength(tk[i], n[i]))
                return false;
        }
        int top = n[0], right = n[0], bottom = n[0], left = n[0];
        if (tk.size() == 2)
        {
            left = right = n[1];
        }
        else if (tk.size() == 3)
        {
            left = right = n[1];
            bottom = n[2];
        }
        else if (tk.size() == 4)
        {
            right = n[1];
            bottom = n[2];
            left = n[3];
        }
        paddingTop = top;
        paddingRight = right;
        paddingBottom = bottom;
        paddingLeft = left;
        return true;
    }
    case JZStyleValueKind::BorderShorthand:
    {
        // 当前只有 border 属于这一类 (每边 border 尚未实现)
        if (property != "border")
            return false;
        // 简写: "<w>px [solid|none] <color>" —— 只取宽度与颜色, style 仅支持 solid/none
        if (lowerStr(v) == "none")
        {
            borderColor = JZOptional<JZColor>::None();
            borderWidth = 0;
            return true;
        }
        std::vector<std::string> tk = splitTokens(v);
        bool gotW = false, gotC = false;
        int w = borderWidth;
        JZColor c;
        for (size_t i = 0; i < tk.size(); i++)
        {
            if (!gotW && parseLength(tk[i], w))
            {
                gotW = true;
                continue;
            }
            if (!gotC && parseColorValue(tk[i], c))
                gotC = true;
        }
        if (!gotW && !gotC)
            return false;
        if (gotW)
            borderWidth = w;
        if (gotC)
            borderColor = JZOptional<JZColor>::Some(c);
        return true;
    }
    default:
        return false;   // Keyword / Number / String 当前都未实现
    }

    return false;
}
