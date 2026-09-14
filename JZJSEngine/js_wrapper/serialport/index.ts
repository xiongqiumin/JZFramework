/**
 * @jz/serialport —— 内部实现 (internal definition)
 *
 * 把引擎注入的 __jz.serialport 原子 API 包装成 Node "serialport" 包风格。
 * 本文件由 tsc 编译为 dist/index.js, 由用户入口模块
 * `import { SerialPort } from './serialport/dist/index.js'` 被动加载(QuickJS 模块加载器
 * 解析到该 specifier 时回调 readModuleCode), 以分离式(不内嵌进 C++)方式运行。
 *
 * 运行时契约: 依赖全局 __jz.serialport (由 C++ 注册)。
 * 同时以标准 Node 形态导出 SerialPort / list, 便于将来切换 JS 运行时。
 */

import type { OpenOptions, PortInfo, Data } from './types/index';

// 事件发射器: 用 net 包那份(**全框架唯一实现**, 见 net/index.ts 顶部说明)。
// 以前这里自己抄过一份, 两边会分叉; 现在只 import, 不再有第二份定义。
import { EventEmitter } from 'net/dist';

// 引擎注入的原生原子 API(运行时由 C++ 注册到 globalThis.__jz)。
declare const __jz: any;
declare var console: { error(...a: any[]): void; log(...a: any[]): void; warn(...a: any[]): void };

// 注: 事件发射器不再在本文件定义 —— 已改为 import net 包的 EventEmitter(见文件头的 import),
// 全框架只有一份实现(net/index.ts)。这里保留空行以免行号大跳。

// ---------------------------------------------------------------------------
// 字节转换: 兼容 string / Uint8Array / ArrayBuffer, 以及 hex/base64/latin1
// ---------------------------------------------------------------------------
function toBytes(data: any, enc: string | null): Uint8Array {
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

// ---------------------------------------------------------------------------
// SerialPort
// ---------------------------------------------------------------------------
export class SerialPort extends EventEmitter {
    _h: any;
    constructor(opts: OpenOptions = {}) {
        super();
        this._h = __jz.serialport.createPort(opts as any);
        this._h.on('data', (b: Uint8Array) => this.emit('data', b));
        this._h.on('close', () => this.emit('close'));
        this._h.on('error', (m: string) => this.emit('error', new Error(m)));
        const autoOpen = opts.autoOpen === undefined ? true : opts.autoOpen;
        if (autoOpen) {
            if (this._h.open()) this.emit('open');
            else this.emit('error', new Error('open failed'));
        }
    }
    setPortName(name: string): this { this._h.setPortName(String(name)); return this; }
    get portName(): string { return this._h.portName; }
    get isOpen(): boolean { return this._h.isOpen; }
    open(): this { if (this._h.open()) this.emit('open'); return this; }
    close(): this { this._h.close(); return this; }
    write(data: Data): this { this._h.write(toBytes(data, null)); return this; }
    read(maxSize: number): Uint8Array { return maxSize > 0 ? this._h.read(maxSize | 0) : this._h.readAll(); }
    readAll(): Uint8Array { return this._h.readAll(); }
    setBaudRate(v: number): this { this._h.setBaudRate(v | 0); return this; }
    setDataBits(v: number): this { this._h.setDataBits(v | 0); return this; }
    setParity(v: string): this { this._h.setParity(String(v)); return this; }
    setStopBits(v: number): this { this._h.setStopBits(v); return this; }
    setFlowControl(v: string): this { this._h.setFlowControl(String(v)); return this; }
    setDtr(v: boolean): this { this._h.setDtr(!!v); return this; }
    setRts(v: boolean): this { this._h.setRts(!!v); return this; }
}

export function list(): PortInfo[] {
    return __jz.serialport.list();
}

const serialport = { SerialPort, list };
export default serialport;

// ---------------------------------------------------------------------------
// 运行时桥接: 标准 Node 形态已由上面 export 提供; 引擎(QuickJS)侧把模块挂到全局,
// 便于全局脚本直接访问(真正的 import 用法由 `import { SerialPort }` 提供)。
// ---------------------------------------------------------------------------
if (typeof globalThis !== 'undefined') {
    (globalThis as any).serialport = serialport;
    (globalThis as any).SerialPort = SerialPort;
}
