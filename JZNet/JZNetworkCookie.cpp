#include "JZNetworkCookie.h"
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include "JZStd.h"
#include "JZUrl.h"

namespace
{

// 解析 HTTP-date（只认 IMF-fixdate: "Wed, 21 Oct 2015 07:28:00 GMT"）。
// 解析失败返回 0；其它历史格式当作"没有过期时间"(会话 cookie)，而不是当成已过期。
int64_t parseHttpDateMs(const std::string& s)
{
    char wday[16] = { 0 };
    char mon[16] = { 0 };
    int day = 0, year = 0, hh = 0, mm = 0, ss = 0;
    if (sscanf(s.c_str(), "%15[^,], %d %15s %d %d:%d:%d", wday, &day, mon, &year, &hh, &mm, &ss) != 7)
        return 0;

    static const char* months[] = { "jan", "feb", "mar", "apr", "may", "jun",
                                    "jul", "aug", "sep", "oct", "nov", "dec" };
    std::string lm = str_toLower(mon);
    int month = -1;
    for (int i = 0; i < 12; i++)
    {
        if (lm == months[i])
        {
            month = i;
            break;
        }
    }
    if (month < 0)
        return 0;

    std::tm tm = {};
    tm.tm_year = year - 1900;
    tm.tm_mon = month;
    tm.tm_mday = day;
    tm.tm_hour = hh;
    tm.tm_min = mm;
    tm.tm_sec = ss;

#ifdef _WIN32
    time_t t = _mkgmtime(&tm);
#else
    time_t t = timegm(&tm);
#endif
    if (t == (time_t)-1)
        return 0;
    return (int64_t)t * 1000;
}

// 解析一条 Set-Cookie。outRemoved=true 表示这条是"删除"(Max-Age<=0 / Expires 已过)。
bool parseSetCookie(const std::string& header, const std::string& host,
                    int64_t nowMs, JZNetworkCookie& out, bool& outRemoved)
{
    outRemoved = false;

    size_t semi = header.find(';');
    std::string pair = (semi == std::string::npos) ? header : header.substr(0, semi);
    size_t eq = pair.find('=');
    if (eq == std::string::npos)
        return false;

    out.name = str_trimmed(pair.substr(0, eq));
    out.value = str_trimmed(pair.substr(eq + 1));
    if (out.name.empty())
        return false;

    // 没有 Domain 属性时是 host-only，只回给同一个 host
    out.domain = str_toLower(host);
    out.hostOnly = true;
    out.path = "/";
    out.secure = false;
    out.httpOnly = false;
    out.expiresAt = 0;

    bool hasMaxAge = false;
    long long maxAge = 0;
    bool hasExpires = false;
    int64_t expiresAt = 0;

    size_t pos = (semi == std::string::npos) ? header.size() : semi + 1;
    while (pos < header.size())
    {
        size_t next = header.find(';', pos);
        std::string attr = (next == std::string::npos) ? header.substr(pos)
                                                       : header.substr(pos, next - pos);
        pos = (next == std::string::npos) ? header.size() : next + 1;

        attr = str_trimmed(attr);
        if (attr.empty())
            continue;

        size_t aeq = attr.find('=');
        std::string key = str_toLower(str_trimmed((aeq == std::string::npos) ? attr : attr.substr(0, aeq)));
        std::string val = (aeq == std::string::npos) ? std::string() : str_trimmed(attr.substr(aeq + 1));

        if (key == "domain" && !val.empty())
        {
            if (val[0] == '.')
                val = val.substr(1);
            val = str_toLower(val);
            // 只接受本 host 或其父域(防伪造其它域的 cookie)
            if (out.domain == val)
            {
                out.domain = val;
                out.hostOnly = false;
            }
            else if (out.domain.size() > val.size() + 1 &&
                     out.domain.compare(out.domain.size() - val.size(), val.size(), val) == 0 &&
                     out.domain[out.domain.size() - val.size() - 1] == '.')
            {
                out.domain = val;
                out.hostOnly = false;
            }
            else
            {
                return false;   // Domain 与当前 host 无关: 丢弃
            }
        }
        else if (key == "path" && !val.empty())
        {
            out.path = val;
        }
        else if (key == "max-age")
        {
            hasMaxAge = true;
            maxAge = atoll(val.c_str());
        }
        else if (key == "expires")
        {
            hasExpires = true;
            expiresAt = parseHttpDateMs(val);
        }
        else if (key == "secure")
        {
            out.secure = true;
        }
        else if (key == "httponly")
        {
            out.httpOnly = true;
        }
    }

    // Max-Age 优先于 Expires(RFC 6265 5.3)
    if (hasMaxAge)
    {
        if (maxAge <= 0)
        {
            outRemoved = true;
            return true;
        }
        out.expiresAt = nowMs + maxAge * 1000;
    }
    else if (hasExpires && expiresAt > 0)
    {
        out.expiresAt = expiresAt;
        if (expiresAt <= nowMs)
            outRemoved = true;
    }

    return true;
}

bool sameCookieSlot(const JZNetworkCookie& a, const JZNetworkCookie& b)
{
    return a.name == b.name && a.domain == b.domain && a.path == b.path;
}

}   // namespace

bool JZNetworkCookie::isExpired(int64_t nowMs) const
{
    return expiresAt > 0 && expiresAt <= nowMs;
}

bool JZNetworkCookie::matches(const std::string& scheme, const std::string& host,
                              const std::string& reqPath) const
{
    if (secure && scheme != "https")
        return false;

    std::string h = str_toLower(host);
    if (hostOnly)
    {
        if (h != domain)
            return false;
    }
    else
    {
        if (h != domain)
        {
            if (h.size() <= domain.size())
                return false;
            if (h.compare(h.size() - domain.size(), domain.size(), domain) != 0)
                return false;
            if (h[h.size() - domain.size() - 1] != '.')
                return false;
        }
    }

    // 路径前缀匹配(RFC 6265 5.1.4)
    std::string rp = reqPath.empty() ? std::string("/") : reqPath;
    if (rp.compare(0, path.size(), path) != 0)
        return false;
    if (rp.size() == path.size() || path == "/")
        return true;
    return rp[path.size()] == '/';
}

void JZNetworkCookieJar::clear()
{
    m_cookies.clear();
}

void JZNetworkCookieJar::purgeExpired(int64_t nowMs)
{
    for (size_t i = 0; i < m_cookies.size();)
    {
        if (m_cookies[i].isExpired(nowMs))
            m_cookies.erase(m_cookies.begin() + i);
        else
            i++;
    }
}

void JZNetworkCookieJar::saveFromResponse(const JZUrl& url, const std::vector<std::string>& setCookieHeaders,
                                          int64_t nowMs)
{
    purgeExpired(nowMs);

    for (size_t i = 0; i < setCookieHeaders.size(); i++)
    {
        JZNetworkCookie c;
        bool removed = false;
        if (!parseSetCookie(setCookieHeaders[i], url.host(), nowMs, c, removed))
            continue;

        size_t j = 0;
        while (j < m_cookies.size() && !sameCookieSlot(m_cookies[j], c))
            j++;

        if (removed)
        {
            if (j < m_cookies.size())
                m_cookies.erase(m_cookies.begin() + j);
            continue;
        }

        if (j < m_cookies.size())
            m_cookies[j] = c;      // 同名同域同路径: 覆盖
        else
            m_cookies.push_back(c);
    }
}

std::string JZNetworkCookieJar::headerFor(const JZUrl& url, int64_t nowMs)
{
    purgeExpired(nowMs);

    std::string result;
    for (size_t i = 0; i < m_cookies.size(); i++)
    {
        const JZNetworkCookie& c = m_cookies[i];
        if (!c.matches(url.scheme(), url.host(), url.path()))
            continue;
        if (!result.empty())
            result += "; ";
        result += c.name + "=" + c.value;
    }
    return result;
}
