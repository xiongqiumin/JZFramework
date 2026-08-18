#include "JZNetworkReply.h"
#include "JZNetworkAccessManager.h"
#include "JZPlatform.h"
#include "JZDebug.h"
#include "JZTx.h"

#include <cctype>

static std::string toLowerStr(const std::string &s)
{
    std::string r = s;
    for (char &c : r)
        c = (char)std::tolower((unsigned char)c);
    return r;
}

//JZNetworkReply
JZNetworkReply::JZNetworkReply(JZObject *parent)
    :JZObject(parent), sigReadyRead(this), sigFinished(this)
{
    m_error = None;
    m_statusCode = 0;
    m_conn = nullptr;
    m_timeOut = 30 * 1000;
    m_redirectPolicy = SameOrigin;
    m_maxRedirects = 5;
    m_redirectCount = 0;
    m_method = JZNetworkRequest::Get;
}

JZNetworkReply::~JZNetworkReply()
{
    close();
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

std::string JZNetworkReply::rawHeaders() const
{
    return m_head;
}

const std::map<std::string, std::string>& JZNetworkReply::headers() const
{
    return m_headers;
}

std::string JZNetworkReply::header(const std::string &key) const
{
    std::string k = toLowerStr(key);
    auto it = m_headers.find(k);
    return (it != m_headers.end()) ? it->second : std::string();
}

JZByteArray JZNetworkReply::readAll()
{
    JZByteArray ret = m_body;
    m_body.clear();
    return ret;
}

void JZNetworkReply::close()
{
    if (m_conn != nullptr)
    {
        m_conn->emitFinish(UserCancelError);
        m_conn = nullptr;
    }    
}

void JZNetworkReply::setReadLimit(int64_t rateBytesPerSec)
{
    if (rateBytesPerSec < 0) rateBytesPerSec = 0;
    m_readLimitBytesPerSec = rateBytesPerSec;
    if (m_conn != nullptr)
        m_conn->setReadLimit(rateBytesPerSec);
}

int64_t JZNetworkReply::timeout() const
{
    return m_timeOut;
}

void JZNetworkReply::setTimeOut(int64_t timeout)
{
    m_timeOut = timeout;
}

void JZNetworkReply::setFollowRedirects(bool enable)
{
    if (enable)
    {
        // 显式开启：若当前为 NoRedirect 则放宽到允许所有，否则保持既有策略
        if (m_redirectPolicy == NoRedirect)
            m_redirectPolicy = AllAllowed;
    }
    else
    {
        m_redirectPolicy = NoRedirect;
    }
}

bool JZNetworkReply::isFollowRedirects() const
{
    return m_redirectPolicy != NoRedirect;
}

void JZNetworkReply::setRedirectPolicy(RedirectPolicy policy)
{
    m_redirectPolicy = policy;
}

JZNetworkReply::RedirectPolicy JZNetworkReply::redirectPolicy() const
{
    return m_redirectPolicy;
}

void JZNetworkReply::setMaxRedirects(int max)
{
    m_maxRedirects = max;
}

int JZNetworkReply::maxRedirects() const
{
    return m_maxRedirects;
}

int JZNetworkReply::redirectCount() const
{
    return m_redirectCount;
}

void JZNetworkReply::emitFinish(NetworkError error)
{
    JZTX_FUNC_TRACE
    if (m_error != None)
        return;

    m_error = error;
    sigFinished.emit();    
}