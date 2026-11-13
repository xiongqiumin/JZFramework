// 11_buffer_encoding —— Node Buffer 兼容 + WHATWG TextEncoder/TextDecoder + atob/btoa。
//
// 这些运行时启动即有(全局), 也可显式 import('buffer')。对照真实 Node v26 的行为。

import { check, eq } from './helper/assert.js';

console.log('--- 11_buffer_encoding ---');

// ---- 1) Buffer.from / toString(utf8 / hex / base64) ----
eq('Buffer.from("hello").length', Buffer.from('hello').length, 5);
eq('Buffer.from("hello").toString(hex)', Buffer.from('hello').toString('hex'), '68656c6c6f');
eq('Buffer.from("hello").toString(utf8)', Buffer.from('hello').toString('utf8'), 'hello');
eq('Buffer.from("hello").toString(base64)', Buffer.from('hello').toString('base64'), 'aGVsbG8=');
eq('Buffer.from("aGVsbG8=","base64").toString()', Buffer.from('aGVsbG8=', 'base64').toString(), 'hello');
eq('Buffer.from("68656c6c6f","hex").toString()', Buffer.from('68656c6c6f', 'hex').toString(), 'hello');

// ---- 2) concat / alloc / isBuffer / equals / indexOf ----
eq('Buffer.concat', Buffer.concat([Buffer.from('ab'), Buffer.from('cd')]).toString(), 'abcd');
eq('Buffer.alloc(3).length', Buffer.alloc(3).length, 3);
check('Buffer.alloc 全 0', Buffer.alloc(3)[0] === 0 && Buffer.alloc(3)[2] === 0);
check('Buffer.isBuffer true', Buffer.isBuffer(Buffer.from('x')));
check('Buffer.isBuffer false(Uint8Array)', !Buffer.isBuffer(new Uint8Array(1)));
check('Buffer.equals true', Buffer.from([1, 2, 3]).equals(Buffer.from([1, 2, 3])));
check('Buffer.equals false', !Buffer.from([1, 2, 3]).equals(Buffer.from([1, 2, 4])));
eq('Buffer.indexOf 字节', Buffer.from('abc').indexOf(97), 0);
eq('Buffer.indexOf 字符串', Buffer.from('abc').indexOf('b'), 1);

// ---- 3) slice / subarray / copy ----
eq('Buffer.slice', Buffer.from('hello').slice(1, 3).toString(), 'el');
eq('Buffer.subarray', Buffer.from('hello').subarray(1, 3).toString(), 'el');
{
    const src = Buffer.from('hello');
    const dst = Buffer.alloc(5);
    eq('Buffer.copy 返回 n', src.copy(dst), 5);
    eq('Buffer.copy 内容', dst.toString(), 'hello');
}

// ---- 4) utf8 多字节(中文 / emoji) ----
eq('Buffer.byteLength 中', Buffer.byteLength('中'), 3);
eq('Buffer.from 中 length', Buffer.from('中').length, 3);
eq('Buffer.from 中 roundtrip', Buffer.from('中').toString('utf8'), '中');
eq('Buffer.from 中 hex', Buffer.from('中').toString('hex'), 'e4b8ad');
eq('Buffer emoji length', Buffer.from('😀').length, 4);
eq('Buffer emoji roundtrip', Buffer.from('😀').toString('utf8'), '😀');
check('Buffer.isEncoding', Buffer.isEncoding('hex') && Buffer.isEncoding('utf8') && !Buffer.isEncoding('xyz'));

// ---- 5) TextEncoder / TextDecoder(WHATWG) ----
eq('TextEncoder hello', Array.from(new TextEncoder().encode('hello')), [104, 101, 108, 108, 111]);
eq('TextDecoder hello', new TextDecoder().decode(new TextEncoder().encode('hello')), 'hello');
eq('TextEncoder 中 length', new TextEncoder().encode('中').length, 3);
eq('TextEncoder/TextDecoder 中 roundtrip', new TextDecoder().decode(new TextEncoder().encode('中')), '中');
eq('TextEncoder/TextDecoder emoji roundtrip', new TextDecoder().decode(new TextEncoder().encode('😀')), '😀');

// ---- 6) atob / btoa(QuickJS 原生全局, 与 Web/Node 同义) ----
eq('atob', atob('aGVsbG8='), 'hello');
eq('btoa', btoa('hello'), 'aGVsbG8=');

// ---- 7) 这些确实是全局(启动即有, 不用 import) ----
check('Buffer 是全局', typeof (globalThis as any).Buffer === 'function');
check('TextEncoder 是全局', typeof (globalThis as any).TextEncoder === 'function');
check('TextDecoder 是全局', typeof (globalThis as any).TextDecoder === 'function');

// ---- 8) 数值读写(与 Node 同名同义): offset 必填, 读/写完自己推进 ----
{
    const b = Buffer.from([0x12, 0x34, 0x56]);
    let p = 0;
    const a1 = b.readUInt8(p); p += 1;
    const a2 = b.readUInt8(p); p += 1;
    const a3 = b.readUInt8(p); p += 1;
    check('readUInt8 顺序读', a1 === 0x12 && a2 === 0x34 && a3 === 0x56 && p === 3);

    const w = Buffer.alloc(4);
    let q = 0;
    q = w.writeUInt16LE(0x1234, q);
    q = w.writeUInt16LE(0x5678, q);
    check('writeUInt16LE 顺序写', w.toString('hex') === '34127856' && q === 4);
    check('readUInt16LE 读回', w.readUInt16LE(0) === 0x1234 && w.readUInt16LE(2) === 0x5678);
    check('readUInt16BE 大端', w.readUInt16BE(0) === 0x3412);

    // 64 位走 bigint(免得掉精度)
    const e = Buffer.alloc(8);
    e.writeBigUInt64LE(0x0102030405060708n, 0);
    check('writeBigUInt64LE / readBigUInt64LE', e.readBigUInt64LE(0) === 0x0102030405060708n);

    // 越界: 与 Node 一样抛 RangeError(code = ERR_OUT_OF_RANGE)
    let threw = '';
    try { w.readUInt32LE(2); } catch (err: any) { threw = String((err && err.code) || err); }
    check('越界抛 ERR_OUT_OF_RANGE', threw === 'ERR_OUT_OF_RANGE');
}

console.log('11_buffer_encoding done');
