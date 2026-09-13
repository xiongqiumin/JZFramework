// ---------------------------------------------------------------------------
// netServer.js —— 对应 C++ common/net/netServer.h + netServer.cpp
//
// 服务端: 监听端口, 维护 netId -> socket 表, 收包分发。
//
// Qt -> JS (2026-09-13 改为"真跑得起来"的版本):
//   QTcpServer    -> 引擎 net 模块的 Server (net/dist/index.js, Node 风格:
//                    listen(port)/close()/listening/on('connection'))
//   QTcpSocket    -> 引擎 net 模块的 Socket (on('data'|'close'|'error')/
//                    write(u8)/end()/destroy()/remoteAddress/remotePort)
//   QHostAddress  -> 直接取 socket.remoteAddress 字符串(不再需要 Address 对象)
//   信号槽        -> 回调数组字段 + 自由函数 emit(slots, ...args)
//   qobject_cast<QTcpSocket*>(sender()) -> 回调闭包捕获的 socket
//   QMap<int,QTcpSocket*> -> Map
//
// 与 C++ 的**语义差异**(必须知道, 也是所有 Qt->JS 网络移植的共性):
//   1) QTcpServer::listen() 是同步的, 返回即"已在监听"; Node 的 listen() 是异步的,
//      监听结果通过 'listening' / 'error' 事件回报 —— 故 startServer() 只能"乐观返回 true",
//      失败会走 onError()。
//   2) 收数据没有同步 read(): readyRead 的对应物是 socket 的 'data' 事件, 见 onData()。
// ---------------------------------------------------------------------------

import { net } from 'net/dist/index.js';
import { NetDataManager } from './netDataManager.js';
import { NetInfo } from './netInfo.js';

export class NetServer
{
    // NetServer(QObject * parent = NULL);
    constructor(parent)
    {
        this.parent = parent || null;

        // QTcpServer *tcpServer;  -> 第一次 startServer() 时按端口创建
        this.tcpServer = null;
        this.m_listenPort = 0;

        this.m_netId = 1;
        this.m_tcpClients = new Map();       // QMap<int, QTcpSocket*> m_tcpClients;
        this.m_socketToNetId = new Map();    // socket -> netId(替代 QMap::key(value) 反查)
        this.m_dataManager = new NetDataManager();

        // signals
        this.newConnect = [];     // void newConnect(int netId);
        this.disConnect = [];     // void disConnect(int netId);
        this.newDataRecv = [];    // void newDataRecv(int netId, NetPackPtr ptr);
        this.listenError = [];    // 新增(JS 侧需要): void listenError(QString msg);
    }

    // ~NetServer();
    dispose()
    {
        this.stopServer();
    }

    // bool NetServer::startServer(int port)
    startServer(port)
    {
        if (this.tcpServer)
            this.stopServer();

        this.m_listenPort = port | 0;
        const server = net.createServer();
        this.tcpServer = server;

        server.on('connection', (socket) => this.onNewConnect(socket));
        server.on('error', (err) =>
        {
            console.error("[net] listen error: " + err);
            emit(this.listenError, String(err));
        });

        server.listen(this.m_listenPort);
        return true;   // 见文件头差异 1): 监听结果是异步的, 这里只能乐观返回
    }

    // void NetServer::stopServer()
    stopServer()
    {
        if (!this.tcpServer)
            return;

        const ids = Array.from(this.m_tcpClients.keys());
        for (const netId of ids)
            closeSocket(this, netId);

        try { this.tcpServer.close(); }
        catch (e) { /* 已关闭 */ }
        this.tcpServer = null;
    }

    // bool NetServer::isOpen()
    isOpen()
    {
        return !!(this.tcpServer && this.tcpServer.listening);
    }

    // int NetServer::boundPort()  —— JS 侧新增: 传 0 让系统分配端口时, 需要知道真实端口
    boundPort()
    {
        if (!this.tcpServer)
            return 0;
        const a = this.tcpServer.address();
        return (a && a.port) ? a.port : 0;
    }

    // void NetServer::closeConnect(int netId)
    closeConnect(netId)
    {
        const socket = this.m_tcpClients.get(netId);
        if (socket)
            socket.end();
    }

    // NetInfo NetServer::netInfo(int netId)
    netInfo(netId)
    {
        const socket = this.m_tcpClients.get(netId);
        const info = new NetInfo();
        if (socket)
        {
            // QHostAddress(socket.peerAddress().toIPv4Address()).toString()
            info.ip = socket.remoteAddress || "";
            info.port = socket.remotePort | 0;
        }
        return info;
    }

    // bool NetServer::sendPack(int netId, const NetPack &pack)
    sendPack(netId, pack)
    {
        // 发送数据包给指定客户
        return this.m_dataManager.sendPack(netId, pack);
    }

    // bool NetServer::sendPackExclude(int netId, const NetPack &pack)
    sendPackExclude(netId, pack)
    {
        // 发送数据包,排除指定客户
        let ret = true;
        for (const key of this.m_tcpClients.keys())
        {
            if (key !== netId)
                ret = this.sendPack(key, pack) && ret;   // ret &= sendPack(...)
        }
        return ret;
    }

    // bool NetServer::sendPackToAll(const NetPack &pack)
    sendPackToAll(pack)
    {
        // 发送数据包给所有
        let ret = true;
        for (const key of this.m_tcpClients.keys())
        {
            ret = this.sendPack(key, pack) && ret;
        }
        return ret;
    }

    // void NetServer::onNewConnect()
    //   C++ 用 nextPendingConnection() 取新连接; 这里由 'connection' 事件直接把 socket 传进来。
    onNewConnect(socket)
    {
        // 开启新的会话
        const netId = this.m_netId++;
        this.m_tcpClients.set(netId, socket);
        this.m_socketToNetId.set(socket, netId);
        this.m_dataManager.newSession(netId, socket);

        socket.on('data', (b) => this.onReadyRead(socket, b));
        socket.on('close', () => this.onDisconnected(socket));
        socket.on('error', (err) => console.error("[net] socket error(netId=" + netId + "): " + err));

        // 发送新连接
        emit(this.newConnect, netId);   // emit newConnect(netId);
    }

    // void NetServer::onDisconnected()
    onDisconnected(socket)
    {
        const netId = this.m_socketToNetId.get(socket);
        if (netId === undefined)
            return;                        // 已经清理过

        emit(this.disConnect, netId);
        closeSocket(this, netId);
    }

    // void NetServer::onReadyRead()
    //   C++ 里是 readyRead() -> recvPack() 同步读; 这里 data 事件已经把字节送来了。
    onReadyRead(socket, chunk)
    {
        const netId = this.m_socketToNetId.get(socket);
        if (netId === undefined)
            return;

        this.m_dataManager.onData(netId, chunk);
        while (true)
        {
            const pack = this.m_dataManager.takePack(netId);
            if (pack)
                emit(this.newDataRecv, netId, pack);   // emit newDataRecv(netId, pack);
            else
                break;
        }
    }
}

// void NetServer::closeSocket(int netId)   (C++ 里是成员函数, JS 侧抽成自由函数)
function closeSocket(self, netId)
{
    // 移除会话
    const socket = self.m_tcpClients.get(netId);
    self.m_tcpClients.delete(netId);
    if (socket)
        self.m_socketToNetId.delete(socket);
    self.m_dataManager.endSession(netId);

    // 关闭socket
    if (socket)
    {
        try { socket.end(); } catch (e) { /* ignore */ }
        try { socket.destroy(); } catch (e) { /* ignore */ }
    }
}

function emit(slots, ...args)
{
    for (let i = 0; i < slots.length; i++)
        slots[i](...args);
}
