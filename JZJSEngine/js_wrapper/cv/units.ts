// cv 的**值对象** —— OpenCV.js 里的 cv.Size / cv.Rect / cv.Point / cv.Scalar / cv.Circle /
// cv.RotatedRect 这一套轻量值类型。
//
// 单独一个文件是为了不让 index.ts(核心) 与 opencvjs.ts(OpenCV.js 形状层) 相互 import ——
// 两边都要用这些类型与 normArg()。
//
// 口径: 本包**所有**要尺寸/点/矩形/颜色的参数位置, 都同时收"值对象"和"简写":
//     size    : Size | 5 | [w,h] | {w,h}
//     point   : Point | [x,y] | {x,y}
//     rect    : Rect | [x,y,w,h] | {x,y,w,h}
//     color   : Scalar | number | [b,g,r] / [b,g,r,a] | '#RRGGBB'
// normArg() 负责把值对象**降级**成后端(JSON)认的形状, index.ts 的 call() 里统一过一道 ——
// 于是每个 op 的包装不用各自写一遍参数归一化。

/** 尺寸（= OpenCV.js 的 cv.Size）。注意宽度在前, 与 cv::Size 一致（width = cols）。 */
export class Size {
  width: number;
  height: number;
  constructor(width: number, height: number) {
    this.width = width;
    this.height = height;
  }
}

/** 点（= cv.Point）。⚠ 同时支持两种读法: `p.x / p.y`（OpenCV.js 的形状）与 `p[0] / p[1]`
 *  （我们内部的简写）—— 于是 `minMaxLoc(...).maxLoc` 这类返回值两边都写得通。 */
export class Point {
  x: number;
  y: number;
  [index: number]: number;
  constructor(x: number, y: number) {
    this.x = x;
    this.y = y;
    this[0] = x;
    this[1] = y;
  }
}

/** 矩形（= cv.Rect）。x/y/width/height 同 OpenCV.js；w/h 是别名（我们自己的简写里用 w/h）。 */
export class Rect {
  x: number;
  y: number;
  width: number;
  height: number;
  constructor(x: number, y: number, width: number, height: number) {
    this.x = x;
    this.y = y;
    this.width = width;
    this.height = height;
  }
  get w(): number { return this.width; }
  get h(): number { return this.height; }
}

/** 颜色/多通道值（= cv.Scalar）。像数组一样用：`new Scalar(0,0,255)`、`s[0]`、`s.length`。 */
export class Scalar {
  [index: number]: number;
  length: number;
  constructor(...values: number[]) {
    for (let i = 0; i < values.length; i++)
      this[i] = values[i];
    this.length = values.length;
  }
}

/** 圆（= cv.Circle）。我们自己的 circle() 直接收 center + radius, 不必非包一层。 */
export class Circle {
  center: Point;
  radius: number;
  constructor(center: Point, radius: number) {
    this.center = center;
    this.radius = radius;
  }
}

/** 迭代算法的终止条件（= cv.TermCriteria）—— 目前只有 kmeans 用得上。
 *  OpenCV.js 写法：`new cv.TermCriteria(cv.TERM_CRITERIA_EPS + cv.TERM_CRITERIA_MAX_ITER, 10, 1.0)` */
export class TermCriteria {
  type: number;
  maxCount: number;
  epsilon: number;
  constructor(type = 3, maxCount = 10, epsilon = 1e-6) {   // 3 = EPS(2) | MAX_ITER(1)
    this.type = type;
    this.maxCount = maxCount;
    this.epsilon = epsilon;
  }
}

/** 带角度的矩形（= cv.RotatedRect）。angle 与 OpenCV 同义（度, 逆时针为正）。 */
export class RotatedRect {
  center: Point;
  size: Size;
  angle: number;
  constructor(center: Point, size: Size, angle: number) {
    this.center = center;
    this.size = size;
    this.angle = angle;
  }
}

/** 是不是本包的 Mat 句柄 —— 用**鸭子类型**判(不 import Mat, 免得与 index.ts 循环)。 */
export function isMatLike(v: any): boolean {
  return !!v && typeof v === 'object' && typeof v.id === 'number' && typeof v.free === 'function';
}

/** 把值对象/简写统一成后端认的形状：Size->{w,h}，Rect->{x,y,w,h}，Point->[x,y]，
 *  Scalar->数字数组，Mat->句柄 id；数组/普通对象递归处理。 */
export function normArg(v: any): any {
  if (v === null || v === undefined)
    return v;
  if (typeof v !== 'object')
    return v;
  if (isMatLike(v))
    return v.id;
  if (v instanceof Scalar) {
    const a: number[] = [];
    for (let i = 0; i < v.length; i++)
      a.push(v[i]);
    return a;
  }
  if (v instanceof Size)
    return { w: v.width, h: v.height };
  if (v instanceof Rect)
    return { x: v.x, y: v.y, w: v.width, h: v.height };
  if (v instanceof Point)
    return [v.x, v.y];
  if (v instanceof RotatedRect)
    return { center: [v.center.x, v.center.y], size: { w: v.size.width, h: v.size.height }, angle: v.angle };
  if (Array.isArray(v)) {
    const out: any[] = [];
    for (let i = 0; i < v.length; i++)
      out.push(normArg(v[i]));
    return out;
  }
  const o: Record<string, any> = {};
  for (const k of Object.keys(v))
    o[k] = normArg((v as any)[k]);
  return o;
}
