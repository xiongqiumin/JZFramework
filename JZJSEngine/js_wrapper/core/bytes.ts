/**
 * @jz/core/bytes —— 二进制读写工具 (ByteReader / ByteWriter / concatBytes / hexOf)
 *
 * 为什么需要这层:
 *   ES 标准的字节读写原语是 `Uint8Array` + `DataView` —— 类型化读写和字节序都已经有了
 *   (`getUint16(offset, true)` / `setUint16(offset, v, true)`), 但**没有"游标自增"**:
 *   每次都要自己传 offset, 且 DataView/Uint8Array 长度固定、不会自动扩容。
 *   协议解析天生是顺序读写(读 1 字节标志、读 2 字节长度、读 4 字节编号...),
 *   本模块提供的就是这层糖, 没有引入任何新能力。
 *
 *   注意: Node 的 `Buffer` 同样没有游标(`buf.readUInt16LE(offset)` 的 offset 必填),
 *   所以这不是"缺了 Node 的 API", 而是 ECMAScript 把便利层留给了生态
 *   (C++ 的 QDataStream、Java 的 ByteBuffer、C# 的 BinaryWriter 才带游标)。
 *
 * 本文件**零依赖**(不引用 __jz), 可以单独按子路径 import, 不必加载 core 的
 * Node 风格模块层(也就不需要 __jz.core 存在):
 *     import { ByteReader, ByteWriter } from '../jz_js/core/dist/bytes.js';
 */

'use strict';

export type Bytes = Uint8Array;

/** number | bigint 归一为 bigint (写 int64 用) */
function toBigInt(v: number | bigint): bigint
{
    return (typeof v === 'bigint') ? v : BigInt(Math.trunc(Number(v) || 0));
}

// ---------------------------------------------------------------------------
// ByteReader: 顺序读(小端)
// ---------------------------------------------------------------------------
export class ByteReader
{
    u8: Uint8Array;
    dv: DataView;
    /** 当前读游标 */
    offset: number;

    constructor(u8: Uint8Array)
    {
        this.u8 = u8;
        // DataView 必须带 byteOffset/byteLength: socket 给的 Uint8Array 往往是大
        // ArrayBuffer 上的视图, 省略这两个参数会从错误的绝对位置开始读。
        this.dv = new DataView(u8.buffer, u8.byteOffset, u8.byteLength);
        this.offset = 0;
    }

    /** 剩余可读字节数 */
    remaining(): number { return this.u8.length - this.offset; }
    eof(): boolean { return this.offset >= this.u8.length; }
    skip(n: number): ByteReader { this.offset += n; return this; }

    /** 单字节: 直接下标取值, 比走 DataView 快 */
    u8v(): number { return this.u8[this.offset++]; }
    i8(): number { const v = this.dv.getInt8(this.offset); this.offset += 1; return v; }
    u16(): number { const v = this.dv.getUint16(this.offset, true); this.offset += 2; return v; }
    i16(): number { const v = this.dv.getInt16(this.offset, true); this.offset += 2; return v; }
    u32(): number { const v = this.dv.getUint32(this.offset, true); this.offset += 4; return v; }
    i32(): number { const v = this.dv.getInt32(this.offset, true); this.offset += 4; return v; }
    /** int64 -> bigint (协议里容量/能量/耗时是 64 位) */
    i64(): bigint { const v = this.dv.getBigInt64(this.offset, true); this.offset += 8; return v; }
    u64(): bigint { const v = this.dv.getBigUint64(this.offset, true); this.offset += 8; return v; }

    /** 定长字节: 用 slice(拷贝), 可安全留存; subarray 会共享底层 buffer */
    bytes(n: number): Uint8Array
    {
        const r = this.u8.slice(this.offset, this.offset + n);
        this.offset += n;
        return r;
    }
}

// ---------------------------------------------------------------------------
// ByteWriter: 先按已知长度分配, 再顺序写(小端)
// ---------------------------------------------------------------------------
export class ByteWriter
{
    u8: Uint8Array;
    dv: DataView;
    /** 当前写游标 */
    offset: number;

    constructor(size: number)
    {
        this.u8 = new Uint8Array(size);
        this.dv = new DataView(this.u8.buffer);
        this.offset = 0;
    }

    /** 剩余可写字节数 */
    remaining(): number { return this.u8.length - this.offset; }
    skip(n: number): ByteWriter { this.offset += n; return this; }

    u8v(v: number): ByteWriter { this.dv.setUint8(this.offset, v & 0xff); this.offset += 1; return this; }
    i8(v: number): ByteWriter { this.dv.setInt8(this.offset, v); this.offset += 1; return this; }
    u16(v: number): ByteWriter { this.dv.setUint16(this.offset, v & 0xffff, true); this.offset += 2; return this; }
    i16(v: number): ByteWriter { this.dv.setInt16(this.offset, v, true); this.offset += 2; return this; }
    u32(v: number): ByteWriter { this.dv.setUint32(this.offset, v >>> 0, true); this.offset += 4; return this; }
    i32(v: number): ByteWriter { this.dv.setInt32(this.offset, v | 0, true); this.offset += 4; return this; }
    i64(v: number | bigint): ByteWriter { this.dv.setBigInt64(this.offset, toBigInt(v), true); this.offset += 8; return this; }
    u64(v: number | bigint): ByteWriter { this.dv.setBigUint64(this.offset, toBigInt(v), true); this.offset += 8; return this; }

    /** 整块写入(定长字符串/定长数组/已组好的 body) */
    raw(b: Uint8Array): ByteWriter { this.u8.set(b, this.offset); this.offset += b.length; return this; }
    /** 重复填同一个字节(如 0xFF 同步头、0x00 保留字段) */
    fill(n: number, v: number): ByteWriter
    {
        for (let i = 0; i < n; i++) this.u8[this.offset++] = v & 0xff;
        return this;
    }

    /** 取底层缓冲(未写满的尾部为 0) */
    bytes(): Uint8Array { return this.u8; }
}

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------

/** 拼接多段字节(Node `Buffer.concat` 的替代: Uint8Array 没有 concat) */
export function concatBytes(parts: Uint8Array[]): Uint8Array
{
    let n = 0;
    for (let i = 0; i < parts.length; i++) n += parts[i].length;
    const out = new Uint8Array(n);
    let o = 0;
    for (let i = 0; i < parts.length; i++) { out.set(parts[i], o); o += parts[i].length; }
    return out;
}

/** 十六进制字符串(调试/抓包比对用; 标准里是 tc39 提案的 Uint8Array.toHex) */
export function hexOf(u8: Uint8Array): string
{
    let s = '';
    for (let i = 0; i < u8.length; i++)
    {
        const b = u8[i];
        s += (b < 16 ? '0' : '') + b.toString(16);
    }
    return s;
}
