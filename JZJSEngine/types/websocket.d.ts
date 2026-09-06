/// <reference path="common.d.ts" />
/// <reference path="events.d.ts" />

/**
 * WebSocket - WHATWG client API, same as browsers and the Node 21+ built-in
 * WebSocket (undici). Node style on()/once()/off() listeners are supported in
 * addition to the onopen/onmessage/onclose/onerror properties.
 *
 * Note: the ws package uses (data, isBinary) for 'message', this implementation
 * follows the standard event.data form instead.
 *
 * Code / reason of the close event: the transport does not expose them,
 * a normal close reports 1000 and an abnormal one reports 1006.
 */
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

/**
 * ws - WebSocket server, aligned with the "ws" npm package
 * (Node has no built-in WebSocket server).
 *
 * Difference: the 'connection' listener gets only the socket, there is no
 * http.IncomingMessage as second argument because JZNet has no HTTP layer.
 */
declare module 'ws' {
    export interface ServerOptions {
        port?: number;
        host?: string;
        /** not supported, accepted and ignored */
        path?: string;
    }

    export class WebSocketServer extends EventEmitter {
        constructor(options?: ServerOptions, cb?: () => void);
        close(cb?: () => void): void;
        address(): { port: number; address: string; family: 'IPv4' } | null;

        on(event: 'listening', listener: () => void): this;
        on(event: 'connection', listener: (socket: WebSocket) => void): this;
        on(event: 'close', listener: () => void): this;
        on(event: 'error', listener: (err: Error) => void): this;
        on(event: string, listener: (...args: any[]) => void): this;
    }

    export const Server: typeof WebSocketServer;
    export function createServer(options?: ServerOptions, cb?: () => void): WebSocketServer;
    /** WebSocket itself is a global, it is not re-exported here */
}

/** the engine injects the same object into the global scope */
declare const ws: typeof import('ws');
