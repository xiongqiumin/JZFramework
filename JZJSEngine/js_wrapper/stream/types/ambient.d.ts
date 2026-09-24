// 'stream' —— 对外的裸模块名(= 目录名): 类型转发给同目录的 ./index.d.ts。
declare module 'stream' {
    export { Stream, Readable, Writable, Duplex, Transform, PassThrough, pipeline, finished } from './index';
    export { default } from './index';
}
