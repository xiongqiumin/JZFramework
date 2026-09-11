// serialport 包对外暴露的模块名 (ambient 模块声明) + 运行期全局 serialport。
//
// 与 core/net 的 ambient.d.ts 同理: ambient `declare module '<裸名>'` 必须写在全局脚本
// 文件里, 不能并进模块形式的 ./index.d.ts(那会变成"模块增强")。
// 模块类型的单一来源 = 同目录 ./index.d.ts。
//
// 注意: serialport 在运行期**同时**是全局值(引擎装入 globalThis, 与 fs/path/net 等
// 必须 import 的模块不同), 所以这里额外声明 `declare const serialport`。

declare module 'serialport' {
    export * from './index';
}

declare const serialport: typeof import('serialport');
