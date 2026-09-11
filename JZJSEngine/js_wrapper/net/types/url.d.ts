/// <reference path="../../types/common.d.ts" />

/**
 * url - module part of the WHATWG URL. The global `URL` / `URLSearchParams`
 * classes are declared in js_wrapper/types/globals.d.ts (they are WHATWG
 * globals, not specific to the 'url' module). url.parse() is the legacy
 * Node helper (kept for compatibility). Note: url.format() is not provided.
 */
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
