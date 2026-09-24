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

    readonly remoteAddress: string | undefined;
    readonly remotePort: number | undefined;
    /** undefined for client sockets created by connect() */
    readonly localAddress: string | undefined;
    readonly localPort: number | undefined;
    readonly bytesRead: number;
    readonly bytesWritten: number;
    readonly destroyed: boolean;
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
    maxConnections: number;

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

/** default export = the module object (same usage as Node: import x from 'net') */
declare const _default: { Socket: typeof Socket, Server: typeof Server, createServer: typeof createServer, createConnection: typeof createConnection, connect: typeof createConnection };
export default _default;
