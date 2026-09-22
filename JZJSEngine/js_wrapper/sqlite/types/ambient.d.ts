// 'sqlite' —— 对外的裸模块名(= 目录名): 这里只声明名字, 类型转发给同目录的 ./index.d.ts
// (ambient `declare module` 只能写在全局脚本文件里, 所以单独一个文件)。
// 与 Node 的 `node:sqlite` 一致: 推荐具名导入 `import { DatabaseSync } from 'sqlite'`;
// 本实现另外给了 default(= 同一个模块对象), 所以默认导入也不会炸。
declare module 'sqlite' {
    export * from './index';
    export { default } from './index';
}
