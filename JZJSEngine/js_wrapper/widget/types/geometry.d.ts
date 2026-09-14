// JZPoint / JZSize / JZRect —— 框架的**值语义**几何类型(全局类)。
//
// 运行期来源: JZJSWidgetModule::registGeometry() 注册类 + JZJSWidget.cpp 里
// jzRegistWidgetJsConstructors() 换上的 C 构造器(js_JZPoint_ctor / js_JZSize_ctor / js_JZRect_ctor)。
// 注册即挂到全局对象上(见 JZJSEngine::registClass), 所以直接用 `new JZRect(...)`,
// 不需要 import。
//
// 构造: 与 C++ 构造一致(JS 实参个数决定):
//   new JZPoint() / new JZPoint(x, y)
//   new JZSize() / new JZSize(w, h)
//   new JZRect() / new JZRect(x, y) / new JZRect(x, y, w, h)
// 参数个数不匹配抛 TypeError; 值类型构造**存一份拷贝** —— JS 侧每次拿到独立对象。
//
// 值语义: 作为返回值时也是**拷贝**(每次调用得到新的 JS 对象), 改它不影响原控件 ——
// 要改控件几何请调 JZWidget.setGeometry()。

declare class JZPoint {
  constructor();
  constructor(x: number, y: number);

  x(): number;
  y(): number;
  setX(x: number): void;
  setY(y: number): void;
  setPoint(x: number, y: number): void;
  /** x === 0 && y === 0 */
  isNull(): boolean;
}

// 未暴露(C++ 有): operator+ / operator- / operator+= / operator-=
declare class JZSize {
  constructor();
  constructor(w: number, h: number);

  width(): number;
  height(): number;
  setWidth(w: number): void;
  setHeight(h: number): void;
  /** w <= 0 || h <= 0 */
  isEmpty(): boolean;
  /** w >= 0 && h >= 0 */
  isValid(): boolean;
}

declare class JZRect {
  constructor();
  constructor(x: number, y: number);
  constructor(x: number, y: number, w: number, h: number);

  // ---- 读 ----
  x(): number;
  y(): number;
  width(): number;
  height(): number;
  /** 左边界, 等于 x() */
  left(): number;
  /** 上边界, 等于 y() */
  top(): number;
  /** 右边界(闭区间): x + width - 1 */
  right(): number;
  /** 下边界(闭区间): y + height - 1 */
  bottom(): number;
  /** 空矩形(x/y/width/height 全 0) */
  isEmpty(): boolean;

  // ---- 写 ----
  setX(x: number): void;
  setY(y: number): void;
  setWidth(w: number): void;
  setHeight(h: number): void;
  setRect(x: number, y: number, w: number, h: number): void;

  // ---- 角 / 中心 / 尺寸(按值返回拷贝) ----
  topLeft(): JZPoint;
  bottomRight(): JZPoint;
  center(): JZPoint;
  size(): JZSize;

  // ---- 判定 / 运算 ----
  contains(x: number, y: number): boolean;
  intersects(other: JZRect): boolean;
  /** 交集; 无交返回空矩形 */
  intersect(other: JZRect): JZRect;
  /** 并集的外接框 */
  united(other: JZRect): JZRect;
}

// 未暴露(C++ 有): JZRect.topRight / bottomLeft / clear / contains(JZPoint)
