#include "JZCo.h"
#include "JZTimer.h"
#include "JZCoWebSocketServer.h"
#include "JZDebug.h"

//JZCoWebSocketServer
JZCoWebSocketServer::JZCoWebSocketServer(JZObject* parent)
	:JZObject(parent)
{
	m_server = new JZWebSocketServer(this);
	connect(&m_server->sigAccept, this, &JZCoWebSocketServer::onAccept);
}

JZCoWebSocketServer::~JZCoWebSocketServer()
{
	close();
}

bool JZCoWebSocketServer::listen(int port)
{
	return m_server->listen(port);
}

void JZCoWebSocketServer::close()
{
	for (int i = 0; i < m_pendingList.size(); i++)
		delete m_pendingList[i];
	m_pendingList.clear();
	m_server->close();
	m_coCtrl.wakeAll();
}

bool JZCoWebSocketServer::isListening() const
{
	return m_server->isListening();
}

void JZCoWebSocketServer::onAccept(JZWebSocket* web_socket)
{
	JZCoWebSocket* co_socket = new JZCoWebSocket(this, web_socket);
	m_pendingList.push_back(co_socket);
	m_coCtrl.wakeAll();
}

JZCoWebSocket* JZCoWebSocketServer::getPendingSocket()
{
	if (m_pendingList.size() == 0)
		return nullptr;

	JZCoWebSocket* pre_socket = m_pendingList.front();
	vec_popFront(m_pendingList);
	return pre_socket;
}

JZCoWebSocket* JZCoWebSocketServer::accept()
{
	if (m_pendingList.size() > 0)
		return getPendingSocket();
	if (!m_server->isListening())
		return nullptr;

	m_coCtrl.wait();
	return getPendingSocket();
}