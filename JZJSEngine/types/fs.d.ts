/// <reference path="common.d.ts" />

type FsEncoding = 'utf8' | 'utf-8' | 'hex' | 'base64' | 'latin1';

interface Stats {
    dev: number;
    mode: number;
    nlink: number;
    uid: number;
    gid: number;
    rdev: number;
    blksize: number;
    ino: number;
    size: number;
    blocks: number;
    atimeMs: number;
    mtimeMs: number;
    ctimeMs: number;
    birthtimeMs: number;
    atime: Date;
    mtime: Date;
    ctime: Date;
    birthtime: Date;
    isFile(): boolean;
    isDirectory(): boolean;
    isBlockDevice(): boolean;
    isCharacterDevice(): boolean;
    isSymbolicLink(): boolean;
    isFIFO(): boolean;
    isSocket(): boolean;
}

interface Dirent {
    name: string;
    isFile(): boolean;
    isDirectory(): boolean;
    isBlockDevice(): boolean;
    isCharacterDevice(): boolean;
    isSymbolicLink(): boolean;
    isFIFO(): boolean;
    isSocket(): boolean;
}

declare module 'fs' {
    export function existsSync(path: string): boolean;
    export function readFileSync(path: string, encoding: FsEncoding): string;
    export function readFileSync(path: string, options?: { encoding?: FsEncoding }): Uint8Array;
    export function writeFileSync(path: string, data: string | Uint8Array | ArrayBuffer, encoding?: FsEncoding): void;
    export function appendFileSync(path: string, data: string | Uint8Array | ArrayBuffer, encoding?: FsEncoding): void;
    export function unlinkSync(path: string): void;
    export function mkdirSync(path: string, options?: { recursive?: boolean }): void;
    export function rmdirSync(path: string): void;
    export function rmSync(path: string, options?: { recursive?: boolean; force?: boolean }): void;
    export function readdirSync(path: string, options?: { withFileTypes?: false }): string[];
    export function readdirSync(path: string, options: { withFileTypes: true }): Dirent[];
    export function statSync(path: string): Stats;
    export function lstatSync(path: string): Stats;
}

/** the engine injects the same object into the global scope */
declare const fs: typeof import('fs');
