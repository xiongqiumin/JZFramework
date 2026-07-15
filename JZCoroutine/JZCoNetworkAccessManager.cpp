#include "JZCoNetworkAccessManager.h"

JZCoNetworkReply::JZCoNetworkReply(JZCoNetworkAccessManager *manager, JZNetworkReply *reply)
    :JZObject(manager)
{
    m_reply = reply;
    m_reply->setParent(this);
    connect(&reply->sigFinished,this, &JZCoNetworkReply::onReplyFinished);
}

void JZCoNetworkReply::waitFinished()
{
    cond.wait();
}

void JZCoNetworkReply::onReplyFinished()
{
    cond.wakeAll();
}

void JZCoNetworkReply::close()
{
    return m_reply->close();
}

JZNetworkReply::NetworkError JZCoNetworkReply::error() const
{
    return m_reply->error();
}

std::string JZCoNetworkReply::errorString() const{
    return m_reply->errorString();
}

int JZCoNetworkReply::httpStatusCode() const
{
    return m_reply->httpStatusCode();
}

std::string JZCoNetworkReply::httpStatusCodeText() const
{
    return m_reply->httpStatusCodeText();
}

bool JZCoNetworkReply::isFinished() const
{
    return m_reply->isFinished();
}

JZByteArray JZCoNetworkReply::readAll()
{
    return m_reply->readAll();
}

//JZCoNetworkAccessManager
JZCoNetworkAccessManager::JZCoNetworkAccessManager(JZObject *parent)
    :JZObject(parent)
{
    m_manager = new JZNetworkAccessManager(this);
}

JZCoNetworkAccessManager::~JZCoNetworkAccessManager()
{
}

JZCoNetworkReply *JZCoNetworkAccessManager::get(const JZNetworkRequest &req)
{    
    JZNetworkReply *reply = m_manager->get(req);    
    return new JZCoNetworkReply(this, reply);
}

JZCoNetworkReply *JZCoNetworkAccessManager::post(const JZNetworkRequest &req,const JZByteArray &buffer)
{
    JZNetworkReply *reply = m_manager->post(req,buffer);
    return new JZCoNetworkReply(this, reply);
}