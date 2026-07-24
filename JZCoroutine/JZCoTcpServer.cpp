#include "JZCoTcpServer.h"


// ========================== JZCoTcpServer ==========================
JZ_OBJECT_IMPL(JZCoTcpServer, JZObject)

JZCoTcpServer::JZCoTcpServer(JZObject *parent)
    : JZObject(parent)
{
    m_server = new JZTcpServer(this);
    connect(&m_server->sigAccept, this, &JZCoTcpServer::onAccept);
}

JZCoTcpServer::~JZCoTcpServer()
{
    close();
}

bool JZCoTcpServer::listen(int port)
{
    return m_server->listen(port);
}

bool JZCoTcpServer::listen(std::string ip, int port)
{
    return m_server->listen(ip, port);
}

void JZCoTcpServer::close()
{
    for (auto s : m_pendingList)
        delete s;
    m_pendingList.clear();

    m_server->close();
    m_coCtrl.wakeAll();
}

bool JZCoTcpServer::isListening() const
{
    return m_server->isListening();
}

JZCoTcpSocket* JZCoTcpServer::accept(int timeout)
{
    if (!m_pendingList.empty()) {
        auto sock = m_pendingList.front();
        m_pendingList.erase(m_pendingList.begin());
        return sock;
    }

    if (!isListening())
        return nullptr;

    m_coCtrl.wait(timeout);

    if (!m_pendingList.empty()) {
        auto sock = m_pendingList.front();
        m_pendingList.erase(m_pendingList.begin());
        return sock;
    }
    return nullptr;
}

void JZCoTcpServer::onAccept()
{
    JZTcpSocket* socket = m_server->nextPendingConnection();
    if (!socket)
        return;

    auto coSock = new JZCoTcpSocket(this);
    coSock->m_socket = socket;
    coSock->m_socket->setParent(this);
    coSock->connectSignals();

    m_pendingList.push_back(coSock);
    m_coCtrl.wakeAll();
}