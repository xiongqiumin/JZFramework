/**
 * @jz/buffer —— Node Buffer 兼容的字节块 + WHATWG TextEncoder/TextDecoder(与 buffer/index.ts 同源)。
 *
 * 字节能力只此一处: 编码转换(utf8/hex/base64/latin1) + 索引读写 + **数值读写**
 * (`readUInt16LE(offset)` / `writeUInt32LE(v, offset)` 等, 与 Node 同名同义)。
 * offset 必填、不自动前进 —— 没有游标式读写器(Node 也没有); 顺序解析由调用方推进 offset。
 * hex/base64 复用引擎原生 Uint8Array.toHex/toBase64。
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

    // ---- 数值读写(与 Node 同名同义; offset 必填, 返回 offset + 字节数) ----
    readUInt8(offset?: number): number;
    readInt8(offset?: number): number;
    readUInt16LE(offset?: number): number;
    readUInt16BE(offset?: number): number;
    readInt16LE(offset?: number): number;
    readInt16BE(offset?: number): number;
    readUInt32LE(offset?: number): number;
    readUInt32BE(offset?: number): number;
    readInt32LE(offset?: number): number;
    readInt32BE(offset?: number): number;
    readBigUInt64LE(offset?: number): bigint;
    readBigUInt64BE(offset?: number): bigint;
    readBigInt64LE(offset?: number): bigint;
    readBigInt64BE(offset?: number): bigint;
    readFloatLE(offset?: number): number;
    readFloatBE(offset?: number): number;
    readDoubleLE(offset?: number): number;
    readDoubleBE(offset?: number): number;
    /** 变长(1~6 字节) */
    readUIntLE(offset: number, byteLength: number): number;
    readUIntBE(offset: number, byteLength: number): number;
    readIntLE(offset: number, byteLength: number): number;
    readIntBE(offset: number, byteLength: number): number;

    writeUInt8(value: number, offset?: number): number;
    writeInt8(value: number, offset?: number): number;
    writeUInt16LE(value: number, offset?: number): number;
    writeUInt16BE(value: number, offset?: number): number;
    writeInt16LE(value: number, offset?: number): number;
    writeInt16BE(value: number, offset?: number): number;
    writeUInt32LE(value: number, offset?: number): number;
    writeUInt32BE(value: number, offset?: number): number;
    writeInt32LE(value: number, offset?: number): number;
    writeInt32BE(value: number, offset?: number): number;
    writeBigUInt64LE(value: number | bigint, offset?: number): number;
    writeBigUInt64BE(value: number | bigint, offset?: number): number;
    writeBigInt64LE(value: number | bigint, offset?: number): number;
    writeBigInt64BE(value: number | bigint, offset?: number): number;
    writeFloatLE(value: number, offset?: number): number;
    writeFloatBE(value: number, offset?: number): number;
    writeDoubleLE(value: number, offset?: number): number;
    writeDoubleBE(value: number, offset?: number): number;
    writeUIntLE(value: number, offset: number, byteLength: number): number;
    writeUIntBE(value: number, offset: number, byteLength: number): number;
    writeIntLE(value: number, offset: number, byteLength: number): number;
    writeIntBE(value: number, offset: number, byteLength: number): number;

    /** 就地两两交换(Node Buffer 有, Uint8Array 没有) */
    swap16(): this;
    swap32(): this;
    swap64(): this;

    /** 整块写入(Uint8Array.prototype.set 的同义) */
    set(src: Uint8Array, offset?: number): void;
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
