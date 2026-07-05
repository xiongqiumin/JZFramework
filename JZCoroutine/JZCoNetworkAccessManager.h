#ifndef JZ_CO_NETWORK_ACCESS_MANAGER_H_
#define JZ_CO_NETWORK_ACCESS_MANAGER_H_

#include "JZCo.h"
#include "JZNetworkAccessManager.h"

class JZCoNetworkAccessManager;
class JZCoNetworkReply : public JZObject
{
protected:
    JZCoNetworkReply(JZCoNetworkAccessManager *manager, JZNetworkReply *reply);

    void waitFinished();
    JZByteArray readAll();

protected:
    friend JZCoNetworkAccessManager;
    
    void onReplyFinished();

    JZCoCondition cond;
    JZNetworkReply *m_reply;
};

class JZCoNetworkAccessManager : public JZObject
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