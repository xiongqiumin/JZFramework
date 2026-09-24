/**
 * @jz/worker_threads —— Node `worker_threads` 兼容层(与 worker_threads/index.ts 同源)。
 * 范围: Worker / parentPort(MessagePort) / isMainThread / threadId / workerData。
 * 简化: 入口脚本按**运行根**解析; terminate() 同步; 不做 MessageChannel/BroadcastChannel。
 */

export class MessagePort {
    postMessage(value: any): void;
    close(): void;
    ref(): void;
    unref(): void;

    on(event: 'message', listener: (value: any) => void): this;
    on(event: string, listener: (...args: any[]) => void): this;
    once(event: string, listener: (...args: any[]) => void): this;
    off(event: string, listener?: (...args: any[]) => void): this;
}

export class Worker {
    /** entry 按**运行根**(入口 js 所在目录)解析 */
    constructor(entry: string, options?: { workerData?: any });

    /** 该 worker 的 id(主线程恒为 0) */
    readonly threadId: number;
    readonly exited: boolean;
    readonly exitCode: number;

    postMessage(value: any): void;
    /** 同步结束该 worker(Node 返回 Promise, 引擎是同步的) */
    terminate(): void;

    on(event: 'message', listener: (value: any) => void): this;
    on(event: 'exit', listener: (code: number) => void): this;
    on(event: string, listener: (...args: any[]) => void): this;
    once(event: string, listener: (...args: any[]) => void): this;
    off(event: string, listener?: (...args: any[]) => void): this;
}

/** 主线程 true; worker 内 false */
export const isMainThread: boolean;
/** 本引擎 id(主线程恒为 0) */
export const threadId: number;
/** new Worker 时随包进来的数据; 主线程为 null */
export const workerData: any;
/** worker 侧与父引擎通信的端口; 主线程为 null */
export const parentPort: MessagePort | null;
/** 占位(引擎不支持共享 env) */
export const SHARE_ENV: symbol;

declare const _default: {
    Worker: typeof Worker;
    MessagePort: typeof MessagePort;
    parentPort: typeof parentPort;
    isMainThread: typeof isMainThread;
    threadId: typeof threadId;
    workerData: typeof workerData;
    SHARE_ENV: typeof SHARE_ENV;
};
export default _default;
