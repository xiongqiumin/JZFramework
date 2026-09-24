/// <reference path="../../types/common.d.ts" />
/// <reference path="../../types/events.d.ts" />
/// <reference path="../../types/fetch.d.ts" />

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
    /** client: response status; server side uses ServerRequest */
    readonly statusCode: number;
    readonly statusMessage: string;
    /** plain object with lowercased keys (same as Node) */
    readonly headers: Record<string, string>;
    readonly httpVersion: string;
    readonly url: string;
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

export class ServerRequest extends EventEmitter {
    /** request method, e.g. 'GET' | 'POST' */
    readonly method: string;
    readonly url: string;
    /** plain object with lowercased keys (same as Node server req.headers) */
    readonly headers: Record<string, string>;
    readonly httpVersion: string;
    /** peer address */
    readonly remoteAddress: string;
    /** complete request body (empty when there is none) */
    readonly body: Uint8Array;
    /** body decoded with the current encoding (defaults to utf8) */
    readonly text: string;
    setEncoding(encoding: DataEncoding | null): this;

    on(event: 'data', listener: (chunk: Chunk) => void): this;
    on(event: 'end', listener: () => void): this;
    on(event: 'error', listener: (err: Error) => void): this;
    on(event: string, listener: (...args: any[]) => void): this;
}

// ------------------------------------------------------------ server
export class ServerResponse extends EventEmitter {
    statusCode: number;
    readonly finished: boolean;

    setHeader(name: string, value: string | number): this;
    getHeader(name: string): string | undefined;
    removeHeader(name: string): void;
    writeHead(statusCode: number, headers?: Record<string, string | number>): this;

    /** sent immediately (buffered by the underlying transport), same as Node write() */
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
       listener: (req: ServerRequest, res: ServerResponse) => void): this;
    on(event: 'listening', listener: () => void): this;
    on(event: 'close', listener: () => void): this;
    on(event: 'error', listener: (err: Error) => void): this;
    on(event: string, listener: (...args: any[]) => void): this;
}

export function createServer(requestListener?:
    (req: ServerRequest, res: ServerResponse) => void): Server;
export function createServer(options: object,
    requestListener?: (req: ServerRequest, res: ServerResponse) => void): Server;

/** status code -> reason phrase */
export const STATUS_CODES: Record<number, string>;
/** methods supported by this implementation */
export const METHODS: string[];

/** default export = the module object (same usage as Node: import x from 'http') */
declare const _default: { IncomingMessage: typeof IncomingMessage, ClientRequest: typeof ClientRequest, ServerRequest: typeof ServerRequest, ServerResponse: typeof ServerResponse, Server: typeof Server, createServer: typeof createServer, request: typeof request, get: typeof get, METHODS: typeof METHODS, STATUS_CODES: typeof STATUS_CODES };
export default _default;
