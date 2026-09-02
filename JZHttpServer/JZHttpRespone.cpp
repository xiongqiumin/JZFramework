#include "JZHttpRespone.h"
#include <sstream>
#include "JZStd.h"

JZHttpRespone::JZHttpRespone()
    : m_statusCode(200)
    , m_statusText("OK")
{
    setHeader("Server", "JZHttpServer");
}

JZHttpRespone::~JZHttpRespone()
{
}

int JZHttpRespone::statusCode() const
{
    return m_statusCode;
}

void JZHttpRespone::setStatusCode(int code)
{
    m_statusCode = code;
    m_statusText = getStatusText(code);
}

const std::string& JZHttpRespone::statusText() const
{
    return m_statusText;
}

void JZHttpRespone::setStatusText(const std::string& text)
{
    m_statusText = text;
}

const std::string& JZHttpRespone::header(const std::string& key) const
{
    static const std::string empty;
    // HTTP 头名大小写不敏感 (RFC 7230)，统一以小写 key 查找
    auto it = m_headers.find(str_toLower(key));
    if (it != m_headers.end())
        return it->second;
    return empty;
}

void JZHttpRespone::setHeader(const std::string& key, const std::string& value)
{
    m_headers[str_toLower(key)] = value;
}

void JZHttpRespone::removeHeader(const std::string& key)
{
    m_headers.erase(str_toLower(key));
}

const std::map<std::string, std::string>& JZHttpRespone::headers() const
{
    return m_headers;
}

const JZByteArray& JZHttpRespone::body() const
{
    return m_body;
}

void JZHttpRespone::setBody(const JZByteArray& body)
{
    m_body = body;
}

void JZHttpRespone::setBody(const std::string& body)
{
    m_body = JZByteArray(body.c_str(), body.size());
}

const std::string& JZHttpRespone::contentType() const
{
    return header("Content-Type");
}

void JZHttpRespone::setContentType(const std::string& type)
{
    setHeader("Content-Type", type);
}

void JZHttpRespone::setText(const std::string& text)
{
    setContentType("text/plain; charset=utf-8");
    setBody(text);
}

void JZHttpRespone::setHtml(const std::string& html)
{
    setContentType("text/html; charset=utf-8");
    setBody(html);
}

void JZHttpRespone::setJson(const std::string& json)
{
    setContentType("application/json; charset=utf-8");
    setBody(json);
}

void JZHttpRespone::setFile(const JZByteArray& data, const std::string& filename)
{
    setContentType("application/octet-stream");
    setHeader("Content-Disposition", "attachment; filename=\"" + filename + "\"");
    setBody(data);
}

JZByteArray JZHttpRespone::toHeadByteArray(int contentLength) const
{
    std::ostringstream oss;
    oss << "HTTP/1.1 " << m_statusCode << " " << m_statusText << "\r\n";

    for (const auto& pair : m_headers)
    {
        oss << pair.first << ": " << pair.second << "\r\n";
    }

    oss << "Content-Length: " << contentLength << "\r\n";
    oss << "\r\n";

    std::string headerStr = oss.str();
    return JZByteArray(headerStr.c_str(), (int)headerStr.size());
}

JZByteArray JZHttpRespone::toByteArray() const
{
    JZByteArray result = toHeadByteArray((int)m_body.size());
    result.append(m_body);

    return result;
}

std::string JZHttpRespone::getStatusText(int code)
{
    switch (code)
    {
        case 100: return "Continue";
        case 101: return "Switching Protocols";
        case 200: return "OK";
        case 201: return "Created";
        case 202: return "Accepted";
        case 203: return "Non-Authoritative Information";
        case 204: return "No Content";
        case 205: return "Reset Content";
        case 206: return "Partial Content";
        case 300: return "Multiple Choices";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 303: return "See Other";
        case 304: return "Not Modified";
        case 305: return "Use Proxy";
        case 307: return "Temporary Redirect";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 402: return "Payment Required";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 406: return "Not Acceptable";
        case 407: return "Proxy Authentication Required";
        case 408: return "Request Timeout";
        case 409: return "Conflict";
        case 410: return "Gone";
        case 411: return "Length Required";
        case 412: return "Precondition Failed";
        case 413: return "Request Entity Too Large";
        case 414: return "Request-URI Too Long";
        case 415: return "Unsupported Media Type";
        case 416: return "Requested Range Not Satisfiable";
        case 417: return "Expectation Failed";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        case 505: return "HTTP Version Not Supported";
        default: return "Unknown";
    }
}
