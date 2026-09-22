/**
 * @jz/crypto —— Node `crypto` 模块(子集: randomUUID / randomBytes / randomInt / createHash)。
 *
 * 走 __jz.core 的随机与 md5 原子能力。默认导出 = 模块对象, 具名导出 = 各函数。
 */
'use strict';

import { fromBytes, toBytes } from 'internal/dist';

declare const __jz: any;

export function randomUUID(): string { return __jz.core.uuid(); }

export function randomBytes(size: any, cb: any) {
    const u8 = __jz.core.randomBytes(size);
    if (cb) cb(null, u8);
    return u8;
}

export function randomInt(min: any, max: any) {
    if (max === undefined) { max = min; min = 0; }
    return __jz.core.randomInt(min, max);
}

export function createHash(algorithm: any) {
    algorithm = String(algorithm).toLowerCase();
    if (algorithm !== 'md5')
        throw new Error("ERR_OSSL_EVP_UNSUPPORTED: Unsupported algorithm: " + algorithm);
    let acc = new Uint8Array(0);
    return {
        update: function (data: any, enc: any) {
            const b = toBytes(data, enc);
            const merged = new Uint8Array(acc.length + b.length);
            merged.set(acc, 0);
            merged.set(b, acc.length);
            acc = merged;
            return this;
        },
        digest: function (enc: any) {
            const d = __jz.core.md5(acc);
            return enc ? fromBytes(d, enc) : d;
        },
    };
}

const crypto = { randomUUID, randomBytes, randomInt, createHash };
export default crypto;
