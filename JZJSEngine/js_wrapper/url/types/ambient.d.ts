// 'url' —— 对外的裸模块名(= 目录名): 这里只声明名字, 类型转发给同目录的 ./index.d.ts
// (ambient `declare module` 只能写在全局脚本文件里, 所以单独一个文件)。
// 注: 全局 URL / URLSearchParams 的声明见 ../../types/globals.d.ts。
declare module 'url' {
    export * from './index';
    export { default } from './index';
}
