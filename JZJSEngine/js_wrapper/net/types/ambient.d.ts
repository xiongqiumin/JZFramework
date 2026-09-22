// net 包对外暴露的 Node 风格模块名 (ambient 模块声明):
//   net / dgram / http / mqtt / url / os / ws(对应本目录 websocket.d.ts) / events。
//
// 与 core/types/ambient.d.ts 同理: ambient `declare module '<裸名>'` 必须写在全局脚本文件里,
// 不能并进同为模块形式的 ./net.d.ts 等(那会变成"模块增强")。模块类型的单一来源仍是
// 同目录的 <module>.d.ts, 本文件只声明裸露模块名并转发。
//
// 全局类 URL / URLSearchParams / WebSocket 不是某模块的导出(运行期就是全局对象),
// 见 js_wrapper/types/globals.d.ts。

declare module 'net' {
    export * from './net';
}

declare module 'dgram' {
    export * from './dgram';
}

declare module 'http' {
    export * from './http';
}

declare module 'mqtt' {
    export * from './mqtt';
}

declare module 'url' {
    export * from './url';
}

declare module 'os' {
    export * from './os';
}

declare module 'ws' {
    export * from './websocket';
}

// events: 运行期(builtinModules)的默认导出 = net 包出口里的 EventEmitter 类,
// 而类型侧的 EventEmitter 是**全局类**(../../types/events.d.ts, 运行期它本来在全局作用域),
// 全局脚本文件不是模块、不能 `export * from`, 所以这里直接引用全局那个类名。
// 具名导入(Node 习惯)与默认导入都可用: import { EventEmitter } from 'events'。
// ⚠ 少这个文件时: `import ... from 'events'` 报 TS2591(找不到名字), 而运行期其实是好的。
declare module 'events' {
    export { EventEmitter };
    export default EventEmitter;
}
