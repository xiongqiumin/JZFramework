/// <reference path="../types/common.d.ts" />
/**
 * @jz/buffer —— Node Buffer 兼容的二进制字节块 + WHATWG TextEncoder/TextDecoder。
 *
 * 和已有 `bytes` 的关系(不重复):
 *   - `bytes`(ByteReader/ByteWriter)补的是 ES 标准缺的"游标式顺序读写"(协议帧: 读 2 字节长度、
 *     4 字节编号…), 等价于 QDataStream/ByteBuffer;
 *   - `Buffer` 重点是**编码转换**(utf8/hex/base64/latin1)与索引读写。
 *   两者能组合: Buffer 内部就是 Uint8Array, 所以 `new ByteReader(buf)` 直接能吃。
 *
 * 实现说明: Buffer 用 Proxy 包一个内部 Uint8Array(而不是 `extends Uint8Array`), 这样既能像
 * Node 那样 `buf[0]` 索引、`buf.length`、当 Uint8Array 传给 DataView, 又能避免和
 * `Uint8Array.from` 的静态签名冲突, 也不依赖引擎是否支持 TypedArray 子类化。
 *
 * 这三个都是**全局**(对齐 Node: Buffer / TextEncoder / TextDecoder 启动即有, 不用 import);
 * 同时 `buffer` 作为内置名可 import(默认导出 = Buffer 类, 也具名导出 Buffer/TextEncoder/TextDecoder)。
 *
 * 编码实现:
 *   - hex / base64 直接复用 QuickJS 原生 `Uint8Array.toHex()/toBase64()/fromHex()/fromBase64()`
 *     (tc39 arraybuffer-base64 提案, 引擎已内置, 见 js_wrapper/types/common.d.ts);
 *   - utf8 自己实现(引擎无原生 TextEncoder);
 *   - atob/btoa 也是 QuickJS 原生全局, 这里不重复造。
 */

'use strict';

// ---------------------------------------------------------------------------
// 共享: utf8 编解码(纯 JS; 引擎无原生 TextEncoder)
// ---------------------------------------------------------------------------
function utf8Encode(str: string): Uint8Array
{
    const out: number[] = [];
    for (let i = 0; i < str.length; i++)
    {
        let code = str.charCodeAt(i);
        if (code < 0x80)
        {
            out.push(code);
        }
        else if (code < 0x800)
        {
            out.push(0xC0 | (code >> 6), 0x80 | (code & 0x3F));
        }
        else if (code >= 0xD800 && code <= 0xDBFF)
        {
            // 代理对 -> 码点
            const lo = str.charCodeAt(++i);
            code = 0x10000 + ((code - 0xD800) << 10) + (lo - 0xDC00);
            out.push(0xF0 | (code >> 18), 0x80 | ((code >> 12) & 0x3F),
                     0x80 | ((code >> 6) & 0x3F), 0x80 | (code & 0x3F));
        }
        else
        {
            out.push(0xE0 | (code >> 12), 0x80 | ((code >> 6) & 0x3F), 0x80 | (code & 0x3F));
        }
    }
    return Uint8Array.from(out);
}
function utf8Decode(u8: Uint8Array): string
{
    let s = '';
    let i = 0;
    const n = u8.length;
    while (i < n)
    {
        const b = u8[i++];
        if (b < 0x80)
        {
            s += String.fromCharCode(b);
        }
        else if (b < 0xE0)
        {
            s += String.fromCharCode(((b & 0x1F) << 6) | (u8[i++] & 0x3F));
        }
        else if (b < 0xF0)
        {
            s += String.fromCharCode(((b & 0x0F) << 12) | ((u8[i++] & 0x3F) << 6) | (u8[i++] & 0x3F));
        }
        else
        {
            let c = ((b & 0x07) << 18) | ((u8[i++] & 0x3F) << 12) | ((u8[i++] & 0x3F) << 6) | (u8[i++] & 0x3F);
            c -= 0x10000;
            s += String.fromCharCode(0xD800 + (c >> 10), 0xDC00 + (c & 0x3FF));
        }
    }
    return s;
}

const ENCODINGS = ['utf8', 'utf-8', 'hex', 'base64', 'latin1', 'binary', 'ascii'];
function normEnc(enc?: string): string
{
    enc = (enc || 'utf8').toLowerCase();
    if (enc === 'utf-8') enc = 'utf8';
    if (enc === 'binary' || enc === 'ascii') enc = 'latin1';
    return enc;
}

/** 字符串 -> 字节(按编码); 非字符串原样转成 Uint8Array 拷贝 */
function toBytes(input: any, enc?: string): Uint8Array
{
    if (input instanceof Buffer) return (input as any)._u8.slice();
    if (input instanceof Uint8Array) return input.slice();
    if (input instanceof ArrayBuffer) return new Uint8Array(input);
    if (Array.isArray(input)) return Uint8Array.from(input as number[]);
    if (typeof input === 'string')
    {
        const e = normEnc(enc);
        if (e === 'hex') return Uint8Array.fromHex(input);
        if (e === 'base64') return Uint8Array.fromBase64(input);
        if (e === 'latin1')
        {
            const o = new Uint8Array(input.length);
            for (let i = 0; i < input.length; i++) o[i] = input.charCodeAt(i) & 0xff;
            return o;
        }
        return utf8Encode(input);
    }
    throw new TypeError('Buffer.from: 不支持的输入类型');
}

/** 取出底层 Uint8Array(Buffer 实例可能是 Proxy, 直接读 _u8) */
function asU8(x: any): Uint8Array
{
    return (x instanceof Buffer) ? (x as any)._u8 : x;
}

// ---------------------------------------------------------------------------
// Buffer(Node 兼容子集; Proxy 包内部 Uint8Array)
// ---------------------------------------------------------------------------
const BUFFER_TRAP: ProxyHandler<any> = {
    get(t: any, p: any)
    {
        if (p === Symbol.iterator) return t._u8[Symbol.iterator].bind(t._u8);
        if (p === 'buffer') return t._u8.buffer;
        if (p === 'byteOffset') return t._u8.byteOffset;
        if (p === 'byteLength') return t._u8.byteLength;
        if (p === 'length') return t._u8.length;
        if (typeof p === 'string' && /^\d+$/.test(p)) return t._u8[Number(p)];
        return t[p];
    },
    set(t: any, p: any, v: any)
    {
        if (typeof p === 'string' && /^\d+$/.test(p)) { t._u8[Number(p)] = v; return true; }
        t[p] = v;
        return true;
    }
};

export class Buffer
{
    _u8: Uint8Array;

    constructor(input?: any, enc?: string)
    {
        this._u8 = (typeof input === 'number') ? new Uint8Array(input)
                  : (input == null) ? new Uint8Array(0)
                  : toBytes(input, enc);
        // 包成 Proxy: 让 buf[0] / buf.length / 传 DataView 的 buffer/byteOffset/byteLength 都透明。
        return new Proxy(this, BUFFER_TRAP) as any;
    }

    static from(input: any, enc?: string): Buffer { return new Buffer(input, enc); }

    static alloc(size: number, fill?: any, enc?: string): Buffer
    {
        const b = new Buffer(size);
        if (fill !== undefined && fill !== 0) b.fill(fill, 0, undefined, enc);
        return b;
    }
    static allocUnsafe(size: number): Buffer { return new Buffer(size); }
    static allocUnsafeSlow(size: number): Buffer { return new Buffer(size); }

    static concat(list: Uint8Array[], totalLength?: number): Buffer
    {
        if (!list.length) return new Buffer(0);
        let n = totalLength ?? 0;
        if (totalLength == null) for (const p of list) n += asU8(p).length;
        const out = new Uint8Array(n);
        let o = 0;
        for (const p of list) { const u = asU8(p); out.set(u, o); o += u.length; }
        return new Buffer(out);
    }

    static isBuffer(x: any): boolean { return x instanceof Buffer; }
    static isEncoding(enc: string): boolean { return ENCODINGS.indexOf((enc || '').toLowerCase()) >= 0; }
    static byteLength(str: string, enc?: string): number { return toBytes(str, enc).length; }
    static compare(a: Uint8Array, b: Uint8Array): number
    {
        const ua = asU8(a), ub = asU8(b);
        const n = Math.min(ua.length, ub.length);
        for (let i = 0; i < n; i++) if (ua[i] !== ub[i]) return ua[i] - ub[i];
        return ua.length - ub.length;
    }

    toString(enc?: string, start?: number, end?: number): string
    {
        const e = normEnc(enc);
        const sub = this._u8.subarray(start ?? 0, end ?? this._u8.length);
        if (e === 'hex') return sub.toHex().toLowerCase();
        if (e === 'base64') return sub.toBase64();
        if (e === 'latin1')
        {
            let s = '';
            for (let i = 0; i < sub.length; i++) s += String.fromCharCode(sub[i]);
            return s;
        }
        return utf8Decode(sub);
    }
    toJSON(): any { return { type: 'Buffer', data: Array.from(this._u8) }; }
    equals(other: Uint8Array): boolean { return Buffer.compare(this._u8, asU8(other)) === 0; }
    compare(other: Uint8Array): number { return Buffer.compare(this._u8, asU8(other)); }

    copy(target: Uint8Array, targetStart?: number, sourceStart?: number, sourceEnd?: number): number
    {
        const ts = targetStart ?? 0;
        const ss = sourceStart ?? 0;
        const se = sourceEnd ?? this._u8.length;
        const n = Math.min(se - ss, target.length - ts);
        for (let i = 0; i < n; i++) (target as any)[ts + i] = this._u8[ss + i];
        return n;
    }
    slice(start?: number, end?: number): Buffer { return new Buffer(this._u8.slice(start, end)); }
    subarray(start?: number, end?: number): Buffer { return new Buffer(this._u8.subarray(start, end)); }

    fill(value: any, offset?: number, end?: number, enc?: string): this
    {
        const o = offset ?? 0;
        const e = end ?? this._u8.length;
        let fillByte: number;
        if (typeof value === 'string') fillByte = toBytes(value, enc)[0] ?? 0;
        else if (value instanceof Uint8Array) fillByte = value[0];
        else fillByte = (value | 0) & 0xff;
        for (let i = o; i < e; i++) this._u8[i] = fillByte;
        return this;
    }

    indexOf(value: any, byteOffset?: number, enc?: string): number
    {
        const v = (typeof value === 'number') ? Uint8Array.from([value & 0xff])
                : (typeof value === 'string') ? toBytes(value, enc)
                : (value instanceof Uint8Array ? value : Uint8Array.from([value & 0xff]));
        const start = byteOffset ?? 0;
        outer: for (let i = start; i <= this._u8.length - v.length; i++)
        {
            for (let j = 0; j < v.length; j++) if (this._u8[i + j] !== v[j]) continue outer;
            return i;
        }
        return -1;
    }

    write(str: string, a?: number | string, b?: number | string, c?: string): number
    {
        let off = 0, enc = 'utf8', len: number | undefined;
        if (typeof a === 'string') enc = normEnc(a);
        else if (typeof a === 'number')
        {
            off = a;
            if (typeof b === 'number') { len = b; if (typeof c === 'string') enc = normEnc(c); }
            else if (typeof b === 'string') enc = normEnc(b);
        }
        const bytes = toBytes(str, enc);
        const n = len == null ? this._u8.length - off : Math.min(len, bytes.length);
        for (let i = 0; i < n; i++) this._u8[off + i] = bytes[i];
        return n;
    }
}

// ---------------------------------------------------------------------------
// TextEncoder / TextDecoder(WHATWG, 全局)
// ---------------------------------------------------------------------------
export class TextEncoder
{
    readonly encoding = 'utf-8';
    encode(input?: string): Uint8Array { return utf8Encode(input ?? ''); }
    encodeInto(input: string, dest: Uint8Array): { read: number; written: number }
    {
        const bytes = utf8Encode(input ?? '');
        const n = Math.min(bytes.length, dest.length);
        for (let i = 0; i < n; i++) dest[i] = bytes[i];
        return { read: n, written: n };
    }
}
export class TextDecoder
{
    readonly encoding = 'utf-8';
    constructor(label?: string, _opts?: any) { void label; }
    decode(input?: Uint8Array, _opts?: any): string
    {
        if (input == null) return '';
        return utf8Decode(input instanceof Uint8Array ? input : new Uint8Array(0));
    }
}

// ---- 挂全局(对齐 Node: 启动即有, 不用 import) ----
(globalThis as any).Buffer = Buffer;
(globalThis as any).TextEncoder = TextEncoder;
(globalThis as any).TextDecoder = TextDecoder;

export default Buffer;
