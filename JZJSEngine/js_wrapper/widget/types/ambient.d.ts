// @jz/widget 对外暴露的裸模块名(ambient 模块声明)。
// 必须写在全局脚本文件里(本文件无顶层 import/export), 不能并进同为模块形式的 ./widget.d.ts
// (那会变成"模块增强")。模块类型的单一来源仍是同目录的 widget.d.ts, 本文件只声明裸名并转发。
//
// 注: 这里只是 **UI 树数据节点**(.ui 代码生成用); 真控件是引擎注册的全局类
// (JZWindow / JZPushButton / ... 见同目录 widgets.d.ts / views.d.ts), 不需要 import。
declare module 'widget' {
    export * from './widget';
}
