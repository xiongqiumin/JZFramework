// 'worker_threads' —— 对外的裸模块名(= 目录名): 类型转发给同目录的 ./index.d.ts。
// (TS7 + moduleResolution:bundler 下 ambient 内的 `export *` 只转发 default, 故显式具名再导出。)
declare module 'worker_threads' {
    export { Worker, MessagePort, parentPort, isMainThread, threadId, workerData, SHARE_ENV } from './index';
    export { default } from './index';
}
