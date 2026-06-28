#include "JZCoNetworkAccessManager.h"

JZCoNetworkAccessManager::JZCoNetworkAccessManager()
{
    m_manager = new JZNetworkAccessManager(this);
}

JZCoNetworkAccessManager::~JZCoNetworkAccessManager()
{
    
}

JZNetworkReply *JZCoNetworkAccessManager::get(const JZNetworkRequest &req)
{
    auto co = g_scheduler->currentCoroutine();

    JZNetworkReply *reply = m_manager->get(req);
    connect(&reply->sigFinished,this,[co]{ g_scheduler->resume(co); });
    g_scheduler->yield(co);

    return reply;
}

JZNetworkReply *JZCoNetworkAccessManager::post(const JZNetworkRequest &req,const JZByteArray &buffer)
{
    auto co = g_scheduler->currentCoroutine();

    JZNetworkReply *reply = m_manager->post(req,buffer);
    connect(&reply->sigFinished,this,[co]{ g_scheduler->resume(co); });
    g_scheduler->yield(co);
    
    return reply;
}