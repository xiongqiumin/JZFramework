// jzwidget.ts —— JS 侧控件基类（占位）
//
// 在 C++ 把真实 JZWidget 注册进 quickjs 之前，这里先用纯 TS 占位给出"形状"。
// codegen 生成的实体类（如 MainPanel）继承这些类，构造里 setupUi() 建出真实控件树。
// 待 C++ 注册完成后，只需把本文件的实现替换成引擎暴露的真实控件即可，生成器产物不变。

export type EventName = string;
export type Handler = (...args: any[]) => void;

export class Widget {
  type: string = "Widget";
  id?: string;
  protected _children: Widget[] = [];

  constructor(parent?: Widget) {
    if (parent) parent._children.push(this);
  }

  setGeometry(x: number, y: number, w: number, h: number): this { return this; }
  setVisible(v: boolean): this { return this; }
  show(): void {}
  on(event: EventName, cb: Handler): this { return this; }
  off(event: EventName, cb: Handler): this { return this; }
}

export class WindowWidget extends Widget {
  type = "Window";
  setTitle(t: string): this { return this; }
  close(): void {}
}

export class LabelWidget extends Widget {
  type = "Label";
  setText(t: string): this { return this; }
  getText(): string { return ""; }
}

export class PushButtonWidget extends Widget {
  type = "PushButton";
  setText(t: string): this { return this; }
  onClick(cb: () => void): this { return this; }
}

export class EditWidget extends Widget {
  type = "Edit";
  setText(t: string): this { return this; }
  getText(): string { return ""; }
}
