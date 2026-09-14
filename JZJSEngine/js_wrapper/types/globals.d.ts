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
