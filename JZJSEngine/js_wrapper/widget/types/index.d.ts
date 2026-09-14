// JZWidget 家族的 TS 类型声明入口(全局声明, 无运行期 JS 产物)。
//
// ============================ 这个包是什么 ============================
// 运行期**没有** widget 的 JS 模块: 控件/布局/画笔全部由 C++ 侧 JZJSWidgetModule
// (JZJSEngine/JSWidget/) 用 ClassBind + C 构造器注册到**引擎全局对象**上, 直接用全局名:
//
//     const win = new JZWindow(900, 660, "Hello");
//     const root = new JZVBoxLayout(8, 6);
//     win.setLayout(root);
//     const btn = new JZPushButton(null, 120, 32, "Click Me");
//     btn.onClicked(() => console.log("clicked"));
//     root.addWidget(btn, 0);
//     win.show();
//
// 所以本包**不需要 import**, 也不需要 tsc 编译产物; 用户工程只要把部署的 jz_types 收进
// include 即可(见 js_wrapper/types/index.d.ts 的用法说明):
//     "include": ["src/**/*.ts", "jz_types/**/*.d.ts"]
//
// ============================ 文件划分 ============================
// 与 JZJSWidgetModule 的 registXxx() 一一对应(改绑定就改对应文件, 别新开一套):
//   geometry.d.ts -> JZPoint / JZSize / JZRect          (registGeometry)
//   color.d.ts    -> JZColor                            (registColor)
//   image.d.ts    -> JZImage                            (registImage)
//   events.d.ts   -> JZEvent 及其子类                    (registEvents)
//   widgets.d.ts  -> JZWidget / JZWindow / 基础控件       (registWidgets)
//   dialogs.d.ts  -> JZDialog + JZMessageBox / JZFileDialog (registDialogs, 模态)
//   views.d.ts    -> 容器与视图控件                       (registViews)
//   items.d.ts    -> JZTableWidgetItem / JZListWidgetItem (registItems, 容器子项)
//   layout.d.ts   -> JZLayout / 四种具体布局              (registLayouts)
//   pen.d.ts      -> JZPen / JZBrush / JZFont            (registPens)
//   matrix.d.ts   -> JZMatrix2D                         (registMatrix)
//   painter.d.ts  -> JZPainter                          (registPainter)
//   loader.d.ts   -> 全局 uiLoader / jzWidget            (registUiLoader)
// 构造实参由 jzRegistWidgetJsConstructors() 的 C 构造器决定(js_JZxxx_ctor),
// 支持形态一览:
//   JZWidget        () / (parent)
//   JZWindow        () / (parent) / (w, h) / (w, h, title) / (w, h, title, parent)
//   JZLabel         () / (parent) / (text) / (text, parent)
//   JZRadioButton   () / (parent) / (text) / (text, parent)
//   JZGroupBox      () / (parent) / (title) / (title, parent)
//   JZPushButton    () / (parent) / (parent, w, h, text)
//   JZSplitter      () / (parent) / (orientation) / (orientation, parent)   0=横 1=纵
//   JZScrollBar     同 JZSplitter
//   其余控件         () / (parent)
//   JZLayout        () / (pad) / (pad, spacing)  (实际造 JZVBoxLayout)
//   JZVBoxLayout    () / (pad) / (pad, spacing)      JZHBoxLayout 同
//   JZGridLayout    () / (pad) / (gap)
//   JZSelfLayout    () / (pad)
//   JZPoint         () / (x, y)          JZSize  () / (w, h)
//   JZRect          () / (x, y) / (x, y, w, h)
//   JZColor         () / (colorEnum) / (r, g, b) / (r, g, b, a)
//   JZImage         () / (filePath) / (bytes: Uint8Array), 另加静态 JZImage.create(w, h)
//   JZPen           () / (color) / (color, width)
//   JZBrush         () / (color) / (color, opacity)
//   JZFont          () / (size) / (family, size) / (family, size, style)
//   JZMatrix2D      () / (a, b, c, d, e, f) / (other)
//
// ============================ 所有权(重要) ============================
//   * **挂进对象树 = 所有权转移**: addWidget / insertWidget / addTab / setWidget /
//     setParentWidget / setLayout 都会把 JS 包装翻成"C++ 拥有", 之后由对象树负责 delete;
//     同时按控件登记到 JZJSWidgetModule —— 父节点持一份"子控件 JS 包装"的引用防 GC。
//     所以挂进树的控件/布局**不需要**自己 keep, 放手也不会被回收。
//   * **顶层对象归 JS 所有**(一般就是窗口, 没有父控件): JS 放手 → 包装析构 → 窗口 delete
//     → 没有窗口了, code_runtime 退出。故顶层窗口必须留一份引用。
//   * setParentWidget(null) 把控件交回 JS 拥有(脱离树)。

/// <reference path="./geometry.d.ts" />
/// <reference path="./color.d.ts" />
/// <reference path="./image.d.ts" />
/// <reference path="./events.d.ts" />
/// <reference path="./widgets.d.ts" />
/// <reference path="./views.d.ts" />
/// <reference path="./layout.d.ts" />
/// <reference path="./pen.d.ts" />
/// <reference path="./matrix.d.ts" />
/// <reference path="./painter.d.ts" />
/// <reference path="./loader.d.ts" />

// 另一层(模块形式的 'widget', 与真控件无关): UI 树数据节点构建器, 只有 .ui 代码生成用得到
// (JZJSUiLoader::generateTsClass 生成的类里 `import { Widget } from 'widget'`)。类型契约在同目录
// widget.d.ts, 运行期实现在 ../index.ts; 裸模块名的 ambient 声明见 ambient.d.ts。
export * from './widget';
