// JZPoint / JZSize / JZRect —— 框架的**值语义**几何类型(全局类)。
//
// 运行期来源: JZJSWidgetModule::registGeometry()(JZJSEngine/JSWidget/JZJSWidgetModule.cpp)
// 用 ClassBind 把这三个类注册进引擎, 注册即挂到全局对象上(见 JZJSEngine::registClass),
// 所以直接用 `new JZRect()` / `const r: JZRect` —— 不需要 import, 也与 './widget' 里那套
// 纯 JS 构建器(Widget / JZWindow / JZCanvas)不是同一套东西。
//
// 构造: 只有无参形式可用 —— 绑定层用默认工厂 `new Class()` 并**忽略实参**(JZJSBind.h 的
// makeDefaultCreate), 所以 `new JZRect(1, 2, 3, 4)` 不报错, 但结果仍是空矩形(全 0)!
// 赋值请用 setRect()/setXxx()。
//
// 值语义: 作为返回值时是**拷贝**(每次调用得到新的 JS 对象), 改它不影响原控件 ——
// 要改控件几何请调 JZWidget.setGeometry()。

declare class JZPoint {
  constructor();

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
