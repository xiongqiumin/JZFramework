// JZPainter —— 绘制入口(全局类; JZJSWidgetModule::registPainter, 创建请求转发给 JZJSPaintScope)。
//
// 与 C++ 一致: 在 onPaint 回调里**自建**, 只在本次绘制派发期间有效(派发结束立即失效):
//   win.onPaint = (e) => {
//     const p = new JZPainter(win);
//     p.setPen(new JZPen('#ff0000', 2));            // 样式对象(推荐)
//     p.drawRect(0, 0, 100, 50);
//     p.fillRoundRect(0, 60, 100, 30, 6, '#339933');  // 填充接口自带颜色
//     p.setTransform(JZMatrix2D.rotation(15));     // 变换矩阵(见 matrix.d.ts)
//   };
//
// 两种模式(构造实参决定):
//   new JZPainter(widget): 屏幕画笔, 等价 C++ 的栈对象 `JZPainter p(this)`。
//     约束① 只能在 onPaint 派发内创建; ② 同一次绘制里每个控件只能有一份(违反即抛异常)。
//   new JZPainter(image): 离屏画笔, 自持图层; 析构或 end() 时把内容合成回 image。
//     要求 image 尺寸有效 —— 用 JZImage.create(w, h) 造图(见 image.d.ts)。
//
// 坐标系: 控件本地坐标; 变换/裁剪都作用于此。

/** @final */
declare class JZPainter {
  /** 屏幕画笔: 只能在 onPaint 回调内创建 */
  constructor(widget: JZWidget);
  /** 离屏画笔: 把绘制合成到 image(需要尺寸有效的图, 如 JZImage.create(w, h)) */
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
  /** 直接给矩阵(推荐), 见 matrix.d.ts */
  setTransform(matrix: JZMatrix2D): void;
  /** 便捷形式: 2x3 仿射部分 [a b c d e f](行主序, 与 GDI+/Canvas 同序) */
  setTransform(a: number, b: number, c: number, d: number, e: number, f: number): void;
  /** 当前变换矩阵(拷贝); 读出来改完再 setTransform 回去 */
  transform(): JZMatrix2D;
  resetTransform(): void;
  /** 矩形裁剪(本地坐标) */
  setClip(x: number, y: number, w: number, h: number): void;

  // ---- 画笔 / 画刷 / 字体 ----
  // 三种样式都收**样式对象**(推荐): JZPen / JZBrush / JZFont(见 pen.d.ts)。
  // 另外保留了"颜色字符串 + 线宽"的便捷形式(color 也接受 JZColor 对象), 给 Qt 兼容层与
  // 老代码用; 传对象时**不要**再给后面的参数(线宽/不透明度写进对象里)。
  setPen(pen: JZPen): void;
  setPen(color: string | JZColor, width?: number): void;
  setBrush(brush: JZBrush): void;
  setBrush(color: string | JZColor, opacity?: number): void;
  setFont(font: JZFont): void;
  setFont(family: string, size: number): void;

  // ---- 图元 ----
  drawLine(x1: number, y1: number, x2: number, y2: number): void;
  drawRect(x: number, y: number, w: number, h: number): void;
  /** 描边圆角矩形; ⚠ 内部**按当前 brush 填充**(要改填充色先 setBrush) */
  drawRoundRect(x: number, y: number, w: number, h: number, r: number): void;
  fillRect(x: number, y: number, w: number, h: number, color: string): void;
  fillRoundRect(x: number, y: number, w: number, h: number, r: number, color: string): void;
  drawEllipse(x: number, y: number, w: number, h: number): void;
  fillEllipse(x: number, y: number, w: number, h: number, color: string): void;
  drawText(x: number, y: number, text: string): void;
  /** img 不能为 null(绑定层会抛异常) */
  drawImage(img: JZImage, x: number, y: number, w: number, h: number): void;
}

// 未暴露(C++ 有): pen()/brush()/font() 读取(transform() 已暴露, 见 matrix.d.ts), drawArc,
// drawPath/fillPath (JZPainterPath), drawCurve, drawPixmap。
// 注: fillRect / fillRoundRect / fillEllipse 都带颜色实参(内部用自己的画刷), 所以
//     setBrush 对它们没有效果 —— 可见效果在 drawRoundRect 的内部填充与路径填充上。
