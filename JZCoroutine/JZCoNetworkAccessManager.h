#ifndef JZ_CO_NETWORK_ACCESS_MANAGER_H_
#define JZ_CO_NETWORK_ACCESS_MANAGER_H_

#include "JZCo.h"
#include "JZNetworkAccessManager.h"

class JZCoNetworkAccessManager : public JZObject
{
public:
    JZCoNetworkAccessManager();
    ~JZCoNetworkAccessManager();

    JZNetworkReply *get(const JZNetworkRequest &req);
    JZNetworkReply *post(const JZNetworkRequest &req,const JZByteArray &buffer);

protected:
    JZNetworkAccessManager *m_manager;
};

#endif