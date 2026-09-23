// JZPen / JZBrush / JZFont —— 画笔样式值类型(全局类; JZJSWidgetModule::registPens
// + JZJSWidget.cpp 的 js_JZPen_ctor / js_JZBrush_ctor / js_JZFont_ctor)。
//
// 三者都只是**描述**(不含任何设备资源), 由 JZPainter 消费:
//   const p = new JZPainter(this);
//   p.setPen(new JZPen('#ff0000', 2));                     // 颜色 + 线宽
//   p.setBrush(new JZBrush(new JZColor(80, 160, 90), 128)); // 半透明填充
//   p.setFont(new JZFont('微软雅黑', 16, JZFont.Bold));
//
// 值语义: 读取接口(color()/family()/...)按值返回**拷贝** —— 改它不影响画笔里那份;
// 要改画笔状态就改完对象再 setPen/setBrush/setFont 一次。
//
// 颜色形参同时接受 JZColor 对象与 '#rgb' / '#rrggbb' / '#rrggbbaa' / 颜色名 字符串
// (与 JZPainter.setPen 的便捷形式同一套解析, 见 color.d.ts)。

/** @final */
declare class JZPen {
  /** 默认画笔(黑色, 线宽 1) */
  constructor();
  constructor(color: JZColor | string);
  constructor(color: JZColor | string, width: number);

  setColor(color: JZColor): void;
  color(): JZColor;
  /** 线宽(像素, 随变换矩阵缩放) */
  setWidth(width: number): void;
  width(): number;
}

/** @final */
declare class JZBrush {
  /** 默认画刷(黑色, 不透明) */
  constructor();
  constructor(color: JZColor | string);
  /** opacity: 0~255(缺省 255 = 不透明) */
  constructor(color: JZColor | string, opacity: number);

  setColor(color: JZColor): void;
  color(): JZColor;
  /** 0~255, 越界抛异常 */
  setOpacity(opacity: number): void;
  opacity(): number;
}

// 字体描述(仿 Qt QFont): 字体族 + 像素字号 + 风格。空字族 = 默认字体(渲染后端决定, 当前微软雅黑)。
/** @final */
declare class JZFont {
  constructor();
  /** 只给字号(等价 C++ JZFont(int size)) */
  constructor(size: number);
  constructor(family: string, size: number);
  /** style 为位或: JZFont.Normal / Bold / Italic / Underline / StrikeOut */
  constructor(family: string, size: number, style: number);

  /** 风格位常量(与 C++ JZFont::Style 同值, 挂在构造器上) */
  static readonly Normal: 0;
  static readonly Bold: 1;
  static readonly Italic: 2;
  static readonly Underline: 4;
  static readonly StrikeOut: 8;

  setFamily(family: string): void;
  family(): string;
  /** 像素字号 */
  setSize(size: number): void;
  size(): number;
  setStyle(style: number): void;
  style(): number;
  setBold(on: boolean): void;
  bold(): boolean;
  setItalic(on: boolean): void;
  italic(): boolean;
  setUnderline(on: boolean): void;
  underline(): boolean;
  setStrikeOut(on: boolean): void;
  strikeOut(): boolean;
  /** 值相等(比字体族/字号/风格位); C++ 侧就是 JZFont::operator== */
  equal(other: JZFont): boolean;

  // ---- 字体度量(等价 Qt QFontMetrics::width/height) ----
  // 由 C++ 的 JZFontMetrics 门面提供: 后端(GDI+)注册了度量引擎就是**真度量**,
  // 否则按 UTF-8 估算(ASCII ~0.55em / CJK 1em)。
  // 自绘控件排版用(替代"字符数 × 系数"的估算):
  //   const f = painter.font();
  //   painter.drawText(x + ((w - f.measureText(t)) / 2 | 0), y, t);
  /** 文本像素宽度(measureText 的别名, 语义相同) */
  measureText(text: string): number;
  textWidth(text: string): number;
  /** 单行文本行高(像素) */
  textHeight(): number;
  /** 基线以上高度(像素) */
  ascent(): number;
  /** 基线以下高度(像素) */
  descent(): number;
}

// C++ 的 operator!= 没有单独暴露 —— JS 侧写 !a.equal(b) 即可。
