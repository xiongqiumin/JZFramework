/// <reference path="common.d.ts" />
/// <reference path="events.d.ts" />

/**
 * 全局(WHATWG)类: URL / URLSearchParams / WebSocket。
 * 它们是浏览器/Node 的全局对象, 不是某个 'url'/'ws' 模块的导出,
 * 所以单独放在这里作为全局 ambient 声明; 对应的 'url'/'ws' 模块导出
 * 见 js_wrapper/net/types/。
 */

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
