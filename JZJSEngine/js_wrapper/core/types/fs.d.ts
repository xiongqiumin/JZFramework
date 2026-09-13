/// <reference path="../../types/common.d.ts" />

type FsEncoding = 'utf8' | 'utf-8' | 'hex' | 'base64' | 'latin1';

export interface CopyOptions {
    /** 目录必须显式置 true(与 Node 一致); 文件不需要 */
    recursive?: boolean;
    /** 同名文件是否覆盖, 默认 true */
    force?: boolean;
}

/** Node fs.constants 子集(复制标志 + open flags, O_* 按 Windows CRT 取值) */
export declare const constants: {
    COPYFILE_EXCL: number;
    COPYFILE_FICLONE: number;
    COPYFILE_FICLONE_FORCE: number;
    O_RDONLY: number;
    O_WRONLY: number;
    O_RDWR: number;
    O_APPEND: number;
    O_CREAT: number;
    O_TRUNC: number;
    O_EXCL: number;
    O_SYNC: number;
    O_NOFOLLOW: number;
};

/** Node fs 的 flags: 字符串形式('r'/'r+'/'w'/'w+'/'a'/'a+' , 可带 'x')或 fs.constants.O_* 位组合 */
export type FsOpenFlags = string | number;

export interface Stats {
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

export interface Dirent {
    name: string;
    isFile(): boolean;
    isDirectory(): boolean;
    isBlockDevice(): boolean;
    isCharacterDevice(): boolean;
    isSymbolicLink(): boolean;
    isFIFO(): boolean;
    isSocket(): boolean;
}

export function existsSync(path: string): boolean;
export function readFileSync(path: string, encoding: FsEncoding): string;
export function readFileSync(path: string, options?: { encoding?: FsEncoding }): Uint8Array;
/** fd 形式: 从当前游标读到 EOF */
export function readFileSync(fd: number, encoding: FsEncoding): string;
export function readFileSync(fd: number, options?: { encoding?: FsEncoding }): Uint8Array;
export function writeFileSync(path: string | number, data: string | Uint8Array | ArrayBuffer, encoding?: FsEncoding): void;
export function appendFileSync(path: string | number, data: string | Uint8Array | ArrayBuffer, encoding?: FsEncoding): void;
export function unlinkSync(path: string): void;
export function openSync(path: string, flags?: FsOpenFlags, mode?: number): number;
export function closeSync(fd: number): void;
export function readSync(fd: number, buffer: Uint8Array, offset?: number | null, length?: number | null, position?: number | null): number;
export function writeSync(fd: number, buffer: Uint8Array, offset?: number | null, length?: number | null, position?: number | null): number;
export function writeSync(fd: number, string: string, position?: number | null, encoding?: FsEncoding): number;
export function fstatSync(fd: number): Stats;
export function copyFileSync(src: string, dest: string, mode?: number): void;
export function renameSync(oldPath: string, newPath: string): void;
export function cpSync(src: string, dest: string, options?: CopyOptions): void;
export function mkdirSync(path: string, options?: { recursive?: boolean }): void;
export function rmdirSync(path: string): void;
export function rmSync(path: string, options?: { recursive?: boolean; force?: boolean }): void;
export function readdirSync(path: string, options?: { withFileTypes?: false }): string[];
export function readdirSync(path: string, options: { withFileTypes: true }): Dirent[];
export function statSync(path: string): Stats;
export function lstatSync(path: string): Stats;
