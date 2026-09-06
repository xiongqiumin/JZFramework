/// <reference path="common.d.ts" />
/// <reference path="events.d.ts" />

/**
 * dgram - UDP (aligned with the Node.js built-in "dgram" module).
 *
 * Differences:
 * - only 'udp4' is supported.
 * - bind() ignores the address argument (JZNet binds to all interfaces).
 * - setBroadcast/setTTL/setMulticast* are not available.
 */
declare module 'dgram' {
    export interface RemoteInfo {
        address: string;
        port: number;
        family: 'IPv4';
        size: number;
    }

    export interface SocketAddress {
        address: string;
        port: number;
        family: 'IPv4';
    }

    export class Socket extends EventEmitter {
        bind(port?: number, cb?: () => void): this;
        bind(port: number, address?: string, cb?: () => void): this;
        bind(options: { port?: number; address?: string }, cb?: () => void): this;

        send(msg: Data, port: number, address?: string, cb?: (err?: Error) => void): void;
        send(msg: Data, offset: number, length: number, port: number, address?: string,
             cb?: (err?: Error) => void): void;

        close(cb?: () => void): this;
        address(): SocketAddress;

        on(event: 'listening' | 'close', listener: () => void): this;
        on(event: 'message', listener: (msg: Chunk, rinfo: RemoteInfo) => void): this;
        on(event: 'error', listener: (err: Error) => void): this;
        on(event: string, listener: (...args: any[]) => void): this;
    }

    export function createSocket(type: 'udp4',
                                 listener?: (msg: Chunk, rinfo: RemoteInfo) => void): Socket;
}

/** the engine injects the same object into the global scope */
declare const dgram: typeof import('dgram');
