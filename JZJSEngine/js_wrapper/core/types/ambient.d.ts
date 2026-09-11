// core 包对外暴露的 Node 风格模块名 (ambient 模块声明): fs / path / process / crypto。
//
// 为什么单独一个文件、而不是并进同目录的 ./fs.d.ts:
//   ambient `declare module '<裸名>'` 只能写在**全局脚本文件**里(无顶层 import/export)。
//   ./fs.d.ts 是模块文件(有 export), 在模块里写 `declare module 'fs'` 会被当作"模块增强",
//   要求目标模块已存在 => 不成立。所以本包的类型 = 模块形式的 <module>.d.ts(单一来源)
//   + 本文件(仅声明裸露的模块名并转发过去)。
//
// 转发目标就是同目录的模块类型文件, 相对路径按**本文件所在目录**解析(部署时目录同构)。
// 用户侧用法(类型按需引入, 运行期被擦除, 不触发模块加载):
//   import type { Stats } from 'fs';
//   import type { Platform } from 'os';      // <- net 包, 见 ../net/types/ambient.d.ts

declare module 'fs' {
    export * from './fs';
}

declare module 'path' {
    export * from './path';
}

declare module 'process' {
    export * from './process';
}

declare module 'crypto' {
    export * from './crypto';
}
