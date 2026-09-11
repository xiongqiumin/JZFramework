// @jz/widget 对外暴露的裸模块名(ambient 模块声明)。
// 必须写在全局脚本文件里(本文件无顶层 import/export), 不能并进同为模块形式的 ./widget.d.ts
// (那会变成"模块增强")。模块类型的单一来源仍是同目录的 widget.d.ts, 本文件只声明裸名并转发。
declare module 'widget' {
    export * from './widget';
}
