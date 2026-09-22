// ---------------------------------------------------------------------------
// net/netDataManager.js —— 对应 C++ common/net/netDataManager.h + netDataManager.cpp
//
// 帧的组包(packData)/收发(sendPack/recvPack/recvData)/拆包(parsePack)与包队列。
//
// 注意(JS 侧现状): 本文件是 C++ 实现的**逐行翻译**, 包括 parsePack() 的失败策略
// ——任一步校验不过就清空整段缓冲。JS 协议层实际运行的是更强的增量分帧器
// (./protocol.js 的 FrameParser: 失败只跳 1 字节重扫, 并用 maxFrameLen 识别假同步头),
// 两者后续会合并到本文件。
//
// Qt -> JS(已落实, 2026-09-13): QByteArray -> Uint8Array;
//   QDataStream(小端) -> ByteWriter/ByteReader;
//   QMap -> Map;
//   **QTcpSocket -> 引擎 net 模块的 Node 风格 Socket**: 关键差别是
//   QTcpSocket.read() 是**同步阻塞**读, 而 JS 引擎 socket 是 `on('data')` 事件驱动,
//   没有阻塞读 —— 所以收包路径由 recvPack() 改为 onData(sessionId, chunk);
//   QTcpSocket.waitForBytesWritten() 同样不存在, sendPack() 只负责入队。
//   Q_ASSERT -> console.assert。
// ---------------------------------------------------------------------------

import bytes from 'bytes';             // 默认导入取成员(具名导入在 tsgo 下 TS2614, 见 protocol.ts 说明)
const ByteWriter = bytes.ByteWriter;
import { NetPack, NetPackManager, PackInfo } from './netPack.js';
import { NetPackVariant, NETPACK_VARIANT } from './netPackDefine.js';

// uchar CaculateSum(uchar* pData, int nLen)
export function CaculateSum(data, offset, nLen)
{
    let btRet = 0;
    for (let i = 0; i < nLen; ++i)
    {
        btRet += data[offset + i];
    }

    return btRet & 0xff;
}

// class NetDataManager
export class NetDataManager
{
  [key: string]: any;   // 从 Qt/C++ 逐行翻译过来的动态成员, 见 globals.d.ts 的说明
    // NetDataManager();
    constructor()
    {
        this.mRecvSize = 1024 * 1024;
        this.mRecvBuffer = new Uint8Array(this.mRecvSize);   // char *mRecvBuffer = new char[mRecvSize];

        this.packHead = new Uint8Array(8);                   // QByteArray packHead;
        this.packHead.fill(0xff);

        this.packTail = new Uint8Array(1);                   // QByteArray packTail;
        this.packTail[0] = 0x5A;

        // struct SessionInfo { QTcpSocket *socket; QVector<NetPackPtr> packList; QByteArray buffer; };
        this.SessionInfo = class SessionInfo
        {
            constructor()
            {
                this.socket = null;
                this.packList = [];
                this.buffer = new Uint8Array(0);
            }
        };

        this.m_sessionInfo = new Map();   // QMap<int, SessionInfo> m_sessionInfo;
    }

    // ~NetDataManager();  -> 释放接收缓冲
    dispose()
    {
        this.mRecvBuffer = null;
    }

    // void newSession(int sessionId, QTcpSocket *socket);
    newSession(sessionId, socket)
    {
        const info = this.m_sessionInfo.get(sessionId) || new this.SessionInfo();
        info.socket = socket;
        this.m_sessionInfo.set(sessionId, info);
    }

    // void endSession(int sessionId);
    endSession(sessionId)
    {
        this.m_sessionInfo.delete(sessionId);
    }

    // void recvPack(int sessionId);
    //   C++ 语义 = "从 QTcpSocket 同步读到读空为止"。JS 引擎的 socket 是事件驱动的
    //   (socket.on('data')), **没有阻塞读**, 没有任何等价的同步入口 —— 收包路径必须改走
    //   onData()。这里保留同签名只为兼容旧调用点, 并记录一次"阻塞读不可用"。
    recvPack(sessionId)
    {
        noteNoBlockingIO("recvPack(同步读)");
    }

    // void onData(int sessionId, Uint8Array chunk);
    //   socket 'data' 回调: 把新到的字节追加进会话缓冲, 再交给 parsePack 分帧。
    onData(sessionId, chunk)
    {
        const info = this.m_sessionInfo.get(sessionId);
        if (!info)
            return;
        if (chunk && chunk.length)
            info.buffer = concat(info.buffer, chunk);
        this.parsePack(sessionId);
    }

    // void recvData(int sessionId, QByteArray data);
    recvData(sessionId, data)
    {
        const info = this.m_sessionInfo.get(sessionId);
        info.buffer = concat(info.buffer, data);
        this.parsePack(sessionId);
    }

    // QByteArray NetDataManager::packData(const NetPack &pack)
    packData(pack)
    {
        const send_buffer = [];
        const head_buffer = [];
        const data_buffer = [];

        pack.pack(data_buffer);          // pack.pack(data_buffer);
        pack.info.Pack(head_buffer);     // pack.info.Pack(head_buffer);

        send_buffer.push(this.packHead);
        send_buffer.push(...head_buffer);
        send_buffer.push(...data_buffer);

        const merged = concat(...send_buffer);
        const chkSum = CaculateSum(merged, 8, merged.length - 8);   // CaculateSum(ptr + 8, len - 8)
        const out = concat(merged, new Uint8Array([chkSum, this.packTail[0]]));

        return out;
    }

    // bool sendPack(int sessionId, const NetPack &pack, bool sync = false);
    sendPack(sessionId, pack, sync)
    {
        if (!this.m_sessionInfo.has(sessionId))
            return false;

        const send_buffer = this.packData(pack);
        const socket = this.m_sessionInfo.get(sessionId).socket;
        console.assert(socket !== null);   // Q_ASSERT(socket);

        // JS 引擎的 socket 是异步写: write() 只把数据交给引擎的 IO 循环,
        // 不存在 waitForBytesWritten() 这种"确认已发出"的同步入口。
        const ret = socket.write(send_buffer);
        if (sync)
            noteNoBlockingIO("waitForBytesWritten(同步等待写完)");

        return ret !== false;
    }

    // NetPackPtr takePack(int sessionId,int type,int param);
    takePackBy(sessionId, type, param)
    {
        let pack = null;
        const pack_list = this.m_sessionInfo.get(sessionId).packList;
        if (pack_list.length > 0)
        {
            if (type === 0) // 取第一个
            {
                pack = pack_list[0];
                pack_list.shift();
            }
            else
            {
                for (let i = 0; i < pack_list.length; i++)
                {
                    if ((type === 1 && pack_list[i].type() === param)
                        || (type === 2 && i === param))
                    {
                        pack = pack_list[i];
                        pack_list.splice(i, 1);
                        break;
                    }
                }
            }
        }

        return pack;
    }

    // NetPackPtr takePack(int sessionId);
    takePack(sessionId)
    {
        return this.takePackBy(sessionId, 0, 0);
    }

    // NetPackPtr takePackByType(int sessionId, int type);
    takePackByType(sessionId, type)
    {
        return this.takePackBy(sessionId, 1, type);
    }

    // NetPackPtr takePackBySeq(int sessionId, int seq);
    takePackBySeq(sessionId, seq)
    {
        return this.takePackBy(sessionId, 2, seq);
    }

    // void NetDataManager::parsePack(int sessionId)
    parsePack(sessionId)
    {
        const buffer = { data: this.m_sessionInfo.get(sessionId).buffer };
        const head = new PackInfo();
        let pack_buffer = null;
        let tail_buffer = null;

        const CHECK_SIZE = 1;

        const head_size = 8;
        const min_pack_head = 6;
        const tail_size = 2;  // tail + chk

        const min_pack_len = head_size + min_pack_head + this.packTail.length;
        let buffer_start = 0;
        while (buffer.data.length - buffer_start >= min_pack_len)
        {
            let start = indexOf(buffer.data, this.packHead, buffer_start);
            let end = 0;
            let headOffset = 0;
            if (start < 0)
            {
                console.error("[Net]Pack head invaild");
                buffer_start = buffer.data.length; // 全是无用数据，清除
                break;
            }

            // 接收数据不全
            start += this.packHead.length;
            headOffset = start;
            if (!head.UnPack(buffer.data, start))
            {
                this.m_sessionInfo.get(sessionId).buffer = buffer.data;
                return;
            }

            const pack_info_len = head.IsRecvPack() ? 7 : 6;
            start += pack_info_len;

            const bodySize = head.Length - 8 - 2 - pack_info_len;
            if (bodySize < 0)
            {
                console.error("[Net]Pack length invaild");
                buffer_start = buffer.data.length; // 全是无用数据，清除
                break;
            }

            // 接收数据不全
            if (start + bodySize + this.packTail.length > buffer.data.length)
            {
                console.assert(false); // upack 的时候已经检查了
                break;
            }

            end = start + bodySize;
            const chk = buffer.data[end];
            end += CHECK_SIZE;

            const clc_ck = CaculateSum(buffer.data, headOffset, head.Length - 8 - 2);
            if (chk !== clc_ck)
            {
                console.error("[Net]Pack chksum invaild");
                buffer_start = buffer.data.length; // 全是无用数据，清除
                break;
            }

            tail_buffer = buffer.data.slice(end, end + this.packTail.length);
            if (!equals(tail_buffer, this.packTail))
            {
                console.error("[Net]Pack tail invaild");
                buffer_start = buffer.data.length; // 全是无用数据，清除
                break;
            }
            pack_buffer = buffer.data.slice(start, start + bodySize);

            const pack = NetPackManager.instance().createPack(NETPACK_VARIANT);
            pack.info = head;
            if (pack.unPack(pack_buffer))
                this.m_sessionInfo.get(sessionId).packList.push(pack);
            else
                pack = null;   // delete pack;
            start += bodySize + CHECK_SIZE + this.packTail.length;

            // 下一次读起始
            buffer_start = start;
        }
        this.m_sessionInfo.get(sessionId).buffer = buffer.data.slice(buffer_start);
    }
}

// ---------------------------------------------------------------------------
// 移植缺口账本: Qt 的**同步阻塞 IO** 在 JS 引擎里没有对应物
//
// QTcpSocket 有 read()/waitForReadyRead()/waitForBytesWritten()/waitForConnected()
// 这类"阻塞到完成"的入口; 引擎的 socket 只有事件回调(on('data')/on('connect')...)。
// 凡是调用到这些同步入口的地方都被记在这里, 就是"必须改成异步"的清单。
// ---------------------------------------------------------------------------
export const blockingIoNotes = new Map();   // 名字 -> 次数

export function noteNoBlockingIO(what)
{
    blockingIoNotes.set(what, (blockingIoNotes.get(what) || 0) + 1);
    console.error("[net] JS 引擎的 socket 无阻塞 IO, 已跳过: " + what);
}

export function blockingIoReport()
{
    const list = Array.from(blockingIoNotes.entries()).sort((a, b) => b[1] - a[1]);
    for (const [k, n] of list)
        console.log("[net] 同步 IO 缺口 ×" + n + ": " + k);
    return list;
}

// ---- 小工具: 对应 QByteArray 的 indexOf / mid / append 语义 ----
function concat(...parts)
{
    let n = 0;
    for (let i = 0; i < parts.length; i++)
        n += (parts[i] && parts[i].length) ? parts[i].length : 0;
    const out = new Uint8Array(n);
    let o = 0;
    for (let i = 0; i < parts.length; i++)
    {
        if (!parts[i] || !parts[i].length)
            continue;
        out.set(parts[i], o);
        o += parts[i].length;
    }
    return out;
}

function indexOf(haystack, needle, from)
{
    const n = haystack.length - needle.length;
    for (let i = from; i <= n; i++)
    {
        let ok = true;
        for (let j = 0; j < needle.length; j++)
        {
            if (haystack[i + j] !== needle[j]) { ok = false; break; }
        }
        if (ok) return i;
    }
    return -1;
}

function equals(a, b)
{
    if (a.length !== b.length) return false;
    for (let i = 0; i < a.length; i++)
        if (a[i] !== b[i]) return false;
    return true;
}
