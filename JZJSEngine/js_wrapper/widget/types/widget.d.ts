// @jz/widget 模块的**类型导出**(被 types/index.d.ts 与 ambient.d.ts 的 `export * from './widget'` 引用)。
//
// 这里只声明"模块形式可用"的那部分:
//   * UI 树数据节点 `Widget`(JZJSUiLoader::generateTsClass 生成代码用, `import { Widget } from 'widget'`)
//   * 真控件类(JZWindow / JZPushButton / ...), 运行期由 classes.ts 既挂到 globalThis、又作为模块命名导出
//     (见运行期 index.ts 的 `export * from './classes'`)—— 两者是同一份类对象。
//
// 真控件类的**完整类型**(构造重载 / 方法 / 继承)已经在同目录各 .d.ts 里以全局 ambient 形式声明
// (widgets.d.ts / views.d.ts / dialogs.d.ts ...)。这里用 `typeof globalThis.X` 复用那些声明,
// 不重复抄一遍类型。
//
// 其余引擎全局类(JZColor / JZPainter / JZLayout / JZEvent 家族 / JZPen / JZImage / JZPoint·JZSize·JZRect
// / JZMatrix2D / JZMessageBox / JZFileDialog / uiLoader / jzApp ...) 仍是全局, 直接当全局用, 无需 import。

/// <reference path="./widgets.d.ts" />
/// <reference path="./views.d.ts" />
/// <reference path="./dialogs.d.ts" />
/// <reference path="./layout.d.ts" />
/// <reference path="./geometry.d.ts" />
/// <reference path="./color.d.ts" />
/// <reference path="./image.d.ts" />
/// <reference path="./events.d.ts" />
/// <reference path="./pen.d.ts" />
/// <reference path="./matrix.d.ts" />
/// <reference path="./painter.d.ts" />
/// <reference path="./loader.d.ts" />

// ============================ UI 树数据节点 ============================
export type EventName = string;
export type Handler = (...args: any[]) => void;
export type StyleMap = Record<string, string | number | boolean>;

export interface UiNodeJson {
  type: string;
  id?: string;
  props?: { [k: string]: any };
  children?: UiNodeJson[];
}

// 形状与运行期 index.ts 的 `class Widget` 对齐(只有数据节点 API, 真控件不在这里)。
export class Widget {
  type: string;
  id?: string;
  constructor(type: string, parent?: Widget);
  setId(id: string): this;
  setProperty(name: string, value: any): this;
  setGeometry(x: number, y: number, w: number, h: number): this;
  setStyle(style: StyleMap): this;
  setVisible(v: boolean): this;
  append(child: Widget): this;
  on(event: EventName, cb: Handler): this;
  off(event: EventName, cb: Handler): this;
  handlers(event: EventName): Handler[];
  toJson(): UiNodeJson;
}

// ============================ 真控件类(模块命名导出) ============================
// 类型复用全局 ambient 声明; 值就是运行期挂在 globalThis 上、并由 classes.ts 导出到模块的那一份。
export const JZWidget:        typeof globalThis.JZWidget;
export const JZWindow:        typeof globalThis.JZWindow;
export const JZMainWindow:    typeof globalThis.JZMainWindow;
export const JZDialog:        typeof globalThis.JZDialog;
export const JZMenuBar:       typeof globalThis.JZMenuBar;
export const JZLabel:         typeof globalThis.JZLabel;
export const JZPushButton:    typeof globalThis.JZPushButton;
export const JZLineEdit:      typeof globalThis.JZLineEdit;
export const JZCheckbox:      typeof globalThis.JZCheckbox;
export const JZRadioButton:   typeof globalThis.JZRadioButton;
export const JZSlider:        typeof globalThis.JZSlider;
export const JZProgressBar:   typeof globalThis.JZProgressBar;
export const JZComboBox:      typeof globalThis.JZComboBox;
export const JZSpinBox:       typeof globalThis.JZSpinBox;
export const JZDoubleSpinBox: typeof globalThis.JZDoubleSpinBox;
export const JZDateEdit:      typeof globalThis.JZDateEdit;
export const JZGroupBox:      typeof globalThis.JZGroupBox;
export const JZSwitch:        typeof globalThis.JZSwitch;
export const JZTextEdit:      typeof globalThis.JZTextEdit;
export const JZChart:         typeof globalThis.JZChart;
export const JZImageLabel:    typeof globalThis.JZImageLabel;
export const JZListWidget:    typeof globalThis.JZListWidget;
export const JZTableWidget:   typeof globalThis.JZTableWidget;
export const JZStackedWidget: typeof globalThis.JZStackedWidget;
export const JZTabWidget:     typeof globalThis.JZTabWidget;
export const JZSplitter:      typeof globalThis.JZSplitter;
export const JZScrollArea:    typeof globalThis.JZScrollArea;
export const JZScrollBar:     typeof globalThis.JZScrollBar;
