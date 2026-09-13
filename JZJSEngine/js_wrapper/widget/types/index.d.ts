// @jz/widget 类型汇总入口, 便于按包路径引用(如 `import type { JZWindow } from '@jz/widget'`)。
//
// 两套东西, 别混用:
//   1) './widget' —— **纯 JS 构建器**: Web-Components 风格的 UI 树构建 + Canvas 2D 画笔
//      (运行期实现 ../index.ts), 需要 import。
//   2) 本目录其它 .d.ts —— **引擎注册的全局类**(JZPoint/JZColor/JZPainter/JZWidget/uiLoader...),
//      运行期由 JZJSWidgetModule 的各 registXxx() 挂到全局对象上, 直接用全局名, 不需要 import;
//      它们是全局脚本文件(无顶层 import/export), 由下面这些引用拉进工程。
//
// 全局类的文件划分与 registXxx() 一一对应:
//   geometry.d.ts -> JZPoint / JZSize / JZRect         (registGeometry)
//   color.d.ts    -> JZColor                           (registColor)
//   image.d.ts    -> JZImage                           (registImage)
//   events.d.ts   -> JZEvent 及其子类                   (registEvents)
//   widgets.d.ts  -> JZWidget / 各控件 / JZChart        (registWidgets)
//   layout.d.ts   -> JZLayout / 四种具体布局            (registWidgets)
//   painter.d.ts  -> JZPainter                         (registPainter)
//   loader.d.ts   -> 全局 uiLoader 命名空间             (registUiLoader)
//
// 名字冲突提醒: 全局 JZWindow/JZCanvas 与 './widget' 导出的同名构建器类**不是同一个东西**
// (前者是原生控件包装, 后者只是 UI 树的数据节点)。
/// <reference path="./geometry.d.ts" />
/// <reference path="./color.d.ts" />
/// <reference path="./image.d.ts" />
/// <reference path="./events.d.ts" />
/// <reference path="./widgets.d.ts" />
/// <reference path="./layout.d.ts" />
/// <reference path="./painter.d.ts" />
/// <reference path="./loader.d.ts" />

export * from './widget';
