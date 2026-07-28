#include <algorithm>
#include <climits>
#include <sstream>
#include <iomanip>

#include "JZUrl.h"
#include "JZStd.h"
#include "JZString.h"

static std::string urlEncode(const std::string& str)
{
    JZString js = JZString::fromStdString(str);

    std::stringstream ss;
    ss << std::hex << std::uppercase << std::setfill('0');

    // 2. 遍历每个字符（支持中文多字节）
    for (int i = 0; i < js.length(); i++) {
        JZChar c = js.at(i);

        // 如果是 ASCII 字符（英文/数字/符号）
        if (c.isAscii()) {
            char ch = c.toChar();

            if (isalnum(static_cast<unsigned char>(ch)) || 
                ch == '-' || ch == '_' || ch == '.' || ch == '~') {
                ss << ch;
            } 
            else if (ch == ' ') {
                ss << "%20";  // 空格 → %20
            } 
            else {
                // 特殊字符编码
                ss << "%" << std::setw(2) << static_cast<int>(static_cast<unsigned char>(ch));
            }
        } 
        // 3. 重点：中文（多字节 UTF-8）逐个字节编码！
        else {
            std::string utf8 = JZString(c).toStdString();
            for (unsigned char b : utf8) {
                ss << "%" << std::setw(2) << static_cast<int>(b);
            }
        }
    }

    return ss.str();
}

//JZUrlQuery
void JZUrlQuery::addQueryItem(const std::string& key, const std::string& value)
{
    m_queryItems[key] = value;
}

void JZUrlQuery::removeQueryItem(const std::string& key)
{
    map_remove(m_queryItems,key);
}

std::string JZUrlQuery::toString() const
{
    if (m_queryItems.empty())
        return "";

    std::stringstream ss;
    ss << "?";

    bool first = true;
    for (const auto& pair : m_queryItems) {
        if (!first)
            ss << "&";
        ss << pair.first << "=" << urlEncode(pair.second);
        first = false;
    }
    return ss.str();
}

void JZUrlQuery::clear()
{
    m_queryItems.clear();
}

void JZUrlQuery::fromString(const std::string& queryStr)
{
    clear();
    if (queryStr.empty())
        return;

    size_t start = 0;
    while (start < queryStr.size()) {
        size_t end = queryStr.find('&', start);
        std::string param = queryStr.substr(start, end - start);

        size_t eqPos = param.find('=');
        if (eqPos != std::string::npos) {
            std::string key = param.substr(0, eqPos);
            std::string value = param.substr(eqPos + 1);
            addQueryItem(key, value);
        }

        if (end == std::string::npos)
            break;
        start = end + 1;
    }
}

//JZUrl
JZUrl::JZUrl()
{ 
    m_port = 0; 
}

JZUrl::JZUrl(std::string original_url) 
{
    m_port = 0;
    parse_url(original_url);
}

void JZUrl::clear() { 
    m_scheme.clear();
    m_host.clear();
    m_port = 0;
}

std::string JZUrl::url() const
{
    std::stringstream ss;

    // 1. 协议
    if (!m_scheme.empty()) {
        ss << m_scheme << "://";
    }

    // 2. 主机
    ss << m_host;

    // 3. 端口（只显示非默认端口）
    if (m_port != 0) {
        bool is_default = (m_scheme == "http"  && m_port == 80) ||
                          (m_scheme == "https" && m_port == 443);
        if (!is_default) {
            ss << ":" << m_port;
        }
    }

    // 4. 路径（必须加！你之前缺的就是这个）
    // 空路径默认给 /，符合 URL 标准
    ss << (m_path.empty() ? "/" : m_path);

    // 5. 查询参数（已自动编码：空格→%20，中文→UTF8%XX，&→%26）
    ss << m_query.toString();

    return ss.str();
}

void JZUrl::setUrl(const std::string &url)
{
    m_port = 0;
    parse_url(url);
}

const std::string& JZUrl::scheme() const { return m_scheme; }

const std::string& JZUrl::host() const { return m_host; }

uint16_t JZUrl::port() const { return m_port; }

const std::string& JZUrl::path() const { return m_path; }

void JZUrl::parse_url(std::string url) 
{
    // 清空所有成员
    m_scheme.clear();
    m_host.clear();
    m_port = 0;
    m_path = "/";
    m_query.clear();

    // 1. 解析 scheme
    size_t scheme_end = url.find("://");
    if (scheme_end != std::string::npos) {
        m_scheme = url.substr(0, scheme_end);
        url = url.substr(scheme_end + 3);
    }

    // 2. 拆分 query
    size_t q_pos = url.find('?');
    std::string query_str;
    if (q_pos != std::string::npos) {
        query_str = url.substr(q_pos + 1);
        url = url.substr(0, q_pos);
    }

    // ==============================
    // 核心：先把 host:port 切出来
    // ==============================
    size_t first_slash = url.find('/');
    std::string host_port;

    if (first_slash == std::string::npos) {
        host_port = url;
        m_path = "/";
    } else {
        host_port = url.substr(0, first_slash);
        m_path = url.substr(first_slash);
    }

    // ==============================
    // 解析端口：从后往前找最后一个冒号
    // ==============================
    size_t colon = host_port.find_last_of(':');  // 👈 关键！
    if (colon != std::string::npos) {
        // 提取端口
        std::string port_str = host_port.substr(colon + 1);
        m_port = (uint16_t)std::stoi(port_str);

        // 提取主机
        m_host = host_port.substr(0, colon);
    } else {
        m_host = host_port;
    }

    // 设置默认端口
    set_default_port();

    // 解析参数
    m_query.fromString(query_str);
}

bool JZUrl::isDefaultPort() const {
    if (m_scheme == "http"  && m_port == 80)  return true;
    if (m_scheme == "https" && m_port == 443) return true;
    if (m_scheme == "ftp"   && m_port == 21)  return true;
    return false;
}

void JZUrl::set_default_port() 
{
    if (m_port != 0)
        return;

    if (m_scheme == "https") {
        m_port = 443;
    } else if (m_scheme == "http") {
        m_port = 80;
    } else if (m_scheme == "wss") {
        m_port = 443;
    } else if (m_scheme == "ws") {
        m_port = 80;
    } else if (m_scheme == "ftp") {
        m_port = 21;
    } else {
        clear();
    }
}

const JZUrlQuery &JZUrl::query() const
{
    return m_query;
}

void JZUrl::setQuery(const JZUrlQuery& query)
{
    m_query = query;
}