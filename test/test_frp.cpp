#include "JZApplication.h"
#include "JZFrpClient.h"
#include "JZFrpServer.h"
#include "JZTcpServer.h"
#include "JZTcpSocket.h"
#include "JZDebug.h"

using namespace std;

int main()
{
    JZApplication a;

    JZFrpPacketInit();

    JZTcpServer local_server;
    local_server.listen(7777);
    connect(&local_server.sigAccept, [&local_server] {
        JZTcpSocket *socket = local_server.nextPendingConnection();
        if (!socket)
            return;

        connect(&socket->sigReadyRead, [socket] {
                JZByteArray recv = socket->readAll();
                string recv_str = str_fromUtf8(recv);
                string hello = "server";
                socket->write(hello.data(), hello.size());
            });
        });

    JZFrpServer frps;
    frps.listen(8888);

    JZFrpClientConfig frpc_cfg;
    frpc_cfg.serverIp = "127.0.0.1";
    frpc_cfg.serverPort = 8888;
    frpc_cfg.addProxy("test", "127.0.0.1", 7777, 9999);

    JZFrpClient frpc;
    frpc.setConfig(frpc_cfg);
    frpc.connectToServer();
    if (!frpc.waitForConnected(30 * 1000))
    {
        jzDebug() << "frpc connect failed";
        return 1;
    }

    JZTcpSocket remote_client;
    remote_client.connectToHost("127.0.0.1", 9999);
    connect(&remote_client.sigConnect, [&remote_client] {
            string hello = "hello";
            remote_client.write(hello.data(), hello.size());
        });
    connect(&remote_client.sigReadyRead, [&remote_client] {
            JZByteArray recv = remote_client.readAll();
            string recv_str = str_fromUtf8(recv);
            string hello = "client";
            remote_client.write(hello.data(), hello.size());
            jzDebug() << "remote recv:" << recv_str;
        });

    return a.exec();
}