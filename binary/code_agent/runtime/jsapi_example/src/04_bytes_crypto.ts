// 04_bytes_crypto —— 协议帧打包/解包(bytes) + 摘要与随机数(crypto)。
//
// 编译跑起来:  <tsc> -p tsconfig.json
//              code_runtime --entry <本目录>/build/04_bytes_crypto.js
//
// 上位机脚本最常干的两件底层事都在这一份里:
//   * 设备通信的帧: ByteWriter 顺序写 / ByteReader 顺序读(自带游标, 小端);
//   * 校验与去重: md5、随机字节、uuid。
//
// bytes 是本框架自带的包(Node 没这个名), crypto 只实现了常用那几个。

import { ByteWriter, ByteReader, concatBytes, hexOf } from 'bytes';
import crypto from 'crypto';
import { check } from './helper/assert.js';

console.log('--- 04_bytes_crypto ---');

// ---- 1) 组帧: 站号 + 功能码 + 长度 + 数据 ----
const payload = Uint8Array.fromHex('0a0b0c0d');
const w = new ByteWriter(1 + 1 + 2 + payload.length);   // 先算长度再定长分配(定长帧的常规做法)
w.u8v(0x01);            // 站号
w.u8v(0x03);            // 功能码
w.u16(payload.length);  // 长度(u16 小端)
w.raw(payload);         // 数据
const frame = w.bytes();
check('帧长度 = 8', frame.length === 8);
check('帧内容 little endian', hexOf(frame) === '010304000a0b0c0d');
console.log('frame = ' + hexOf(frame));

// ---- 2) 解帧: 游标自增, 不用自己算 offset ----
const r = new ByteReader(frame);
check('读站号', r.u8v() === 0x01);
check('读功能码', r.u8v() === 0x03);
const len = r.u16();
check('读长度', len === 4);
check('remaining 与长度对得上', r.remaining() === len);
const body = r.bytes(len);
check('读数据本体', hexOf(body) === '0a0b0c0d');
check('读完到 EOF', r.eof());

// ---- 3) 拼接 + 有符号/64 位 ----
const joined = concatBytes([Uint8Array.fromHex('0102'), Uint8Array.fromHex('0304')]);
check('concatBytes', joined.toHex() === '01020304');

const num = new ByteWriter(12);
num.i32(-2);
num.u64(0x0102030405060708n);       // 64 位用 bigint 传, 免得掉精度
const rd = new ByteReader(num.bytes());
check('i32 有符号', rd.i32() === -2);
check('u64 -> bigint', rd.u64() === 0x0102030405060708n);

// ---- 4) crypto ----
check("md5('abc')", crypto.createHash('md5').update('abc').digest('hex') === '900150983cd24fb0d6963f7d28e17f72');
check('md5(bytes)', crypto.createHash('md5').update(Uint8Array.fromHex('616263')).digest('hex') === '900150983cd24fb0d6963f7d28e17f72');
check('md5 原始字节长度 16', crypto.createHash('md5').update('abc').digest().length === 16);

const uuid = crypto.randomUUID();
check('randomUUID 形如 8-4-4-4-12', uuid.length === 36 && uuid.split('-').length === 5);
const rb = crypto.randomBytes(8);
check('randomBytes', rb instanceof Uint8Array && rb.length === 8);
const n = crypto.randomInt(1, 10);
check('randomInt 落在 [1,10)', n >= 1 && n < 10);

console.log('04_bytes_crypto done (uuid=' + uuid + ')');
