// ---------------------------------------------------------------------------
// 例子本地补的引擎全局声明 —— 只补"引擎装到全局、而 js_wrapper 还没写 .d.ts"的那几个。
//
// 目前只有 worker 这一块: `__jz.worker.*`(worker 侧的原子 API)与 `new Worker(...)`
// (主线程侧的句柄)。契约抄自 JZJSEngine/JZJSWorkerModule.h 的文件头注释:
//   __jz.worker = { isMainThread, workerData, selfId(), parentId(), ids(),
//                   childIds(), postTo(id,value), onMessage((fromId,value)=>..), exit() }
//   Worker      = new Worker(entry[,{workerData}]); .id/.exited/.exitCode
//                 .postMessage(v) / .on('exit',cb) / .terminate()
// 09_worker.ts / helper/worker_sum.ts 用的就是这套(那两份文件里另有使用说明)。
// ---------------------------------------------------------------------------

/** 引擎注入的命名空间(这里只声明 worker 相关那部分; 宿主其余能力走各内置包) */
declare const __jz: {
    worker: {
        /** 主线程 true; worker 线程 false */
        isMainThread(): boolean;
        /** 创建本引擎时随包带进来的数据(主线程没有) */
        workerData: any;
        /** 本引擎的 id: 主线程恒为 0 */
        selfId(): number;
        /** 创建本引擎的那个 id: 主线程为 -1 */
        parentId(): number;
        /** 当前活着的 worker id 列表 */
        ids(): number[];
        /** 本引擎直接创建的子 worker id 列表 */
        childIds(): number[];
        /** 把 value 投给 id 那个引擎(消息里的对象可以带共享引用与环) */
        postTo(id: number, value: any): void;
        /** 收消息: 第一个参数是发送方 id */
        onMessage(cb: (fromId: number, value: any) => void): void;
        /** 本引擎退出 */
        exit(): void;
    };
};

/** worker 句柄(主线程侧 `new Worker(入口脚本)` 的返回值; 入口按**运行根**解析) */
declare class Worker {
    constructor(entry: string, options?: { workerData?: any });

    /** 分配到的 worker id */
    readonly id: number;
    readonly exited: boolean;
    readonly exitCode: number;

    /** 给该 worker 发消息 */
    postMessage(value: any): void;
    /** worker 结束时触发 */
    on(event: 'exit', listener: (code: number) => void): void;
    /** 结束该 worker */
    terminate(): void;
}
