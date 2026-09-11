// @jz/widget 公共类型契约(单一来源)。运行期实现见 ../index.ts, 用户侧用
// `import type { JZWindow } from 'widget'` 取类型(由 ambient.d.ts 把裸名 'widget' 指到本文件)。

export type EventName = string;
export type Handler = (...args: any[]) => void;
export type StyleMap = Record<string, string | number | boolean>;
export type ColorValue = string | { r: number; g: number; b: number; a?: number };

export interface UiNodeJson {
  type: string;
  id?: string;
  props?: { [k: string]: any };
  children?: UiNodeJson[];
}

// 路径图元(控件本地坐标), JS 侧累积后经原生适配层转成 JZPainterPath。
export type PathOp =
  | ['moveTo', number, number]
  | ['lineTo', number, number]
  | ['cubic', number, number, number, number, number, number]
  | ['quad', number, number, number, number]
  | ['arc', number, number, number, number, number]
  | ['ellipse', number, number, number, number]
  | ['rect', number, number, number, number]
  | ['close'];

// 原生 painter 适配层需实现的接口(JZPainter 经 ClassBind 暴露时提供)。
export interface PainterBackend {
  save(): void;
  restore(): void;
  translate(dx: number, dy: number): void;
  rotate(deg: number): void;
  scale(sx: number, sy: number): void;
  setTransform(a: number, b: number, c: number, d: number, e: number, f: number): void;
  resetTransform(): void;
  setClip(x: number, y: number, w: number, h: number): void;
  setPen(color: ColorValue, width: number): void;
  setBrush(color: ColorValue): void;
  setFont(family: string, size: number): void;
  drawLine(x1: number, y1: number, x2: number, y2: number): void;
  drawRect(x: number, y: number, w: number, h: number): void;
  drawRoundRect(x: number, y: number, w: number, h: number, r: number): void;
  fillRect(x: number, y: number, w: number, h: number, color: ColorValue): void;
  fillRoundRect(x: number, y: number, w: number, h: number, r: number, color: ColorValue): void;
  drawArc(x: number, y: number, r: number, startDeg: number, endDeg: number): void;
  drawEllipse(x: number, y: number, w: number, h: number): void;
  fillEllipse(x: number, y: number, w: number, h: number, color: ColorValue): void;
  drawText(x: number, y: number, text: string): void;
  drawImage(img: any, x: number, y: number, w: number, h: number): void;
  strokePath(ops: PathOp[]): void;
  fillPath(ops: PathOp[]): void;
}

// 画笔上下文: HTML Canvas 2D 风格, 仅暴露 JZPainter 真实支持的能力。
export class JZCanvasContext {
  constructor(backend: PainterBackend);
  fillStyle: ColorValue;
  strokeStyle: ColorValue;
  lineWidth: number;
  font: string;
  save(): void;
  restore(): void;
  translate(dx: number, dy: number): void;
  rotate(rad: number): void;
  scale(sx: number, sy: number): void;
  setTransform(a: number, b: number, c: number, d: number, e: number, f: number): void;
  resetTransform(): void;
  clip(x: number, y: number, w: number, h: number): void;
  fillRect(x: number, y: number, w: number, h: number): void;
  strokeRect(x: number, y: number, w: number, h: number): void;
  fillRoundRect(x: number, y: number, w: number, h: number, r: number): void;
  strokeRoundRect(x: number, y: number, w: number, h: number, r: number): void;
  fillText(text: string, x: number, y: number): void;
  drawImage(img: any, x: number, y: number, w: number, h: number): void;
  beginPath(): void;
  moveTo(x: number, y: number): void;
  lineTo(x: number, y: number): void;
  rect(x: number, y: number, w: number, h: number): void;
  closePath(): void;
  arc(cx: number, cy: number, r: number, startRad: number, endRad: number): void;
  ellipse(cx: number, cy: number, rx: number, ry: number): void;
  quadraticCurveTo(cx: number, cy: number, x: number, y: number): void;
  bezierCurveTo(c1x: number, c1y: number, c2x: number, c2y: number, x: number, y: number): void;
  fill(): void;
  stroke(): void;
}

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

export class JZWindow extends Widget {
  constructor(parent?: Widget);
  setTitle(t: string): this;
  show(): void;
  close(): void;
}

export class JZCanvas extends Widget {
  constructor(parent?: Widget);
  setWidth(w: number): this;
  setHeight(h: number): this;
  setOnPaint(cb: (canvas: JZCanvas, ctx: JZCanvasContext) => void): this;
  paint(backend: PainterBackend): void;
}
