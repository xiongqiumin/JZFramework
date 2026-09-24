/**
 * @jz/buffer —— Node Buffer 兼容的字节块 + WHATWG TextEncoder/TextDecoder(与 buffer/index.ts 同源)。
 *
 * Buffer 是 Uint8Array 子类, 重点在编码转换(utf8/hex/base64/latin1)与索引读写, 与 bytes 的游标式
 * 顺序读写(ByteReader/ByteWriter)互补、不重复。hex/base64 复用引擎原生 Uint8Array.toHex/toBase64。
 */

/** Node Buffer 兼容子集(Uint8Array 子类) */
export class Buffer extends Uint8Array {
    constructor(input: any, encoding?: string);

    static from(input: any, encoding?: string): Buffer;
    static alloc(size: number, fill?: any, encoding?: string): Buffer;
    static allocUnsafe(size: number): Buffer;
    static allocUnsafeSlow(size: number): Buffer;
    static concat(list: Uint8Array[], totalLength?: number): Buffer;
    static isBuffer(x: any): boolean;
    static isEncoding(encoding: string): boolean;
    static byteLength(str: string, encoding?: string): number;
    static compare(a: Uint8Array, b: Uint8Array): number;

    toString(encoding?: string, start?: number, end?: number): string;
    toJSON(): { type: 'Buffer'; data: number[] };
    equals(other: Uint8Array): boolean;
    compare(other: Uint8Array): number;
    copy(target: Uint8Array, targetStart?: number, sourceStart?: number, sourceEnd?: number): number;
    slice(start?: number, end?: number): Buffer;
    subarray(start?: number, end?: number): Buffer;
    fill(value: any, offset?: number, end?: number, encoding?: string): this;
    indexOf(value: any, byteOffset?: number, encoding?: string): number;
    write(str: string, offset?: number, length?: number, encoding?: string): number;
}

/** WHATWG TextEncoder(utf-8 编码) */
export class TextEncoder {
    readonly encoding: string;
    encode(input?: string): Uint8Array;
    encodeInto(input: string, dest: Uint8Array): { read: number; written: number };
}

/** WHATWG TextDecoder(utf-8 解码) */
export class TextDecoder {
    readonly encoding: string;
    constructor(label?: string, options?: any);
    decode(input?: Uint8Array, options?: any): string;
}

export default Buffer;
