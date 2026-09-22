// sqlite 包对外暴露的 Node 风格模块名 (ambient 模块声明): sqlite。
//
// 与 core/net/serialport 的 ambient.d.ts 同理: `declare module '<裸名>'` 只能写在
// **全局脚本文件**里(无顶层 import/export), 所以单独一个文件, 只声明裸名并转发给
// 同目录的模块类型 ./index.d.ts(单一来源)。
//
// 形态与 Node 的 node:sqlite 一致 = namespace(**没有 default**):
//   import { DatabaseSync } from 'sqlite';        // 对
//   import sqlite from 'sqlite';                  // 错: 运行期没有 default 导出
// ⚠ 少这个文件时 `import ... from 'sqlite'` 报 TS2307(Cannot find module),
//   而运行期其实是好的 —— 名字有类型声明才算"两边同名"。
declare module 'sqlite' {
    export * from './index';
}
