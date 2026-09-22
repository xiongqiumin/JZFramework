/**
 * @jz/internal —— 框架内部工具 (**不是** Node 内置名, 没有 ambient 声明, 用户不要引)。
 * 只给 js_wrapper 各包的实现互相 import 用。
 */

/** 各种入参 -> Uint8Array(字符串按 enc 编码; Uint8Array / ArrayBuffer 原样/视图)
 *  ⚠ any 是刻意的(见实现里说明: Uint8Array 的 ArrayBufferLike/ArrayBuffer 泛型不互相赋值) */
export function toBytes(data: any, enc: any): any;
/** bytes -> 字符串(默认 utf8) */
export function fromBytes(u8: any, enc: any): any;
/** 形参位置上是"编码名"还是别的(write(data, enc) / write(data, cb) 重载判断) */
export function isEnc(v: any): boolean;

declare const internal: {
    toBytes: typeof toBytes;
    fromBytes: typeof fromBytes;
    isEnc: typeof isEnc;
};
export default internal;
