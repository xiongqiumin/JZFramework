/**
 * @jz/modbus —— modbus-serial 风格的 Modbus 主站封装（RTU / TCP）
 *
 * 宿主侧:
 *   JZJSModbusModule（JZJSEngine/JZJSModbusModule.{h,cpp}）注册 `__jz.modbus.*` 原子 API，
 *   底层是 JZModbus 的 JZModbusClient（m_ctx = rtuClient / tcpClient）。
 *   本文件是被动加载的 Node 风格适配层（用户 `import './jz_js/modbus/dist/index.js'` 时载入），
 *   只做接口塑形：Promise 化、事件、请求排队、结果对象形状。
 *
 * 用法（与 modbus-serial 一致）:
 *   const ModbusRTU = require("modbus-serial")      ->  import ModbusRTU from '.../modbus/dist/index.js'
 *   const client = new ModbusRTU();
 *   await client.connectTCP("127.0.0.1", { port: 502 });
 *   client.setID(1);
 *   const { data } = await client.readHoldingRegisters(0, 10);
 *   await client.close();
 *
 * 与 modbus-serial 的差异（重要）:
 *  1. `buffer` 字段不提供（运行时没有 Node 的 Buffer）；读接口返回 `{ data }`。
 *  2. FC1/FC2 的 `data` 长度 = 请求数量（modbus-serial 会补齐到一个字节的整数倍）。
 *  3. 不支持的连接方式：connectTcpRTUBuffered / connectUDP / connectAsciiSerial /
 *     connectTelnet / connectC701 / connectBle —— JZModbus 只有 RTU 串口与 TCP。
 *  4. 不支持的接口：FC43 readDeviceIdentification、FC17/20/22、Enron 模式、服务端
 *     （ServerTCP / ServerSerial——JZModbus 服务端是"参数表"模型，与 modbus-serial 的
 *     vector 回调模型对不上，暂不封装）。
 *  5. `setTimeout(ms)` 只作为客户端侧守卫；JZModbus 底层固定 5s 超时，所以实际生效的是
 *     min(ms, 5000)。默认（不调用 setTimeout）直接用底层 5s。
 *  6. TCP 连接建立之后对端断开，本层不会主动 emit('error'|'close')，靠 isOpen() 与下一次
 *     请求报错体现（C++ 侧没有回推通道）。
 *
 * 'use strict' 下必须声明 __jz（原生注入的全局对象）
 */
'use strict';

declare const __jz: any;
// setTimeout / clearTimeout 由引擎注入为全局(见 JZJSEngine 的时间任务), 编译期需自行声明
declare function setTimeout(cb: (...a: any[]) => void, ms?: number): any;
declare function clearTimeout(id: any): void;

type AnyFn = (...args: any[]) => void;

// ---------------------------------------------------------------------------
// 最小 EventEmitter（不依赖 net 包的全局 EventEmitter，也不做 Node 那种
// "没有 error 监听器就抛异常" 的特例，避免纯 Promise 用法被误伤）
// ---------------------------------------------------------------------------
class Emitter
{
    _events: { [name: string]: AnyFn[] };

    constructor()
    {
        this._events = {};
    }

    on(name: string, fn: AnyFn): Emitter
    {
        if (typeof fn !== 'function')
            return this;
        (this._events[name] || (this._events[name] = [])).push(fn);
        return this;
    }

    once(name: string, fn: AnyFn): Emitter
    {
        const self = this;
        const wrap = function (this: any, ...args: any[])
        {
            self.off(name, wrap as AnyFn);
            fn.apply(this, args);
        };
        return this.on(name, wrap as AnyFn);
    }

    off(name: string, fn?: AnyFn): Emitter
    {
        const list = this._events[name];
        if (!list)
            return this;
        if (!fn)
            delete this._events[name];
        else
        {
            for (let i = list.length - 1; i >= 0; i--)
            {
                if (list[i] === fn)
                    list.splice(i, 1);
            }
        }
        return this;
    }

    removeListener(name: string, fn?: AnyFn): Emitter
    {
        return this.off(name, fn);
    }

    removeAllListeners(name?: string): Emitter
    {
        if (typeof name === 'string')
            delete this._events[name];
        else
            this._events = {};
        return this;
    }

    listeners(name: string): AnyFn[]
    {
        return (this._events[name] || []).slice();
    }

    listenerCount(name: string): number
    {
        const list = this._events[name];
        return list ? list.length : 0;
    }

    emit(name: string, ...args: any[]): boolean
    {
        const list = this._events[name];
        if (!list || list.length === 0)
            return false;
        const copy = list.slice();
        for (let i = 0; i < copy.length; i++)
            copy[i].apply(this, args);
        return true;
    }
}

// ---------------------------------------------------------------------------
// 工具
// ---------------------------------------------------------------------------
const FC = {
    READ_COILS: 1,
    READ_DISCRETE_INPUTS: 2,
    READ_HOLDING_REGISTERS: 3,
    READ_INPUT_REGISTERS: 4,
    WRITE_SINGLE_COIL: 5,
    WRITE_SINGLE_REGISTER: 6,
    WRITE_MULTIPLE_COILS: 15,
    WRITE_MULTIPLE_REGISTERS: 16,
};

const DEFAULT_TIMEOUT = 5000;      // 与 JZModbus 底层的固定超时一致
const NATIVE_TIMEOUT = 5000;

function noop() { /* keep the queue alive */ }

function isReadFunc(func: number): boolean
{
    return func === FC.READ_COILS || func === FC.READ_DISCRETE_INPUTS ||
        func === FC.READ_HOLDING_REGISTERS || func === FC.READ_INPUT_REGISTERS;
}

function num(v: any, def: number): number
{
    if (typeof v === 'number' && isFinite(v))
        return v;
    if (typeof v === 'string' && v.length > 0)
    {
        const n = parseInt(v, 10);
        if (!isNaN(n))
            return n;
    }
    return def;
}

function makeError(message: string, code?: number): Error
{
    const err: any = new Error(message);
    if (typeof code === 'number')
        err.code = code;
    return err;
}

// 异步原子只 resolve（失败信息在 ok/message 里），这里统一转成 Promise
function unwrap(promise: any): Promise<any>
{
    return Promise.resolve(promise).then(function (r: any)
    {
        if (r && r.ok === false)
            throw makeError(r.message || 'modbus request failed', r.code);
        return r;
    });
}

// ---------------------------------------------------------------------------
// ModbusRTU —— 同时也是 TCP 客户端（modbus-serial 里两者是同一个类）
// ---------------------------------------------------------------------------
export class ModbusRTU extends Emitter
{
    /** 底层连接句柄（__jz.modbus.create 的原生对象），未连接时为 null */
    _native: any;
    /** 是否已连接 */
    _opened: boolean;
    /** 从站号 / unit id（setID 写入，RTU 时也是地址字节） */
    _id: number;
    /** PLC 地址模式（JZModbus 扩展） */
    _plcMode: boolean;
    /** 客户端侧超时守卫；null = 只用底层 5s */
    _timeout: number | null;
    /** 请求串行队列（同一连接同时只允许一个在途请求） */
    _queue: Promise<any>;

    constructor()
    {
        super();
        this._native = null;
        this._opened = false;
        this._id = 1;
        this._plcMode = false;
        this._timeout = null;
        this._queue = Promise.resolve();
    }

    // -----------------------------------------------------------------
    // 连接
    // -----------------------------------------------------------------
    /** 串口 Modbus RTU（JZModbus 内部已做帧缓冲，等价于 modbus-serial 的 connectRTUBuffered） */
    connectRTU(path: string, options?: any): Promise<void>
    {
        options = options || {};
        return this._open({
            type: 'rtu',
            portName: path,
            baudRate: num(options.baudRate, 9600),
            dataBits: num(options.dataBits, 8),
            stopBits: num(options.stopBits, 1),
            parity: options.parity || 'none',
            slave: this._id,
            plcMode: this._plcMode,
        }, options);
    }

    /** modbus-serial 的 connectRTUBuffered 在本实现里与 connectRTU 等价 */
    connectRTUBuffered(path: string, options?: any): Promise<void>
    {
        return this.connectRTU(path, options);
    }

    /** Modbus TCP（原生 Modbus TCP，MBAP 头） */
    connectTCP(ip: string, options?: any): Promise<void>
    {
        options = options || {};
        return this._open({
            type: 'tcp',
            ip: ip,
            port: num(options.port, 502),
            slave: this._id,
            plcMode: this._plcMode,
        }, options);
    }

    /** JZModbus 只有"原生 RTU 串口"与"原生 TCP"，没有 RTU-over-TCP / UDP / ASCII / Telnet */
    connectTcpRTUBuffered(): Promise<void>
    {
        return Promise.reject(makeError('connectTcpRTUBuffered is not supported by @jz/modbus (no RTU-over-TCP in JZModbus)'));
    }

    connectUDP(): Promise<void>
    {
        return Promise.reject(makeError('connectUDP is not supported by @jz/modbus'));
    }

    connectAsciiSerial(): Promise<void>
    {
        return Promise.reject(makeError('connectAsciiSerial is not supported by @jz/modbus'));
    }

    connectTelnet(): Promise<void>
    {
        return Promise.reject(makeError('connectTelnet is not supported by @jz/modbus'));
    }

    connectC701(): Promise<void>
    {
        return Promise.reject(makeError('connectC701 is not supported by @jz/modbus'));
    }

    connectBle(): Promise<void>
    {
        return Promise.reject(makeError('connectBle is not supported by @jz/modbus'));
    }

    /** 底层连接是否已建立 */
    get isOpen(): boolean
    {
        if (!this._native)
            return false;
        return __jz.modbus.isOpen(this._native) === true;
    }

    /** 客户端侧超时(ms)。底层固定 5s，所以实际生效的是 min(ms, 5000) */
    setTimeout(timeout: number): void
    {
        this._timeout = num(timeout, DEFAULT_TIMEOUT);
    }

    getTimeout(): number | null
    {
        return this._timeout;
    }

    /** 从站号 / unit id */
    setID(id: number): void
    {
        this._id = num(id, 1);
        if (this._native)
            __jz.modbus.setSlave(this._native, this._id);
    }

    getID(): number
    {
        return this._id;
    }

    /** PLC 地址模式(JZModbus 扩展，非 modbus-serial 接口) */
    setPlcMode(flag: boolean): void
    {
        this._plcMode = !!flag;
        if (this._native)
            __jz.modbus.setPlcMode(this._native, this._plcMode);
    }

    /** 底层连接信息(JZModbus 扩展：便于排错) */
    getInfo(): any
    {
        if (!this._native)
            return null;
        return __jz.modbus.info(this._native);
    }

    /** 最近一次底层错误文本(JZModbus 扩展) */
    getLastError(): string
    {
        if (!this._native)
            return '';
        return __jz.modbus.lastError(this._native);
    }

    /** 关闭连接（可传回调，与 modbus-serial 一致） */
    close(callback?: AnyFn): void
    {
        const wasOpen = this._opened;
        if (this._native)
        {
            __jz.modbus.close(this._native);
            this._native = null;
        }
        this._opened = false;
        if (wasOpen)
            this.emit('close');
        if (typeof callback === 'function')
            callback();
    }

    /** 与 close 等价（并丢弃排队中的请求） */
    destroy(callback?: AnyFn): void
    {
        const self = this;
        this._queue = Promise.resolve();
        this.close(function ()
        {
            if (typeof callback === 'function')
                callback();
        });
        void self;
    }

    // -----------------------------------------------------------------
    // 读（FC1 / FC2 / FC3 / FC4）
    // -----------------------------------------------------------------
    /** FC1 读线圈 -> { data: boolean[] } */
    readCoils(address: number, length: number): Promise<any>
    {
        return this._request(FC.READ_COILS, address, length);
    }

    /** FC2 读离散输入 -> { data: boolean[] } */
    readDiscreteInputs(address: number, length: number): Promise<any>
    {
        return this._request(FC.READ_DISCRETE_INPUTS, address, length);
    }

    /** FC3 读保持寄存器 -> { data: number[] } */
    readHoldingRegisters(address: number, length: number): Promise<any>
    {
        return this._request(FC.READ_HOLDING_REGISTERS, address, length);
    }

    /** FC4 读输入寄存器 -> { data: number[] } */
    readInputRegisters(address: number, length: number): Promise<any>
    {
        return this._request(FC.READ_INPUT_REGISTERS, address, length);
    }

    // -----------------------------------------------------------------
    // 写（FC5 / FC6 / FC15 / FC16）
    // -----------------------------------------------------------------
    /** FC5 写单个线圈 -> { address, state } */
    writeCoil(address: number, state: boolean): Promise<any>
    {
        return this._request(FC.WRITE_SINGLE_COIL, address, !!state);
    }

    /** FC6 写单个寄存器 -> { address, value } */
    writeRegister(address: number, value: number): Promise<any>
    {
        return this._request(FC.WRITE_SINGLE_REGISTER, address, num(value, 0));
    }

    /** FC15 写多个线圈 -> { address, length } */
    writeCoils(address: number, states: boolean[]): Promise<any>
    {
        return this._request(FC.WRITE_MULTIPLE_COILS, address, states || []);
    }

    /** FC16 写多个寄存器 -> { address, length } */
    writeRegisters(address: number, values: number[]): Promise<any>
    {
        return this._request(FC.WRITE_MULTIPLE_REGISTERS, address, values || []);
    }

    // -----------------------------------------------------------------
    // 内部实现
    // -----------------------------------------------------------------
    _open(config: any, options: any): Promise<void>
    {
        const self = this;

        // 重复 connect：先关掉旧连接
        if (this._native)
            this.close();

        let native: any = null;
        try
        {
            native = __jz.modbus.create(config);
        }
        catch (e)
        {
            return Promise.reject(e);
        }
        this._native = native;

        let timeout = num(options.timeout, this._timeout === null ? DEFAULT_TIMEOUT : this._timeout);
        if (timeout <= 0)
            timeout = DEFAULT_TIMEOUT;
        if (timeout > NATIVE_TIMEOUT)
            timeout = NATIVE_TIMEOUT;   // 底层固定 5s

        return unwrap(__jz.modbus.open(native, timeout)).then(function ()
        {
            self._opened = true;
            self.emit('open');
        }, function (err: any)
        {
            // 连接失败：丢掉句柄（不主动 emit('error')，交给 Promise 的 catch；
            // emit 交给调用方自己决定，避免没有监听器时静默）
            self._native = null;
            self._opened = false;
            throw err;
        });
    }

    /** 串行化请求：同一连接同时只允许一个在途请求 */
    _request(func: number, address: number, arg: any): Promise<any>
    {
        const self = this;
        const run = function ()
        {
            return self._doRequest(func, address, arg);
        };
        const p = this._queue.then(run, run as any);
        this._queue = p.then(noop, noop);
        return p;
    }

    _doRequest(func: number, address: number, arg: any): Promise<any>
    {
        const self = this;
        const native = this._native;

        if (!native || !this._opened)
            return Promise.reject(makeError('Port Not Open'));
        if (__jz.modbus.isOpen(native) !== true)
            return Promise.reject(makeError('Port Not Open'));

        const pending = isReadFunc(func)
            ? __jz.modbus.read(native, func, num(address, 0), num(arg, 1))
            : __jz.modbus.write(native, func, num(address, 0), arg);

        return this._guard(pending).then(function (r: any)
        {
            if (!r || r.ok !== true)
            {
                const err = makeError((r && r.message) || 'modbus request failed', r && r.code);
                if (self.listenerCount('error') > 0)
                    self.emit('error', err);
                throw err;
            }
            return self._shape(func, address, arg, r);
        });
    }

    /** 客户端侧超时守卫（底层 5s 仍在跑，这里只是让用户能更早拿到失败） */
    _guard(pending: any): Promise<any>
    {
        const ms = this._timeout;
        if (!ms || ms <= 0)
            return Promise.resolve(pending);

        return new Promise(function (resolve, reject)
        {
            let done = false;
            const timer = setTimeout(function ()
            {
                if (done)
                    return;
                done = true;
                reject(makeError('timed out after ' + ms + ' ms'));
            }, ms);
            Promise.resolve(pending).then(function (v: any)
            {
                if (done)
                    return;
                done = true;
                clearTimeout(timer);
                resolve(v);
            }, function (e: any)
            {
                if (done)
                    return;
                done = true;
                clearTimeout(timer);
                reject(e);
            });
        });
    }

    /** 把原生结果塑形成 modbus-serial 的形状 */
    _shape(func: number, address: number, arg: any, r: any): any
    {
        const raw = (r && r.data) ? r.data : [];
        const echoAddr = (typeof r.address === 'number') ? r.address : num(address, 0);

        switch (func)
        {
        case FC.READ_COILS:
        case FC.READ_DISCRETE_INPUTS:
            {
                const bits: boolean[] = [];
                for (let i = 0; i < raw.length; i++)
                    bits.push(raw[i] !== 0);
                return { data: bits };
            }
        case FC.READ_HOLDING_REGISTERS:
        case FC.READ_INPUT_REGISTERS:
            {
                const regs: number[] = [];
                for (let i = 0; i < raw.length; i++)
                    regs.push(num(raw[i], 0));
                return { data: regs };
            }
        case FC.WRITE_SINGLE_COIL:
            return { address: echoAddr, state: !!arg };
        case FC.WRITE_SINGLE_REGISTER:
            return { address: echoAddr, value: num(arg, 0) };
        case FC.WRITE_MULTIPLE_COILS:
        case FC.WRITE_MULTIPLE_REGISTERS:
            return { address: echoAddr, length: (arg && arg.length) ? arg.length : 0 };
        default:
            return r;
        }
    }
}

/** 便捷别名：modbus-serial 里 TCP 客户端就是同一个类（只是走 connectTCP） */
export const ModbusTCP = ModbusRTU;

export default ModbusRTU;
