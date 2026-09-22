/// <reference path="../../types/common.d.ts" />

/** digest output encodings (note: not the full Node BinaryToTextEncoding set) */
export type BinaryToTextEncoding = 'hex' | 'base64' | 'latin1' | 'binary';

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

/** default export = the module object (same usage as Node: import x from 'crypto') */
declare const _default: { randomUUID: typeof randomUUID, randomBytes: typeof randomBytes, randomInt: typeof randomInt, createHash: typeof createHash };
export default _default;
