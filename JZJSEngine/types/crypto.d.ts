/// <reference path="common.d.ts" />

/**
 * crypto - Node.js built-in "crypto" module (subset backed by JZCore).
 *
 * Only the purely synchronous subset is implemented (JZCore has no async
 * CSPRNG / OpenSSL binding). The API mirrors Node so existing code ports over:
 *
 *   const crypto = require('crypto');
 *   crypto.randomUUID();                       // "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"
 *   crypto.randomBytes(16);                    // Uint8Array(16)
 *   crypto.randomInt(0, 100);                  // int in [0, 100)
 *   crypto.createHash('md5').update('a').digest('hex');
 *
 * Differences from Node:
 * - createHash() only supports 'md5' (throws otherwise), it is the only
 *   algorithm JZCrypto implements. No Hmac / cipher / sign / webcrypto.subtle.
 * - randomBytes(size, cb) accepts a Node-style callback for API compatibility,
 *   but still runs synchronously (returns undefined, result via cb).
 */
declare module 'crypto' {
    /** digest output encodings (note: not the full Node BinaryToTextEncoding set) */
    type BinaryToTextEncoding = 'hex' | 'base64' | 'latin1' | 'binary';

    /** incremental hash object returned by createHash() (chainable .update()) */
    export interface Hash {
        update(data: Data, encoding?: DataEncoding): Hash;
        /** no encoding -> raw bytes; encoding -> encoded string */
        digest(): Uint8Array;
        digest(encoding: BinaryToTextEncoding): string;
    }

    /** RFC 4122 v4 uuid, e.g. "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" */
    export function randomUUID(): string;

    /** synchronous: returns size bytes; with callback runs async-style (returns undefined) */
    export function randomBytes(size: number): Uint8Array;
    export function randomBytes(size: number,
        callback: (err: Error | null, buf: Uint8Array) => void): void;

    /** integer in [min, max); randomInt(max) == randomInt(0, max) */
    export function randomInt(max: number): number;
    export function randomInt(min: number, max: number): number;

    /** only 'md5' is supported; throws on anything else */
    export function createHash(algorithm: string): Hash;
}

/** the engine injects the same object into the global scope */
declare const crypto: typeof import('crypto');
