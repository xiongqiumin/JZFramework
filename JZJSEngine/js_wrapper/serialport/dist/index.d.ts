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
import type { OpenOptions, PortInfo, Data } from './types/index';
declare class EventEmitter {
    _ev: Record<string, Function[]>;
    constructor();
    on(e: string, l: Function): this;
    once(e: string, l: Function): this;
    off(e: string, l?: Function): this;
    removeListener(e: string, l: Function): this;
    removeAllListeners(e?: string): this;
    listenerCount(e: string): number;
    eventNames(): string[];
    emit(e: string, ...args: any[]): boolean;
}
export declare class SerialPort extends EventEmitter {
    _h: any;
    constructor(opts?: OpenOptions);
    setPortName(name: string): this;
    get portName(): string;
    get isOpen(): boolean;
    open(): this;
    close(): this;
    write(data: Data): this;
    read(maxSize: number): Uint8Array;
    readAll(): Uint8Array;
    setBaudRate(v: number): this;
    setDataBits(v: number): this;
    setParity(v: string): this;
    setStopBits(v: number): this;
    setFlowControl(v: string): this;
    setDtr(v: boolean): this;
    setRts(v: boolean): this;
}
export declare function list(): PortInfo[];
declare const serialport: {
    SerialPort: typeof SerialPort;
    list: typeof list;
};
export default serialport;
