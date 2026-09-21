// @jz/widget 对外暴露的裸模块名(ambient 模块声明)。
// 必须写在全局脚本文件里(本文件无顶层 import/export), 不能并进同为模块形式的 ./widget.d.ts
// (那会变成"模块增强")。模块类型的单一来源仍是同目录的 widget.d.ts, 本文件只声明裸名并转发。
//
// 注: UI 树数据节点(Widget)与真控件类(JZWindow / JZPushButton / ...)都从这里导出,
// 所以 `import { Widget } from 'widget'` 与 `import { JZWindow } from 'widget'` 都可用;
// 其余引擎全局类(JZColor / JZPainter / JZLayout / JZEvent 家族 ...) 仍是全局, 直接当全局用。
declare module 'widget' {
    export * from './widget';
}
