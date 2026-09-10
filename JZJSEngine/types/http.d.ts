/// <reference path="common.d.ts" />
/// <reference path="events.d.ts" />
/// <reference path="fetch.d.ts" />

/**
 * http - HTTP client + server (aligned with the Node.js built-in "http" module).
 *
 * Client differences:
 * - only GET and POST are supported (JZNetworkRequest::HttpMethod), other
 *   methods reject with an error.
 * - IncomingMessage is not a Readable stream, it only emits 'data'/'end'.
 * - ClientRequest.abort() is deprecated in Node, use destroy().
 *
 * Server differences (backed by JZHttpServer, catch-all route in C++):
 * - ServerResponse is NOT streaming: write() only buffers, the whole response
 *   is sent on end() (no chunked transfer, no 'drain').
 * - req is not a stream either: the complete body is already there, use
 *   req.body (Uint8Array) / req.text, or listen to 'data'/'end' (emitted once).
 * - no 'upgrade' / 'checkContinue' / 'connect' events, no HTTP/2, no https
 *   (use JZNetObject::setSsl from C++ if TLS is needed).
 */
declare module 'http' {
    export interface RequestOptions {
        /** full url; alternative to protocol/host/port/path */
        url?: string;
        protocol?: string;
        host?: string;
        hostname?: string;
        port?: number;
        path?: string;
        method?: 'GET' | 'POST';
        headers?: Record<string, string>;
        /** ms, 0 = no timeout */
        timeout?: number;
    }

    export class IncomingMessage extends EventEmitter {
        /** client: response status; server: undefined */
        readonly statusCode: number;
        readonly statusMessage: string;
        readonly headers: Headers;
        readonly httpVersion: string;
        readonly url: string;
        /** server side only: 'GET' | 'POST' | ... */
        readonly method?: string;
        /** server side only: peer address */
        readonly remoteAddress?: string;
        /** server side only: complete request body (empty when there is none) */
        readonly body?: Uint8Array;
        /** server side only: body decoded with the current encoding */
        readonly text?: string;
        setEncoding(encoding: DataEncoding | null): this;

        on(event: 'data', listener: (chunk: Chunk) => void): this;
        on(event: 'end', listener: () => void): this;
        on(event: 'error', listener: (err: Error) => void): this;
        on(event: string, listener: (...args: any[]) => void): this;
    }

    export class ClientRequest extends EventEmitter {
        setHeader(name: string, value: string | number): void;
        getHeader(name: string): string | undefined;
        removeHeader(name: string): void;

        write(chunk: Data, encoding?: DataEncoding, cb?: () => void): boolean;
        end(data?: Data, encoding?: DataEncoding, cb?: () => void): this;
        setTimeout(ms: number, cb?: () => void): this;
        destroy(error?: Error): this;
        /** @deprecated use destroy() */
        abort(): void;

        on(event: 'response', listener: (res: IncomingMessage) => void): this;
        on(event: 'error', listener: (err: Error) => void): this;
        on(event: 'timeout', listener: () => void): this;
        on(event: string, listener: (...args: any[]) => void): this;
    }

    export function request(options: RequestOptions | string,
                            cb?: (res: IncomingMessage) => void): ClientRequest;
    export function request(url: string, options: RequestOptions,
                            cb?: (res: IncomingMessage) => void): ClientRequest;
    export function request(url: string, cb?: (res: IncomingMessage) => void): ClientRequest;

    /** same as request() with method GET and an automatic end() */
    export function get(options: RequestOptions | string,
                        cb?: (res: IncomingMessage) => void): ClientRequest;
    export function get(url: string, options: RequestOptions,
                        cb?: (res: IncomingMessage) => void): ClientRequest;
    export function get(url: string, cb?: (res: IncomingMessage) => void): ClientRequest;

    // ------------------------------------------------------------ server
    export class ServerResponse extends EventEmitter {
        statusCode: number;
        readonly finished: boolean;

        setHeader(name: string, value: string | number): this;
        getHeader(name: string): string | undefined;
        removeHeader(name: string): void;
        writeHead(statusCode: number, headers?: Record<string, string | number>): this;

        /** buffered, NOT sent until end() */
        write(chunk: Data, encoding?: DataEncoding, cb?: () => void): boolean;
        end(data?: Data | (() => void), encoding?: DataEncoding | (() => void),
            cb?: () => void): this;

        on(event: 'finish', listener: () => void): this;
        on(event: 'close', listener: () => void): this;
        on(event: string, listener: (...args: any[]) => void): this;
    }

    export interface ServerAddress {
        port: number;
        address: string;
        family: string;
    }

    export interface ListenOptions {
        port: number;
        host?: string;
    }

    export class Server extends EventEmitter {
        readonly listening: boolean;
        listen(port?: number, listeningListener?: () => void): this;
        listen(port: number, host?: string, listeningListener?: () => void): this;
        listen(options: ListenOptions, listeningListener?: () => void): this;
        close(cb?: () => void): this;
        /** null when not listening */
        address(): ServerAddress | null;

        on(event: 'request',
           listener: (req: IncomingMessage, res: ServerResponse) => void): this;
        on(event: 'listening', listener: () => void): this;
        on(event: 'close', listener: () => void): this;
        on(event: 'error', listener: (err: Error) => void): this;
        on(event: string, listener: (...args: any[]) => void): this;
    }

    export function createServer(requestListener?:
        (req: IncomingMessage, res: ServerResponse) => void): Server;
    export function createServer(options: object,
        requestListener?: (req: IncomingMessage, res: ServerResponse) => void): Server;

    /** status code -> reason phrase */
    export const STATUS_CODES: Record<number, string>;
    /** methods supported by this implementation */
    export const METHODS: string[];
}

/** the engine injects the same object into the global scope */
declare const http: typeof import('http');
