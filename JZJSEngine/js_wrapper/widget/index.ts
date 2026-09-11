// @jz/widget —— Web Components 风格的 JS 侧控件构建器
//
// 设计定位(对齐 JZJSUiLoader 的纯数据层):
//   每个 JZWidget 在 JS 侧就是一个 custom element。`type` 对应原生控件类名
//   (见 JZJSUiLoader::knownTypes, 如 "Window" / "Canvas")。`toJson()` 产出
//   JZUiNode 兼容的 JSON 树(type/id/props/children), 由原生侧(JZWidget)后续
//   构建真实控件。样式走 CSS3(setStyle), 事件用 on()/off() 暂存于节点,
//   原生侧建完真实控件后连 signal 为下一步。
//
// 本文件不依赖 __jz 原生绑定, 可独立运行/类型检查; 原生桥接在 C++ 侧接入。

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

// ---------------------------------------------------------------------------
// Painter —— 按 HTML Canvas 2D 习惯暴露, 仅暴露 JZPainter 真实支持的能力
// ---------------------------------------------------------------------------

// 路径图元(控件本地坐标), JS 侧累积后经原生适配层转成 JZPainterPath。
// arc 存 中心+半径+角度(度), 原生转 bounding box 调 JZPainterPath::addArc。
export type PathOp =
  | ['moveTo', number, number]
  | ['lineTo', number, number]
  | ['cubic', number, number, number, number, number, number]
  | ['quad', number, number, number, number]
  | ['arc', number, number, number, number, number]   // cx, cy, r, startDeg, endDeg
  | ['ellipse', number, number, number, number]        // x, y, w, h (bounding box)
  | ['rect', number, number, number, number]
  | ['close'];

// 原生 painter 适配层需实现的接口(JZPainter 经 ClassBind 暴露时提供)。
// 坐标单位 = 控件本地坐标(像素/度), 与 JZPainter 一致; 颜色用 CSS 风格字符串。
export interface PainterBackend {
  save(): void;
  restore(): void;
  translate(dx: number, dy: number): void;
  rotate(deg: number): void;                              // 角度制(屏幕顺时针为正)
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

// 画笔上下文: HTML Canvas 2D 风格。JZPainter 没有的能力(clearRect/globalAlpha/
// 渐变/measureText/阴影/虚线等)一律不暴露。
export class JZCanvasContext {
  private _backend: PainterBackend;
  private _fill: ColorValue = '#000000';
  private _stroke: ColorValue = '#000000';
  private _lineWidth = 1;
  private _fontFamily = 'sans-serif';
  private _fontSize = 12;
  private _path: PathOp[] = [];

  constructor(backend: PainterBackend) { this._backend = backend; }

  // ---- 样式(HTML 习惯: 写属性, 作用于后续绘制) ----
  get fillStyle(): ColorValue { return this._fill; }
  set fillStyle(c: ColorValue) { this._fill = c; }
  get strokeStyle(): ColorValue { return this._stroke; }
  set strokeStyle(c: ColorValue) { this._stroke = c; }
  get lineWidth(): number { return this._lineWidth; }
  set lineWidth(w: number) { this._lineWidth = w; }
  get font(): string { return this._fontSize + 'px ' + this._fontFamily; }
  set font(s: string) {
    const m = /(\d+(?:\.\d+)?)\s*px\s*(.*)/.exec(s.trim());
    if (m) { this._fontSize = parseFloat(m[1]); this._fontFamily = m[2] || this._fontFamily; }
  }

  // ---- 状态 / 变换 ----
  save(): void { this._backend.save(); }
  restore(): void { this._backend.restore(); }
  translate(dx: number, dy: number): void { this._backend.translate(dx, dy); }
  rotate(rad: number): void { this._backend.rotate(rad * 180 / Math.PI); }   // HTML 弧度 -> JZPainter 角度
  scale(sx: number, sy: number): void { this._backend.scale(sx, sy); }
  setTransform(a: number, b: number, c: number, d: number, e: number, f: number): void {
    this._backend.setTransform(a, b, c, d, e, f);
  }
  resetTransform(): void { this._backend.resetTransform(); }
  // JZPainter 仅支持矩形裁剪, 故为立即模式 rect clip(非 HTML 的 path clip)
  clip(x: number, y: number, w: number, h: number): void { this._backend.setClip(x, y, w, h); }

  // ---- 矩形 / 圆角(立即模式, HTML 同名) ----
  fillRect(x: number, y: number, w: number, h: number): void {
    this._backend.fillRect(x, y, w, h, this._fill);
  }
  strokeRect(x: number, y: number, w: number, h: number): void {
    this._backend.setPen(this._stroke, this._lineWidth);
    this._backend.drawRect(x, y, w, h);
  }
  fillRoundRect(x: number, y: number, w: number, h: number, r: number): void {
    this._backend.fillRoundRect(x, y, w, h, r, this._fill);
  }
  strokeRoundRect(x: number, y: number, w: number, h: number, r: number): void {
    this._backend.setPen(this._stroke, this._lineWidth);
    this._backend.drawRoundRect(x, y, w, h, r);
  }

  // ---- 文本 / 图片(立即模式, HTML 同名) ----
  fillText(text: string, x: number, y: number): void {
    this._backend.setFont(this._fontFamily, this._fontSize);
    this._backend.drawText(x, y, text);
  }
  drawImage(img: any, x: number, y: number, w: number, h: number): void {
    this._backend.drawImage(img, x, y, w, h);
  }

  // ---- 路径(HTML 习惯: 累积当前路径, fill/stroke 提交; 经 JZPainterPath) ----
  beginPath(): void { this._path = []; }
  moveTo(x: number, y: number): void { this._path.push(['moveTo', x, y]); }
  lineTo(x: number, y: number): void { this._path.push(['lineTo', x, y]); }
  rect(x: number, y: number, w: number, h: number): void { this._path.push(['rect', x, y, w, h]); }
  closePath(): void { this._path.push(['close']); }
  arc(cx: number, cy: number, r: number, startRad: number, endRad: number): void {
    this._path.push(['arc', cx, cy, r, startRad * 180 / Math.PI, endRad * 180 / Math.PI]);
  }
  ellipse(cx: number, cy: number, rx: number, ry: number): void {
    this._path.push(['ellipse', cx - rx, cy - ry, rx * 2, ry * 2]);
  }
  quadraticCurveTo(cx: number, cy: number, x: number, y: number): void {
    this._path.push(['quad', cx, cy, x, y]);
  }
  bezierCurveTo(c1x: number, c1y: number, c2x: number, c2y: number, x: number, y: number): void {
    this._path.push(['cubic', c1x, c1y, c2x, c2y, x, y]);
  }
  fill(): void { this._backend.fillPath(this._path); }
  stroke(): void {
    this._backend.setPen(this._stroke, this._lineWidth);
    this._backend.strokePath(this._path);
  }
}

// ---------------------------------------------------------------------------
// 控件构建器
// ---------------------------------------------------------------------------

export class Widget {
  type: string;
  id?: string;
  protected _props: { [k: string]: any };
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

// JZWindow: 顶层窗口容器, 可挂子控件
export class JZWindow extends Widget {
  constructor(parent?: Widget) { super("Window", parent); }
  setTitle(t: string): this { return this.setProperty("title", t); }
  show(): void {}
  close(): void {}
}

// JZCanvas: 自定义绘制表面, 通过 setOnPaint 注册 JS 绘制回调
export class JZCanvas extends Widget {
  private _onPaint?: (canvas: JZCanvas, ctx: JZCanvasContext) => void;

  constructor(parent?: Widget) { super("Canvas", parent); }
  setWidth(w: number): this { this._props["width"] = w; return this; }
  setHeight(h: number): this { this._props["height"] = h; return this; }

  setOnPaint(cb: (canvas: JZCanvas, ctx: JZCanvasContext) => void): this {
    this._onPaint = cb;
    return this;
  }
  // 原生侧绘制阶段调用此触发 JS 绘制(下一阶段接入真实 JZPainter 作为 backend)
  paint(backend: PainterBackend): void {
    if (this._onPaint) this._onPaint(this, new JZCanvasContext(backend));
  }
}
