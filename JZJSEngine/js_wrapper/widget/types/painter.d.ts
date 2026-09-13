// JZPainter —— 绘制入口(全局类; JZJSWidgetModule::registPainter, 创建请求转发给 JZJSPaintScope)。
//
// 与 C++ 一致: 在 onPaint 回调里**自建**, 只在本次绘制派发期间有效(派发结束立即失效):
//   win.onPaint = (e) => {
//     const p = new JZPainter(win);
//     p.setPen('#ff0000', 2);
//     p.drawRect(0, 0, 100, 50);
//     p.fillRoundRect(0, 60, 100, 30, 6, '#339933');
//   };
//
// 两种模式(构造实参决定):
//   new JZPainter(widget): 屏幕画笔, 等价 C++ 的栈对象 `JZPainter p(this)`。
//     约束① 只能在 onPaint 派发内创建; ② 同一次绘制里每个控件只能有一份(违反即抛异常)。
//   new JZPainter(image): 离屏画笔, 自持图层; 析构或 end() 时把内容合成回 image。
//     要求 image 尺寸有效(当前 JS 侧造不出这种图, 见 image.d.ts)。
//
// 坐标系: 控件本地坐标; 变换/裁剪都作用于此。颜色参数用 CSS 风格字符串
// ('#rgb' / '#rrggbb' / '#rrggbbaa' / 颜色名), 详见 color.d.ts。

declare class JZPainter {
  /** 屏幕画笔: 只能在 onPaint 回调内创建 */
  constructor(widget: JZWidget);
  /** 离屏画笔: 把绘制合成到 image(需要尺寸有效的图) */
  constructor(image: JZImage);

  /** 显式结束: 离屏模式立刻把图层合成回 image, 之后本对象失效; 屏幕模式提前补 Pop(可重复调用) */
  end(): void;

  // ---- 状态 ----
  save(): void;
  restore(): void;

  // ---- 变换(控件本地坐标系; 逐次组合) ----
  translate(dx: number, dy: number): void;
  /** 角度制, 屏幕顺时针为正 */
  rotate(deg: number): void;
  scale(sx: number, sy: number): void;
  /** 3×2 仿射矩阵 [a b c d e f] */
  setTransform(a: number, b: number, c: number, d: number, e: number, f: number): void;
  resetTransform(): void;
  /** 矩形裁剪(本地坐标) */
  setClip(x: number, y: number, w: number, h: number): void;

  // ---- 画笔 / 画刷 / 字体 ----
  setPen(color: string, width: number): void;
  setBrush(color: string): void;
  setFont(family: string, size: number): void;

  // ---- 图元 ----
  drawLine(x1: number, y1: number, x2: number, y2: number): void;
  drawRect(x: number, y: number, w: number, h: number): void;
  drawRoundRect(x: number, y: number, w: number, h: number, r: number): void;
  fillRect(x: number, y: number, w: number, h: number, color: string): void;
  fillRoundRect(x: number, y: number, w: number, h: number, r: number, color: string): void;
  drawEllipse(x: number, y: number, w: number, h: number): void;
  fillEllipse(x: number, y: number, w: number, h: number, color: string): void;
  drawText(x: number, y: number, text: string): void;
  /** img 不能为 null(绑定层会抛异常) */
  drawImage(img: JZImage, x: number, y: number, w: number, h: number): void;
}

// 未暴露(C++ 有): pen()/brush()/font()/transform() 读取, drawArc, drawPath/fillPath
// (JZPainterPath), drawCurve, drawPixmap。
// 因此 './widget' 里 PainterBackend 声明的 drawArc / strokePath / fillPath 目前**没有**
// 对应的原生实现(JZCanvasContext 的 arc()/fill()/stroke() 只能配 JS 侧实现的后端用)。
