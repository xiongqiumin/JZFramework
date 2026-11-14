#include "JZHttpCookie.h"
#include <cstdlib>
#include "JZStd.h"

JZHttpCookie::JZHttpCookie(const std::string& cookieName, const std::string& cookieValue)
{
    name = cookieName;
    value = cookieValue;
}

std::string JZHttpCookie::toHeaderValue() const
{
    std::string s = name + "=" + value;
    if (!path.empty())
        s += "; Path=" + path;
    if (!domain.empty())
        s += "; Domain=" + domain;
    if (!expires.empty())
        s += "; Expires=" + expires;
    if (maxAge >= 0)
        s += "; Max-Age=" + std::to_string(maxAge);
    if (httpOnly)
        s += "; HttpOnly";
    if (secure)
        s += "; Secure";
    if (!sameSite.empty())
        s += "; SameSite=" + sameSite;
    return s;
}

void JZHttpCookie::parseRequestHeader(const std::string& header,
                                      std::map<std::string, std::string>& out)
{
    // 形如 "a=1; b=2"; 以 ';' 分隔，每段 "name=value"（值里允许出现 '='）
    size_t pos = 0;
    while (pos < header.size())
    {
        size_t semi = header.find(';', pos);
        std::string pair = (semi == std::string::npos) ? header.substr(pos)
                                                       : header.substr(pos, semi - pos);
        pos = (semi == std::string::npos) ? header.size() : semi + 1;

        pair = str_trimmed(pair);
        if (pair.empty())
            continue;

        size_t eq = pair.find('=');
        std::string name = (eq == std::string::npos) ? pair : str_trimmed(pair.substr(0, eq));
        std::string value = (eq == std::string::npos) ? std::string()
                                                     : str_trimmed(pair.substr(eq + 1));
        if (name.empty())
            continue;

        if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
            value = value.substr(1, value.size() - 2);

        out[name] = value;   // 同名: 后出现的覆盖先出现的
    }
}
