/// <reference path="../../types/common.d.ts" />
/// <reference path="../../types/events.d.ts" />
/// <reference path="../../types/globals.d.ts" />

/**
 * ws - WebSocket server, aligned with the "ws" npm package
 * (Node has no built-in WebSocket server).
 *
 * Difference: the 'connection' listener gets only the socket, there is no
 * http.IncomingMessage as second argument because JZNet has no HTTP layer.
 *
 * NOTE: the global `WebSocket` client class is declared in
 * js_wrapper/types/globals.d.ts (it is a WHATWG global, not part of 'ws').
 */
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

/** default export = the module object (same usage as Node: import x from 'ws') */
declare const _default: { WebSocket: typeof WebSocket, WebSocketServer: typeof WebSocketServer, Server: typeof Server, createServer: typeof createServer };
export default _default;
