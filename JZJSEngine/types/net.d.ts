/// <reference path="common.d.ts" />
/// <reference path="events.d.ts" />

/**
 * net - TCP (aligned with the Node.js built-in "net" module).
 *
 * Not implemented (no support in the underlying JZNet layer):
 *   socket.pause()/resume()/pipe(), 'drain', 'lookup', 'ready',
 *   setNoDelay(), setKeepAlive(), Unix domain socket listen(path).
 * A net.Socket here is NOT a Duplex stream, it is an EventEmitter
 * with 'data'/'end' events only.
 */
declare module 'net' {
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
        end(data?: Data, encoding?: DataEncoding, cb?: () => void): this;
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
}

/** the engine injects the same object into the global scope */
declare const net: typeof import('net');
