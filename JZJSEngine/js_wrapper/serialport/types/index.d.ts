/**
 * @jz/serialport —— 外部定义 (external definition, public contract)
 *
 * 对齐 Node.js "serialport" 包(事实标准, 现 @serialport/* monorepo)。
 * 与本包内部实现 src/index.ts 配套; 运行时由引擎加载 dist/index.js 提供。
 *
 * 与上游差异:
 * - open()/close() 同步; list() 同步返回数组(无 Promise)。
 * - read()/readAll() 为主动拉取(同时仍经 'data' 事件推送)。
 * - baudRate/parity/stopBits/... 可在运行时改; parity/flowControl 用上游同样字符串拼写。
 */

/** 最小事件发射器契约(本包自带实现, 与 Node events.EventEmitter 同名 API) */
export interface EventEmitter {
    on(event: string, listener: (...args: any[]) => void): this;
    once(event: string, listener: (...args: any[]) => void): this;
    off(event: string, listener: (...args: any[]) => void): this;
    removeListener(event: string, listener: (...args: any[]) => void): this;
    removeAllListeners(event?: string): this;
    emit(event: string, ...args: any[]): boolean;
    listenerCount(event: string): number;
    eventNames(): string[];
}

/** enumeration result, mirrors JZSerialPortInfo */
export interface PortInfo {
    /** unique device id (system instance id) */
    id: string;
    /** port name, e.g. "COM3" / "/dev/ttyUSB0" / "UART1" */
    portName: string;
    /** human readable description, e.g. "USB Serial" */
    description: string;
}

export interface OpenOptions {
    /** port name, e.g. "COM3" / "/dev/ttyUSB0". Same as setPortName(). */
    path?: string;
    /** default 9600 */
    baudRate?: number;
    dataBits?: 5 | 6 | 7 | 8;
    stopBits?: 1 | 1.5 | 2;
    parity?: 'none' | 'even' | 'odd' | 'space' | 'mark';
    flowControl?: 'none' | 'hardware' | 'software';
    /** initial DTR level */
    dtr?: boolean;
    /** initial RTS level */
    rts?: boolean;
    /** default true: new SerialPort() 时立即 open() */
    autoOpen?: boolean;
}

/** string / Uint8Array / ArrayBuffer */
export type Data = string | Uint8Array | ArrayBuffer;

export class SerialPort extends EventEmitter {
    constructor(options?: OpenOptions);

    /** enumerate available ports (mirrors JZSerialPort::availablePorts) */
    static list(): PortInfo[];

    setPortName(name: string): this;
    readonly portName: string;

    /** returns false when no driver is installed / port is busy */
    open(): boolean;
    close(): void;
    readonly isOpen: boolean;
    /** 打开着的串口默认吊住进程; unref() 后不再吊住(串口照常收发), ref() 恢复 */
    ref(): this;
    unref(): this;

    write(data: Data): this;

    /** read up to maxSize bytes from the receive buffer */
    read(maxSize: number): Uint8Array;
    /** read and drain all buffered received bytes */
    readAll(): Uint8Array;

    setBaudRate(baudRate: number): boolean;
    setDataBits(bits: 5 | 6 | 7 | 8): boolean;
    setParity(parity: 'none' | 'even' | 'odd' | 'space' | 'mark'): boolean;
    setStopBits(stopBits: 1 | 1.5 | 2): boolean;
    setFlowControl(flowControl: 'none' | 'hardware' | 'software'): boolean;

    setDtr(level: boolean): boolean;
    setRts(level: boolean): boolean;

    on(event: 'data', listener: (chunk: Uint8Array) => void): this;
    on(event: 'open' | 'close', listener: () => void): this;
    on(event: 'error', listener: (err: Error) => void): this;
    on(event: string, listener: (...args: any[]) => void): this;
}

/** module-level alias of SerialPort.list() */
export function list(): PortInfo[];

export const serialport: { SerialPort: typeof SerialPort; list: typeof list };
