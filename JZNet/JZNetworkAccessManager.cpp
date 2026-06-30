#include "mongoose.h"
#include "JZNetworkAccessManager.h"
#include "JZStd.h"
#include "JZTx.h"

//JZNetworkAccessManager
JZNetworkAccessManager::JZNetworkAccessManager(JZObject *parent)
    :JZObject(parent)
{    
}

JZNetworkAccessManager::~JZNetworkAccessManager()
{    
}

JZNetworkReply *JZNetworkAccessManager::get(const JZNetworkRequest &request)
{
    JZNetworkReply* reply = new JZNetworkReply(this);
    reply->m_send = str_toUtf8(request.buildHttpHeader(JZNetworkRequest::Get));        
    reply->open(request);

    return reply;
}

JZNetworkReply *JZNetworkAccessManager::post(const JZNetworkRequest &request, const JZByteArray &body)
{
    JZTxTracer t(__FUNCTION__);

    JZNetworkRequest mutableRequest = request;

    int actualBodyLen = body.size();
    mutableRequest.setHeader(JZNetworkRequest::ContentLengthHeader, std::to_string(actualBodyLen));
    
    JZNetworkReply *reply = new JZNetworkReply(this);
    reply->m_send = str_toUtf8(mutableRequest.buildHttpHeader(JZNetworkRequest::Post));        
    reply->m_send.append(body);
    reply->open(request);

    return reply;
}