/**
 * @jz/internal —— 框架内部共享工具 (**不是** Node 内置名, 也没有裸名)
 *
 * 只给 js_wrapper 内部各包用(net / dgram / http / ws / mqtt / fs ... 的 data 与
 * encoding 转换都是同一套)。用户代码不要 import 这里: 它不是契约的一部分, 随时会变。
 *
 * 为什么要单独一个包而不是各包抄一份: 这几个函数是**行为对齐 Node 的边界**
 * (utf8/hex/base64/latin1 各自的语义、ArrayBuffer 与 Uint8Array 的接受范围),
 * 抄多份就会各自漂移 —— 以前 core 与 net 就各有一份。
 */
'use strict';

declare const __jz: any;

/** 各种入参 -> Uint8Array(字符串按 enc 编码; Uint8Array / ArrayBuffer 原样/视图)
 *  ⚠ 返回类型写 any 是刻意的: Uint8Array<ArrayBufferLike> 与 Uint8Array<ArrayBuffer>
 *  在这套 lib 下不互相赋值, 写成 Uint8Array 会让所有调用点(如 ServerResponse.end)报 TS2322。 */
export function toBytes(data: any, enc: any): any {
    if (data === null || data === undefined) return new Uint8Array(0);
    if (typeof data === 'string') {
        if (enc === 'hex') return (Uint8Array as any).fromHex(data);
        if (enc === 'base64') return (Uint8Array as any).fromBase64(data);
        if (enc === 'latin1') {
            const r = new Uint8Array(data.length);
            for (let i = 0; i < data.length; i++) r[i] = data.charCodeAt(i) & 0xff;
            return r;
        }
        return __jz.bytes.fromString(data);
    }
    if (data instanceof Uint8Array) return data;
    if (data instanceof ArrayBuffer) return new Uint8Array(data);
    throw new TypeError('data must be string, Uint8Array or ArrayBuffer');
}

/** bytes -> 字符串(默认 utf8) */
export function fromBytes(u8: any, enc: any): any {
    if (!enc || enc === 'utf8' || enc === 'utf-8') return __jz.bytes.toString(u8);
    if (enc === 'hex') return u8.toHex();
    if (enc === 'base64') return u8.toBase64();
    if (enc === 'latin1') {
        let s = '';
        for (let i = 0; i < u8.length; i++) s += String.fromCharCode(u8[i]);
        return s;
    }
    throw new TypeError('unknown encoding: ' + enc);
}

/** 形参位置上是"编码名"还是别的(Node 的 write(data, enc) / write(data, cb) 重载要用) */
export function isEnc(v: any): boolean { return typeof v === 'string'; }

export default { toBytes, fromBytes, isEnc };
