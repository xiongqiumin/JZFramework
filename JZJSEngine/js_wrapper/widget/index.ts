// @jz/widget (运行期 JS) —— **UI 树数据节点构建器**, 只服务 .ui 代码生成:
//
//   JZJSUiLoader::generateTsClass() 生成的类形如:
//     import { Widget } from 'widget';
//     export class PanelUi {
//       root: Widget;
//       btn: Widget;
//       private setupUi() {
//         this.root = new Widget("Window");
//         this.btn  = new Widget("PushButton");
//         this.btn.setId("btn");
//         this.root.append(this.btn);
//       }
//     }
//   这些字段只是"UI 树节点"的**类型占位**(import 只出现在类型位置, tsc 会把它擦除),
//   所以本模块的 Widget 不是真控件 —— 它只是 type/id/props/children 的数据节点。
//
// ⚠ 真控件不在本模块: 它们是**引擎注册的全局类**(见 ./types/widgets.d.ts / views.d.ts):
//   new JZWindow(w, h, title) / new JZPushButton(...) / new JZLabel(text) ...
//   父子上树用 setParent() / setParentWidget() / 布局 addWidget() / JZWindow.addWidget(),
//   绘制用全局 JZPainter(见 ./types/painter.d.ts)。要建真实界面请直接用全局类;
//   本模块只在你实现"UI 定义 -> 数据树"这一层(如 .ui 代码生成)时才需要。
//
// 另外本模块**顺带**干两件事(引擎装配 widget 模块时会加载本包, 加载即生效, 幂等):
//   1. 定义控件族的**类层**(./classes.ts): JZWidget/JZWindow/JZLabel... 全局类在这里定义,
//      构造器转发给 C 侧的 init_c_xxx; 方法由 C++ 挂到这些原型上。控件族对 JZJSEngine 透明
//      (引擎不注册它们), 类型契约见 ./types/*.d.ts。
//   2. 给真控件装上对象树支持(./tree.ts: 父子登记 + 保活 + 子控件弱引用父, 以及
//      setParent() / parent() / children() / JZLayout.children() / JZLayout.parentWidget())。
//      那些方法是 C++ 侧还没注册的, 所以**必须 import 本包才可用**:
//        import '@jz/widget';
// 顺序要紧: 先类层(定义全局类), 再 tree(往那些原型上装方法)。

// 控件族的类层(必须最先执行)
import './classes';

// 真控件的对象树(保活 / 弱引用父) —— 模块加载即装到全局类的原型上, 见 ./tree.ts
export * from './tree';

// 列表/表格的视图镜像(JZTableWidget/JZListWidget 的 dataView()) —— 同样挂在原型上, 见 ./dataview.ts
export * from './dataview';

// 控件的通用属性表(setProperty/property) —— **纯 JS 侧存储**, 见 ./props.ts
export * from './props';

// 菜单句柄保活(JZMenu/JZMenuBar 交回的是借用句柄, 见 ./menus.ts)
export * from './menus';

// 信号/事件装成原型属性(默认 null) —— 见 ./signals.ts
export * from './signals';

export type EventName = string;
export type Handler = (...args: any[]) => void;
export type StyleMap = Record<string, string | number | boolean>;

export interface UiNodeJson {
  type: string;
  id?: string;
  props?: { [k: string]: any };
  children?: UiNodeJson[];
}

// UI 树节点(type 对应原生控件类型名, 如 "Window" / "PushButton" / "Canvas")。
// toJson() 产出 JZUiNode 兼容的 JSON 树(type/id/props/children)。
export class Widget {
  type: string;
  id?: string;
  protected _props: { [k: string]: string | number | boolean | StyleMap };
  protected _children: Widget[];
  protected _handlers: { [ev: string]: Handler[] };

  constructor(type: string, parent?: Widget) {
    this.type = type;
    this._props = {};
    this._children = [];
    this._handlers = {};
    if (parent) parent.append(this);
  }

  setId(id: string): this { this.id = id; return this; }
  setProperty(name: string, value: any): this { this._props[name] = value; return this; }

  setGeometry(x: number, y: number, w: number, h: number): this {
    this._props["x"] = x;
    this._props["y"] = y;
    this._props["width"] = w;
    this._props["height"] = h;
    return this;
  }

  setStyle(style: StyleMap): this {
    const cur = (this._props["style"] || {}) as StyleMap;
    this._props["style"] = Object.assign({}, cur, style);
    return this;
  }

  setVisible(v: boolean): this { this._props["visible"] = v; return this; }
  append(child: Widget): this { this._children.push(child); return this; }

  on(event: EventName, cb: Handler): this {
    (this._handlers[event] || (this._handlers[event] = [])).push(cb);
    return this;
  }
  off(event: EventName, cb: Handler): this {
    const list = this._handlers[event];
    if (list) {
      const i = list.indexOf(cb);
      if (i >= 0) list.splice(i, 1);
    }
    return this;
  }
  // 原生侧构建完真实控件后, 取事件回调以便连 signal
  handlers(event: EventName): Handler[] { return this._handlers[event] || []; }

  toJson(): UiNodeJson {
    const node: UiNodeJson = { type: this.type };
    if (this.id !== undefined) node.id = this.id;
    if (Object.keys(this._props).length) node.props = this._props;
    if (this._children.length) node.children = this._children.map(c => c.toJson());
    return node;
  }
}
