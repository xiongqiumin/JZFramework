// JZColor —— RGBA 颜色值类型(全局类; registColor + js_JZColor_ctor)。
//
// 构造: () / (colorEnum) / (r, g, b) / (r, g, b, a)
//   new JZColor()                 默认黑, alpha = 255
//   new JZColor(2)                颜色枚举值(见文件末尾的取值表)
//   new JZColor(255, 128, 0)      不传 a 时 alpha = 255
//
// 位置: 作为**颜色值对象**使用 —— 作为返回值(如 JZChart.seriesColor()), 也作为实参
// (JZPen/JZBrush 构造器、JZPainter.setPen/setBrush 的颜色形参、JZWidget 系的颜色接口)。
// 那些形参同时接受字符串颜色(解析见 JZJSWidgetModule.cpp 的 jzParseColor:
// '#rgb' / '#rrggbb' / '#rrggbbaa' / black white red green blue yellow cyan magenta
// gray(grey) transparent), 所以两种写法等价:
//   p.setPen(new JZPen('#ff0000', 2));                    // 字符串
//   p.setPen(new JZPen(new JZColor(255, 0, 0), 2));       // JZColor
// JZColor 本身没有 fromString 之类的解析入口。

/** @final */
declare class JZColor {
  constructor();
  constructor(color: number);
  constructor(r: number, g: number, b: number);
  constructor(r: number, g: number, b: number, a: number);

  red(): number;
  green(): number;
  blue(): number;
  alpha(): number;

  setRgb(r: number, g: number, b: number): void;
  setRgba(r: number, g: number, b: number, a: number): void;
  /** 值相等(比 rgba 四个分量) */
  equal(other: JZColor): boolean;
}

// ColorEnum 的**值**(与 C++ JZColor::ColorEnum 同序), 但**没有**挂成 JS 常量
// (只有 JZChart 挂了类型常量), 所以不能写 JZColor.Red —— 直接传数字:
//   new JZColor(2)  等价 C++ 的 JZColor(JZColor::Red)
//   0=Black 1=White 2=Red 3=Green 4=Blue 5=Yellow 6=Cyan 7=Magenta
//   8=Gray 9=LightGray 10=DarkGray 11=Orange 12=Purple 13=Transparent
//
// 未暴露(C++ 有): 指向颜色名的字符串化、调色板常量。
