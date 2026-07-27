#include "mongoose.h"
#include "JZNetworkAccessManager.h"
#include "JZStd.h"
#include "JZTx.h"
#include "JZDebug.h"

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

void JZNetworkConnect::open(const std::string& url)
{
    JZSocketInfo info;
    info.type = JZSocketInfo::HttpClient;
    info.url = url;
    
    m_url = url;
    m_conn = openConn(info);
    m_timeId = startTimer(200);
    m_recvTime = JZPlatform::getTickCountMs();
}

void JZNetworkConnect::reopen()
{
    resetConnection();

    writeConn(m_conn, WriteBin, m_reply->m_send.data(), m_reply->m_send.size());
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
}

void JZNetworkConnect::closeConnection()
{
    m_closing = true;
    if (m_conn != INVALID_NET_HANDLE)
    {
        closeConn(m_conn, false);
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
            closeConn(m_conn, true);
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
        emitFinish(JZNetworkReply::ConnectError);
        sigDisconnected.emit();
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
        if (http->httpCode >= 200 && http->httpCode < 300)
        {
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
        writeConn(m_conn, WriteBin, m_reply->m_send.data(), m_reply->m_send.size());
    }
    else if (ev == JZ_NET_EV_TLS_HS)
    {
    }
    else if (ev == JZ_NET_EV_ERROR)
    {
        emitFinish(JZNetworkReply::ConnectError);
    }
    else if (ev == JZ_NET_EV_READ)
    {
        m_recvTime = JZPlatform::getTickCountMs();

        if (m_chunkMode && event->binData.size() > 0)
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

void JZNetworkAccessManager::open(JZNetworkReply *reply, const JZNetworkRequest &request)
{
    std::string hostKey = request.hostKey();

    // 1) 复用同一主机的空闲连接（按主机复用，而非完整 URL）
    JZNetworkConnect* conn = findIdleConnection(hostKey);
    if (conn)
    {
        conn->m_reply = reply;
        reply->m_conn = conn;
        conn->m_requestClose = reply->m_request.isConnectionClose();
        conn->reopen();
        return;
    }

    // 2) 超过每主机连接上限时，先回收一个空闲连接再新建
    if (connectionCount(hostKey) >= m_maxConnectionsPerHost)
        evictIdleConnection(hostKey);

    // 3) 新建连接
    conn = new JZNetworkConnect(this);
    conn->m_reply = reply;
    reply->m_conn = conn;
    conn->m_hostKey = hostKey;
    conn->m_requestClose = reply->m_request.isConnectionClose();
    conn->open(request.url());
    connect(&conn->sigDisconnected, this, &JZNetworkAccessManager::onDisconnected);
    m_netConnects.push_back(conn);
}

JZNetworkReply *JZNetworkAccessManager::get(const JZNetworkRequest &request)
{
    JZNetworkReply* reply = new JZNetworkReply(this);
    reply->m_request = request;

    reply->m_send = str_toUtf8(request.buildHttpHeader(JZNetworkRequest::Get));        
    open(reply, request);
    return reply;
}

JZNetworkReply *JZNetworkAccessManager::post(const JZNetworkRequest &request, const JZByteArray &body)
{
    JZTX_FUNC_TRACE

    JZNetworkRequest mutableRequest = request;
    int actualBodyLen = body.size();
    mutableRequest.setHeader(JZNetworkRequest::ContentLengthHeader, std::to_string(actualBodyLen));

    JZNetworkReply *reply = new JZNetworkReply(this);
    reply->m_request = mutableRequest;
    reply->m_send = str_toUtf8(mutableRequest.buildHttpHeader(JZNetworkRequest::Post));        
    reply->m_send.append(body);
    open(reply, mutableRequest);

    return reply;
}