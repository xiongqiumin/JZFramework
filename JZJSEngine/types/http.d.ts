/// <reference path="common.d.ts" />
/// <reference path="events.d.ts" />
/// <reference path="fetch.d.ts" />

/**
 * http - HTTP client (aligned with the Node.js built-in "http" module).
 *
 * Differences:
 * - no server side: JZNet has no HTTP server, so http.createServer() is absent.
 * - only GET and POST are supported (JZNetworkRequest::HttpMethod), other
 *   methods reject with an error.
 * - IncomingMessage is not a Readable stream, it only emits 'data'/'end'.
 * - ClientRequest.abort() is deprecated in Node, use destroy().
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
        readonly statusCode: number;
        readonly statusMessage: string;
        readonly headers: Headers;
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

    /** status code -> reason phrase */
    export const STATUS_CODES: Record<number, string>;
    /** methods supported by this implementation */
    export const METHODS: string[];
}

/** the engine injects the same object into the global scope */
declare const http: typeof import('http');
