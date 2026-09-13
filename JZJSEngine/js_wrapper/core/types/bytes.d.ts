/**
 * @jz/core/bytes —— 二进制读写工具的类型 (与 bytes.ts 同源)。
 *
 * 零依赖, 可单独按子路径使用:
 *   import { ByteReader, ByteWriter } from '<...>/jz_js/core/dist/bytes.js';
 *
 * 说明: 这一层补的是 ES 标准缺的"游标自增"(DataView 要求自己传 offset,
 * 且长度固定不扩容)。Node 的 Buffer 同样没有游标, 所以不是缺 Node API。
 */

export type Bytes = Uint8Array;

/** 顺序读(小端)。构造时 DataView 会带 byteOffset/byteLength, 直接吃 socket 给的 Uint8Array 视图。 */
export class ByteReader {
    constructor(u8: Uint8Array);

    /** 当前读游标 */
    offset: number;

    remaining(): number;
    eof(): boolean;
    skip(n: number): this;

    /** 单字节 */
    u8v(): number;
    i8(): number;
    u16(): number;
    i16(): number;
    u32(): number;
    i32(): number;
    /** int64 -> bigint */
    i64(): bigint;
    u64(): bigint;

    /** 定长字节(拷贝, 可留存) */
    bytes(n: number): Uint8Array;
}

/** 顺序写(小端)。构造时给定总长(协议帧通常是先算长度再分配)。 */
export class ByteWriter {
    constructor(size: number);

    /** 当前写游标 */
    offset: number;

    remaining(): number;
    skip(n: number): this;

    u8v(v: number): this;
    i8(v: number): this;
    u16(v: number): this;
    i16(v: number): this;
    u32(v: number): this;
    i32(v: number): this;
    i64(v: number | bigint): this;
    u64(v: number | bigint): this;

    /** 整块写入 */
    raw(b: Uint8Array): this;
    /** 重复填同一字节 */
    fill(n: number, v: number): this;
    /** 取底层缓冲(未写满的尾部为 0) */
    bytes(): Uint8Array;
}

/** 拼接多段字节(Node `Buffer.concat` 的替代) */
export function concatBytes(parts: Uint8Array[]): Uint8Array;

/** 十六进制字符串(调试/抓包比对用) */
export function hexOf(u8: Uint8Array): string;
