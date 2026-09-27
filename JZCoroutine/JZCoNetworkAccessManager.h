#ifndef JZ_CO_NETWORK_ACCESS_MANAGER_H_
#define JZ_CO_NETWORK_ACCESS_MANAGER_H_
#include "JZCo.h"
#include "JZNetworkAccessManager.h"

class JZCoNetworkAccessManager;
class JZFRAMEWORK_API JZCoNetworkReply : public JZObject
{
public:
    // 协程等待请求完成 / 读取响应体(供外部调用者使用)
    void waitFinished();
    
    void close();

    JZNetworkReply::NetworkError error() const;    
    std::string errorString() const;

    int httpStatusCode() const;
    std::string httpStatusCodeText() const;
    bool isFinished() const;

    JZByteArray readAll();

protected:
    friend JZCoNetworkAccessManager;

    JZCoNetworkReply(JZCoNetworkAccessManager *manager, JZNetworkReply *reply);

    void onReplyFinished();

    JZCoCondition cond;
    JZNetworkReply *m_reply;
};

class JZFRAMEWORK_API JZCoNetworkAccessManager : public JZObject
{
public:
    JZCoNetworkAccessManager(JZObject *parent = nullptr);
    ~JZCoNetworkAccessManager();

    JZCoNetworkReply *get(const JZNetworkRequest &req);
    JZCoNetworkReply *post(const JZNetworkRequest &req,const JZByteArray &buffer);

protected:
    JZNetworkAccessManager *m_manager;
};

#endif