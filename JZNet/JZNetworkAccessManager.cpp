#include "mongoose.h"
#include "JZNetworkAccessManager.h"
#include "JZStd.h"
#include "JZTx.h"
#include "JZDebug.h"
#include "JZByteArray.h"

#include <cctype>

static std::string toLowerStr(const std::string &s)
{
    std::string r = s;
    for (char &c : r)
        c = (char)std::tolower((unsigned char)c);
    return r;
}

static std::string trimStr(const std::string &s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos)
        return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

//from mongoose
static bool is_hex_digit(int c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
        (c >= 'A' && c <= 'F');
}

static int skip_chunk(const char* buf, int len, int* pl, int* dl) {
    int i = 0, n = 0;
    if (len < 3) return 0;
    while (i < len && is_hex_digit(buf[i])) i++;
    if (i == 0) return -1;                     // Error, no length specified
    if (i > (int)sizeof(int) * 2) return -1;  // Chunk length is too big
    if (len < i + 1 || buf[i] != '\r' || buf[i + 1] != '\n') return -1;  // Error
    if (mg_str_to_num(mg_str_n(buf, (size_t)i), 16, &n, sizeof(int)) == false)
        return -1;                    // Decode chunk length, overflow
    if (n < 0) return -1;           // Error. TODO(): some checks now redundant
    if (n > len - i - 4) return 0;  // Chunk not yet fully buffered
    if (buf[i + n + 2] != '\r' || buf[i + n + 3] != '\n') return -1;  // Error
    *pl = i + 2, * dl = n;
    return i + 2 + n + 2;
}

//JZNetworkConnect
JZNetworkConnect::JZNetworkConnect(JZNetworkAccessManager *manager)
    :JZNetObject(manager), sigDisconnected(this)
{  
    m_conn = INVALID_NET_HANDLE;
    m_reply = nullptr;
    m_recvTime = 0;
    m_timeId = INVALID_TIMER_ID;
    m_chunkMode = false;
    m_closing = false;
    m_manager = manager;
}

JZNetworkConnect::~JZNetworkConnect()
{
    close();
}

std::string JZNetworkConnect::url()
{
    return m_url;
}

// 构造发给 HTTP 代理的 CONNECT 请求：明文建立到真实目标的 TCP 隧道（代理只搬字节，不解密）
std::string JZNetworkConnect::buildConnectRequest()
{
    std::string hostport = m_targetHost + ":" + std::to_string(m_targetPort);
    std::string req = "CONNECT " + hostport + " HTTP/1.1\r\n";
    req += "Host: " + hostport + "\r\n";
    if (!m_proxyAuth.empty())
        req += "Proxy-Authorization: " + m_proxyAuth + "\r\n";
    req += "\r\n";
    return req;
}

void JZNetworkConnect::open(const std::string& url)
{
    JZSocketInfo info;
    if (m_isHttpsProxy)
    {
        JZUrl url_info(url);
        info.type = JZSocketInfo::HttpClientHttpsProxy;
        info.url = url_info.host() + ":" + std::to_string(url_info.port());
    }
    else
    {
        info.type = JZSocketInfo::HttpClient;
        info.url = url;
    }

    m_url = url;
    openConn(info);
    m_timeId = startTimer(200);
    m_recvTime = JZPlatform::getTickCountMs();
}

void JZNetworkConnect::reopen()
{
    resetConnection();

    JZNetReqEvent* req = new JZNetReqEvent();
    req->netCmd = JZNetReqEvent::Net_HttpClientReopen;
    req->conn = m_conn;
    if (m_isHttpsProxy && m_proxyPhase == ProxyPhaseConnectSent)
    {
        JZ_ASSERT(0);
    }
    else
    {
        req->buffer = m_reply->m_send;
    }
    postReqEvent(req);

    if(m_timeId != INVALID_TIMER_ID)
        m_timeId = startTimer(200);

    m_recvTime = JZPlatform::getTickCountMs();
}

void JZNetworkConnect::resetConnection()
{
    // keep-alive：清空上一次请求/响应遗留的状态，使连接可复用
    m_chunkMode = false;
    m_chunkBuffer.clear();
    m_closing = false;
    // 断开/复用前重置下行限速, 避免限速残留到连接复用
    setReadLimit(0);
}

void JZNetworkConnect::closeConnection()
{
    m_closing = true;
    if (m_conn != INVALID_NET_HANDLE)
    {
        closeConn(false);
        m_conn = INVALID_NET_HANDLE;
    }
}

void JZNetworkConnect::close()
{
    if (m_reply)
        emitFinish(JZNetworkReply::ConnectError);
    else
        closeConnection();
}

JZByteArray JZNetworkConnect::getHttpBody(const JZByteArray& data)
{
    JZTX_FUNC_TRACE
    // HTTP 头和 Body 的分隔符：\r\n\r\n
    const char separator[] = "\r\n\r\n";
    const size_t sep_len = sizeof(separator) - 1;  // 固定 4 字节

    // 查找分隔符位置
    auto sep_pos = std::search(data.begin(), data.end(), separator, separator + sep_len);

    // 没找到分隔符 → 返回空
    if (sep_pos == data.end())
    {
        return JZByteArray();
    }

    // 分隔符之后的所有内容 = Body
    sep_pos += sep_len;
    size_t body_len = data.end() - sep_pos;
    return JZByteArray(sep_pos, body_len);
}

void JZNetworkConnect::emitFinish(JZNetworkReply::NetworkError error)
{
    if (m_reply)
    {
        m_reply->m_conn = nullptr;
        auto reply = m_reply;
        m_reply = nullptr;
        reply->emitFinish(error);
    }

    if (m_timeId != INVALID_TIMER_ID)
    {
        killTimer(m_timeId);
        m_timeId = INVALID_TIMER_ID;
    }

    resetConnection();

    // 复用前重置空闲回收定时器
    if (m_timeId != INVALID_TIMER_ID)
        killTimer(m_timeId);
    m_timeId = startTimer((int)m_manager->m_connectionIdleTimeoutMs);
    m_recvTime = JZPlatform::getTickCountMs();
}

void JZNetworkConnect::timerEvent(JZTimerEvent* event)
{
    if (!m_reply)
    {
        // keep-alive 空闲连接：超过空闲超时则回收
        if (JZPlatform::getTickCountMs() - m_recvTime >= m_manager->m_connectionIdleTimeoutMs)
            close();
        return;
    }

    if (m_reply->m_timeOut != -1 && JZPlatform::getTickCountMs() - m_recvTime > m_reply->m_timeOut)
    {
        emitFinish(JZNetworkReply::TimeOutError);
    }
}

void JZNetworkConnect::appendChunk(const JZByteArray& data)
{
    m_chunkBuffer.append(data);

    const char* s = m_chunkBuffer.data();
    int buf_len = m_chunkBuffer.size();

    int ofs = 0;
    int cl = 0;
    do
    {
        int o = 0, pl, dl, len = buf_len;
        bool has_data = false;

        // Find zero-length chunk (the end of the body)
        while ((cl = skip_chunk(s + o, len - o, &pl, &dl)) > 0 && dl)
        {
            // 取出当前chunk纯数据，追加到m_body
            const char* data_ptr = s + ofs + o + pl;
            m_reply->m_body.append(data_ptr, dl);
            has_data = true;

            o += cl;
        }
        ofs += o;
        if (has_data)
            m_reply->sigReadyRead.emit();

        if (cl == 0)
        {
            break;  // 数据尚未完整，等待更多数据
        }
        if (cl < 0) {
            m_chunkBuffer.remove(0, ofs);
            emitFinish(JZNetworkReply::ConnectError);
            closeConn(true);
            return;
        }

        // 命中0长度结束块 dl=0，把结束块字节也计入ofs
        ofs += cl;
        // 先裁剪已消费数据，再结束（keep-alive 时 emitFinish 会清空缓冲）
        m_chunkBuffer.remove(0, ofs);
        emitFinish(JZNetworkReply::NoError);
        return;
    } while (0);

    m_chunkBuffer.remove(0, ofs);
}

void JZNetworkConnect::onNetEvent(JZNetResEvent* event)
{
    JZTX_FUNC_TRACE

    int ev = event->event;
    if (ev == JZ_NET_EV_CLOSE)
    {
        m_closing = true;
        if (m_conn != INVALID_NET_HANDLE)
        {
            m_conn = INVALID_NET_HANDLE;
            emitFinish(JZNetworkReply::ConnectError);
            sigDisconnected.emit();
        }
        deleteLater();
        return;
    }
    if (!m_reply)
        return;

    if (ev == JZ_NET_EV_HTTP_MSG)
    {
        JZHttpResData *http = (JZHttpResData*)(event->extData.get());
        m_reply->m_statusCode = http->httpCode;
        m_reply->m_statusText = http->httpCodeText;
        m_reply->m_body = getHttpBody(event->binData);

        // event->binData 为完整 HTTP 报文，仅解析一次：提取 head / Location / 所有头
        std::string location;
        struct mg_http_message hm;
        if (mg_http_parse(event->binData.data(), (size_t)event->binData.size(), &hm) <= 0)
        {
            // 报文解析失败：连接异常，关闭并上报错误
            emitFinish(JZNetworkReply::ConnectError);
            closeConn(true);
            return;
        }

        m_reply->m_head.assign(hm.head.buf, hm.head.len);

        m_reply->m_headers.clear();
        for (int i = 0; i < MG_MAX_HTTP_HEADERS; i++)
        {
            struct mg_str *name = &hm.headers[i].name;
            if (name->len == 0)
                break;   // 头数组以空 name 结尾
            std::string key = toLowerStr(std::string(name->buf, name->len));
            std::string val = trimStr(std::string(hm.headers[i].value.buf, hm.headers[i].value.len));
            m_reply->m_headers[key] = val;
        }

        struct mg_str *loc = mg_http_get_header(&hm, "Location");
        if (loc)
            location.assign(loc->buf, loc->len);

        // 3xx 重定向
        bool isRedirect = (http->httpCode == 301 || http->httpCode == 302 ||
                           http->httpCode == 303 || http->httpCode == 307 ||
                           http->httpCode == 308);
        if (isRedirect && !location.empty())
        {
            if (m_manager->followRedirect(m_reply, location))
                return;   // 已发起重定向请求，不结束 reply
        }

        if (http->httpCode >= 200 && http->httpCode < 300)
        {
            m_reply->sigReadyRead.emit();
            emitFinish(JZNetworkReply::NoError);
        }
        else
        {
            emitFinish(JZNetworkReply::HttpError);
        }
    }
    else if (ev == JZ_NET_EV_HTTP_CHUNK_MODE)
    {
        m_chunkMode = true;

        JZHttpResData* http = (JZHttpResData*)(event->extData.get());
        m_reply->m_statusCode = http->httpCode;
        m_reply->m_statusText = http->httpCodeText;

        if(event->binData.size() > 0)
            appendChunk(event->binData);
    }
    else if (ev == JZ_NET_EV_CONNECT)
    {
        if (m_isHttpsProxy && m_proxyPhase == ProxyPhaseConnectSent)
        {
            // 连上代理后，先发送 CONNECT 建立隧道，暂不发真实 https 请求
            std::string connectReq = buildConnectRequest();
            writeConn(WriteBin, connectReq.data(), (int)connectReq.size());
        }
        else
        {
            writeConn(WriteBin, m_reply->m_send.data(), m_reply->m_send.size());
        }
    }
    else if (ev == JZ_NET_EV_TLS_HS)
    {
        // 隧道 TLS 握手完成：此时才在加密隧道上发送真正的 https 请求
        if (m_isHttpsProxy)
        {
            m_proxyPhase = ProxyPhaseTunnelUp;
            writeConn(WriteBin, m_reply->m_send.data(), m_reply->m_send.size());
        }
    }
    else if (ev == JZ_NET_EV_ERROR)
    {
        emitFinish(JZNetworkReply::ConnectError);
    }
    else if (ev == JZ_NET_EV_READ)
    {
        m_recvTime = JZPlatform::getTickCountMs();
        if (m_isHttpsProxy && m_proxyPhase == ProxyPhaseConnectSent)
        {
            m_httpsProxyBuffer.append(event->binData);

            mg_http_message msg;
            int n = mg_http_parse(m_httpsProxyBuffer.data(), m_httpsProxyBuffer.size(), &msg);
            if (n < 0)
            {
                emitFinish(JZNetworkReply::ConnectError);
                closeConn(true);
            }
            else if(n > 0)
            {
                int httpCode = mg_http_status(&msg);
                if (httpCode == 200)
                {
                    JZNetReqEvent* tls_event = new JZNetReqEvent();
                    tls_event->netCmd = JZNetReqEvent::Net_HttpClientProxyTlsInit;
                    tls_event->tlsHost = m_targetHost;
                    tls_event->conn = m_conn;
                    postReqEvent(tls_event);
                }
                else
                {
                    emitFinish(JZNetworkReply::ConnectError);
                    closeConn(true);
                }
            }
        }
        else if (m_chunkMode && event->binData.size() > 0)
            appendChunk(event->binData);
    }
    else if (ev == JZ_NET_EV_WRITE)
    {
    }
}

//JZNetworkAccessManager
JZNetworkAccessManager::JZNetworkAccessManager(JZObject *parent)
    :JZObject(parent)
{
    m_maxConnectionsPerHost = 6;
    m_connectionIdleTimeoutMs = 30 * 1000;
}

JZNetworkAccessManager::~JZNetworkAccessManager()
{
    // 显式关闭所有池中连接，避免依赖子对象析构顺序
    // 先快照并清空原容器：close() 可能经 sigDisconnected → onDisconnected
    // 修改 m_netConnects，清空后其查找不会再命中，避免遍历中误改
    std::vector<JZNetworkConnect*> connects = m_netConnects;
    m_netConnects.clear();
    for (JZNetworkConnect* conn : connects)
        conn->close();
}

void JZNetworkAccessManager::setMaxConnectionsPerHost(int n)
{
    m_maxConnectionsPerHost = n;
}

int JZNetworkAccessManager::maxConnectionsPerHost() const
{
    return m_maxConnectionsPerHost;
}

void JZNetworkAccessManager::setConnectionIdleTimeout(int64_t ms)
{
    m_connectionIdleTimeoutMs = ms;
}

int64_t JZNetworkAccessManager::connectionIdleTimeout() const
{
    return m_connectionIdleTimeoutMs;
}

void JZNetworkAccessManager::setSsl(const JZSSLConfig &ssl)
{
    m_ssl = ssl;
}

void JZNetworkAccessManager::setProxy(const JZProxyConfig &proxy)
{
    m_proxyConfig = proxy;
}

const JZOptional<JZProxyConfig> &JZNetworkAccessManager::proxy() const
{
    return m_proxyConfig;
}

// 从代理 URL 解析: http://[user:pass@]host:port（scheme 与 userinfo 均可选）
JZProxyConfig JZProxyConfig::fromUrl(std::string url)
{
    JZProxyConfig cfg;
    std::string rest = url;
    size_t p = rest.find("://");
    if (p != std::string::npos)
        rest = rest.substr(p + 3);

    std::string auth, hostport;
    size_t at = rest.find('@');
    if (at != std::string::npos)
    {
        auth = rest.substr(0, at);
        hostport = rest.substr(at + 1);
    }
    else
    {
        hostport = rest;
    }

    size_t colon = hostport.rfind(':');
    if (colon != std::string::npos && colon + 1 < hostport.size())
    {
        cfg.host = hostport.substr(0, colon);
        try
        {
            cfg.port = std::stoi(hostport.substr(colon + 1));
        }
        catch (...)
        {
            cfg.host.clear();
            cfg.port = 0;
        }
    }

    if (!auth.empty())
    {
        size_t ac = auth.find(':');
        if (ac != std::string::npos)
        {
            cfg.user = auth.substr(0, ac);
            cfg.password = auth.substr(ac + 1);
        }
        else
        {
            cfg.user = auth;
        }
    }
    return cfg;
}

// 生成代理认证头值: "Basic <base64(user:password)>"，无用户名则返回空
static std::string makeProxyAuthHeader(const JZProxyConfig &p)
{
    if (p.user.empty())
        return std::string();
    JZByteArray cred = str_toUtf8(p.user + ":" + p.password);
    return "Basic " + str_fromUtf8(cred.toBase64());
}

// 是否经由 HTTP 代理转发：仅 http 目标且已配置代理时生效
static bool useHttpProxy(const JZOptional<JZProxyConfig> &p, const JZNetworkRequest &request)
{
    return p.isSome() && !p->host.empty() && request.url().scheme() == "http";
}

// 是否经由 HTTP 代理转发 https：配置代理且目标为 https 时，使用 CONNECT 隧道
static bool useHttpsProxy(const JZOptional<JZProxyConfig> &p, const JZNetworkRequest &request)
{
    return p.isSome() && !p->host.empty() && request.url().scheme() == "https";
}

void JZNetworkAccessManager::onDisconnected()
{
    auto conn = jzobject_cast<JZNetworkConnect*>(sender());
    for (int i = 0; i < (int)m_netConnects.size(); i++)
    {
        if (m_netConnects[i] == conn)
        {
            vec_removeAt(m_netConnects, i);
            break;
        }
    }
}

JZNetworkConnect* JZNetworkAccessManager::findIdleConnection(const std::string &hostKey)
{
    for (size_t i = 0; i < m_netConnects.size(); i++)
    {
        auto conn = m_netConnects[i];
        if (conn->m_hostKey == hostKey && conn->m_reply == nullptr && !conn->m_closing)
            return conn;
    }
    return nullptr;
}

int JZNetworkAccessManager::connectionCount(const std::string &hostKey)
{
    int count = 0;
    for (size_t i = 0; i < m_netConnects.size(); i++)
    {
        if (m_netConnects[i]->m_hostKey == hostKey)
            count++;
    }
    return count;
}

// 超过每主机上限时，回收最久未使用的空闲连接（LRU）
void JZNetworkAccessManager::evictIdleConnection(const std::string &hostKey)
{
    JZNetworkConnect* oldest = nullptr;
    int64_t oldestTime = 0;
    for (size_t i = 0; i < m_netConnects.size(); i++)
    {
        auto conn = m_netConnects[i];
        if (conn->m_hostKey == hostKey && conn->m_reply == nullptr && !conn->m_closing)
        {
            if (!oldest || conn->m_recvTime < oldestTime)
            {
                oldest = conn;
                oldestTime = conn->m_recvTime;
            }
        }
    }
    if (oldest)
        oldest->close();
}

std::string JZNetworkAccessManager::hostKey(const JZNetworkRequest& request) const
{
    bool httpProxy  = useHttpProxy(m_proxyConfig, request);
    bool httpsProxy = useHttpsProxy(m_proxyConfig, request);

    if (httpsProxy)
    {
        // 物理连到代理，CONNECT 隧道绑定到具体 https 目标：key 含代理地址 + 目标主机，
        // 既区分不同代理，又避免把隧道复用到其它 https 目标（隧道不可跨目标复用）。
        auto& url = request.url();
        return "proxy-https://" + m_proxyConfig->host + ":" + std::to_string(m_proxyConfig->port)
             + "/" + url.host() + ":" + std::to_string(url.port());
    }
    if (httpProxy)
    {
        // 物理连到代理；请求行带绝对 URL，同一代理连接可服务任意 http 目标，
        // 故 key 仅含代理地址（与目标主机无关），从而最大化代理连接复用。
        return "proxy-http://" + m_proxyConfig->host + ":" + std::to_string(m_proxyConfig->port);
    }

    // 直连：物理端点即目标主机，按 scheme + host + port 生成 key。
    auto& url = request.url();
    std::string scheme = url.scheme();
    std::string key = scheme + "://" + url.host();
    if ((scheme == "https" && url.port() != 443) || (scheme == "http" && url.port() != 80))
        key += ":" + std::to_string(url.port());
    return key;
}

void JZNetworkAccessManager::open(JZNetworkReply *reply, const JZNetworkRequest &request)
{
    std::string hostKey = this->hostKey(request);

    // HTTP 代理：http 目标用绝对 URI 转发；https 目标经 CONNECT 隧道。
    // 两者都连接到代理地址，真实响应经隧道/转发返回。
    bool httpsProxy = useHttpsProxy(m_proxyConfig, request);
    std::string connectUrl = request.url().url();
    if (useHttpProxy(m_proxyConfig, request) || httpsProxy)
        connectUrl = "http://" + m_proxyConfig->host + ":" + std::to_string(m_proxyConfig->port);

    // 1) 复用空闲连接：hostKey 已区分直连 / 代理转发 / https 代理隧道，
    //    故复用到的连接其物理模式（含 http、https）必然与本次请求一致。
    JZNetworkConnect* conn = findIdleConnection(hostKey);
    if (conn)
    {
        conn->m_reply = reply;
        reply->m_conn = conn;
        conn->reopen();
        return;
    }

    // 2) 超过每主机连接上限时，先回收一个空闲连接再新建
    if (connectionCount(hostKey) >= m_maxConnectionsPerHost)
        evictIdleConnection(hostKey);

    // 3) 新建连接
    conn = new JZNetworkConnect(this);
    conn->setSslConfig(m_ssl);
    conn->m_reply = reply;
    reply->m_conn = conn;
    conn->m_hostKey = hostKey;
    if (httpsProxy)
    {
        // 标记为 https 经代理隧道，并记录真实目标（SNI/CONNECT 用）与代理认证
        conn->m_isHttpsProxy = true;
        conn->m_proxyPhase = JZNetworkConnect::ProxyPhaseConnectSent;
        conn->m_targetHost = request.url().host();
        conn->m_targetPort = request.url().port();
        conn->m_proxyAuth = makeProxyAuthHeader(m_proxyConfig.value());
    }
    conn->open(connectUrl);
    connect(&conn->sigDisconnected, this, &JZNetworkAccessManager::onDisconnected);
    m_netConnects.push_back(conn);
}

JZNetworkReply *JZNetworkAccessManager::get(const JZNetworkRequest &request)
{
    bool proxy = useHttpProxy(m_proxyConfig, request);
    JZNetworkRequest req = request;
    if (proxy)
    {
        // http 代理：认证头放内层请求（代理据此转发），请求行用绝对 URI
        std::string auth = makeProxyAuthHeader(m_proxyConfig.value());
        if (!auth.empty())
            req.setRawHeader("Proxy-Authorization", auth);
    }
    // https 代理：认证头放 CONNECT 请求上（见 buildConnectRequest），内层请求保持加密

    JZNetworkReply* reply = new JZNetworkReply(this);
    reply->m_request = req;
    reply->m_method = JZNetworkRequest::Get;

    reply->m_send = str_toUtf8(req.buildHttpHeader(JZNetworkRequest::Get, proxy));        
    open(reply, req);
    return reply;
}

JZNetworkReply *JZNetworkAccessManager::post(const JZNetworkRequest &request, const JZByteArray &body)
{
    JZTX_FUNC_TRACE

    bool proxy = useHttpProxy(m_proxyConfig, request);
    JZNetworkRequest mutableRequest = request;
    int actualBodyLen = body.size();
    mutableRequest.setHeader(JZNetworkRequest::ContentLengthHeader, std::to_string(actualBodyLen));
    if (proxy)
    {
        // http 代理：认证头放内层请求，请求行用绝对 URI
        std::string auth = makeProxyAuthHeader(m_proxyConfig.value());
        if (!auth.empty())
            mutableRequest.setRawHeader("Proxy-Authorization", auth);
    }
    // https 代理：认证头放 CONNECT 请求上，内层请求保持加密

    JZNetworkReply *reply = new JZNetworkReply(this);
    reply->m_request = mutableRequest;
    reply->m_method = JZNetworkRequest::Post;
    reply->m_postBody = body;
    reply->m_send = str_toUtf8(mutableRequest.buildHttpHeader(JZNetworkRequest::Post, proxy));        
    reply->m_send.append(body);
    open(reply, mutableRequest);

    return reply;
}

// 根据 base URL 解析重定向 Location（支持绝对/协议相对/根相对/相对路径）
static std::string resolveRedirectUrl(const JZUrl &base, const std::string &location)
{
    if (str_startsWith(location, "http://") || str_startsWith(location, "https://"))
        return location;

    std::string authority = base.scheme() + "://" + base.host();
    if (base.port() != 0)
        authority += ":" + std::to_string(base.port());

    if (str_startsWith(location, "//"))
        return base.scheme() + ":" + location;

    if (!location.empty() && location[0] == '/')
        return authority + location;

    // 相对路径：基于当前 path 所在目录
    std::string path = base.url();
    size_t q = path.find('?');
    if (q != std::string::npos)
        path = path.substr(0, q);
    size_t lastSlash = path.rfind('/');
    std::string dir = (lastSlash != std::string::npos) ? path.substr(0, lastSlash + 1) : (authority + "/");
    return dir + location;
}

bool JZNetworkAccessManager::followRedirect(JZNetworkReply *reply, const std::string &location)
{
    if (reply->m_redirectPolicy == JZNetworkReply::NoRedirect)
        return false;
    if (reply->m_redirectCount >= reply->m_maxRedirects)
        return false;

    std::string newUrl = resolveRedirectUrl(reply->m_request.url(), location);
    if (newUrl.empty())
        return false;

    // 重定向策略校验（AllAllowed 跳过）
    if (reply->m_redirectPolicy != JZNetworkReply::AllAllowed)
    {
        const JZUrl &base = reply->m_request.url();
        JZUrl target(newUrl);
        if (reply->m_redirectPolicy == JZNetworkReply::SameOrigin)
        {
            if (target.scheme() != base.scheme() ||
                target.host() != base.host() ||
                target.port() != base.port())
                return false;
        }
        else if (reply->m_redirectPolicy == JZNetworkReply::SameHost)
        {
            if (target.host() != base.host())
                return false;
        }
    }

    int code = reply->m_statusCode;

    // 决定重定向后的方法与 body：
    // 307/308 保持原方法；303 一律转 GET；301/302 的 POST 传统上转 GET
    JZNetworkRequest::HttpMethod method = reply->m_method;
    JZByteArray body = reply->m_postBody;
    if (code == 303 || (code != 307 && code != 308 && method == JZNetworkRequest::Post)) {
        method = JZNetworkRequest::Get;
        body.clear();
    }

    // 重置 reply 状态，准备发送新请求
    reply->m_redirectCount++;
    reply->m_request.setUrl(JZUrl(newUrl));
    reply->m_method = method;
    reply->m_postBody = body;
    reply->m_body.clear();
    reply->m_error = JZNetworkReply::None;
    reply->m_statusCode = 0;
    reply->m_statusText.clear();
    reply->m_head.clear();
    reply->m_headers.clear();

    // 重建发送缓冲（重定向目标若仍走 http 代理，则请求行继续用绝对 URI）
    bool proxy = useHttpProxy(m_proxyConfig, reply->m_request);
    if (method == JZNetworkRequest::Post) {
        int len = body.size();
        reply->m_request.setHeader(JZNetworkRequest::ContentLengthHeader, std::to_string(len));
        reply->m_send = str_toUtf8(reply->m_request.buildHttpHeader(JZNetworkRequest::Post, proxy));
        reply->m_send.append(body);
    }
    else {
        reply->m_send = str_toUtf8(reply->m_request.buildHttpHeader(JZNetworkRequest::Get, proxy));
    }

    // 从当前连接摘除 reply，交给 open 重新分配连接（同主机可复用 keep-alive 连接）
    JZNetworkConnect* oldConn = reply->m_conn;
    if (oldConn) {
        oldConn->m_reply = nullptr;
        oldConn->resetConnection();
    }
    reply->m_conn = nullptr;

    open(reply, reply->m_request);
    return true;
}