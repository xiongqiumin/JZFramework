#include <string>
#include "JZHttpRequest.h"
#include "mongoose.h"
#include "JZDebug.h"

namespace
{
// URL 解码: 直接复用 mongoose 的 mg_url_decode(is_form_url_encoded=1 时 '+' -> 空格)。
// 解码后长度不会超过原串, dst 缓冲按 src_len+1 分配即可。
std::string urlDecode(const std::string& s)
{
    std::string out(s.size() + 1, '\0');
    int n = mg_url_decode(s.data(), s.size(), &out[0], out.size(), 1);
    if (n < 0)
    {
        jzWarning() << "url" << s << "decode failed";
        return std::string();   // 非法序列: 原样返回
    }

    out.resize((size_t)n);
    return out;
}

// URL 编码: 直接复用 mongoose 的 mg_url_encode(非 url-safe 字符统一编为 %XX)。
std::string urlEncode(const std::string& s)
{
    // 编码后最长 3 倍 + 1, mg_url_encode 内部会在溢出时返回 0(不写入), 故预留余量。
    std::string out(s.size() * 3 + 1, '\0');
    size_t n = mg_url_encode(s.data(), s.size(), &out[0], out.size());
    out.resize(n);
    return out;
}

}   // namespace

JZHttpRequest::JZHttpRequest()
    : m_method(UNKNOWN)
{
}

JZHttpRequest::~JZHttpRequest()
{
}

JZHttpRequest::Method JZHttpRequest::method() const
{
    return m_method;
}

void JZHttpRequest::setMethod(Method method)
{
    m_method = method;
    m_methodString = methodToString(method);
}

const std::string& JZHttpRequest::methodString() const
{
    return m_methodString;
}

void JZHttpRequest::setMethodString(const std::string& methodString)
{
    m_methodString = methodString;
    m_method = stringToMethod(methodString);
}

const std::string& JZHttpRequest::path() const
{
    return m_path;
}

void JZHttpRequest::setPath(const std::string& path)
{
    m_path = path;
}

std::string JZHttpRequest::query() const
{
    // 不存储原始 query 串, 直接由参数表拼接还原。
    std::string result;
    bool first = true;
    for (const auto& kv : m_queryParams)
    {
        if (!first)
            result += '&';
        first = false;
        result += urlEncode(kv.first);
        result += '=';
        result += urlEncode(kv.second);
    }
    return result;
}

void JZHttpRequest::setQuery(const std::string& query)
{
    // query 串一旦被设置立即重新解析并**覆盖**整个参数表, 保证
    // queryParam()/queryParams() 始终可用(调用方无需自行解析 query 串)。
    m_queryParams.clear();
    size_t pos = 0;
    while (pos < query.size())
    {
        size_t amp = query.find('&', pos);
        std::string kv = (amp == std::string::npos) ? query.substr(pos)
                                                    : query.substr(pos, amp - pos);
        if (!kv.empty())
        {
            size_t eq = kv.find('=');
            std::string key = urlDecode((eq == std::string::npos) ? kv : kv.substr(0, eq));
            std::string val = (eq == std::string::npos) ? std::string()
                                                        : urlDecode(kv.substr(eq + 1));
            m_queryParams[key] = val;    // 同名参数: 后者覆盖前者
        }
        if (amp == std::string::npos)
            break;
        pos = amp + 1;
    }
}

const std::string& JZHttpRequest::header(const std::string& key) const
{
    static const std::string empty;
    auto it = m_headers.find(key);
    if (it != m_headers.end())
        return it->second;
    return empty;
}

void JZHttpRequest::setHeader(const std::string& key, const std::string& value)
{
    m_headers[key] = value;
}

const std::map<std::string, std::string>& JZHttpRequest::headers() const
{
    return m_headers;
}

const std::string& JZHttpRequest::param(const std::string& key) const
{
    static const std::string empty;
    auto it = m_params.find(key);
    if (it != m_params.end())
        return it->second;
    return empty;
}

void JZHttpRequest::setParam(const std::string& key, const std::string& value)
{
    m_params[key] = value;
}

const std::map<std::string, std::string>& JZHttpRequest::params() const
{
    return m_params;
}

const std::string& JZHttpRequest::queryParam(const std::string& key) const
{
    static const std::string empty;
    auto it = m_queryParams.find(key);
    if (it != m_queryParams.end())
        return it->second;
    return empty;
}

void JZHttpRequest::setQueryParam(const std::string& key, const std::string& value)
{
    m_queryParams[key] = value;
}

const std::map<std::string, std::string>& JZHttpRequest::queryParams() const
{
    return m_queryParams;
}

const JZByteArray& JZHttpRequest::body() const
{
    return m_body;
}

void JZHttpRequest::setBody(const JZByteArray& body)
{
    m_body = body;
}

const std::string& JZHttpRequest::contentType() const
{
    return header("Content-Type");
}

const std::string& JZHttpRequest::userAgent() const
{
    return header("User-Agent");
}

const std::string& JZHttpRequest::remoteAddr() const
{
    return m_remoteAddr;
}

void JZHttpRequest::setRemoteAddr(const std::string& addr)
{
    m_remoteAddr = addr;
}

JZHttpRequest::Method JZHttpRequest::stringToMethod(const std::string& method)
{
    std::string upperMethod = method;
    std::transform(upperMethod.begin(), upperMethod.end(), upperMethod.begin(), ::toupper);

    if (upperMethod == "GET")
        return GET;
    else if (upperMethod == "POST")
        return POST;
    else if (upperMethod == "PUT")
        return PUT;
    else if (upperMethod == "DELETE")
        return DELETE;
    else if (upperMethod == "HEAD")
        return HEAD;
    else if (upperMethod == "OPTIONS")
        return OPTIONS;
    else if (upperMethod == "PATCH")
        return PATCH;
    else
        return UNKNOWN;
}

std::string JZHttpRequest::methodToString(Method method)
{
    switch (method)
    {
        case GET: return "GET";
        case POST: return "POST";
        case PUT: return "PUT";
        case DELETE: return "DELETE";
        case HEAD: return "HEAD";
        case OPTIONS: return "OPTIONS";
        case PATCH: return "PATCH";
        default: return "UNKNOWN";
    }
}
