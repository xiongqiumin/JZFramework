// JZMatrix2D —— 2D 仿射变换矩阵(全局值类型; JZJSWidgetModule::registMatrix
// + JZJSWidget.cpp 的 js_JZMatrix2D_ctor)。
//
// 表示: 标准 3x3 的行主序仿射矩阵(末行恒 [0,0,1]), 只维护前两行:
//   | a b c |        x' = a*x + b*y + c
//   | d e f |   =>   y' = d*x + e*y + f
// 与 GDI+ / Canvas 的 [a b c d e f] 同序。
//
// 用途: 传给 JZPainter.setTransform(m); 或用静态工厂造好再设:
//   const p = new JZPainter(this);
//   p.setTransform(JZMatrix2D.rotation(30));                      // 绕原点转 30° (屏幕顺时针为正)
//   p.setTransform(JZMatrix2D.translation(10, 20).multiplied(JZMatrix2D.scaling(2, 2)));
//
// 组合语义(与 C++ 一致, post-multiply): translate/rotate/scale 都是把新变换**右乘**到当前矩阵,
// 即"在已经变换过的坐标系内"继续变换(先 translate 到中心再 rotate = 绕中心旋转)。
//
// 值语义: 读取接口与 multiplied()/inverse() 都返回**新矩阵**, 改它不影响别人。

declare class JZMatrix2D {
  /** 单位阵 */
  constructor();
  /** 2x3 仿射部分 [a b c d e f](行主序) */
  constructor(a: number, b: number, c: number, d: number, e: number, f: number);
  /** 拷贝构造 */
  constructor(other: JZMatrix2D);

  /** 平移矩阵 */
  static translation(dx: number, dy: number): JZMatrix2D;
  /** 缩放矩阵 */
  static scaling(sx: number, sy: number): JZMatrix2D;
  /** 旋转矩阵; 角度制, 屏幕顺时针为正 */
  static rotation(deg: number): JZMatrix2D;
  /** 旋转矩阵; 弧度制 */
  static rotationRadians(rad: number): JZMatrix2D;

  /** 置为单位阵 */
  reset(): void;
  isIdentity(): boolean;

  /** 元素读取(0~2; 越界抛异常) */
  at(row: number, col: number): number;
  /** 元素写入(0~2; 越界抛异常)。末行通常保持 0,0,1 */
  setAt(row: number, col: number, value: number): void;
  /** 设置 2x3 仿射部分 [a b c d e f], 末行自动置 [0,0,1] */
  setMatrix(a: number, b: number, c: number, d: number, e: number, f: number): void;
  /** 设置完整 3x3(含末行) */
  setMatrix3(m00: number, m01: number, m02: number,
             m10: number, m11: number, m12: number,
             m20: number, m21: number, m22: number): void;

  // ---- 就地组合(右乘) ----
  translate(dx: number, dy: number): void;
  scale(sx: number, sy: number): void;
  /** 角度制, 屏幕顺时针为正 */
  rotate(deg: number): void;
  rotateRadians(rad: number): void;
  /** this = this * o (先应用 o, 再应用 this) */
  multiply(other: JZMatrix2D): void;

  // ---- 返回新矩阵 ----
  /** this * o (不改本对象) */
  multiplied(other: JZMatrix2D): JZMatrix2D;
  /** 仿射逆矩阵; 退化(行列式≈0)时返回单位阵 */
  inverse(): JZMatrix2D;

  // ---- 缩放量(x/y 轴单位向量变换后的长度) ----
  scaleX(): number;
  scaleY(): number;
}

// 未暴露(C++ 有): map(x, y, &ox, &oy) / mapSize(w, h, &ow, &oh)(C++ 的输出参数不适合 JS 直通),
// operator== / operator!=。
