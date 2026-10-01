/// <reference path="../../types/common.d.ts" />
/// <reference path="../../types/events.d.ts" />

export interface TcpNetConnectOpts {
    port: number;
    host?: string;
    /** not supported, accepted and ignored */
    localAddress?: string;
    /** ms, 0 = no timeout */
    timeout?: number;
}

export interface ListenOptions {
    port?: number;
    host?: string;
    /** not supported, accepted and ignored */
    backlog?: number;
}

export interface AddressInfo {
    port: number;
    address: string;
    family: 'IPv4';
}

export class Socket extends EventEmitter {
    constructor(options?: TcpNetConnectOpts);

    connect(port: number, host?: string, connectListener?: () => void): this;
    connect(options: TcpNetConnectOpts, connectListener?: () => void): this;

    write(data: Data, encoding?: DataEncoding, cb?: (err?: Error) => void): boolean;
    end(data?: Data, encoding?: DataEncoding, cb?: (err?: Error) => void): this;
    destroy(error?: Error): this;

    /** emits 'timeout' when there is no activity for ms; 0 disables */
    setTimeout(ms: number, cb?: () => void): this;
    /** 'data' events deliver string instead of Uint8Array */
    setEncoding(encoding: DataEncoding | null): this;
    /** buffer 'data' until resume (stream semantics) */
    pause(): this;
    resume(): this;
    isPaused(): boolean;
    /** 本端 {address, family, port}; 未连接时为 null */
    address(): { address: string; family: 'IPv4' | 'IPv6'; port: number } | null;
    /** ⚠ mongoose 不暴露按连接的 TCP_NODELAY / SO_KEEPALIVE 开关: 接受并忽略(no-op) */
    setNoDelay(noDelay?: boolean | (() => void), cb?: () => void): this;
    setKeepAlive(enable?: boolean | (() => void), initialDelay?: number | (() => void), cb?: () => void): this;

    readonly remoteAddress: string | undefined;
    readonly remotePort: number | undefined;
    readonly remoteFamily: 'IPv4' | 'IPv6' | undefined;
    /** undefined for client sockets created by connect() */
    readonly localAddress: string | undefined;
    readonly localPort: number | undefined;
    readonly localFamily: 'IPv4' | 'IPv6' | undefined;
    readonly bytesRead: number;
    readonly bytesWritten: number;
    readonly destroyed: boolean;
    readonly writableEnded: boolean;
    /** true while connecting */
    readonly pending: boolean;
    readonly readyState: 'opening' | 'open' | 'closed';
    readonly connecting: boolean;

    /**
     * 连接中的/已连接的 socket 默认会把进程留住(对齐 Node 的 handle ref 语义;
     * 只 `new Socket()` 不 connect 不算)。不想被吊住就 unref(), 之后可以 ref() 恢复。
     */
    ref(): this;
    /** 让这个 socket 不再吊住进程(socket 照常收发, 见 ref 说明) */
    unref(): this;

    on(event: 'connect' | 'close' | 'end' | 'timeout', listener: () => void): this;
    on(event: 'data', listener: (chunk: Chunk) => void): this;
    on(event: 'error', listener: (err: Error) => void): this;
    on(event: string, listener: (...args: any[]) => void): this;
}

export class Server extends EventEmitter {
    listen(port?: number, listeningListener?: () => void): this;
    listen(port: number, host?: string, listeningListener?: () => void): this;
    listen(options: ListenOptions, listeningListener?: () => void): this;

    close(cb?: (err?: Error) => void): this;
    /** null when not listening */
    address(): AddressInfo | null;

    readonly listening: boolean;
    /** 达到上限后新连接会被直接丢掉(对齐 Node), 不发 'connection' */
    maxConnections: number;
    /** 当前活跃连接数; 回调形状与 Node 一致: (err, count) */
    getConnections(cb: (err: Error | null, count: number) => void): this;
    /** 关掉本服务 accept 出来的所有连接(不影响监听本身) */
    closeAllConnections(): this;
    /** ⚠ 本引擎无法分辨"空闲连接", 按 closeAllConnections 处理 */
    closeIdleConnections(): this;

    /** 监听中的 server 默认把进程留住(对齐 Node); 不想被吊住就 unref() */
    ref(): this;
    /** 让监听中的 server 不再吊住进程(仍照常 accept) */
    unref(): this;

    on(event: 'listening' | 'close', listener: () => void): this;
    on(event: 'connection', listener: (socket: Socket) => void): this;
    on(event: 'error', listener: (err: Error) => void): this;
    on(event: string, listener: (...args: any[]) => void): this;
}

export function createServer(connectionListener?: (socket: Socket) => void): Server;
export function createServer(options: ListenOptions, connectionListener?: (socket: Socket) => void): Server;

export function createConnection(port: number, host?: string, connectListener?: () => void): Socket;
export function createConnection(options: TcpNetConnectOpts, connectListener?: () => void): Socket;
export const connect: typeof createConnection;

/** 对齐 Node: 4 = IPv4, 6 = IPv6, 0 = 都不是 */
export function isIP(input: string): 0 | 4 | 6;
export function isIPv4(input: string): boolean;
export function isIPv6(input: string): boolean;

/** default export = the module object (same usage as Node: import x from 'net') */
declare const _default: {
    Socket: typeof Socket, Server: typeof Server,
    createServer: typeof createServer, createConnection: typeof createConnection,
    connect: typeof createConnection,
    isIP: typeof isIP, isIPv4: typeof isIPv4, isIPv6: typeof isIPv6,
};
export default _default;
