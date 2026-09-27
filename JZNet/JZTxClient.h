#ifndef JZ_TX_CLIENT_H_
#define JZ_TX_CLIENT_H_
#include "JZThread.h"
#include "JZNetworkAccessManager.h"

class JZFRAMEWORK_API JZTxClient : public JZThread
{
public:
	bool syncSend(const JZByteArray& buffer);
	virtual void run() override;

	JZNetworkAccessManager* m_mgr;
};

#endif // !JZ_TX_CLIENT_H_
