#include "JZCo.h"
#include "JZTimer.h"
#include "JZCoWebSocket.h"
#include "JZDebug.h"

//JZCoWebSocket
JZCoWebSocket::JZCoWebSocket(JZObject* parent)
	:JZObject(parent)
{
	m_waitType = WaitNone;
	m_client = new JZWebSocket(this);
	connectSignals();
}

JZCoWebSocket::JZCoWebSocket(JZObject* parent, JZWebSocket* client)
	:JZObject(parent)
{
	m_waitType = WaitNone;
	m_client = client;
	m_client->setParent(this);
	connectSignals();
}

JZCoWebSocket::~JZCoWebSocket()
{
	disconnectFromHost();
}

void JZCoWebSocket::connectSignals()
{
	connect(&m_client->sigConnected, this, &JZCoWebSocket::onConnected, JZ::QueuedConnection);
	connect(&m_client->sigDisconnected, this, &JZCoWebSocket::onDisconnected, JZ::QueuedConnection);
	connect(&m_client->sigTextMessageReceived, this, &JZCoWebSocket::onTextMessageReceived, JZ::QueuedConnection);
	connect(&m_client->sigBinaryMessageReceived, this, &JZCoWebSocket::onBinaryMessageReceived, JZ::QueuedConnection);
	connect(&m_client->sigError, this, &JZCoWebSocket::onError, JZ::QueuedConnection);
}

void JZCoWebSocket::yield(int waitType,int timeout)
{
	JZ_ASSERT(m_waitType == WaitNone);

	m_waitType = waitType;
	m_coCtrl.wait(timeout);
	m_waitType = WaitNone;
}

bool JZCoWebSocket::connectToHost(const JZNetworkRequest& req, int timeout)
{
	m_textQueue = std::queue<std::string>();
	m_binQueue = std::queue<JZByteArray>();

	m_client->connectToHost(req);
	yield(WaitConnect,timeout);
	return m_client->isConnect();
}

void JZCoWebSocket::disconnectFromHost()
{
	m_client->disconnectFromHost();
}

bool JZCoWebSocket::isConnect() const
{
	return m_client->isConnect();
}

void JZCoWebSocket::sendTextMessage(const std::string& message)
{
	m_client->sendTextMessage(message);
}

void JZCoWebSocket::sendBinaryMessage(const JZByteArray& message)
{
	m_client->sendBinaryMessage(message);
}

bool JZCoWebSocket::getTextMessage(std::string& message)
{
	if (m_textQueue.size() > 0)
	{
		message = m_textQueue.front();
		m_textQueue.pop();
		return true;
	}
	return false;
}

bool JZCoWebSocket::getBinaryMessage(JZByteArray& message)
{
	if (m_binQueue.size() > 0)
	{
		message = m_binQueue.front();
		m_binQueue.pop();
		return true;
	}
	return false;
}

JZCoWebSocket::MessageType JZCoWebSocket::getAnyMessage(std::string& textMsg, JZByteArray& binMsg)
{
	if (m_textQueue.size() > 0)
	{
		textMsg = m_textQueue.front();
		m_textQueue.pop();
		return Text;
	}

	if (m_binQueue.size() > 0)
	{
		binMsg = m_binQueue.front();
		m_binQueue.pop();
		return Binary;
	}

	return None;
}

bool JZCoWebSocket::waitTextMessage(std::string& message, int timeout)
{
	if (m_textQueue.size() > 0)
		return getTextMessage(message);

	yield(WaitTextMessage, timeout);
	return getTextMessage(message);
}

bool JZCoWebSocket::waitBinaryMessage(JZByteArray& message, int timeout)
{
	if (m_binQueue.size() > 0)
		return getBinaryMessage(message);

	yield(WaitBinaryMessage, timeout);
	return getBinaryMessage(message);
}

JZCoWebSocket::MessageType JZCoWebSocket::waitAnyMessage(std::string& textMsg, JZByteArray& binMsg, int timeout)
{
	if (m_textQueue.size() > 0 || m_binQueue.size() > 0)
		return getAnyMessage(textMsg, binMsg);

	yield(WaitAnyMessage, timeout);
	return getAnyMessage(textMsg, binMsg);
}

void JZCoWebSocket::onConnected()
{
	if(m_waitType == WaitConnect)
		m_coCtrl.wakeAll();
}

void JZCoWebSocket::onDisconnected()
{
	if (m_waitType != WaitNone)
		m_coCtrl.wakeAll();
}

void JZCoWebSocket::onTextMessageReceived(const std::string& message)
{
	m_textQueue.push(message);
	if (m_waitType == WaitTextMessage || m_waitType == WaitAnyMessage)
	{
		m_coCtrl.wakeAll();
	}
}

void JZCoWebSocket::onBinaryMessageReceived(const JZByteArray& message)
{
	m_binQueue.push(message);
	if (m_waitType == WaitBinaryMessage || m_waitType == WaitAnyMessage)
	{
		m_coCtrl.wakeAll();
	}
}

void JZCoWebSocket::onError()
{
	m_coCtrl.wakeAll();
}