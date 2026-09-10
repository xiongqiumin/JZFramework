/**
 * @jz/modbus —— 外部定义 (external definition, public contract)
 *
 * modbus-serial 风格的主站封装（RTU 串口 / Modbus TCP）。运行时由引擎加载
 * dist/index.js 提供, 底层是 JZModbus 的 JZModbusClient（经 JZJSModbusModule
 * 的 __jz.modbus 原子 API 暴露）。与包内实现 index.ts 配套。
 *
 * 与上游 `modbus-serial` 的差异（重要）:
 * - 不提供 `buffer` 字段（运行时没有 Node 的 Buffer）; 读接口只返回 `{ data }`;
 * - FC1/FC2 的 `data` 长度 = 请求数量（上游会补齐到一个字节的整数倍）;
 * - 只支持 connectRTU / connectRTUBuffered / connectTCP; 其余 connect* 一律 reject;
 * - 不支持 FC43 readDeviceIdentification / FC17 / FC20 / FC22、Enron 模式、
 *   服务端（ServerTCP / ServerSerial）;
 * - setTimeout(ms) 只是客户端侧守卫, 底层固定 5s 超时, 实际生效 min(ms, 5000);
 * - TCP 连接建立后对端断开不会主动 emit('error'|'close'), 靠 isOpen() 与下一次
 *   请求报错体现（C++ 侧没有回推通道）。
 */

/** 任意事件监听器 */
export type Listener = (...args: any[]) => void;

/** 最小事件发射器契约（本包自带实现, 与 Node events.EventEmitter 同名 API） */
export interface Emitter {
    on(event: string, listener: Listener): this;
    once(event: string, listener: Listener): this;
    off(event: string, listener?: Listener): this;
    removeListener(event: string, listener?: Listener): this;
    removeAllListeners(event?: string): this;
    listeners(event: string): Listener[];
    listenerCount(event: string): number;
    emit(event: string, ...args: any[]): boolean;
}

/** 串口校验位 */
export type Parity = 'none' | 'even' | 'odd' | 'space' | 'mark';

/** connectRTU / connectRTUBuffered 的可选项 */
export interface RTUOptions {
    /** 波特率, 默认 9600 */
    baudRate?: number;
    /** 数据位, 默认 8 */
    dataBits?: 5 | 6 | 7 | 8;
    /** 停止位, 默认 1 */
    stopBits?: 1 | 1.5 | 2;
    /** 校验位, 默认 'none' */
    parity?: Parity;
    /** 客户端侧超时守卫(ms), 实际生效 min(timeout, 5000); 默认 5000 */
    timeout?: number;
}

/** connectTCP 的可选项 */
export interface TCPOptions {
    /** 端口, 默认 502 */
    port?: number;
    /** 客户端侧超时守卫(ms), 实际生效 min(timeout, 5000); 默认 5000 */
    timeout?: number;
}

/** FC1 / FC2 读取结果 */
export interface BitReadResult {
    data: boolean[];
}

/** FC3 / FC4 读取结果 */
export interface RegisterReadResult {
    data: number[];
}

/** FC5 写单个线圈的回显 */
export interface WriteCoilResult {
    address: number;
    state: boolean;
}

/** FC6 写单个寄存器的回显 */
export interface WriteRegisterResult {
    address: number;
    value: number;
}

/** FC15 / FC16 写多个的回显 */
export interface WriteMultipleResult {
    address: number;
    length: number;
}

/** getInfo() 返回的底层连接信息（JZModbus 扩展, 便于排错） */
export interface ConnectionInfo {
    /** JZModbus 的 modbusType 枚举值 */
    modbusType: number;
    /** 是否为 RTU 连接 */
    rtu: boolean;
    /** 是否为主站 */
    master: boolean;
    /** 从站号 / unit id */
    slave: number;
    /** PLC 地址模式 */
    plcMode: boolean;
    /** RTU: 串口名, e.g. "COM3" */
    portName: string;
    /** RTU: 波特率 */
    baud: number;
    /** RTU: 数据位 */
    dataBit: number;
    /** RTU: 停止位 */
    stopBit: number;
    /** RTU: 校验位 */
    parityBit: number;
    /** TCP: 对端地址 */
    ip: string;
    /** TCP: 对端端口 */
    port: number;
}

/**
 * Modbus 主站（modbus-serial 里 RTU 与 TCP 客户端是同一个类, 只是 connect 方式不同）。
 *
 * 同一连接同时只允许一个在途请求: 内部对请求排队串行执行。
 * 未连接时的任何读写都会 reject('Port Not Open')。
 */
export class ModbusRTU extends Emitter {
    constructor();

    // ---- 连接 ----
    /** 串口 Modbus RTU（内部已做帧缓冲, 等价于 modbus-serial 的 connectRTUBuffered） */
    connectRTU(path: string, options?: RTUOptions): Promise<void>;
    /** 与 connectRTU 等价 */
    connectRTUBuffered(path: string, options?: RTUOptions): Promise<void>;
    /** Modbus TCP（原生 MBAP 头） */
    connectTCP(ip: string, options?: TCPOptions): Promise<void>;

    /** 不支持（JZModbus 无 RTU-over-TCP）: 返回 rejected Promise */
    connectTcpRTUBuffered(): Promise<void>;
    /** 不支持: 返回 rejected Promise */
    connectUDP(): Promise<void>;
    /** 不支持: 返回 rejected Promise */
    connectAsciiSerial(): Promise<void>;
    /** 不支持: 返回 rejected Promise */
    connectTelnet(): Promise<void>;
    /** 不支持: 返回 rejected Promise */
    connectC701(): Promise<void>;
    /** 不支持: 返回 rejected Promise */
    connectBle(): Promise<void>;

    /** 底层连接是否已建立 */
    readonly isOpen: boolean;

    /** 客户端侧超时(ms); 底层固定 5s, 实际生效 min(ms, 5000) */
    setTimeout(timeout: number): void;
    getTimeout(): number | null;

    /** 从站号 / unit id（RTU 时也是地址字节） */
    setID(id: number): void;
    getID(): number;

    /** PLC 地址模式（JZModbus 扩展, 非 modbus-serial 接口） */
    setPlcMode(flag: boolean): void;

    /** 底层连接信息（JZModbus 扩展）, 未连接返回 null */
    getInfo(): ConnectionInfo | null;

    /** 最近一次底层错误文本（JZModbus 扩展）, 未连接返回 '' */
    getLastError(): string;

    /** 关闭连接（可传回调, 与 modbus-serial 一致） */
    close(callback?: () => void): void;
    /** 与 close 等价（并丢弃排队中的请求） */
    destroy(callback?: () => void): void;

    // ---- 读（FC1 / FC2 / FC3 / FC4） ----
    /** FC1 读线圈 -> { data: boolean[] } */
    readCoils(address: number, length: number): Promise<BitReadResult>;
    /** FC2 读离散输入 -> { data: boolean[] } */
    readDiscreteInputs(address: number, length: number): Promise<BitReadResult>;
    /** FC3 读保持寄存器 -> { data: number[] } */
    readHoldingRegisters(address: number, length: number): Promise<RegisterReadResult>;
    /** FC4 读输入寄存器 -> { data: number[] } */
    readInputRegisters(address: number, length: number): Promise<RegisterReadResult>;

    // ---- 写（FC5 / FC6 / FC15 / FC16） ----
    /** FC5 写单个线圈 -> { address, state } */
    writeCoil(address: number, state: boolean): Promise<WriteCoilResult>;
    /** FC6 写单个寄存器 -> { address, value } */
    writeRegister(address: number, value: number): Promise<WriteRegisterResult>;
    /** FC15 写多个线圈 -> { address, length } */
    writeCoils(address: number, states: boolean[]): Promise<WriteMultipleResult>;
    /** FC16 写多个寄存器 -> { address, length } */
    writeRegisters(address: number, values: number[]): Promise<WriteMultipleResult>;

    // ---- 事件重载 ----
    on(event: 'open' | 'close', listener: () => void): this;
    on(event: 'error', listener: (err: Error) => void): this;
    on(event: string, listener: Listener): this;
}

/** 便捷别名: modbus-serial 里 TCP 客户端就是同一个类（只是走 connectTCP） */
export const ModbusTCP: typeof ModbusRTU;

export default ModbusRTU;
