// JZColor —— RGBA 颜色值类型(全局类; JZJSWidgetModule::registColor)。
//
// 位置: 只作为**读取/改色**的值对象使用(如 JZChart.seriesColor() 的返回值)。
// 颜色常量枚举(JZColor.Black / Red / ...)与 "#rrggbb" 解析入口都没有暴露到 JS ——
// 字符串形式的颜色只被 **画笔**接口接受(见 painter.d.ts 的 setPen/setBrush 与模块内的
// jzParseColor: '#rgb' / '#rrggbb' / '#rrggbbaa' / black white red green blue yellow
// cyan magenta gray(grey) transparent)。
//
// 构造: 无参(默认黑, alpha=255) —— 绑定层忽略实参(见 geometry.d.ts 顶部说明)。

declare class JZColor {
  constructor();

  red(): number;
  green(): number;
  blue(): number;
  alpha(): number;

  setRgb(r: number, g: number, b: number): void;
  setRgba(r: number, g: number, b: number, a: number): void;
}

// 未暴露(C++ 有): JZColor(ColorEnum)、指向颜色名的字符串化、调色板常量。
