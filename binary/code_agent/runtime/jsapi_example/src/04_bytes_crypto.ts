// 04_bytes_crypto —— 协议帧打包/解包(Buffer 数值读写) + 摘要与随机数(crypto)。
//
// 编译跑起来:  <tsc> -p tsconfig.json
//              code_runtime --entry <本目录>/build/04_bytes_crypto.js
//
// 上位机脚本最常干的两件底层事都在这一份里:
//   * 设备通信的帧: Buffer 的 writeXxx(v, off) / readXxx(off) 顺序读写(小端, off 自己推进);
//   * 校验与去重: md5、随机字节、uuid。
//
// Buffer 是全局(Node 同), 数值读写这套名字与 Node 完全同名同义 —— 本例子在 Node 下也能直接跑。
// (本框架没有"游标自增"的读写器: Node 也没有, offset 一律显式传。)

import crypto from 'crypto';
import { check } from './helper/assert.js';

console.log('--- 04_bytes_crypto ---');

// ---- 1) 组帧: 站号 + 功能码 + 长度 + 数据 ----
const payload = Uint8Array.fromHex('0a0b0c0d');
const frame = Buffer.alloc(1 + 1 + 2 + payload.length);   // 先算长度再定长分配(定长帧的常规做法)
let o = 0;
o = frame.writeUInt8(0x01, o);               // 站号
o = frame.writeUInt8(0x03, o);               // 功能码
o = frame.writeUInt16LE(payload.length, o);  // 长度(u16 小端)
frame.set(payload, o);                       // 数据
o += payload.length;
check('帧长度 = 8', frame.length === 8);
check('帧内容 little endian', frame.toString('hex') === '010304000a0b0c0d');
console.log('frame = ' + frame.toString('hex'));

// ---- 2) 解帧: offset 自己推进(写回值就是下一个 offset) ----
let p = 0;
check('读站号', frame.readUInt8(p) === 0x01); p += 1;
check('读功能码', frame.readUInt8(p) === 0x03); p += 1;
const len = frame.readUInt16LE(p); p += 2;
check('读长度', len === 4);
check('remaining 与长度对得上', frame.length - p === len);
const body = frame.subarray(p, p + len); p += len;
check('读数据本体', body.toString('hex') === '0a0b0c0d');
check('读完到 EOF', p === frame.length);

// ---- 3) 拼接 + 有符号/64 位 ----
const joined = Buffer.concat([Uint8Array.fromHex('0102'), Uint8Array.fromHex('0304')]);
check('Buffer.concat', joined.toString('hex') === '01020304');

const num = Buffer.alloc(12);
num.writeInt32LE(-2, 0);                        // 有符号
num.writeBigUInt64LE(0x0102030405060708n, 4);   // 64 位用 bigint 传, 免得掉精度
check('i32 有符号', num.readInt32LE(0) === -2);
check('u64 -> bigint', num.readBigUInt64LE(4) === 0x0102030405060708n);

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
