#ifndef JZ_NET_WORK_ACCESS_MANAGER_H_
#define JZ_NET_WORK_ACCESS_MANAGER_H_

#include "JZNetObject.h"
#include "JZByteArray.h"
#include "JZNetClient.h"
#include "JZNetworkRequest.h"
#include "JZNetworkReply.h"

class JZNetworkConnect : public JZNetObject
{
public:
    JZNetworkConnect(JZNetworkAccessManager *manager);
    ~JZNetworkConnect();

jz_signals:
    JZSigVoid sigDisconnected;

protected:
    friend JZNetworkAccessManager;
    friend JZNetworkReply;

    virtual void onNetEvent(JZNetResEvent* event) override;
    virtual void timerEvent(JZTimerEvent *event) override;
    void emitFinish(JZNetworkReply::NetworkError error);

    std::string url();
    void open(const std::string& url);
    void reopen();
    void close();
    
    JZByteArray getHttpBody(const JZByteArray& data);
    void appendChunk(const JZByteArray& data);

    std::string m_url;
    uint64_t m_conn;

    JZByteArray m_chunkBuffer;
    JZNetworkReply *m_reply;
    JZNetworkAccessManager* m_manager;
    
    int m_timeId;
    int64_t m_recvTime;
    bool m_chunkMode;
};

class JZNetworkAccessManager : public JZObject
{
public:
    JZNetworkAccessManager(JZObject *parent = nullptr);
    ~JZNetworkAccessManager();

    JZNetworkReply *get(const JZNetworkRequest &request);
    JZNetworkReply *post(const JZNetworkRequest &request, const JZByteArray &buffer);

protected:    
    friend JZNetworkConnect;

    void open(JZNetworkReply *reply, const std::string &url);
    void onDisconnected();

    std::vector<JZNetworkConnect*> m_netConnects;
};


#endif // !JZ_NET_WORK_ACCESS_MANAGER_H_
