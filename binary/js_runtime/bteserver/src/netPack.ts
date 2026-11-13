// ---------------------------------------------------------------------------
// net/netPack.js —— 对应 C++ common/net/netPack.h + netPack.cpp
//
// 包信息(PackInfo) / 包基类(NetPack) / 包工厂(NetPackManager)。
//
// Qt -> JS: QByteArray -> Uint8Array; QDataStream(小端) -> 由 ByteWriter
//           (本工程自己的游标, 见 protocol.ts; 字节块是全局 Buffer) 承担;
//           QMap -> Map; QAtomicInteger -> 普通 number;
//           Q_ASSERT -> console.assert; QSharedPointer<NetPack> -> 对象引用。
//           DECLAR_PACK 宏在 JS 里展开为"给类挂静态成员"(netPackCreate/NetType),
//           并在子类里覆写 type()。
// ---------------------------------------------------------------------------

import { ByteWriter } from './protocol.js';   // 本工程的游标写(见 protocol.ts 的说明)
import {
    BTS_CMD_CONNECT_RESP, BTS_CMD_SEND_INFO_RESP, BTS_CMD_GET_RTC_RESP, BTS_CMD_SET_RTC_RESP,
    BGS66_CMD_SET_RT_INTERVAL_RESP, BTS_CMD_SEND_PARALLEL_RESP, BTS70_CMD_SET_TESTID_RESP,
    BTS_CMD_SET_BGSAUX_MAP_RESP70, BTS_CMD_SET_BGSAUX_RESP, BTS_CMD_SET_STEP_EX_RESP76,
    BTS_CMD_START_RESP70, BTS_CMD_STOP_RESP, BTS_CMD_SEND_LOG_RESP, BTS_CMD_SEND_DFDATA_RESP76,
} from './PackDefine.js';

// ---------------------------------------------------------------------------
// struct PackInfo
// ---------------------------------------------------------------------------
export class PackInfo
{
  [key: string]: any;   // 从 Qt/C++ 逐行翻译过来的动态成员, 见 globals.d.ts 的说明

    // static 常量必须在类体里声明(类外赋值不给类补类型, 见 globals.d.ts 的说明)。
    static LittleEndian: number = 0;   // QDataStream::LittleEndian

    // PackInfo();
    constructor()
    {
        this.DeviceID = 0;
        this.SubDevID = 0;
        this.Length = 0;
        this.CmdID = 0xffff;   // C++ 里初值是 -1(ushort 存不下), JS 侧记号用 0xffff
        this.Result = 0;
    }

    // bool IsRecvPack() const;
    IsRecvPack()
    {
        console.assert(this.CmdID !== 0xffff);   // Q_ASSERT(CmdID != -1);
        if (this.CmdID === BTS_CMD_CONNECT_RESP
            || this.CmdID === BTS_CMD_SEND_INFO_RESP
            || this.CmdID === BTS_CMD_GET_RTC_RESP
            || this.CmdID === BTS_CMD_SET_RTC_RESP
            || this.CmdID === BGS66_CMD_SET_RT_INTERVAL_RESP
            || this.CmdID === BTS_CMD_SEND_PARALLEL_RESP
            || this.CmdID === BTS70_CMD_SET_TESTID_RESP
            || this.CmdID === BTS_CMD_SET_BGSAUX_MAP_RESP70
            || this.CmdID === BTS_CMD_SET_BGSAUX_RESP
            || this.CmdID === BTS_CMD_SET_STEP_EX_RESP76
            || this.CmdID === BTS_CMD_START_RESP70
            || this.CmdID === BTS_CMD_STOP_RESP
            || this.CmdID === BTS_CMD_SEND_LOG_RESP
            || this.CmdID === BTS_CMD_SEND_DFDATA_RESP76)
            return true;

        return false;
    }

    // int headLength() const;
    headLength()
    {
        return this.IsRecvPack() ? 7 : 6;
    }

    // int bodyLength() const;
    bodyLength()
    {
        return this.Length - 8 - 2 - this.headLength();
    }

    // bool Pack(QByteArray &buffer) const;
    Pack(buffer)
    {
        // 注: Qt 侧要 out.setByteOrder(QDataStream::LittleEndian); 这里的 ByteWriter
        // 本身就是小端(bytes 包), 没有也不需要字节序设置接口。
        const out = new ByteWriter(6 + (this.IsRecvPack() ? 1 : 0));

        out.u8v(this.DeviceID);
        out.u8v(this.SubDevID);
        out.u16(this.Length);
        out.u16(this.CmdID);
        if (this.IsRecvPack())
            out.u8v(this.Result);

        // QDataStream 写到传入的 buffer 上, JS 侧返回写好的字节由调用方拼接
        buffer.push(out.bytes());
        return true;
    }

    // bool UnPack(QByteArray &buffer,int offset);
    UnPack(buffer, offset)
    {
        let pack_len = 6;
        const head_len = 8;
        const tail_len = 2;
        if (offset + pack_len + tail_len > buffer.length)
            return false;

        this.DeviceID = buffer[offset];
        this.SubDevID = buffer[offset + 1];
        this.Length = buffer[offset + 2] | (buffer[offset + 3] << 8);
        this.CmdID = buffer[offset + 4] | (buffer[offset + 5] << 8);

        if (this.IsRecvPack())
        {
            this.Result = buffer[offset + 6];
            pack_len += 1;
        }
        const body_len = this.Length - pack_len - head_len - tail_len;
        if (offset + pack_len + body_len + tail_len > buffer.length)
            return false;

        return true;
    }
}
// PackInfo.LittleEndian 已挪进类体(见 PackInfo 类开头)。

// ---------------------------------------------------------------------------
// class NetPack
// ---------------------------------------------------------------------------
export class NetPack
{
  [key: string]: any;   // 从 Qt/C++ 逐行翻译过来的动态成员, 见 globals.d.ts 的说明
    // NetPack();
    constructor()
    {
        this.info = new PackInfo();
    }

    // virtual ~NetPack();
    // virtual int type() const = 0;
    type() { throw new Error('NetPack.type() 是纯虚函数, 应由子类实现'); }
    // virtual bool pack(QByteArray &buffer) const = 0;
    pack(buffer) { throw new Error('NetPack.pack() 是纯虚函数, 应由子类实现'); }
    // virtual bool unPack(const QByteArray &buffer) = 0;
    unPack(buffer) { throw new Error('NetPack.unPack() 是纯虚函数, 应由子类实现'); }
}
// typedef QSharedPointer<NetPack> NetPackPtr;  -> JS 直接传对象引用

// ---------------------------------------------------------------------------
// DECLAR_PACK(Class, ClassType) 宏对应的辅助函数
//   static NetPack *netPackCreate(){ return new Class();}
//   static const int NetType = ClassType;
//   virtual int type() const{ return Class::NetType; }
// ---------------------------------------------------------------------------
export function netPackCreate(Class)
{
    return new Class();
}

// ---------------------------------------------------------------------------
// class NetPackManager
// ---------------------------------------------------------------------------
export class NetPackManager
{
  [key: string]: any;   // 从 Qt/C++ 逐行翻译过来的动态成员, 见 globals.d.ts 的说明

    // static NetPackManager *m_inst(单例); 类外的赋值不给类补类型, 所以声明在类体里。
    static _inst: any = null;

    // static NetPackManager *instance();
    static instance()
    {
        if (NetPackManager._inst === null)
            NetPackManager._inst = new NetPackManager();
        return NetPackManager._inst;
    }

    // NetPackManager();
    constructor()
    {
        this.mPackFactory = new Map();   // QMap<int, CreatePackFunc> mPackFactory;
        this.mPackSeq = 1;               // QAtomicInteger<unsigned int> mPackSeq;
    }

    // template<class T> void registPack()
    //   int id = T::NetType;
    //   Q_ASSERT(!mPackFactory.contains(id));
    //   mPackFactory[id] = T::netPackCreate;
    registPack(Class)
    {
        const id = Class.NetType;
        console.assert(!this.mPackFactory.has(id));
        this.mPackFactory.set(id, () => netPackCreate(Class));   // T::netPackCreate
    }

    // int genSeq();
    genSeq()
    {
        return this.mPackSeq++;
    }

    // NetPack *createPack(int id);
    createPack(id)
    {
        if (this.mPackFactory.has(id))
            return this.mPackFactory.get(id)();

        console.assert(false);   // Q_ASSERT(0);
        return null;
    }
}
// NetPackManager._inst 已挪进类体(见 NetPackManager 类开头)。
