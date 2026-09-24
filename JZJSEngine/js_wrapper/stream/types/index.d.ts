/**
 * @jz/stream —— Node stream 兼容子集(与 stream/index.ts 同源)。
 * 范围: Readable / Writable / Duplex / Transform / PassThrough / pipeline / finished。
 * 简化: 不做真正背压、同步/微任务交付; 游标式读写请用 bytes。
 */

export type StreamChunk = any;
export type StreamCallback = (err?: any) => void;

export class Stream extends EventEmitter {
    constructor(opts?: any);
    on(event: string, listener: (...args: any[]) => void): this;
    emit(event: string, ...args: any[]): boolean;
}

export class Readable extends Stream {
    constructor(opts?: any);
    _read(size?: number): void;
    push(chunk: StreamChunk): boolean;
    on(event: string, listener: (...args: any[]) => void): this;
    pipe<T extends Writable>(dest: T): T;
    static from(iterable: Iterable<StreamChunk> | AsyncIterable<StreamChunk>): Readable;
}

export class Writable extends Stream {
    constructor(opts?: any);
    _write(chunk: StreamChunk, enc: string, cb: StreamCallback): void;
    write(chunk: StreamChunk, enc?: string | StreamCallback, cb?: StreamCallback): boolean;
    end(chunk?: StreamChunk, cb?: StreamCallback): this;
}

export class Transform extends Readable {
    constructor(opts?: any);
    _transform(chunk: StreamChunk, enc: string, cb: (err: any, out?: StreamChunk) => void): void;
    write(chunk: StreamChunk, enc?: string | StreamCallback, cb?: StreamCallback): boolean;
    end(chunk?: StreamChunk, cb?: StreamCallback): this;
}

export class Duplex extends Transform {
    constructor(opts?: any);
}
export class PassThrough extends Transform {
    constructor(opts?: any);
}

export function pipeline(...args: any[]): any;
export function finished(stream: any, cb: StreamCallback): void;

declare const _default: {
    Stream: typeof Stream;
    Readable: typeof Readable;
    Writable: typeof Writable;
    Duplex: typeof Duplex;
    Transform: typeof Transform;
    PassThrough: typeof PassThrough;
    pipeline: typeof pipeline;
    finished: typeof finished;
};
export default _default;
