#include <string>

#include "JZDebug.h"
#include "JZByteArray.h"
#include "JZTimer.h"
#include "JZEventLoop.h"

#include "JZTcpServer.h"
#include "JZTcpSocket.h"
#include "JZFrpServer.h"
#include "JZFrpClient.h"
#include "JZApplication.h"

#include "net/net_test_common.h"

using namespace std;

// ===================== 单线程 FRP 隧道回环测试 =====================
// 结构: 本地 echo 服务(7777) <- FRP 客户端(frpc) <- FRP 服务端(frps:8888)
//        <- 远端客户端(连 frps 暴露端口 9999)
// 远端客户端发带序号的消息, 经隧道到达本地 echo 服务原样回传, 校验序号连续性并统计 TPS
class FrpTestSingle : public JZObject
{
public:
    FrpTestSingle(JZEventLoop *loop, int port)
    {
        m_prefix = "frp";

        // 本地 echo 服务
        m_local = new JZTcpServer(this);
        m_local->listen(7777);
        connect(&m_local->sigAccept, this, &FrpTestSingle::onLocalAccept);

        // FRP 服务端
        m_frps = new JZFrpServer(this);
        m_frps->listen(8888);

        // FRP 客户端: 把本地 7777 通过 frps 暴露到 9999
        m_frpcCfg.serverIp = "127.0.0.1";
        m_frpcCfg.serverPort = 8888;
        m_frpcCfg.addProxy("test", "127.0.0.1", 7777, 9999);
        m_frpc = new JZFrpClient();
        m_frpc->setConfig(m_frpcCfg);
        m_frpc->connectToServer();

        // 隧道代理端口就绪后再连远端客户端, 失败则定时重试
        m_connectTimer = new JZTimer(this);
        m_connectTimer->setSingleShot(true);
        connect(&m_connectTimer->sigTimeout, this, &FrpTestSingle::tryConnectRemote);
        m_connectTimer->start(200);

        m_tps.begin();
        netInstallExit(this, loop, 15000);
        jzDebug() << "==== FRP 单线程测试启动 (15s 后退出) ====";
    }

    void tryConnectRemote()
    {
        if (m_remote)
            return;   // 已经连上

        auto *c = new JZTcpSocket(this);
        connect(&c->sigConnected, this, &FrpTestSingle::onRemoteConnect);
        connect(&c->sigReadyRead, this, &FrpTestSingle::onRemoteRead);
        connect(&c->sigError, [this, c](const string &) {
                jzDebug() << "remote connect to proxy failed, retry...";
                c->deleteLater();
                m_connectTimer->start(200);
            });
        c->connectToHost("127.0.0.1", 9999);
    }

    void onLocalAccept()
    {
        m_localSock = m_local->nextPendingConnection();
        if (m_localSock)
            connect(&m_localSock->sigReadyRead, this, &FrpTestSingle::onLocalRead);
    }

    void onLocalRead()
    {
        m_localBuf += str_fromUtf8(m_localSock->readAll());
        size_t pos;
        while ((pos = m_localBuf.find('\n')) != string::npos)
        {
            string line = m_localBuf.substr(0, pos);
            m_localBuf.erase(0, pos + 1);
            m_localSock->write(str_toUtf8(line + "\n"));   // 原样回传
        }
    }

    void onRemoteConnect()
    {
        m_remote = (JZTcpSocket *)sender();
        m_connectTimer->stop();
        sendNext();
    }

    void onRemoteRead()
    {
        m_remoteBuf += str_fromUtf8(m_remote->readAll());
        size_t pos;
        while ((pos = m_remoteBuf.find('\n')) != string::npos)
        {
            string line = m_remoteBuf.substr(0, pos);
            m_remoteBuf.erase(0, pos + 1);

            uint64_t seq;
            if (!netParseMsg(line, m_prefix, seq) || seq != m_expect)
                netReportError("FRP", "校验失败 期望=" + to_string(m_expect) + " 收到=" + line);
            m_expect++;
            m_tps.hit();
            sendNext();
        }
    }

    void sendNext() { m_remote->write(str_toUtf8(netMakeMsg(m_prefix, m_expect))); }

    JZTcpServer *m_local;
    JZTcpSocket *m_localSock = nullptr;
    JZTcpSocket *m_remote = nullptr;
    JZFrpServer *m_frps;
    JZFrpClient *m_frpc;
    JZFrpClientConfig m_frpcCfg;
    JZTimer *m_connectTimer;
    string m_localBuf, m_remoteBuf;
    string m_prefix = "frp";
    uint64_t m_expect = 0;
    NetTpsCounter m_tps{"FRP 单线程"};
};

int main()
{
    JZApplication a;

    jzDebug() << "########## FRP 单线程隧道测试 (15s) ##########";
    g_netTestActive = true;
    JZEventLoop loop;
    FrpTestSingle test(&loop, 9999);
    loop.exec();
    double tps = test.m_tps.average();
    netCheckTps("FRP 单线程", tps);
    g_netTestActive = false;
    jzDebug() << "【OK】FRP 单线程通过";
    
    return 0;
}
