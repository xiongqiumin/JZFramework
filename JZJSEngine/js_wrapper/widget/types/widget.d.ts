// @jz/widget 模块(裸名 'widget')的公共类型契约 —— 单一来源, 运行期实现见 ../index.ts。
//
// 只有**UI 树数据节点**(给 .ui 代码生成用), 没有真控件:
//   真控件是引擎注册的全局类, 见同目录 widgets.d.ts / views.d.ts, 直接用全局名即可
//   (new JZWindow(...) / new JZLabel(...)), 不需要 import。
//
// 裸模块名 'widget' 的 ambient 声明见同目录 ambient.d.ts。

export type EventName = string;
export type Handler = (...args: any[]) => void;
export type StyleMap = Record<string, string | number | boolean>;

export interface UiNodeJson {
  type: string;
  id?: string;
  props?: { [k: string]: any };
  children?: UiNodeJson[];
}

/** UI 树节点: type 对应原生控件类型名(如 "Window" / "PushButton") */
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
