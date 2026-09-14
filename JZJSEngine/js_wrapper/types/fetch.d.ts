/// <reference path="common.d.ts" />

/**
 * fetch - WHATWG fetch, same as Node 18+ (undici) and browsers.
 */

/** @final */
declare class Headers {
    constructor(init?: Record<string, string> | Headers);

    get(name: string): string | null;
    has(name: string): boolean;
    set(name: string, value: string): void;
    append(name: string, value: string): void;
    delete(name: string): void;

    forEach(callback: (value: string, name: string) => void): void;
    keys(): string[];
    values(): string[];
    entries(): Array<[string, string]>;
    toJSON(): Record<string, string>;
}

/** @final */
declare class Response {
    readonly status: number;
    readonly statusText: string;
    readonly ok: boolean;
    readonly url: string;
    readonly headers: Headers;
    readonly bodyUsed: boolean;

    text(): Promise<string>;
    json(): Promise<any>;
    arrayBuffer(): Promise<ArrayBuffer>;
    /** extension: keeps the body as raw bytes */
    uint8Array(): Promise<Uint8Array>;

    static error(): Response;
}

interface RequestInit {
    method?: string;
    headers?: Record<string, string> | Headers;
    body?: Data;
    /** ms, 0 = no timeout */
    timeout?: number;
}

declare function fetch(url: string, init?: RequestInit): Promise<Response>;
