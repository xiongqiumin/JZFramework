#include "JZNetworkReply.h"
#include "JZNetworkAccessManager.h"
#include "JZPlatform.h"
#include "JZDebug.h"
#include "JZTx.h"

//JZNetworkReply
JZNetworkReply::JZNetworkReply(JZNetworkAccessManager *parent)
    :JZNetObject(parent), sigFinished(this)
{
    m_error = None;
    m_statusCode = 0;
    m_conn = INVALID_NET_HANDLE;    
    m_recvTime = 0;
    m_timeId = INVAILD_TIMER_ID;
    m_timeOut = 30 * 1000;
}

JZNetworkReply::~JZNetworkReply()
{
    close();
}

void JZNetworkReply::open(const JZNetworkRequest& request)
{
    JZSocketInfo info;
    info.type = JZSocketInfo::HttpClient;
    info.url = request.url();
    
    m_conn = openConn(info);
    m_timeId = startTimer(200);
    m_recvTime = JZPlatform::getTickCountMs();
}

JZNetworkReply::NetworkError JZNetworkReply::error() const
{
    return m_error;
}

std::string JZNetworkReply::errorString() const
{
    switch (m_error) {
        case NoError:
            return "NoError";
        case ConnectError:
            return "ConnectError";
        case HttpError:
            return "HttpError " + std::to_string(m_statusCode);
        case TimeOutError:
            return "TimeOutError";
        case UserCancelError:
            return "UserCancelError";
        default:
            return "UnknownError";
    }
}

int JZNetworkReply::httpStatusCode() const
{
    return m_statusCode;
}

std::string JZNetworkReply::httpStatusCodeText() const
{
    return m_statusText;
}

bool JZNetworkReply::isFinished() const
{
    return m_error != None;
}

JZByteArray JZNetworkReply::readAll()
{
    JZByteArray ret = m_body;
    m_body.clear();
    return ret;
}

void JZNetworkReply::close()
{
    if (m_timeId != INVAILD_TIMER_ID)
    {
        killTimer(m_timeId);
        m_timeId = INVAILD_TIMER_ID;
    }

    if (m_conn)
    {
        closeConn(m_conn, true);
        m_conn = INVALID_NET_HANDLE;

        emitFinish(UserCancelError);
    }    
    m_recvTime = 0;
}

int64_t JZNetworkReply::timeout() const
{
    return m_timeOut;
}

void JZNetworkReply::setTimeOut(int64_t timeout)
{
    m_timeOut = timeout;
}

void JZNetworkReply::emitFinish(NetworkError error)
{
    JZTX_FUNC_TRACE
    if (m_error != None)
        return;

    m_error = error;
    sigFinished.emit();    
}

void JZNetworkReply::timerEvent(JZTimerEvent* event)
{
    if (m_timeOut != -1 && JZPlatform::getTickCountMs() - m_recvTime > m_timeOut)
    {
        emitFinish(TimeOutError);
        close();
    }
}

JZByteArray JZNetworkReply::getHttpBody(const JZByteArray& data)
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

void JZNetworkReply::onNetEvent(JZNetResEvent* event)
{
    JZTX_FUNC_TRACE
    int ev = event->event;
    if (ev == JZ_NET_EV_HTTP_MSG)
    {
        JZHttpResData *http = (JZHttpResData*)(event->extData.get());
        m_statusCode = http->httpCode;
        m_statusText = http->httpCodeText;
        m_body = getHttpBody(event->binData);
        if (http->httpCode >= 200 && http->httpCode < 300)
        {
            emitFinish(JZNetworkReply::NoError);
        }
        else
        {
            emitFinish(JZNetworkReply::HttpError);
        }
    }
    else if (ev == JZ_NET_EV_CONNECT)
    {
        writeConn(m_conn, WriteBin, m_send.data(), m_send.size());
    }
    else if (ev == JZ_NET_EV_TLS_HS)
    {
    }
    else if (ev == JZ_NET_EV_CLOSE)
    {
        emitFinish(JZNetworkReply::ConnectError);
    }
    else if (ev == JZ_NET_EV_ERROR)
    {
        emitFinish(JZNetworkReply::ConnectError);
    }
    else if (ev == JZ_NET_EV_READ)
    {
        m_recvTime = JZPlatform::getTickCountMs();
    }
}