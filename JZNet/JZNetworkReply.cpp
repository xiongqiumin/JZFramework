#include "JZNetworkReply.h"
#include "JZNetworkAccessManager.h"
#include "JZPlatform.h"
#include "JZDebug.h"
#include "JZTx.h"

//JZNetworkReply
JZNetworkReply::JZNetworkReply(JZObject *parent)
    :JZObject(parent), sigReadyRead(this), sigFinished(this)
{
    m_error = None;
    m_statusCode = 0;
    m_conn = nullptr;
    m_timeOut = 30 * 1000;
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