/// <reference path="common.d.ts" />
/// <reference path="events.d.ts" />

/**
 * 全局(WHATWG)类: URL / URLSearchParams / WebSocket。
 * 它们是浏览器/Node 的全局对象, 不是某个 'url'/'ws' 模块的导出,
 * 所以单独放在这里作为全局 ambient 声明; 对应的 'url'/'ws' 模块导出
 * 见 js_wrapper/net/types/。
 */

/**
 * 取"当前正在被 JS 调用的那个对象"的包装 —— 也就是该 C++ 方法被调用时的 this。
 *
 * 用途: 事件回调通常写成箭头函数, 而箭头函数没有自己的 this(ESM 里模块顶层的 this 甚至是
 * undefined); `self()` 就是"这个回调是被哪个对象调起来的"的取回方式, 例如:
 *
 *     pane.onPaint = (e) => {
 *         const p = new JZPainter(self<JZWidget>());   // 等价于这里的 this
 *     };
 *
 * 什么时候有值(依据是**注册路径**, 不是运行期猜的):
 *   * 类实例方法 / 属性访问(挂在类原型上的, 如 pane.move() / w.text)     -> 有值(该实例)
 *   * 事件回调(onPaint / onMousePress / onShow ...)                       -> 有值(被派发的控件)
 *   * 信号回调(btn.onClicked = () => ... / spin.onValueChanged = ...)     -> 有值(发信号的对象)
 *   * 命名空间 / 全局函数(process.cwd() / fs.readString())里面            -> 不占帧, 看到的是外层那个实例
 *   * 不在上述调用里(模块顶层 / 光秃秃的 setTimeout 回调)                   -> **null**
 *
 * 嵌套的普通函数调用里也有效(栈顶仍是外层那个对象)。注意: quickjs 没有"取当前 this"的 API
 * (this 只是被调 C 函数的形参), 这是引擎维护的调用栈, 见 JZJSEngine::self / SelfScope。
 */
declare function self<T = any>(): T | null;

/** @final */
declare class URLSearchParams {
    constructor(init?: string | Record<string, string> | URLSearchParams);

    get(name: string): string | null;
    has(name: string): boolean;
    set(name: string, value: string): void;
    append(name: string, value: string): void;
    delete(name: string): void;

    keys(): string[];
    values(): string[];
    entries(): Array<[string, string]>;
    forEach(callback: (value: string, name: string) => void): void;
    toString(): string;
}

/** @final */
declare class URL {
    constructor(url: string);

    href: string;
    /** includes the trailing colon, e.g. "http:" */
    readonly protocol: string;
    /** host without port */
    readonly hostname: string;
    /** hostname:port */
    readonly host: string;
    /** string, empty when not specified */
    readonly port: string;
    readonly pathname: string;
    /** includes the leading "?" */
    readonly search: string;
    readonly searchParams: URLSearchParams;
    /** pathname + search */
    readonly path: string;

    toString(): string;
}

/** @final */
declare class WebSocket extends EventEmitter {
    constructor(url: string, protocols?: string | string[]);

    static readonly CONNECTING: 0;
    static readonly OPEN: 1;
    static readonly CLOSING: 2;
    static readonly CLOSED: 3;

    readonly readyState: 0 | 1 | 2 | 3;
    readonly url: string;
    readonly protocol: string;

    send(data: Data): void;
    close(code?: number, reason?: string): void;
    ping(data?: Data): void;

    /**
     * 连接中的/已连接的 WebSocket 默认把进程留住(对齐 Node 的 handle ref 语义)。
     * ⚠ 注意: Node 的全局 WebSocket(undici) 没有这两个方法(它用内部的 socket.unref());
     * 本引擎的 WebSocket 包装的是连接句柄, 所以直接给到这里 —— 语义同 net.Socket.unref()。
     */
    ref(): this;
    unref(): this;

    onopen: ((ev: { target: WebSocket }) => void) | null;
    onmessage: ((ev: { data: Chunk; target: WebSocket }) => void) | null;
    onclose: ((ev: { code: number; reason: string; wasClean: boolean }) => void) | null;
    onerror: ((ev: { error: Error; message: string }) => void) | null;

    on(event: 'open', listener: () => void): this;
    on(event: 'message', listener: (data: Chunk) => void): this;
    on(event: 'close', listener: (code: number, reason: string) => void): this;
    on(event: 'error', listener: (err: Error) => void): this;
    on(event: string, listener: (...args: any[]) => void): this;
}

/** Node Buffer 兼容的字节块(Node 启动时就是全局, 本运行时同样挂 globalThis)。Uint8Array 子类。 */
declare class Buffer extends Uint8Array {
    constructor(input: any, encoding?: string);
    static from(input: any, encoding?: string): Buffer;
    static alloc(size: number, fill?: any, encoding?: string): Buffer;
    static allocUnsafe(size: number): Buffer;
    static allocUnsafeSlow(size: number): Buffer;
    static concat(list: Uint8Array[], totalLength?: number): Buffer;
    static isBuffer(x: any): boolean;
    static isEncoding(encoding: string): boolean;
    static byteLength(str: string, encoding?: string): number;
    static compare(a: Uint8Array, b: Uint8Array): number;
    toString(encoding?: string, start?: number, end?: number): string;
    toJSON(): { type: 'Buffer'; data: number[] };
    equals(other: Uint8Array): boolean;
    compare(other: Uint8Array): number;
    copy(target: Uint8Array, targetStart?: number, sourceStart?: number, sourceEnd?: number): number;
    slice(start?: number, end?: number): Buffer;
    subarray(start?: number, end?: number): Buffer;
    fill(value: any, offset?: number, end?: number, encoding?: string): this;
    indexOf(value: any, byteOffset?: number, encoding?: string): number;
    write(str: string, offset?: number, length?: number, encoding?: string): number;

    // ---- 数值读写(与 Node 同名同义; offset 必填, 返回 offset + 字节数) ----
    readUInt8(offset?: number): number;
    readInt8(offset?: number): number;
    readUInt16LE(offset?: number): number;
    readUInt16BE(offset?: number): number;
    readInt16LE(offset?: number): number;
    readInt16BE(offset?: number): number;
    readUInt32LE(offset?: number): number;
    readUInt32BE(offset?: number): number;
    readInt32LE(offset?: number): number;
    readInt32BE(offset?: number): number;
    readBigUInt64LE(offset?: number): bigint;
    readBigUInt64BE(offset?: number): bigint;
    readBigInt64LE(offset?: number): bigint;
    readBigInt64BE(offset?: number): bigint;
    readFloatLE(offset?: number): number;
    readFloatBE(offset?: number): number;
    readDoubleLE(offset?: number): number;
    readDoubleBE(offset?: number): number;
    /** 变长(1~6 字节) */
    readUIntLE(offset: number, byteLength: number): number;
    readUIntBE(offset: number, byteLength: number): number;
    readIntLE(offset: number, byteLength: number): number;
    readIntBE(offset: number, byteLength: number): number;

    writeUInt8(value: number, offset?: number): number;
    writeInt8(value: number, offset?: number): number;
    writeUInt16LE(value: number, offset?: number): number;
    writeUInt16BE(value: number, offset?: number): number;
    writeInt16LE(value: number, offset?: number): number;
    writeInt16BE(value: number, offset?: number): number;
    writeUInt32LE(value: number, offset?: number): number;
    writeUInt32BE(value: number, offset?: number): number;
    writeInt32LE(value: number, offset?: number): number;
    writeInt32BE(value: number, offset?: number): number;
    writeBigUInt64LE(value: number | bigint, offset?: number): number;
    writeBigUInt64BE(value: number | bigint, offset?: number): number;
    writeBigInt64LE(value: number | bigint, offset?: number): number;
    writeBigInt64BE(value: number | bigint, offset?: number): number;
    writeFloatLE(value: number, offset?: number): number;
    writeFloatBE(value: number, offset?: number): number;
    writeDoubleLE(value: number, offset?: number): number;
    writeDoubleBE(value: number, offset?: number): number;
    writeUIntLE(value: number, offset: number, byteLength: number): number;
    writeUIntBE(value: number, offset: number, byteLength: number): number;
    writeIntLE(value: number, offset: number, byteLength: number): number;
    writeIntBE(value: number, offset: number, byteLength: number): number;

    swap16(): this;
    swap32(): this;
    swap64(): this;

    /** 整块写入(Uint8Array.prototype.set 的同义) */
    set(src: Uint8Array, offset?: number): void;
}

/** WHATWG TextEncoder(utf-8 编码; Node/浏览器均为全局) */
declare class TextEncoder {
    readonly encoding: string;
    encode(input?: string): Uint8Array;
    encodeInto(input: string, dest: Uint8Array): { read: number; written: number };
}

/** WHATWG TextDecoder(utf-8 解码; Node/浏览器均为全局) */
declare class TextDecoder {
    readonly encoding: string;
    constructor(label?: string, options?: any);
    decode(input?: Uint8Array, options?: any): string;
}

/** QuickJS 原生全局(base64 <-> 二进制串); 与 Web/Node 同义(本运行时也挂 globalThis) */
declare function atob(data: string): string;
declare function btoa(data: string): string;

/**
 * 全局 `process`(引擎注册的最小对象, 见 JZJSEngine::registConsole)。
 *
 * Node 的脚本习惯**不 import** 直接写 process.exit(0) —— 本运行时同样支持, 且语义对齐 Node:
 *   exit(3) / exit("7") -> 3 / 7        exit() / exit(null) -> exitCode(设过)否则 0
 *   exit(-1) -> 4294967295(uint32 回绕) exit(1.5) -> RangeError ERR_OUT_OF_RANGE
 *   exit("x") / exit({}) / exit(true)   -> TypeError ERR_INVALID_ARG_TYPE
 *   exitCode 是可读写属性(整数/整数串; null/undefined 清空), 脚本自然结束时它就是进程退出码。
 *
 * ⚠ 这里只有 cwd / argv / exit / exitCode; 其余成员(execPath / platform ...)在 '@jz/process' 模块里,
 *   两个是同一套底层(行为一致), argv 更是**同一个数组对象**。
 *   已知差异: Node 的 exit 会立即终止当前帧, 这里当前同步帧会跑完。
 */
declare const process: {
    cwd(): string;
    /** 启动参数 = [宿主可执行文件, 入口模块, ...宿主给的参数](见 '@jz/process' 的 argv) */
    argv: string[];
    /** 见上: 参数与退出码规则对齐 Node */
    exit(code?: number | string | null): void;
    exitCode: number | string | null | undefined;
};
