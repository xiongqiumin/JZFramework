#ifndef JZ_NEWWORK_REPLY_H_
#define JZ_NEWWORK_REPLY_H_

#include "JZNetObject.h"
#include "JZByteArray.h"
#include "JZNetworkRequest.h"

class JZNetworkAccessManager;
class JZNetworkReply : public JZNetObject
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

    JZNetworkReply(JZNetworkAccessManager *object);
    ~JZNetworkReply();    

    void open(const JZNetworkRequest& request);
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
    JZSigVoid sigFinished;

protected:    
    friend JZNetworkAccessManager;

    JZByteArray getHttpBody(const JZByteArray& data);
    virtual void onNetEvent(JZNetResEvent* event) override;
    virtual void timerEvent(JZTimerEvent *event) override;
    void emitFinish(NetworkError error);

    NetworkError m_error;

    JZByteArray m_send;
    JZByteArray m_recv;
    JZByteArray m_body;

    int m_statusCode;
    std::string m_statusText;
    
    uint64_t m_conn;
    int m_timeId;
    int64_t m_recvTime;
    int64_t m_timeOut;
};

#endif // !JZ_NEWWORK_REPLY_H_