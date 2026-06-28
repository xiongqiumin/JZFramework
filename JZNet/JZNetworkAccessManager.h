#ifndef JZ_NET_WORK_ACCESS_MANAGER_H_
#define JZ_NET_WORK_ACCESS_MANAGER_H_

#include "JZNetObject.h"
#include "JZByteArray.h"
#include "JZNetClient.h"
#include "JZNetworkRequest.h"
#include "JZNetworkReply.h"

class JZNetworkAccessManager : public JZObject
{
public:
    JZNetworkAccessManager(JZObject *parent = nullptr);
    ~JZNetworkAccessManager();

    JZNetworkReply *get(const JZNetworkRequest &request);
    JZNetworkReply *post(const JZNetworkRequest &request, const JZByteArray &buffer);

protected:    
    friend JZNetworkReply;
};


#endif // !JZ_NET_WORK_ACCESS_MANAGER_H_
