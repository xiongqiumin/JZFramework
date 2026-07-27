#ifndef JZ_NEWWORK_REPLY_H_
#define JZ_NEWWORK_REPLY_H_

#include "JZNetObject.h"
#include "JZByteArray.h"
#include "JZNetworkRequest.h"

class JZNetworkAccessManager;

// HTTP response data
class JZHttpResData : public JZNetResEventData
{
public:
    int httpCode;
    std::string httpCodeText;
};

class JZNetworkConnect;
class JZNetworkReply : public JZObject
{
public:
    enum NetworkError {
        None,
        NoError,
        ConnectError,
        HttpError,
        TimeOutError,
        UserCancelError,
    };

    JZNetworkReply(JZObject*object);
    ~JZNetworkReply();    

    void close();

    int64_t timeout() const;
    void setTimeOut(int64_t timeout);

    NetworkError error() const;    
    std::string errorString() const;

    int httpStatusCode() const;
    std::string httpStatusCodeText() const;
    bool isFinished() const;

    JZByteArray readAll();

jz_signals:
    JZSigVoid sigReadyRead;
    JZSigVoid sigFinished;

protected:    
    friend JZNetworkAccessManager;
    friend JZNetworkConnect;

    void emitFinish(NetworkError error);

    NetworkError m_error;

    JZByteArray m_send;
    JZByteArray m_recv;
    JZByteArray m_body;

    int m_statusCode;
    std::string m_statusText;
    
    JZNetworkRequest m_request;
    JZNetworkConnect *m_conn;
    int m_timeOut;
};

#endif // !JZ_NEWWORK_REPLY_H_