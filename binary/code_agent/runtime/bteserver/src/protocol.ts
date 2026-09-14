// BTE 协议公共层 —— 帧编解码 + 二进制读写 + 命令常量
//
// 逐字节对照 JZBTEServer 的 C++ 实现编写:
//   common/net/netDataManager.cpp  packData() / parsePack()   帧: 同步头/长度/校验和/尾
//   common/net/netPack.cpp         PackInfo::Pack / UnPack    包头 6 字节(发送) / 7 字节(接收)
//   common/PackDefine.h            命令字、枚举、结构体布局
//
// 帧结构(Length 字段含同步头与尾部):
//   sync    8 × 0xFF
//   head    6 字节(发送) / 7 字节(接收):
//             DeviceID u8 | SubDevID u8 | Length u16LE | CmdID u16LE [| Result u8]
//   body    Length - 8 - 2 - headLen
//   chksum  u8 = Σ(head + body), 即自 sync 之后起 Length-8-2 个字节
//   tail    0x5A
//
// 二进制读写器(ByteReader/ByteWriter/hexOf/concatBytes)已提到共用处
// JZJSEngine/js_wrapper/core/bytes.ts, 本文件只 import + 再导出。
// 裸模块名 'core/dist/bytes.js' 由引擎的 libraryDirs 解析(运行根/jz_js、
// exe 的 jz_engine/jz_js); 走这个子路径不会执行 core 的 Node 模块层,
// 也就不需要 __jz.core 存在。

// 注意: `export ... from` 只做转发, 不会在本模块建局部绑定, 所以本地要用的
// 类型必须单独 import 一次。
import { ByteReader, ByteWriter } from 'core/dist/bytes.js';
export { ByteReader, ByteWriter, concatBytes, hexOf } from 'core/dist/bytes.js';

export const SYNC_LEN = 8;
export const SEND_HEAD_LEN = 6;
export const RECV_HEAD_LEN = 7;
export const MIN_FRAME_LEN = 8 + SEND_HEAD_LEN + 2; // 同步头 + 最短包头 + 校验和 + 尾
export const TAIL_BYTE = 0x5a;

// ---------------------------------------------------------------------------
// 命令字(PackDefine.h 的 #define 逐条搬运)
// ---------------------------------------------------------------------------
export const CMD = {
    CONNECT: 0x0000,
    CONNECT_RESP: 0x8000,

    SEND_INFO: 0x72,
    SEND_INFO_RESP: 0x72 + 0x80,

    GET_RTC: 0x02,
    GET_RTC_RESP: 0x82,
    SET_RTC: 0x03,
    SET_RTC_RESP: 0x83,

    BGS66_SET_RT_INTERVAL: 0x5e,
    BGS66_SET_RT_INTERVAL_RESP: 0x5e + 0x80,

    BTS70_SET_TESTID: 0x74,
    BTS70_SET_TESTID_RESP: 0x74 + 0x80,

    SET_BGSAUX_MAP70: 0x73,
    SET_BGSAUX_MAP_RESP70: 0x73 + 0x80,

    SET_BGSAUX: 0x39,
    SET_BGSAUX_RESP: 0x39 + 0x80,

    SET_STEP76_Ex: 0x0200,
    SET_STEP_EX_RESP76: 0x0200 + 0x8000,

    START70: 0x6e,
    START_RESP70: 0x6e + 0x80,

    STOP: 0x11,
    STOP_RESP: 0x91,

    RESUME: 0x14,
    RESUME_RESP: 0x94,
    RESUME_QUERY: 0x35,
    RESUME_QUERY_RESP: 0xb5,
    RESUME_CONFIRM: 0x36,
    RESUME_CONFIRM_RESP: 0xb6,

    STEP_JUMP: 0x6f,
    STEP_JUMP_RESP: 0x6f + 0x80,

    HEART_BEAT: 0x30,

    SEND_LOG: 0x0f,
    SEND_LOG_RESP: 0x8f,

    SEND_PARALLEL: 0x0300,
    SEND_PARALLEL_RESP: 0x8300,
    SEND_PARALLEL_CANCEL: 0x0400,

    SEND_RTDEVSTATUS: 0x44,

    SEND_RTDATA76: 0x07c,
    SEND_DFDATA76: 0x7d,
    SEND_DFDATA_RESP76: 0x7d + 0x80,

    LED_ON: 0x6000,
    LED_ON_RESP: 0x6000 + 0x80,
};

// PackInfo::IsRecvPack() 的那张表: 决定包头是 7 字节(多一个 Result)还是 6 字节
const RECV_CMDS = [
    CMD.CONNECT_RESP, CMD.SEND_INFO_RESP, CMD.GET_RTC_RESP, CMD.SET_RTC_RESP,
    CMD.BGS66_SET_RT_INTERVAL_RESP, CMD.SEND_PARALLEL_RESP, CMD.BTS70_SET_TESTID_RESP,
    CMD.SET_BGSAUX_MAP_RESP70, CMD.SET_BGSAUX_RESP, CMD.SET_STEP_EX_RESP76,
    CMD.START_RESP70, CMD.STOP_RESP, CMD.SEND_LOG_RESP, CMD.SEND_DFDATA_RESP76,
];
const RECV_SET = new Set(RECV_CMDS);

export function isRecvCmd(cmdId) { return RECV_SET.has(cmdId); }
export function headLenOf(cmdId) { return isRecvCmd(cmdId) ? RECV_HEAD_LEN : SEND_HEAD_LEN; }

// 命令名(日志用)
const CMD_NAME = {};
for (const k in CMD) CMD_NAME[CMD[k]] = k;
export function cmdName(cmdId) {
    return CMD_NAME[cmdId] !== undefined ? CMD_NAME[cmdId] : ('0x' + cmdId.toString(16));
}

// ---------------------------------------------------------------------------
// 小工具: 只留 BTE 协议私有的通道位图编解码
//   (hexOf/concatBytes 已提到 core/bytes, 见本文件顶部 import)
// ---------------------------------------------------------------------------

// 通道位图: ChannelID[8] 共 64 位, 字节 i 的 bit j(1<<j) = 第 (i*8+j+1) 个通道
// 对应 C++ stru_BTS_CMD_STEP_REQUEST::setChannel()
export function channelMask(channels) {
    const mask = new Uint8Array(8);
    for (let i = 0; i < channels.length; i++) {
        const idx = channels[i];
        if (idx >= 0 && idx < 64)
            mask[(idx / 8) | 0] |= (1 << (idx % 8));
    }
    return mask;
}

// 把位图还原成通道号数组(0 基)
export function maskChannels(mask) {
    const list = [];
    for (let i = 0; i < 8 && i < mask.length; i++)
        for (let j = 0; j < 8; j++)
            if (mask[i] & (1 << j)) list.push(i * 8 + j);
    return list;
}


// ---------------------------------------------------------------------------
// 组帧 / 解帧
// ---------------------------------------------------------------------------
export function encodeFrame(o) {
    const body = o.body || new Uint8Array(0);
    const headLen = headLenOf(o.cmdId);
    const total = SYNC_LEN + headLen + body.length + 2;
    if (total > 0xffff)
        throw new Error('frame too long: ' + total);

    const w = new ByteWriter(total);
    w.fill(SYNC_LEN, 0xff);
    w.u8v(o.deviceId & 0xff);
    w.u8v(o.subDevId & 0xff);
    w.u16(total);
    w.u16(o.cmdId);
    if (headLen === RECV_HEAD_LEN)
        w.u8v(o.result & 0xff);
    w.raw(body);

    const buf = w.bytes();
    let sum = 0;
    for (let i = SYNC_LEN; i < total - 2; i++) sum = (sum + buf[i]) & 0xff;
    w.u8v(sum);
    w.u8v(TAIL_BYTE);
    return buf;
}

// 解析一段完整帧(不含分帧, 长度已由 FrameParser 保证)
export function decodeFrame(u8) {
    const r = new ByteReader(u8);
    r.skip(SYNC_LEN);
    const deviceId = r.u8v();
    const subDevId = r.u8v();
    const length = r.u16();
    const cmdId = r.u16();
    const result = isRecvCmd(cmdId) ? r.u8v() : 0;
    const body = r.bytes(length - 2 - r.offset);   // 去掉 chksum(1) + tail(1)
    const chksum = r.u8v();
    const tail = r.u8v();
    return { deviceId, subDevId, length, cmdId, result, body, chksum, tail };
}

// ---------------------------------------------------------------------------
// 分帧器: 把 TCP 字节流切成完整帧(处理粘包/半包/脏数据)
//
// 与 C++ parsePack() 的差别(有意为之, 更稳):
//   C++ 一旦发现同步头/长度/校验/尾任一不对就**清空整段缓冲**;
//   这里改成"跳过 1 字节重新找同步头", 只丢真正坏掉的那部分,
//   避免一次错位把后面的好帧一起丢掉(真实设备上更容易恢复)。
// ---------------------------------------------------------------------------
export class FrameParser {
  [key: string]: any;   // 从 Qt/C++ 逐行翻译过来的动态成员, 见 globals.d.ts 的说明
    constructor(opts) {
        opts = opts || {};
        this.buf = new Uint8Array(0);
        this.maxBuffer = opts.maxBuffer || (1024 * 1024);
        // 单帧长度上限, 用于识别"假同步头":
        //   下位机上报的包都很小(实时帧 75B、设备信息 108B、DF 包按协议注释
        //   "最大个数是总长长度不超过 1024 字节"), 所以 2048 已足够宽松。
        //   没有这个上限时, 一段脏数据里凑出 8 个 0xFF 后跟一个很大的 Length,
        //   分帧器会以为"半包未到"而一直等待, 后面的好帧全被堵住。
        //   若某类下位机真会发更大的包, 构造时把 maxFrameLen 调大即可。
        this.maxFrameLen = opts.maxFrameLen || 2048;
        this.stats = { frames: 0, badChecksum: 0, badTail: 0, badLength: 0, falseSync: 0, droppedBytes: 0, overflow: 0 };
    }

    reset() {
        this.buf = new Uint8Array(0);
        this.stats.frames = 0;
        this.stats.badChecksum = 0;
        this.stats.badTail = 0;
        this.stats.badLength = 0;
        this.stats.droppedBytes = 0;
        this.stats.overflow = 0;
    }

    // 喂入一段字节, 返回本次新解析出的帧数组
    push(chunk) {
        if (chunk && chunk.length) {
            const merged = new Uint8Array(this.buf.length + chunk.length);
            merged.set(this.buf, 0);
            merged.set(chunk, this.buf.length);
            this.buf = merged;
        }
        const out = this._parse();
        if (this.buf.length > this.maxBuffer) {
            this.stats.overflow++;
            this.buf = new Uint8Array(0);
        }
        return out;
    }

    _findSync() {
        const n = this.buf.length - SYNC_LEN;
        for (let i = 0; i <= n; i++) {
            let ok = true;
            for (let j = 0; j < SYNC_LEN; j++) {
                if (this.buf[i + j] !== 0xff) { ok = false; break; }
            }
            if (ok) return i;
        }
        return -1;
    }

    _parse() {
        const out = [];
        for (;;) {
            if (this.buf.length < MIN_FRAME_LEN) break;

            const at = this._findSync();
            if (at < 0) {
                // 没有同步头: 保留末尾 7 字节(可能是被截断的 8×0xFF)
                const keep = Math.min(SYNC_LEN - 1, this.buf.length);
                this.stats.droppedBytes += this.buf.length - keep;
                this.buf = this.buf.slice(this.buf.length - keep);
                break;
            }
            if (at > 0) {
                this.stats.droppedBytes += at;
                this.buf = this.buf.slice(at);
            }
            if (this.buf.length < MIN_FRAME_LEN) break;

            const length = this.buf[10] | (this.buf[11] << 8);
            const cmdId = this.buf[12] | (this.buf[13] << 8);
            const headLen = headLenOf(cmdId);
            const bodyLen = length - SYNC_LEN - 2 - headLen;

            if (length < SYNC_LEN + headLen + 2 || bodyLen < 0) {
                // 长度不合法 => 这个同步头是假的
                this.stats.badLength++;
                this.stats.droppedBytes += 1;
                this.buf = this.buf.slice(1);
                continue;
            }
            if (length > this.maxFrameLen) {
                // 长度离谱 => 也是假同步头(否则会一直等一个永远不来的"半包")
                this.stats.falseSync++;
                this.stats.droppedBytes += 1;
                this.buf = this.buf.slice(1);
                continue;
            }
            if (this.buf.length < length) break;   // 半包, 等下次

            const chkPos = length - 2;
            let sum = 0;
            for (let i = SYNC_LEN; i < chkPos; i++) sum = (sum + this.buf[i]) & 0xff;
            if (this.buf[chkPos] !== sum) {
                this.stats.badChecksum++;
                this.stats.droppedBytes += 1;
                this.buf = this.buf.slice(1);
                continue;
            }
            if (this.buf[length - 1] !== TAIL_BYTE) {
                this.stats.badTail++;
                this.stats.droppedBytes += 1;
                this.buf = this.buf.slice(1);
                continue;
            }

            const whole = this.buf.slice(0, length);
            const frame = decodeFrame(whole);
            frame.raw = whole;
            out.push(frame);
            this.stats.frames++;
            this.buf = this.buf.slice(length);
        }
        return out;
    }
}
