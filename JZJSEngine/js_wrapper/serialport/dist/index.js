/**
 * @jz/serialport —— 内部实现 (internal definition)
 *
 * 把引擎注入的 __jz.serialport 原子 API 包装成 Node "serialport" 包风格。
 * 本文件由 tsc 编译为 dist/index.js, 在 JZJSSerialPortModule::initEngine 时
 * 通过 JZJSEngine::runModuleFile 以分离式(不内嵌进 C++)加载。
 *
 * 运行时契约: 依赖全局 __jz.serialport (由 C++ 注册)。
 * 同时以标准 Node 形态导出 SerialPort / list, 便于将来切换 JS 运行时。
 */
// ---------------------------------------------------------------------------
// 共享事件发射器(与 Node events.EventEmitter 同名 API, 仅本包内部使用)
// ---------------------------------------------------------------------------
class EventEmitter {
    constructor() {
        this._ev = Object.create(null);
    }
    on(e, l) {
        if (!this._ev[e])
            this._ev[e] = [];
        this._ev[e].push(l);
        return this;
    }
    once(e, l) {
        const self = this;
        const w = function () {
            self.off(e, w);
            l.apply(self, arguments);
        };
        w.__orig = l;
        return this.on(e, w);
    }
    off(e, l) {
        const a = this._ev[e];
        if (!a)
            return this;
        if (!l) {
            delete this._ev[e];
            return this;
        }
        for (let i = a.length - 1; i >= 0; i--) {
            if (a[i] === l || a[i].__orig === l)
                a.splice(i, 1);
        }
        if (!a.length)
            delete this._ev[e];
        return this;
    }
    removeListener(e, l) { return this.off(e, l); }
    removeAllListeners(e) {
        if (e)
            delete this._ev[e];
        else
            this._ev = Object.create(null);
        return this;
    }
    listenerCount(e) { return this._ev[e] ? this._ev[e].length : 0; }
    eventNames() { return Object.keys(this._ev); }
    emit(e, ...args) {
        const list = this._ev[e];
        if (!list || !list.length) {
            if (e === 'error')
                console.error('Unhandled error:', args[0]);
            return false;
        }
        const arr = list.slice();
        for (let i = 0; i < arr.length; i++) {
            try {
                arr[i].apply(this, args);
            }
            catch (err) {
                console.error('listener error:', err);
            }
        }
        return true;
    }
}
// ---------------------------------------------------------------------------
// 字节转换: 兼容 string / Uint8Array / ArrayBuffer, 以及 hex/base64/latin1
// ---------------------------------------------------------------------------
function toBytes(data, enc) {
    if (data === null || data === undefined)
        return new Uint8Array(0);
    if (typeof data === 'string') {
        if (enc === 'hex')
            return Uint8Array.fromHex(data);
        if (enc === 'base64')
            return Uint8Array.fromBase64(data);
        if (enc === 'latin1') {
            const r = new Uint8Array(data.length);
            for (let i = 0; i < data.length; i++)
                r[i] = data.charCodeAt(i) & 0xff;
            return r;
        }
        return __jz.bytes.fromString(data);
    }
    if (data instanceof Uint8Array)
        return data;
    if (data instanceof ArrayBuffer)
        return new Uint8Array(data);
    throw new TypeError('data must be string, Uint8Array or ArrayBuffer');
}
// ---------------------------------------------------------------------------
// SerialPort
// ---------------------------------------------------------------------------
export class SerialPort extends EventEmitter {
    constructor(opts = {}) {
        super();
        this._h = __jz.serialport.createPort(opts);
        this._h.on('data', (b) => this.emit('data', b));
        this._h.on('close', () => this.emit('close'));
        this._h.on('error', (m) => this.emit('error', new Error(m)));
        const autoOpen = opts.autoOpen === undefined ? true : opts.autoOpen;
        if (autoOpen) {
            if (this._h.open())
                this.emit('open');
            else
                this.emit('error', new Error('open failed'));
        }
    }
    setPortName(name) { this._h.setPortName(String(name)); return this; }
    get portName() { return this._h.portName; }
    get isOpen() { return this._h.isOpen; }
    open() { if (this._h.open())
        this.emit('open'); return this; }
    close() { this._h.close(); return this; }
    write(data) { this._h.write(toBytes(data, null)); return this; }
    read(maxSize) { return maxSize > 0 ? this._h.read(maxSize | 0) : this._h.readAll(); }
    readAll() { return this._h.readAll(); }
    setBaudRate(v) { this._h.setBaudRate(v | 0); return this; }
    setDataBits(v) { this._h.setDataBits(v | 0); return this; }
    setParity(v) { this._h.setParity(String(v)); return this; }
    setStopBits(v) { this._h.setStopBits(v); return this; }
    setFlowControl(v) { this._h.setFlowControl(String(v)); return this; }
    setDtr(v) { this._h.setDtr(!!v); return this; }
    setRts(v) { this._h.setRts(!!v); return this; }
}
export function list() {
    return __jz.serialport.list();
}
const serialport = { SerialPort, list };
export default serialport;
// ---------------------------------------------------------------------------
// 运行时桥接: 标准 Node 形态已由上面 export 提供; 引擎(QuickJS)侧把模块挂到全局,
// 便于全局脚本直接访问(真正的 import 用法由 `import { SerialPort }` 提供)。
// ---------------------------------------------------------------------------
if (typeof globalThis !== 'undefined') {
    globalThis.serialport = serialport;
    globalThis.SerialPort = SerialPort;
}
