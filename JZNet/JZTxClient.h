#ifndef JZ_TX_CLIENT_H_
#define JZ_TX_CLIENT_H_
#include "JZThread.h"
#include "JZNetworkAccessManager.h"
#include <string>

class JZFRAMEWORK_API JZTxClient : public JZThread
{
public:
	bool syncSend(const JZByteArray& buffer);
	virtual void run() override;

	std::string url = "http://127.0.0.1:5000/";   // 上报地址可配; 默认本机收集器(原入库的局域网 IP 已移除, 2026-11-09)

	JZNetworkAccessManager* m_mgr;
};

#endif // !JZ_TX_CLIENT_H_
