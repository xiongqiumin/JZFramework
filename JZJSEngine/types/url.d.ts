/// <reference path="common.d.ts" />

/**
 * URL / URLSearchParams - WHATWG standard, same as Node and browsers.
 * url.parse() is the legacy Node helper (kept for compatibility).
 *
 * Note: url.format() is not provided (JZUrl has no formatter).
 */
declare class URLSearchParams {
    constructor(init?: string | Record<string, string> | URLSearchParams);

    get(name: string): string | null;
    has(name: string): boolean;
    set(name: string, value: string): void;
    append(name: string, value: string): void;
    delete(name: string): void;

    keys(): string[];
    values(): string[];
    entries(): Array<[string, string]>;
    forEach(callback: (value: string, name: string) => void): void;
    toString(): string;
}

declare class URL {
    constructor(url: string);

    href: string;
    /** includes the trailing colon, e.g. "http:" */
    readonly protocol: string;
    /** host without port */
    readonly hostname: string;
    /** hostname:port */
    readonly host: string;
    /** string, empty when not specified */
    readonly port: string;
    readonly pathname: string;
    /** includes the leading "?" */
    readonly search: string;
    readonly searchParams: URLSearchParams;
    /** pathname + search */
    readonly path: string;

    toString(): string;
}

declare module 'url' {
    export interface UrlObject {
        href: string;
        protocol: string;
        hostname: string;
        host: string;
        port: string;
        pathname: string;
        search: string;
        /** parsed query items */
        query: Record<string, string>;
    }

    /** legacy Node compatible parse; URL / URLSearchParams are global */
    export function parse(input: string): UrlObject;
}

/** the engine injects the same object into the global scope */
declare const url: typeof import('url');
