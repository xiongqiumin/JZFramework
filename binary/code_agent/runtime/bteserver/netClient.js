// ---------------------------------------------------------------------------
// netClient.js —— 对应 C++ common/net/netClient.h + netClient.cpp
//
// 客户端: 连服务器、收发包、等待某个包。
//
// Qt -> JS (2026-09-13 改为"真跑得起来"的版本):
//   QTcpSocket -> 引擎 net 模块的 Socket (net/dist/index.js)
//     connectToHost(host,port)        -> socket.connect(port, host)(异步)
//     waitForConnected(ms)            -> **没有对应物**(引擎 socket 无阻塞等待)
//     disconnectFromHost()/abort()    -> socket.end() / socket.destroy()
//     state()/waitForReadyRead(ms)    -> 'connect'/'close' 事件 + readyState
//   QTimer::singleShot(0, lambda) -> setTimeout(..., 0);
//   QObject 信号 -> 回调数组字段(sigConnect/sigDisConnect/sigError/newDataRecv)
//
// 与 C++ 的**语义差异(重要)**:
//   C++ 的 connectToHost()/waitPack() 是**同步阻塞**的 —— 返回即"已连上/已拿到包"。
//   JS 引擎是单线程事件循环, 阻塞等待 = 死锁, 所以:
//     * connectToHost() 只负责"发起连接", 结果经 sigConnect 通知(对应 C++ 的
//       connectToHostAsync()); 需要同步语义的调用点必须改为事件回调。
//     * waitPack()/waitPackAny()/waitPackByType()/waitPackBySeq() 保留同签名但
//       **返回 null 并记账**(不再能靠阻塞拿到包); 异步版本是 waitPackAsync() 等, 返回 Promise。
// ---------------------------------------------------------------------------

import { net } from 'net/dist/index.js';
import { NetDataManager, noteNoBlockingIO } from './netDataManager.js';

export class NetClient
{
    // NetClient(QObject * parent = NULL);
    constructor(parent)
    {
        this.parent = parent || null;

        this.tcpSocket = null;
        this.mWaitTime = 30000;

        this.mUser = 1;
        this.mWaitData = false;
        this.mUserDisconnect = false;

        this.m_dataManager = new NetDataManager();

        // signals
        this.sigConnect = [];      // void sigConnect();
        this.sigDisConnect = [];   // void sigDisConnect();
        this.sigError = [];        // void sigError();
        this.newDataRecv = [];     // void newDataRecv(NetPackPtr pack);
    }

    // ~NetClient();
    dispose()
    {
        this.disconnectFromHost();
        this.tcpSocket = null;
    }

    // QTcpSocket *createSocket()  —— 引擎里没有"先 new 后 connect"的两段式:
    // 连接必须由 socket.connect() 发起, 所以 socket 与"连接动作"绑在一起创建。
    _makeSocket(host, port)
    {
        const socket = net.createConnection({ host: host, port: port | 0 });
        socket.on('connect', () => this.onConnected());
        socket.on('data', (b) => this.onReadyRead(b));
        socket.on('close', () => this.onDisConnected());
        socket.on('error', (err) =>
        {
            console.error("[net] client error: " + err);
            emit(this.sigError);
        });
        this.tcpSocket = socket;
        return socket;
    }

    // void NetClient::disconnectFromHost()
    disconnectFromHost()
    {
        if (!this.isConnect())
            return;

        this.mUserDisconnect = true;
        try { this.tcpSocket.end(); }
        catch (e) { /* ignore */ }
    }

    // bool NetClient::connectToHost(QString host,int port)
    //   C++ 阻塞等连接成功; 这里只发起连接(等价 C++ 的 connectToHostAsync),
    //   返回 true 表示"已发起", 连上与否看 sigConnect。
    connectToHost(host, port)
    {
        if (this.tcpSocket)
        {
            try { this.tcpSocket.destroy(); }
            catch (e) { /* ignore */ }
            this.tcpSocket = null;
        }

        noteNoBlockingIO("NetClient::connectToHost(同步等待连接)");
        this.mUserDisconnect = false;
        this.m_host = host;
        this.m_port = port | 0;
        this.mUser = 1;
        this._makeSocket(host, port);
        return true;
    }

    // void NetClient::connectToHostAsync(QString host, int port)
    connectToHostAsync(host, port)
    {
        this.connectToHost(host, port);
    }

    // bool NetClient::isConnect()
    isConnect()
    {
        return !!(this.tcpSocket && this.tcpSocket.readyState === "open");
    }

    // QTcpSocket *NetClient::clientSocket()
    clientSocket()
    {
        return this.tcpSocket;
    }

    // void NetClient::onConnected()
    onConnected()
    {
        this.m_dataManager.newSession(this.mUser, this.tcpSocket);
        emit(this.sigConnect);   // emit sigConnect();
    }

    // void NetClient::onDisConnected()
    onDisConnected()
    {
        // 发送连接断开信号
        if (!this.mUserDisconnect)
            emit(this.sigDisConnect);

        // 结束会话
        this.m_dataManager.endSession(this.mUser);
        this.mUser = 0;
        this.mUserDisconnect = false;
    }

    // void NetClient::onReadyRead()
    //   C++: readyRead() -> recvPack() 同步读 -> 分发给 newDataRecv
    onReadyRead(chunk)
    {
        // 是否正在等待数据: 等待期间包留在队列里, 由 waitPackAsync 取走
        this.m_dataManager.onData(this.mUser, chunk);
        if (this.mWaitData)
            return;

        this.dispatchPack();
    }

    // void NetClient::dispatchPack()
    dispatchPack()
    {
        while (true)
        {
            const pack = this.m_dataManager.takePack(this.mUser);
            if (pack)
            {
                emit(this.newDataRecv, pack);   // emit newDataRecv(pack);
            }
            else
                break;
        }
    }

    // bool NetClient::sendPack(const NetPack &pack)
    sendPack(pack)
    {
        // 发送数据包
        return this.m_dataManager.sendPack(this.mUser, pack, true);
    }

    // NetPackPtr NetClient::waitPack(int type, int param)
    //   C++ 是阻塞轮询; 引擎里不可能阻塞 —— 保留签名只为编译期兼容, 一律返回 null。
    waitPack(type, param)
    {
        noteNoBlockingIO("NetClient::waitPack(同步等待数据包)");
        return null;
    }

    // NetPackPtr NetClient::waitPackAny()
    waitPackAny()
    {
        return this.waitPack(0, 0);
    }

    // NetPackPtr NetClient::waitPackByType(int type)
    waitPackByType(type)
    {
        return this.waitPack(1, type);
    }

    // NetPackPtr NetClient::waitPackBySeq(int seq)
    waitPackBySeq(seq)
    {
        return this.waitPack(2, seq);
    }

    // Promise<NetPackPtr> waitPackAsync(int type, int param)
    //   异步版本: 用 setTimeout 轮询队列(不阻塞事件循环), 到点或超时(1s)结束。
    waitPackAsync(type, param, timeoutMs)
    {
        const limit = (timeoutMs === undefined) ? this.mWaitTime : timeoutMs;
        const self = this;
        return new Promise((resolve) =>
        {
            self.mWaitData = true;
            const deadline = Date.now() + limit;
            const poll = () =>
            {
                let pack = null;
                if (type === 0) pack = self.m_dataManager.takePack(self.mUser);
                else if (type === 1) pack = self.m_dataManager.takePackByType(self.mUser, param);
                else pack = self.m_dataManager.takePackBySeq(self.mUser, param);

                if (pack)
                {
                    self.mWaitData = false;
                    self.dispatchPack();     // 与 C++ 的 QTimer::singleShot(0, dispatchPack) 对齐
                    resolve(pack);
                    return;
                }
                if (Date.now() >= deadline)
                {
                    self.mWaitData = false;
                    resolve(null);
                    return;
                }
                setTimeout(poll, 10);
            };
            poll();
        });
    }

    waitPackAnyAsync(timeoutMs) { return this.waitPackAsync(0, 0, timeoutMs); }
    waitPackByTypeAsync(type, timeoutMs) { return this.waitPackAsync(1, type, timeoutMs); }
    waitPackBySeqAsync(seq, timeoutMs) { return this.waitPackAsync(2, seq, timeoutMs); }
}

// Qt 的 emit 在 JS 里是"遍历回调数组"
function emit(slots, ...args)
{
    for (let i = 0; i < slots.length; i++)
        slots[i](...args);
}
