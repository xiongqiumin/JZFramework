/// <reference path="../types/globals.d.ts" />
// cv 包 —— 运行期实现：把 OpenCV 的传统图像处理包成顺手的 TS API。
//
// 设计：
//   · 真干活的全在 C++ 侧（__jz.cv，JZJSCvModule）：图像是 C++ 里的一张 **句柄表**，
//     JS 只拿一个 number。于是"截屏 → 去噪 → 颜色分割 → 形态学 → 找轮廓"整条链
//     不发生像素拷贝，也没有 base64/数组来回搬。
//   · 这一层只做两件事：把参数整理成 JSON 送给 __jz.cv.op()，把失败**变成抛错**。
//   · 影像参数一律是 Mat（句柄）。除绘制类（line/rectangle/... 返回画好的新图）外，
//     所有运算都**不改输入**，返回新的 Mat ⇒ 链式处理时输入还能复用/对比。
//   · 用完请 mat.free()（或一轮结束调 releaseAll()）—— 句柄表在 C++ 侧，不会自动回收。
//
// 典型用法（视觉 agent 找红球）：
//     import { imdecode, cvtColor, inRange, morphologyEx, findContours, releaseAll } from 'cv';
//     const img = imdecode(shot);                                   // shot = computeruse.screenshot()
//     const hsv = cvtColor(img, 'BGR2HSV');
//     const mask = inRange(hsv, [0, 120, 80], [10, 255, 255]);      // 红色(低端)
//     const clean = morphologyEx(mask, 'open', { shape: 'ellipse', ksize: [5, 5] });
//     const balls = findContours(clean).filter(c => c.area > 30 && c.circularity > 0.7);
//     ...
//     releaseAll();

import type {
  MatLike, MatInfo, Size2, Point2, RectArg, ColorArg, KernelArg, MatType,
  ColorCode, Interp, BorderType, ThreshType, MorphOp, Colormap, RetrMode, ChainMethod,
  TmMethod, NormType, CmpOp, FlipHow, RotateCode,
  Contour, ComponentsResult, MinMaxLocResult, MeanStdDevResult, MomentsResult,
  MatchTemplateResult, HistResult, ThresholdResult, CvBackend,
  CartToPolarResult, PolarToCartResult, IntegralResult, DistanceTransformResult,
  FloodFillResult, KmeansResult, MinEnclosingCircleResult, FitEllipseResult, FindHomographyResult,
} from './types';

import { normArg, Circle, Point, Rect, RotatedRect, Scalar, Size, TermCriteria, isMatLike } from './units';
import { DECOMP_LU } from './consts';

// 值对象(Size/Rect/Point/Scalar/Circle/RotatedRect) 与 OpenCV.js 的常量表(cv.COLOR_* /
// cv.THRESH_* / cv.CV_* ...) 都从这里转出去 —— `import { COLOR_BGR2GRAY } from 'cv'` 成立。
export * from './units';
export * from './consts';

declare const __jz: any;
// 引擎注入的全局 console（本包不依赖 DOM/Node 类型库）
declare var console: { log(...a: any[]): void; error(...a: any[]): void; warn(...a: any[]): void };
declare function atob(data: string): string;
declare function btoa(data: string): string;
declare function isNaN(v: any): boolean;

/** cv 的错误：`code` 便于调用方按类处理，不靠错误文本判断。 */
export class CvError extends Error {
  code: 'noBackend' | 'notBuilt' | 'failed' | 'invalid' | 'released';
  constructor(code: CvError['code'], message: string) {
    super(message);
    this.name = 'CvError';
    this.code = code;
  }
}

function cvb(): CvBackend {
  if (!__jz || !__jz.cv)
    throw new CvError('noBackend',
      '[cv] 后端未加载：__jz.cv 不存在（宿主没注册 JZJSCvModule，或这个 exe 不带 JS 运行时）');
  return __jz.cv as CvBackend;
}

/** 本构建是否编入了 OpenCV。没编时其余调用都会抛 notBuilt —— 先问这一句再决定走哪条路。 */
export function available(): boolean {
  return !!(__jz && __jz.cv && __jz.cv.available());
}

/** 发一次通用运算；失败抛 CvError（不返回"看着成功的空图"）。
 *  参数统一过一遍 normArg：Mat -> 句柄 id，Size/Rect/Point/Scalar -> 后端认的简写
 *  ⇒ 所有包装函数天然同时收"OpenCV.js 的值对象"和"我们的数组/数字简写"。 */
function call(name: string, params: Record<string, unknown>): any {
  if (!available())
    throw new CvError('notBuilt', `[cv] 本构建未编入 OpenCV，${name} 不可用`);
  const r = cvb().op(name, JSON.stringify(normArg(params)));
  if (!r || r.ok !== true) {
    const msg = (r && r.error) ? String(r.error) : '未知错误';
    throw new CvError('failed', `[cv] ${name} 失败: ${msg}`);
  }
  return r;
}

function matOf(r: any): Mat {
  if (typeof r.mat !== 'number' || r.mat <= 0)
    throw new CvError('invalid', '[cv] 后端没有返回图像句柄');
  return new Mat(r.mat);
}
function matsOf(r: any): Mat[] {
  const ids: number[] = (r && r.mats) || [];
  return ids.map((id) => new Mat(id));
}

// ————————————————————————————————————————————————————————————
// Mat —— 图像句柄
// ————————————————————————————————————————————————————————————
export class Mat {
  /** 句柄 id（C++ 侧那张表的键）。**0 = 还没绑图像**（OpenCV.js 里 `new cv.Mat()` 就是这个状态，
   *  专门当 dst 出参的占位）；不要把 id 当业务数据用。 */
  id: number;
  private freed = false;
  private bytesCache: Uint8Array | null = null;

  /** 三种构造方式（= OpenCV.js 那套，也保留我们的"包句柄"用法）：
   *   · `new Mat()`                        —— 空图，等着被当 dst 写进来（`cvtColor(src, dst, code)`）
   *   · `new Mat(rows, cols, type[, fill])` —— OpenCV.js 写法：直接建一张
   *   · `new Mat(handleId)`                —— 内部：包一个已有句柄（本包的函数用） */
  constructor(rowsOrId?: number, cols?: number, type?: MatType | number, fill?: ColorArg) {
    if (cols === undefined) {
      this.id = (rowsOrId === undefined) ? 0 : rowsOrId;
      return;
    }
    const opts: Record<string, unknown> = { cols, rows: rowsOrId, type: (type === undefined) ? '8UC3' : type };
    if (fill !== undefined) opts.fill = fill;
    else opts.kind = 'zeros';
    const id = reqHandle(cvb().create(JSON.stringify(normArg(opts))));
    this.id = matOf({ mat: id }).id;
  }

  // ---- OpenCV.js 的静态构造（cv.Mat.zeros / ones / eye）----
  /** OpenCV.js 的 `cv.Mat.zeros(rows, cols, type)` */
  static zeros(rows: number, cols: number, type: MatType | number = '8UC3'): Mat { return zeros(cols, rows, type); }
  /** OpenCV.js 的 `cv.Mat.ones(rows, cols, type)` */
  static ones(rows: number, cols: number, type: MatType | number = '8UC3'): Mat { return ones(cols, rows, type); }
  /** OpenCV.js 的 `cv.Mat.eye(rows, cols, type)` —— 单位矩阵（后端没有这个 op, 这里在 JS 里拼好再送进去） */
  static eye(rows: number, cols: number, type: MatType | number = '8UC1'): Mat {
    const t = (typeof type === 'number') ? type : typeOfName(type);
    const ch = ((t >> 3) & 511) + 1;
    const arr = new Array(rows * cols * ch);
    for (let i = 0; i < arr.length; i++) arr[i] = 0;
    for (let i = 0; i < rows; i++)
      for (let c = 0; c < ch; c++)
        arr[(i * cols + i) * ch + c] = 1;
    return matFromArray(rows, cols, t, arr);
  }

  /** @internal dst 出参语义：把 another 的句柄收到自己身上（自己原来那张先放掉）。
   *  后端每个 op 都返回**新句柄**，所以"写进 dst" = 换绑句柄；dst 这个 JS 对象的身份不变。 */
  __bind(another: Mat): void {
    this.release();
    this.id = another.id;
    this.freed = false;
    this.bytesCache = null;
    another.id = 0;                 // 句柄归 this 了
    another.freed = true;
    another.bytesCache = null;
  }

  private release(): void {
    if (!this.freed && this.id > 0)
      cvb().release(this.id);
    this.id = 0;
    this.freed = false;
    this.bytesCache = null;
  }

  /** 图像信息（宽/高/通道/类型…） */
  get info(): MatInfo {
    if (this.freed || this.id <= 0)
      throw new CvError('released', '[cv] 这张图没有绑定图像（`new Mat()` 之后要当 dst 用，或已经 free 过了）');
    const i: any = cvb().info(this.id);
    if (i && i.error) throw new CvError('released', `[cv] ${i.error}`);
    return i as MatInfo;
  }
  get cols(): number { return this.info.cols; }
  get rows(): number { return this.info.rows; }
  /** 通道数（OpenCV.js：`mat.channels()` —— 是**方法**） */
  channels(): number { return this.info.channels; }
  /** 深度：CV_8U=0 / CV_8S=1 / CV_16U=2 / CV_16S=3 / CV_32S=4 / CV_32F=5 / CV_64F=6（方法） */
  depth(): number { return this.info.depth; }
  /** OpenCV 的整型类型（CV_8UC3 = 16 …，方法）：可直接回传给 zeros/convertTo/Mat 构造 */
  type(): number { return this.info.type; }
  /** 类型名（**本包扩展**：OpenCV.js 没有这一项） */
  get typeName(): string { return this.info.typeName; }
  /** 空图？（方法） */
  empty(): boolean { return this.info.empty; }
  /** 像素总数（不含通道，方法） */
  total(): number { return this.info.total; }
  isContinuous(): boolean { return this.info.continuous; }
  /** 每像素字节数 / 每通道字节数（方法） */
  elemSize(): number { return this.info.elemSize; }
  elemSize1(): number { const ch = this.info.channels; return ch ? this.info.elemSize / ch : this.info.elemSize; }
  /** 行字节数（OpenCV.js 里 `step` 是**属性**） */
  get step(): number { return this.info.cols * this.info.elemSize; }
  /** 每通道的行字节数（方法） */
  step1(): number { return this.elemSize1(); }
  /** [rows, cols]（OpenCV.js 里 `matSize` 是**属性**） */
  get matSize(): [number, number] { const i = this.info; return [i.rows, i.cols]; }
  /** 尺寸对象（OpenCV.js 的 `mat.size()` 回 cv.Size） */
  size(): Size { const i = this.info; return new Size(i.cols, i.rows); }

  /** 深拷贝一份（之后可以各自 free） */
  clone(): Mat { return new Mat(cvb().clone(this.id)); }

  /** 释放这个句柄（C++ 侧那份像素也就没了）。之后再用会抛 released。 */
  free(): void { this.release(); }
  /** OpenCV.js 的写法：`mat.delete()`（与 free() 同义；对 `new Mat()` 的空图是空操作） */
  delete(): void { this.release(); }
  /** OpenCV.js 的 `mat.isDeleted()` */
  isDeleted(): boolean { return this.id <= 0; }

  /** 取一个像素：[b,g,r] / [gray] … （类型不同长度不同）；越界返回 null */
  pixel(x: number, y: number): number[] | null {
    return cvb().pixel(this.id, x, y);
  }
  /** 写一个像素（改的是这张图本身；少量采样/打点用，批量请用掩膜运算） */
  setPixel(x: number, y: number, values: number[] | number): boolean {
    const v = typeof values === 'number' ? [values] : values;
    return cvb().setPixel(this.id, x, y, JSON.stringify(v));
  }

  // ---- 原始字节（OpenCV.js 的 mat.data[8U|8S|16U|16S|32S|32F|64F]）----
  /** 原始字节的 base64（8U；非 8U 会先转 8U）—— 存下来/传给 fromData() 用 */
  /** 原始字节的 base64（按真实类型；配合 `fromData(b64, {rows, cols, type})` 能原样还原） */
  dataB64(): string { return cvb().data(this.id); }

  private bytes(): Uint8Array {
    if (!this.bytesCache)
      this.bytesCache = b64ToBytes(this.dataB64());
    return this.bytesCache;
  }
  /** 原始字节按指定 Ctor 重新解释（= OpenCV.js 的 data32F/data64F… 那组视图） */
  private typed(Ctor: any): any {
    const b = this.bytes();
    const n = Math.floor(b.length / Ctor.BYTES_PER_ELEMENT);
    return new Ctor(b.buffer.slice(0, n * Ctor.BYTES_PER_ELEMENT), 0, n);
  }
  /** 原始字节（**快照**，按真实类型排布：32F 就是 4 字节/通道的 float 位模式）。
   *  ⚠ OpenCV.js 的 mat.data 是 WASM 堆视图、改了就生效；我们后端在 C++ 侧, 只能给一份拷贝 ——
   *  改它**不会**写回图像, 要写回请用 fromData() / setPixel()。 */
  get data(): Uint8Array { return this.bytes(); }
  get data8U(): Uint8Array { return this.bytes(); }
  get data8S(): Int8Array { return this.typed(Int8Array); }
  get data16U(): Uint16Array { return this.typed(Uint16Array); }
  get data16S(): Int16Array { return this.typed(Int16Array); }
  get data32S(): Int32Array { return this.typed(Int32Array); }
  get data32F(): Float32Array { return this.typed(Float32Array); }
  get data64F(): Float64Array { return this.typed(Float64Array); }

  /** 第 (row,col) 个像素的第 0 个通道（OpenCV.js 的 ucharAt/floatAt/intAt/doubleAt 同理） */
  ucharAt(row: number, col: number): number { const p = this.pixel(col, row); return p ? p[0] : 0; }
  floatAt(row: number, col: number): number { return this.ucharAt(row, col); }
  intAt(row: number, col: number): number { return this.ucharAt(row, col); }
  doubleAt(row: number, col: number): number { return this.ucharAt(row, col); }
  charAt(row: number, col: number): number { return this.ucharAt(row, col); }
  shortAt(row: number, col: number): number { return this.ucharAt(row, col); }
  ushortAt(row: number, col: number): number { return this.ucharAt(row, col); }

  /** 像素"指针"（OpenCV.js 里 `p[0]` 能读能写）。⚠ 我们每次读写都走 pixel/setPixel, 不是真指针 */
  private ptrAt(row: number, col: number): any {
    const self = this;
    const ch = this.channels();
    const o: any = { length: ch };
    for (let i = 0; i < ch; i++) {
      Object.defineProperty(o, i, {
        enumerable: true,
        get() { const p = self.pixel(col, row); return p ? p[i] : 0; },
        set(v: number) {
          const p = self.pixel(col, row) || [];
          p[i] = v;
          self.setPixel(col, row, p);
        },
      });
    }
    return o;
  }
  ucharPtr(row: number, col = 0): any { return this.ptrAt(row, col); }
  floatPtr(row: number, col = 0): any { return this.ptrAt(row, col); }
  intPtr(row: number, col = 0): any { return this.ptrAt(row, col); }
  doublePtr(row: number, col = 0): any { return this.ptrAt(row, col); }
  ptr(row: number, col = 0): any { return this.ptrAt(row, col); }
  charPtr(row: number, col = 0): any { return this.ptrAt(row, col); }
  shortPtr(row: number, col = 0): any { return this.ptrAt(row, col); }
  ushortPtr(row: number, col = 0): any { return this.ptrAt(row, col); }

  // ---- 子图 / 拷贝 / 转换 / 填充 ----
  /** 取一块子图（⚠ 我们这边是**拷贝**: 改写子图不影响原图, 与 OpenCV.js 的视图语义不同） */
  roi(rect: RectArg): Mat { return crop(this, rect); }
  /** 第 i 行（拷贝） */
  row(i: number): Mat { return crop(this, 0, i, this.cols, 1); }
  /** 第 i 列（拷贝） */
  col(i: number): Mat { return crop(this, i, 0, 1, this.rows); }
  rowRange(start: number, end: number): Mat { return crop(this, 0, start, this.cols, end - start); }
  colRange(start: number, end: number): Mat { return crop(this, start, 0, end - start, this.rows); }
  /** 把内容拷进 dst（dst 的身份保留；OpenCV.js：`src.copyTo(dst)` 无返回） */
  copyTo(dst: Mat): void { dst.__bind(this.clone()); }
  /** 转类型/缩放。OpenCV.js 形状：`src.convertTo(dst, rtype[, alpha, beta])` —— 写进 dst, **无返回**；
   *  不给 dst 时（本包扩展）返回新图。 */
  convertTo(dst: Mat, rtype?: MatType, alpha?: number, beta?: number): void;
  convertTo(rtype?: MatType, alpha?: number, beta?: number): Mat;
  convertTo(a?: Mat | MatType, b?: MatType, c = 1, d = 0): any {
    if (a instanceof Mat) {
      a.__bind(convertTo(this, (b === undefined) ? this.typeName : b, c, d));
      return;
    }
    return convertTo(this, a as MatType, (b === undefined) ? c : (b as any), d);
  }
  /** OpenCV.js 的 `mat.t()`：转置 —— 回**新 Mat**（与 OpenCV.js 一致, 不改自己） */
  t(): Mat { return transpose(this); }

  // ---- 矩阵算子（OpenCV.js 的 Mat.mul / dot / inv / diag）----
  /** OpenCV.js 的 `mat.mul(m[, scale])`：逐元素相乘 —— `A.mul(B, k)` = A∘B·k（尺寸类型要一致）→ 新 Mat */
  mul(m: Mat, scale = 1): Mat { return multiply(this, m, scale); }
  /** OpenCV.js 的 `mat.dot(m)`：点积（尺寸/类型必须一致, 按行优先当向量看）→ number */
  dot(m: Mat): number { return call('dot', { a: this, b: m }).value as number; }
  /** OpenCV.js 的 `mat.inv([method])`：矩阵求逆（默认 DECOMP_LU；奇异时 OpenCV 给全 0 阵, 与 C++ 一致） */
  inv(method: number = DECOMP_LU): Mat { return matOf(call('invert', { src: this, method })); }
  /** OpenCV.js 的 `mat.diag([d])`：抽一条对角线 —— d=0 主对角, d>0 主对角**下方**, d<0 **上方** → 新 Mat */
  diag(d = 0): Mat { return matOf(call('diag', { src: this, d })); }
  /** 整张填成某个值（⚠ 不支持 mask, 与 OpenCV.js 的 setTo(value, mask) 不同） */
  setTo(value: ColorArg): void { this.__bind(full(this.cols, this.rows, this.typeName, value)); }
  /** 重新分配成 rows×cols×type（与 OpenCV.js 一样是"够用就不动"的近似: 这里一律重分配） */
  create(rows: number, cols: number, type: MatType | number): void { this.__bind(zeros(cols, rows, type)); }

  /** 存到磁盘（按扩展名定格式：.png/.jpg/.bmp/...） */
  save(path: string): boolean { return cvb().imwrite(this.id, path); }
  /** 编码成 dataURL（默认为 PNG）—— 可直接交给需要图片的地方 */
  encode(ext = 'png'): string { return cvb().imencode(this.id, ext); }

  toString(): string { return `Mat#${this.id}`; }
}

// ————————————————————————————————————————————————————————————
// 创建 / 存取 / 生命周期
// ————————————————————————————————————————————————————————————

/** 全 0 图像 */
export function zeros(cols: number, rows: number, type: MatType = '8UC3'): Mat {
  return matOf({ mat: reqHandle(cvb().create(JSON.stringify({ cols, rows, type, kind: 'zeros' }))) });
}
/** 全 1 图像 */
export function ones(cols: number, rows: number, type: MatType = '8UC3'): Mat {
  return matOf({ mat: reqHandle(cvb().create(JSON.stringify({ cols, rows, type, kind: 'ones' }))) });
}
/** 用某个颜色填满（例：白色底 full(100, 100, '8UC3', 255)） */
export function full(cols: number, rows: number, type: MatType, fill: ColorArg): Mat {
  return matOf({ mat: reqHandle(cvb().create(JSON.stringify({ cols, rows, type, fill }))) });
}
function reqHandle(id: number): number {
  if (!available())
    throw new CvError('notBuilt', '[cv] 本构建未编入 OpenCV（这个 exe 没接 OpenCV，换个宿主或让构建打开它）');
  if (!id) throw new CvError('invalid', '[cv] 创建/读取图像失败（尺寸、类型或路径不对？）');
  return id;
}

/** 从原始像素字节恢复一张图（b64 来自 mat.dataB64()） */
export function fromData(dataB64: string, opts: { cols: number; rows: number; type?: MatType }): Mat {
  return matOf({ mat: reqHandle(cvb().fromData(dataB64, JSON.stringify(opts || {}))) });
}

/** 读磁盘图片 */
export function imread(path: string): Mat {
  return matOf({ mat: reqHandle(cvb().imread(path)) });
}
/** 从 base64 / dataURL（PNG/JPEG/...）解出一张图 —— 容器里拿截图的第一站 */
export function imdecode(data: string): Mat {
  return matOf({ mat: reqHandle(cvb().imdecode(data)) });
}
/** 编码成 dataURL（默认 PNG；也认 'jpg'/'bmp'/'webp'）—— 与 mat.encode() 同一个口 */
export function imencode(src: Mat, ext = 'png'): string {
  return cvb().imencode(src.id, ext);
}

/** 现在挂着几个句柄（看看有没有忘了 free） */
export function count(): number { return cvb().count(); }
/** 释放全部句柄（一轮处理结束时调一次最省心） */
export function releaseAll(): number { return cvb().releaseAll(); }
/** 支持的操作名（自检/探活用） */
export function ops(): string[] { return cvb().ops(); }
/** 通用出口：op('gaussianBlur', { src: m.id, ksize: [5,5] }) —— 新加的 C++ 侧能力不用等本包更新 */
export function op(name: string, params: Record<string, unknown>): any { return call(name, params); }

// ————————————————————————————————————————————————————————————
// 颜色转换 / 通道
// ————————————————————————————————————————————————————————————

/** 颜色空间转换，如 'BGR2GRAY' / 'BGR2HSV' / 'GRAY2BGR' / 'BGR2RGB' */
export function cvtColor(src: Mat, code: ColorCode): Mat {
  return matOf(call('cvtColor', { src: src.id, code }));
}
/** 分量范围筛选（颜色分割主力）：lo=[0,120,80], hi=[10,255,255] ⇒ 单通道掩膜(255/0) */
export function inRange(src: Mat, lower: ColorArg, upper: ColorArg): Mat {
  return matOf(call('inRange', { src: src.id, lower, upper }));
}
/** 类型/线性变换：dst = src * alpha + beta（改深度/缩放范围用） */
export function convertTo(src: Mat, type: MatType, alpha = 1, beta = 0): Mat {
  return matOf(call('convertTo', { src: src.id, ddepth: type, alpha, beta }));
}
/** 拆通道（3 通道 → 3 张单通道） */
export function split(src: Mat, channels?: number[]): Mat[] {
  return matsOf(call('split', { src: src.id, channels }));
}
/** 合并通道 */
export function merge(mats: Mat[]): Mat {
  return matOf(call('merge', { mats: mats.map((m) => m.id) }));
}
/** 查表映射（256 项起，如提亮/二值化/伪彩） */
export function lut(src: Mat, table: number[]): Mat {
  return matOf(call('lut', { src: src.id, lut: table }));
}
/** 伪彩色（把灰度/单通道映射成彩色，如 'jet' / 'turbo' / 'viridis'） */
export function applyColorMap(src: Mat, colormap: Colormap = 'jet'): Mat {
  return matOf(call('applyColorMap', { src: src.id, colormap }));
}
/** 直方图均衡（多通道会先转灰度） */
export function equalizeHist(src: Mat): Mat {
  return matOf(call('equalizeHist', { src: src.id }));
}
/** 自适应直方图均衡（CLAHE；局部对比度，比全局均匀化更适合"暗角落里的目标"） */
export function clahe(src: Mat, clipLimit = 40, tileGridSize: Size2 = [8, 8]): Mat {
  return matOf(call('clahe', { src: src.id, clipLimit, tileGridSize }));
}

// ————————————————————————————————————————————————————————————
// 阈值
// ————————————————————————————————————————————————————————————

/** 阈值化。`type` 可写 'binary' / 'binaryInv' / 'otsu' 等；返回的 thresh 是实际用的那个。 */
export function threshold(src: Mat, thresh = 0, maxval = 255, type: ThreshType = 'binary'): ThresholdResult {
  const r = call('threshold', { src: src.id, thresh, maxval, type });
  return { mat: matOf(r), thresh: r.thresh as number };
}
/** 自适应阈值（光照不均时比全局阈值稳） */
export function adaptiveThreshold(src: Mat, maxValue = 255, method: 'mean' | 'gaussian' = 'mean',
                                  type: ThreshType = 'binary', blockSize = 11, C = 2): Mat {
  return matOf(call('adaptiveThreshold', { src: src.id, maxValue, method, type, blockSize, C }));
}

// ————————————————————————————————————————————————————————————
// 滤波
// ————————————————————————————————————————————————————————————

/** 均值滤波 */
export function blur(src: Mat, ksize: Size2 = 3, opts?: { borderType?: BorderType }): Mat {
  return matOf(call('blur', { src: src.id, ksize, ...(opts || {}) }));
}
export function boxFilter(src: Mat, ksize: Size2 = 3, normalize = true, opts?: { borderType?: BorderType }): Mat {
  return matOf(call('boxFilter', { src: src.id, ksize, normalize, ...(opts || {}) }));
}
/** 高斯滤波（去噪保边最常用） */
export function gaussianBlur(src: Mat, ksize: Size2 = 5, sigmaX = 0,
                             opts?: { sigmaY?: number; borderType?: BorderType }): Mat {
  return matOf(call('gaussianBlur', { src: src.id, ksize, sigmaX, ...(opts || {}) }));
}
/** 中值滤波（椒盐噪声；核必须奇数，传偶数会自动 +1） */
export function medianBlur(src: Mat, ksize = 5): Mat {
  return matOf(call('medianBlur', { src: src.id, ksize }));
}
/** 双边滤波（保边去噪，慢一些） */
export function bilateralFilter(src: Mat, d = 5, sigmaColor = 50, sigmaSpace = 50): Mat {
  return matOf(call('bilateralFilter', { src: src.id, d, sigmaColor, sigmaSpace }));
}
/** 自定义卷积核（锐化/拉普拉斯等） */
export function filter2D(src: Mat, kernel: KernelArg, opts?: { delta?: number; borderType?: BorderType }): Mat {
  return matOf(call('filter2D', { src: src.id, kernel, ...(opts || {}) }));
}
/** Sobel 梯度（dx/dy 要哪个方向给 1） */
export function sobel(src: Mat, dx = 1, dy = 0, ksize = 3, opts?: { ddepth?: MatType; scale?: number; delta?: number }): Mat {
  return matOf(call('sobel', { src: src.id, dx, dy, ksize, ...(opts || {}) }));
}
export function scharr(src: Mat, dx = 1, dy = 0, opts?: { ddepth?: MatType; scale?: number }): Mat {
  return matOf(call('scharr', { src: src.id, dx, dy, ...(opts || {}) }));
}
export function laplacian(src: Mat, ksize = 1, opts?: { ddepth?: MatType; scale?: number }): Mat {
  return matOf(call('laplacian', { src: src.id, ksize, ...(opts || {}) }));
}
/** Canny 边缘（多通道会先转灰度） */
export function canny(src: Mat, threshold1 = 100, threshold2 = 200, opts?: { apertureSize?: number; l2gradient?: boolean }): Mat {
  return matOf(call('canny', { src: src.id, threshold1, threshold2, ...(opts || {}) }));
}

// ————————————————————————————————————————————————————————————
// 形态学
// ————————————————————————————————————————————————————————————

/** 结构元素（拿到的是一个 Mat：可 imwrite 出来看，也能当 filter2D 的核） */
export function getStructuringElement(shape: 'rect' | 'ellipse' | 'cross' = 'rect', ksize: Size2 = 3): Mat {
  return matOf(call('getStructuringElement', { shape, ksize }));
}
/** 腐蚀（白色区域变瘦：去毛刺、断开细连接） */
export function erode(src: Mat, kernel?: KernelArg, iterations = 1, opts?: { borderType?: BorderType }): Mat {
  return matOf(call('erode', { src: src.id, kernel, iterations, ...(opts || {}) }));
}
/** 膨胀（白色区域变胖：补洞、连断口） */
export function dilate(src: Mat, kernel?: KernelArg, iterations = 1, opts?: { borderType?: BorderType }): Mat {
  return matOf(call('dilate', { src: src.id, kernel, iterations, ...(opts || {}) }));
}
/** 形态学组合：'open'(先腐后膨,去噪点) / 'close'(先膨后腐,补洞) / 'gradient' / 'tophat' / 'blackhat' */
export function morphologyEx(src: Mat, op: MorphOp, kernel?: KernelArg, iterations = 1,
                            opts?: { borderType?: BorderType }): Mat {
  return matOf(call('morphologyEx', { src: src.id, op, kernel, iterations, ...(opts || {}) }));
}
/** 连通域（"最大那块在哪"用它；stats/centroids 直接给了几何信息） */
export function connectedComponents(src: Mat, connectivity: 4 | 8 = 8): ComponentsResult {
  const r = call('connectedComponents', { src: src.id, connectivity });
  return { labels: matOf(r), count: r.count as number, stats: r.stats as number[][], centroids: r.centroids as number[][] };
}

// ————————————————————————————————————————————————————————————
// 几何变换
// ————————————————————————————————————————————————————————————

/** 缩放：给 size=[宽,高]；或给 fx/fy 比例 */
export function resize(src: Mat, sizeOrOpts: Size2 | { fx: number; fy: number; interpolation?: Interp },
                       interpolation: Interp = 'linear'): Mat {
  const p: any = { src: src.id };
  if (typeof sizeOrOpts === 'object' && !Array.isArray(sizeOrOpts) && ('fx' in sizeOrOpts || 'fy' in sizeOrOpts)) {
    Object.assign(p, sizeOrOpts);
    if (!p.interpolation) p.interpolation = interpolation;
  } else {
    p.size = sizeOrOpts;
    p.interpolation = interpolation;
  }
  return matOf(call('resize', p));
}
/** 裁剪（越界会自动裁到图内） */
export function crop(src: Mat, rect: RectArg): Mat;
export function crop(src: Mat, x: number, y: number, w: number, h: number): Mat;
export function crop(src: Mat, a: any, y?: number, w?: number, h?: number): Mat {
  const p: any = { src: src.id };
  if (typeof a === 'number') { p.x = a; p.y = y; p.w = w; p.h = h; }
  else p.rect = a;
  return matOf(call('crop', p));
}
/** 翻转：'horizontal' | 'vertical' | 'both'（也可直接给 OpenCV 的 flipCode：0=上下, >0=左右, <0=都翻） */
export function flip(src: Mat, how: FlipHow | number = 'horizontal'): Mat {
  const how2: any = (typeof how === 'number') ? (how === 0 ? 'vertical' : (how < 0 ? 'both' : 'horizontal')) : how;
  return matOf(call('flip', { src: src.id, how: how2 }));
}
/** 旋转 90/180/270（默认顺时针 90） */
export function rotate(src: Mat, code: RotateCode = 'ROTATE_90_CLOCKWISE'): Mat {
  return matOf(call('rotate', { src: src.id, code }));
}
/** 转置（行列互换） */
export function transpose(src: Mat): Mat {
  return matOf(call('transpose', { src: src.id }));
}
/** 仿射变换（M 是 2×3；一般先用 getRotationMatrix2D 生成） */
export function warpAffine(src: Mat, M: number[][], size?: Size2, opts?: { interpolation?: Interp; borderType?: BorderType }): Mat {
  return matOf(call('warpAffine', { src: src.id, M, size, ...(opts || {}) }));
}
/** 透视变换（H 是 3×3；一般先用 getPerspectiveTransform 生成） */
export function warpPerspective(src: Mat, H: number[][], size?: Size2, opts?: { interpolation?: Interp; borderType?: BorderType }): Mat {
  return matOf(call('warpPerspective', { src: src.id, H, size, ...(opts || {}) }));
}
/** 绕某点旋转+缩放的 2×3 矩阵 */
export function getRotationMatrix2D(center: Point2, angle: number, scale = 1): Mat {
  return matOf(call('getRotationMatrix2D', { center, angle, scale }));
}
/** 4 点 -> 4 点的 3×3 透视矩阵（矫正斜拍/梯形） */
export function getPerspectiveTransform(srcPts: Point2[], dstPts: Point2[]): Mat {
  return matOf(call('getPerspectiveTransform', { src: srcPts, dst: dstPts }));
}
/** 加边框 */
export function copyMakeBorder(src: Mat, top = 0, bottom = 0, left = 0, right = 0,
                               opts?: { borderType?: BorderType; value?: ColorArg }): Mat {
  return matOf(call('copyMakeBorder', { src: src.id, top, bottom, left, right, ...(opts || {}) }));
}
/** 横向拼接 */
export function hconcat(mats: Mat[]): Mat {
  return matOf(call('hconcat', { mats: mats.map((m) => m.id) }));
}
/** 纵向拼接 */
export function vconcat(mats: Mat[]): Mat {
  return matOf(call('vconcat', { mats: mats.map((m) => m.id) }));
}

// ————————————————————————————————————————————————————————————
// 算术 / 位运算
// ————————————————————————————————————————————————————————————
export function add(a: Mat, b: Mat): Mat { return matOf(call('add', { a: a.id, b: b.id })); }
export function subtract(a: Mat, b: Mat): Mat { return matOf(call('subtract', { a: a.id, b: b.id })); }
/** 逐元素乘（scale 可选） */
export function multiply(a: Mat, b: Mat, scale = 1): Mat { return matOf(call('multiply', { a: a.id, b: b.id, scale })); }
export function divide(a: Mat, b: Mat, scale = 1): Mat { return matOf(call('divide', { a: a.id, b: b.id, scale })); }
/** 绝对差（帧间差分找运动目标） */
export function absdiff(a: Mat, b: Mat): Mat { return matOf(call('absdiff', { a: a.id, b: b.id })); }
export function bitwiseAnd(a: Mat, b: Mat): Mat { return matOf(call('bitwiseAnd', { a: a.id, b: b.id })); }
export function bitwiseOr(a: Mat, b: Mat): Mat { return matOf(call('bitwiseOr', { a: a.id, b: b.id })); }
export function bitwiseXor(a: Mat, b: Mat): Mat { return matOf(call('bitwiseXor', { a: a.id, b: b.id })); }
/** 取反（掩膜反相很方便） */
export function bitwiseNot(src: Mat): Mat { return matOf(call('bitwiseNot', { src: src.id })); }
/** 加权混合：a*alpha + b*beta + gamma（做叠加/淡化） */
export function addWeighted(a: Mat, alpha: number, b: Mat, beta: number, gamma = 0): Mat {
  return matOf(call('addWeighted', { a: a.id, alpha, b: b.id, beta, gamma }));
}
/** 比较：与另一张图或一个值比，产出 255/0 掩膜（cmpop: 'gt'/'lt'/'eq'/...） */
export function compare(a: Mat, bOrValue: Mat | ColorArg, cmpop: CmpOp = 'eq'): Mat {
  const p: any = { a: a.id, cmpop };
  if (bOrValue instanceof Mat) p.b = bOrValue.id; else p.value = bOrValue;
  return matOf(call('compare', p));
}
export function min(a: Mat, b: Mat): Mat { return matOf(call('min', { a: a.id, b: b.id })); }
export function max(a: Mat, b: Mat): Mat { return matOf(call('max', { a: a.id, b: b.id })); }
/** 归一化（默认拉到 0..255，便于 imwrite 看结果） */
export function normalize(src: Mat, alpha = 0, beta = 255, normType: NormType = 'minmax'): Mat {
  return matOf(call('normalize', { src: src.id, alpha, beta, normType }));
}

// ————————————————————————————————————————————————————————————
// 统计
// ————————————————————————————————————————————————————————————

/** 均值/标准差（逐通道） */
export function meanStdDev(src: Mat, mask?: Mat): MeanStdDevResult {
  const r = call('meanStdDev', mask ? { src: src.id, mask: mask.id } : { src: src.id });
  return { mean: r.mean as number[], stddev: r.stddev as number[] };
}
/** 最小/最大值及位置 —— "最亮的点在哪儿"这类判断 */
export function minMaxLoc(src: Mat, mask?: Mat): MinMaxLocResult {
  const r = call('minMaxLoc', mask ? { src: src.id, mask: mask.id } : { src: src.id });
  return { minVal: r.minVal, maxVal: r.maxVal, minLoc: r.minLoc, maxLoc: r.maxLoc };
}
/** 非零像素数（掩膜"有多大"）。
 *  ⚠ 只吃**单通道**（OpenCV 的约束）：彩色图先 `cvtColor(img, 'BGR2GRAY')`，否则后端会抛错。 */
export function countNonZero(src: Mat): number { return call('countNonZero', { src: src.id }).value as number; }
/** 逐通道求和 */
export function sum(src: Mat): number[] { return call('sum', { src: src.id }).value as number[]; }
/** OpenCV.js 的 `cv.mean(src[, mask])` -> Scalar（各通道均值, 不足 4 个补 0） */
export function mean(src: Mat, mask?: Mat): Scalar {
  const a = call('mean', { src: src.id, mask }).value as number[];
  return new Scalar(a[0] || 0, a[1] || 0, a[2] || 0, a[3] || 0);
}
/** 范数（两图相似度：norm(a,b,'l2') 越小越像） */
export function norm(a: Mat, b?: Mat, normType: NormType = 'l2'): number {
  const p: any = { a: a.id, normType };
  if (b) p.b = b.id;
  return call('norm', p).value as number;
}

// ————————————————————————————————————————————————————————————
// 轮廓 / 形状 / 匹配
// ————————————————————————————————————————————————————————————

/** 找轮廓。输入的应该是单通道掩膜（多通道会先转灰度）。
 *  每项已经算好 area / length / circularity / boundingRect / center / hull / approx —— 直接筛、直接点。
 *  ⚠ 轮廓是**普通 JS 对象**（不像 OpenCV.js 那样是 Mat），所以不占 C++ 句柄、不用 free；
 *    为了跟 OpenCV.js 的代码长得一样，每项都挂了一个空的 `delete()`（调用无害）。 */
export function findContours(src: Mat, opts?: { mode?: RetrMode; method?: ChainMethod; approxEpsilon?: number }): Contour[] {
  const arr = (call('findContours', { src: src.id, ...(opts || {}) }).value || []) as any[];
  for (const c of arr)
    if (typeof c.delete !== 'function')
      c.delete = function () { };      // 对齐 OpenCV.js: `cnt.delete()` 写了也不报错(本来就没占有句柄)
  return arr as Contour[];
}
/** 找轮廓（**OpenCV.js 形状**）：结果填进 `contours`（MatVector，每项是**轮廓 Mat** 的句柄 ——
 *  点活在 C++ 里, 一个点都不序列化）, 层次结构填进 `hierarchy`（Mat, Nx1 CV_32SC4）。
 *  派生量按需再问：contourArea / boundingRect / convexHull / approxPolyDP ...（与 OpenCV.js 一样）。
 *  实测比"一次把派生量全回"的 findContours 快一个数量级（那版 65 条轮廓要 69KB JSON）。 */
export function findContoursTo(src: Mat, contours: MatVector, hierarchy?: Mat,
                              mode?: RetrMode, method?: ChainMethod): void {
  const p: any = { src: src.id };
  if (mode !== undefined) p.mode = mode;
  if (method !== undefined) p.method = method;
  const r = call('findContoursMats', p);
  const ids: number[] = r.mats || [];
  for (let i = 0; i < ids.length; i++)
    contours.push_back(new Mat(ids[i]));
  if (hierarchy instanceof Mat && r.hierarchy)
    hierarchy.__bind(new Mat(r.hierarchy));
}
/** 图像矩（含 7 个 Hu 不变矩） */
export function moments(src: Mat, binary = false): MomentsResult {
  const r = call('moments', { src: src.id, binary });
  return r as MomentsResult;
}
/** 模板匹配；返回分数图 + 极值位置。用 'sqdiff*' 时**最小值**才是最佳匹配，其余用最大值。 */
export function matchTemplate(src: Mat, templ: Mat, method: TmMethod = 'ccoeffNormed'): MatchTemplateResult {
  const r = call('matchTemplate', { src: src.id, templ: templ.id, method });
  return { mat: matOf(r), minVal: r.minVal, maxVal: r.maxVal, minLoc: r.minLoc, maxLoc: r.maxLoc };
}
/** 一维直方图（counts 直接可用；mat 是 1×bins 的 CV_32F，可 normalize + 画出来） */
export function calcHist(src: Mat, opts?: { channel?: number; bins?: number; min?: number; max?: number }): HistResult {
  const r = call('calcHist', { src: src.id, ...(opts || {}) });
  return { mat: matOf(r), counts: r.counts as number[] };
}

// ————————————————————————————————————————————————————————————
// 绘制（返回**画好的新图**，不改输入；用于可视化/调试标注）
// ————————————————————————————————————————————————————————————

export function line(src: Mat, pt1: Point2, pt2: Point2, color: ColorArg = [0, 0, 255],
                     opts?: { thickness?: number; lineType?: string | number }): Mat {
  return matOf(call('line', { src: src.id, pt1, pt2, color, ...(opts || {}) }));
}
export function rectangle(src: Mat, rect: RectArg, color: ColorArg = [0, 0, 255],
                          opts?: { thickness?: number; lineType?: string | number }): Mat {
  return matOf(call('rectangle', { src: src.id, rect, color, ...(opts || {}) }));
}
export function circle(src: Mat, center: Point2, radius = 5, color: ColorArg = [0, 0, 255],
                       opts?: { thickness?: number; lineType?: string | number }): Mat {
  return matOf(call('circle', { src: src.id, center, radius, color, ...(opts || {}) }));
}
export function ellipse(src: Mat, center: Point2, axes: Size2 = [10, 10], opts?: {
  angle?: number; startAngle?: number; endAngle?: number; color?: ColorArg; thickness?: number;
}): Mat {
  return matOf(call('ellipse', { src: src.id, center, axes, ...(opts || {}) }));
}
export function polylines(src: Mat, pts: Point2[], color: ColorArg = [0, 0, 255],
                          opts?: { closed?: boolean; thickness?: number }): Mat {
  return matOf(call('polylines', { src: src.id, pts, color, ...(opts || {}) }));
}
/** 写字。⚠ OpenCV 内置字体只有 ASCII：中文会画成 '???'。要中文请用控件侧的 JZPainter。 */
export function putText(src: Mat, text: string, org: Point2, opts?: {
  fontFace?: string; fontScale?: number; color?: ColorArg; thickness?: number;
}): Mat {
  return matOf(call('putText', { src: src.id, text, org, ...(opts || {}) }));
}

// ————————————————————————————————————————————————————————————
// OpenCV.js 那一层：Mat 的构造/类型常量 / MatVector / 独立轮廓算子 / CLAHE / imwrite
// ————————————————————————————————————————————————————————————

function b64ToBytes(b64: string): Uint8Array {
  const s = b64.indexOf(',') >= 0 ? b64.slice(b64.indexOf(',') + 1) : b64;
  const raw = atob(s);
  const out = new Uint8Array(raw.length);
  for (let i = 0; i < raw.length; i++)
    out[i] = raw.charCodeAt(i) & 0xFF;
  return out;
}
function bytesToB64(b: Uint8Array): string {
  let s = '';
  const CHUNK = 4096;
  for (let i = 0; i < b.length; i += CHUNK) {
    const part: number[] = [];
    for (let j = i; j < i + CHUNK && j < b.length; j++)
      part.push(b[j]);
    s += String.fromCharCode.apply(null, part as any);
  }
  return btoa(s);
}

/** 每通道字节数（按 CV 深度） */
function depthBytes(depth: number): number {
  if (depth <= 1) return 1;          // 8U / 8S
  if (depth <= 3) return 2;          // 16U / 16S
  if (depth === 4) return 4;         // 32S
  if (depth === 5) return 4;         // 32F
  return 8;                          // 64F
}
/** OpenCV 4.x 的 CV_MAKETYPE：type = depth + ((cn-1) << 3)。⚠ 5.x 换成 5 位通道, 我们跟 4.x。 */
export function CV_MAKETYPE(depth: number, cn: number): number { return depth + ((cn - 1) << 3); }
export function CV_8UC(n: number): number { return CV_MAKETYPE(0, n); }
export function CV_8SC(n: number): number { return CV_MAKETYPE(1, n); }
export function CV_16UC(n: number): number { return CV_MAKETYPE(2, n); }
export function CV_16SC(n: number): number { return CV_MAKETYPE(3, n); }
export function CV_32SC(n: number): number { return CV_MAKETYPE(4, n); }
export function CV_32FC(n: number): number { return CV_MAKETYPE(5, n); }
export function CV_64FC(n: number): number { return CV_MAKETYPE(6, n); }

/** 由 JS 数组造图（OpenCV.js 的 cv.matFromArray）：数组按**通道顺序平铺**（与 OpenCV 一致）。
 *  例：matFromArray(2, 2, CV_8UC3, [0,0,255, 0,255,0, 255,0,0, 0,0,0]) */
export function matFromArray(rows: number, cols: number, type: MatType | number, array: ArrayLike<number>): Mat {
  const t = (typeof type === 'number') ? type : typeOfName(type);
  const depth = t & 7;
  const channels = ((t >> 3) & 511) + 1;
  const per = depthBytes(depth);
  const n = rows * cols * channels;
  const out = new Uint8Array(n * per);
  const dv = new DataView(out.buffer);
  let o = 0;
  for (let i = 0; i < n; i++) {
    const v = (array as any)[i] || 0;
    switch (depth) {
      case 1: dv.setInt8(o, v); break;
      case 2: dv.setUint16(o, v, true); break;
      case 3: dv.setInt16(o, v, true); break;
      case 4: dv.setInt32(o, v, true); break;
      case 5: dv.setFloat32(o, v, true); break;
      case 6: dv.setFloat64(o, v, true); break;
      default: dv.setUint8(o, v);
    }
    o += per;
  }
  return fromData(bytesToB64(out), { cols, rows, type: t as any });
}
/** 深度名 -> 整型（OpenCV 4.x 的 CV_8U=0 …… CV_64F=6） */
const DEPTH_OF: Record<string, number> = { '8U': 0, '8S': 1, '16U': 2, '16S': 3, '32S': 4, '32F': 5, '64F': 6 };

/** 字符串类型名 -> 整型（'8UC3' -> 16） */
function typeOfName(name: MatType | number): number {
  const s = String(name).toUpperCase().replace(/[^0-9A-Z]/g, '');
  const m = s.match(/^(\d+)([USF])C(\d+)$/);
  if (!m) throw new CvError('invalid', `[cv] 不认识的图像类型 '${name}'（例: 8UC1 / 8UC3 / 32FC1）`);
  const depth = DEPTH_OF[(m[1] + m[2])];
  if (depth === undefined) throw new CvError('invalid', `[cv] 不认识的图像类型 '${name}'`);
  return CV_MAKETYPE(depth, parseInt(m[3], 10));
}
/** 由 ImageData({data, width, height}) 造图（OpenCV.js 的 cv.matFromImageData）—— 8UC4(RGBA) */
export function matFromImageData(imageData: { data: ArrayLike<number>; width: number; height: number }): Mat {
  const bytes = new Uint8Array(imageData.data.length);
  for (let i = 0; i < bytes.length; i++)
    bytes[i] = (imageData.data as any)[i] & 0xFF;
  return fromData(bytesToB64(bytes), { cols: imageData.width, rows: imageData.height, type: '8UC4' });
}

/** cv.MatVector：装一串 Mat / 轮廓（OpenCV.js 里是 std::vector<Mat>）。
 *  我们这边 findContours 给的是**轮廓对象**（派生量都算好了），所以两种都装。 */
export class MatVector {
  private items: any[] = [];
  push_back(m: any): void { this.items.push(m); }
  size(): number { return this.items.length; }
  get(i: number): any { return this.items[i]; }
  set(i: number, m: any): void { this.items[i] = m; }
  resize(n: number, m?: any): void {
    while (this.items.length > n) this.items.pop();
    while (this.items.length < n) this.items.push(m);
  }
  /** ⚠ 只清空这张"向量表", 不替调用方释放里面的句柄（各自的 Mat.delete() / releaseAll() 说了算） */
  delete(): void { this.items.length = 0; }
}

// ————————————————————————————————————————————————————————————
// 轮廓算子（OpenCV.js 的 contourArea / boundingRect / minAreaRect / approxPolyDP / convexHull）
//   两种输入都收：
//     · **轮廓 Mat**（findContours 出参那种, 点存在 C++ 里）⇒ 走 contourStat/xxxOf, 只回一个数或一张图,
//       点一个都不搬（OpenCV.js 就是这么按需算的, 也是快的写法）;
//     · 我们的 **Contour**（富形态, 派生量现成）⇒ 直接取字段, 连这趟都不走。
// ————————————————————————————————————————————————————————————

/** 可以是：我们的 Contour、点数组 [[x,y],...]、或一张 Mat（2 通道 32S/32F） */
export type ContourLike = Contour | ReadonlyArray<readonly [number, number]> | Mat;

function isContourObj(c: any): boolean {
  return !!c && typeof c === 'object' && Array.isArray(c.points) && typeof c.area === 'number';
}
/** 取轮廓的点集。⚠ 轮廓是 Mat(句柄) 时这会把点**序列化成 JSON** —— 只在真要"看/算几何"时用;
 *  面积/周长/包围盒这类都走 contourStat（只回一个数, 不搬点）。 */
export function contourPoints(c: ContourLike): [number, number][] {
  if (isContourObj(c))
    return (c as Contour).points as [number, number][];
  if (Array.isArray(c)) {
    const out: [number, number][] = [];
    for (const p of c as any[])
      out.push([(p as any)[0], (p as any)[1]]);
    return out;
  }
  if (isMatLike(c))
    return call('contourPointsOf', { src: (c as any).id }).value as [number, number][];
  throw new CvError('invalid', '[cv] 不认识的轮廓参数（要 Contour / [[x,y],...] / 2 通道 32S·32F 的 Mat）');
}
/** 轮廓的一个标量/小对象 —— OpenCV.js 的口径: 轮廓在 C++ 里（一张轮廓图）, 只回一个数。
 *  传我们的 Contour（富形态）时上面那批函数会直接取现成字段, 连这一趟都不走。 */
function statOf(c: ContourLike, what: string, extra?: Record<string, unknown>): any {
  const p: any = { what };
  if (extra)
    for (const k of Object.keys(extra)) p[k] = (extra as any)[k];
  if (isMatLike(c)) p.src = (c as any).id;
  else p.pts = contourPoints(c);
  return call('contourStat', p).value;
}
/** 轮廓面积 */
export function contourArea(c: ContourLike): number {
  return isContourObj(c) ? (c as Contour).area : statOf(c, 'area');
}
/** 轮廓周长（closed=false 时是 cv::arcLength 的非闭合算法） */
export function arcLength(c: ContourLike, closed = true): number {
  if (isContourObj(c) && closed) return (c as Contour).length;
  return statOf(c, 'length', { closed });
}
/** 正矩形包围盒（回 OpenCV.js 形状的 Rect：x/y/width/height） */
export function boundingRect(c: ContourLike): Rect {
  if (isContourObj(c)) { const b = (c as Contour).boundingRect; return new Rect(b.x, b.y, b.w, b.h); }
  const b = statOf(c, 'bbox');
  return new Rect(b.x, b.y, b.width, b.height);
}
/** 最小面积矩形（回 RotatedRect；点少于 5 个时返回 null） */
export function minAreaRect(c: ContourLike): RotatedRect | null {
  if (isContourObj(c)) {
    const m = (c as Contour).minAreaRect;
    if (!m) return null;
    return new RotatedRect(new Point(m.center[0], m.center[1]), new Size(m.size[0], m.size[1]), m.angle);
  }
  const m = statOf(c, 'minarea');
  return new RotatedRect(new Point(m.center.x, m.center.y), new Size(m.size.width, m.size.height), m.angle);
}
/** 轮廓点数（扩展：省得为了"有几个点"把整条点集搬回来） */
export function contourPointCount(c: ContourLike): number {
  return isContourObj(c) ? (c as Contour).points.length : statOf(c, 'points');
}
/** 圆度 4πA/L²（1.0 = 正圆；扩展：富形态里有现成的） */
export function contourCircularity(c: ContourLike): number {
  return isContourObj(c) ? (c as Contour).circularity : statOf(c, 'circularity');
}
/** 凸包（OpenCV.js 的 cv.convexHull(cnt, hull[, clockwise, returnPoints])）—— 回一张 Nx1 的
 *  轮廓 Mat（32SC2 = 点；returnPoints=false 时是 32SC1 = 下标）。 */
export function convexHullOf(c: ContourLike, clockwise = false, returnPoints = true): Mat {
  const p: any = { clockwise, returnPoints };
  if (isMatLike(c)) p.src = (c as any).id;
  else p.pts = contourPoints(c);
  return matOf(call('convexHullOf', p));
}
/** 多边形近似（OpenCV.js 的 cv.approxPolyDP(cnt, approx, eps, closed)），eps 是**绝对**阈值
 *  —— 回一张 Nx1 的 32SC2 轮廓 Mat。 */
export function approxPolyDPOf(c: ContourLike, eps = 2, closed = true): Mat {
  const p: any = { eps, closed };
  if (isMatLike(c)) p.src = (c as any).id;
  else p.pts = contourPoints(c);
  return matOf(call('approxPolyDPOf', p));
}
/** 轮廓是不是凸的（真 OpenCV 的 cv::isContourConvex） */
export function isContourConvex(c: ContourLike): boolean {
  return statOf(c, 'convex') === true;
}

/** cv.CLAHE（限制对比度的自适应直方图均衡）：`new CLAHE(clip, new Size(8,8)).apply(src, dst)` */
export class CLAHE {
  private clip: number;
  private tiles: Size2;
  constructor(clipLimit = 40, tileGridSize: Size2 = [8, 8]) {
    this.clip = clipLimit;
    this.tiles = tileGridSize;
  }
  apply(src: Mat, dst?: Mat): Mat {
    const r = clahe(src, this.clip, this.tiles);
    if (dst instanceof Mat) { dst.__bind(r); return dst; }
    return r;
  }
}
/** OpenCV.js 的 cv.createCLAHE(clipLimit, tileGridSize) */
export function createCLAHE(clipLimit = 40, tileGridSize: Size2 = [8, 8]): CLAHE {
  return new CLAHE(clipLimit, tileGridSize);
}

/** 存盘（OpenCV.js 的 cv.imwrite(filename, img) —— ⚠ 参数顺序是**文件名在前**） */
export function imwrite(filename: string, img: Mat): boolean { return img.save(filename); }

// ————————————————————————————————————————————————————————————
// 传统算法补充(2026-10-03)
//   逐元素数学/金字塔 · 统计/积分图 · 检测分割 · 轮廓形状 · 直方图 · 几何光流。
//   口径同上面: 不改输入、回新 Mat（或结构化结果）; 点集参数收 Contour / [[x,y],...] / 2 通道 Mat。
// ————————————————————————————————————————————————————————————

/** 点集参数: Contour(取 .points) / [[x,y],...] / 2 通道 Mat —— 后端两种都收 */
function ptsArg(c: any): any {
  if (c && typeof c === 'object' && Array.isArray(c.points)) return c.points;
  if (c instanceof Mat) return c.id;
  return c;
}

// —— (1) 逐元素数学（传统流程里做增强/归一化/梯度幅值相位）——
/** √x 逐元素（OpenCV.js 的 cv.sqrt） */
export function sqrt(src: Mat): Mat { return matOf(call('sqrt', { src: src.id })); }
/** ln(x) 逐元素 */
export function log(src: Mat): Mat { return matOf(call('log', { src: src.id })); }
/** e^x 逐元素 */
export function exp(src: Mat): Mat { return matOf(call('exp', { src: src.id })); }
/** x^power 逐元素（OpenCV.js 的 cv.pow） */
export function pow(src: Mat, power = 2): Mat { return matOf(call('pow', { src: src.id, power })); }
/** 幅值 √(a²+b²) —— Sobel 出来的 dx/dy 合成功率图用 */
export function magnitude(a: Mat, b: Mat): Mat { return matOf(call('magnitude', { a: a.id, b: b.id })); }
/** 相位 atan2(b, a)；degrees=true 回角度（结果是 CV_32F） */
export function phase(a: Mat, b: Mat, degrees = false): Mat {
  return matOf(call('phase', { a: a.id, b: b.id, degrees }));
}
/** 直角坐标 → 极坐标：回 {magnitude, angle} */
export function cartToPolar(a: Mat, b: Mat, degrees = false): CartToPolarResult {
  const m = matsOf(call('cartToPolar', { a: a.id, b: b.id, degrees }));
  return { magnitude: m[0], angle: m[1] };
}
/** 极坐标 → 直角坐标：回 {x, y} */
export function polarToCart(magnitude: Mat, angle: Mat, degrees = false): PolarToCartResult {
  const m = matsOf(call('polarToCart', { mag: magnitude.id, angle: angle.id, degrees }));
  return { x: m[0], y: m[1] };
}

// —— (2) 图像金字塔 ——
/** 降采样（高斯平滑后抽点，尺寸默认减半） */
export function pyrDown(src: Mat, dstsize?: Size2): Mat {
  const opt: Record<string, unknown> = { src: src.id };
  if (dstsize) opt.dstsize = dstsize;
  return matOf(call('pyrDown', opt));
}
/** 上采样（插值放大，尺寸默认翻倍） */
export function pyrUp(src: Mat, dstsize?: Size2): Mat {
  const opt: Record<string, unknown> = { src: src.id };
  if (dstsize) opt.dstsize = dstsize;
  return matOf(call('pyrUp', opt));
}

// —— (3) 统计 / 积分图 / 仿射 ——
/** 行列归约：dim=0 按列（结果 1×cols）/ 1 按行；op = sum|avg|max|min */
export function reduce(src: Mat, dim = 0, op: number | string = 'sum', dtype = -1): Mat {
  return matOf(call('reduce', { src: src.id, dim, op, dtype }));
}
/** 非零点坐标（Nx1 CV_32SC2）—— 掩膜/亮点定位，配合 countNonZero 用 */
export function findNonZero(src: Mat): Mat { return matOf(call('findNonZero', { src: src.id })); }
/** 积分图（前缀和）：回 {sum, sqsum?, tilted?} —— 区域求和 O(1) */
export function integral(src: Mat, opts: { sdepth?: number; sqsum?: boolean; tilted?: boolean } = {}): IntegralResult {
  const m = matsOf(call('integral', Object.assign({ src: src.id }, opts)));
  return { sum: m[0], sqsum: m[1], tilted: m[2] };
}
/** 三点仿射矩阵（2×3 CV_64F）—— 给 warpAffine 用 */
export function getAffineTransform(srcTri: Point2[], dstTri: Point2[]): Mat {
  return matOf(call('getAffineTransform', { src: srcTri, dst: dstTri }));
}

// —— (4) 检测 / 分割 ——
/** 霍夫圆检测：回 1×N CV_32FC3 的 (x, y, r)（没找到就是空图） */
export function houghCircles(src: Mat, method: number | string = 'gradient', dp = 1, minDist = 10,
                             param1 = 100, param2 = 100, minRadius = 0, maxRadius = 0): Mat {
  return matOf(call('HoughCircles', { src: src.id, method, dp, minDist, param1, param2, minRadius, maxRadius }));
}
/** 概率霍夫线段检测：回 N×1 CV_32SC4 的 (x1, y1, x2, y2) —— 表格线/边框 */
export function houghLinesP(src: Mat, rho = 1, theta = Math.PI / 180, threshold = 50,
                            minLineLength = 50, maxLineGap = 10): Mat {
  return matOf(call('HoughLinesP', { src: src.id, rho, theta, threshold, minLineLength, maxLineGap }));
}
/** 距离变换（二值前景 → 每点到背景的距离）；labels=true 再多回一张 32S 标签图 */
export function distanceTransform(src: Mat, distanceType: number | string = 'l2', maskSize: number | string = '3',
                                  labels = false, labelType: number | string = 'ccomp'): DistanceTransformResult {
  const m = matsOf(call('distanceTransform', { src: src.id, distanceType, maskSize, labels, labelType }));
  return { dist: m[0], labels: m[1] };
}
/** 漫水填充：回**填充后的新图** + 填充像素数 + 外接矩形（不改输入） */
export function floodFill(src: Mat, seedPoint: Point2, newVal: ColorArg = [255, 255, 255],
                          opts: { loDiff?: ColorArg; upDiff?: ColorArg; flags?: number; mask?: Mat } = {}): FloodFillResult {
  const params: Record<string, unknown> = Object.assign({ src: src.id, seedPoint, newVal }, opts);
  if (opts.mask) params.mask = opts.mask.id;
  const r = call('floodFill', params);
  return {
    mat: matOf(r),
    count: r.value as number,
    rect: r.rect as { x: number; y: number; w: number; h: number },
    mask: r.mask ? new Mat(r.mask as number) : null,
  };
}
/** 分水岭分割：markers 里标了种子的连通域会被"淹"，回处理后的 markers 新图 */
export function watershed(src: Mat, markers: Mat): Mat {
  return matOf(call('watershed', { src: src.id, markers: markers.id }));
}
/** K 均值聚类：data 每行一个样本（CV_32F）→ {labels, centers, compactness} */
export function kmeans(data: Mat, k = 2, criteria?: { type?: number; maxCount?: number; epsilon?: number },
                       attempts = 3, flags: number | string = 'pp'): KmeansResult {
  const r = call('kmeans', { data: data.id, k, criteria, attempts, flags });
  const m = matsOf(r);
  return { labels: m[0], centers: m[1], compactness: r.value as number };
}
/** Harris 角点响应图（CV_32F，自己阈值/取局部极大） */
export function cornerHarris(src: Mat, blockSize = 2, ksize = 3, k = 0.04,
                             borderType: number | string = 'default'): Mat {
  return matOf(call('cornerHarris', { src: src.id, blockSize, ksize, k, borderType }));
}
/** Shi-Tomasi 角点（Nx1 CV_32FC2）—— 传统"特征点"里最常用的那组 */
export function goodFeaturesToTrack(src: Mat, maxCorners = 100, qualityLevel = 0.01, minDistance = 10,
                                    blockSize = 3, useHarrisDetector = false, k = 0.04): Mat {
  return matOf(call('goodFeaturesToTrack',
    { src: src.id, maxCorners, qualityLevel, minDistance, blockSize, useHarrisDetector, k }));
}

// —— (5) 轮廓 / 形状（直接吃 findContours 的结果）——
/** 画轮廓：contours 里每项可以是轮廓 Mat 或点集；contourIdx=-1 画全部 */
export function drawContours(img: Mat, contours: any[], contourIdx = -1, color: ColorArg = [0, 255, 0],
                             thickness = 1, lineType: number | string = '8'): Mat {
  return matOf(call('drawContours',
    { src: img.id, contours: contours.map(ptsArg), contourIdx, color, thickness, lineType }));
}
/** 后端回的 [a, b] 或 {a, b} 都归一成 {a, b}（JSON 里我们给的是数组） */
function pairOf(v: any, k1: string, k2: string): any {
  if (Array.isArray(v)) { const o: any = {}; o[k1] = v[0]; o[k2] = v[1]; return o; }
  return v;
}
/** 最小外接圆 → {center, radius} */
export function minEnclosingCircle(c: ContourLike): MinEnclosingCircleResult {
  const r = call('minEnclosingCircle', { pts: ptsArg(c) });
  return { center: pairOf(r.center, 'x', 'y'), radius: r.radius as number };
}
/** 拟合椭圆（≥5 点）→ {center, size, angle} */
export function fitEllipse(c: ContourLike): FitEllipseResult {
  const r = call('fitEllipse', { pts: ptsArg(c) });
  return {
    center: pairOf(r.center, 'x', 'y'),
    size: pairOf(r.size, 'width', 'height'),
    angle: r.angle as number,
  };
}
/** 拟合直线：回 4×1 CV_32F 的 (vx, vy, x0, y0) */
export function fitLine(c: ContourLike, distType: number | string = 'l2', param = 0,
                        reps = 0.01, aeps = 0.01): Mat {
  return matOf(call('fitLine', { pts: ptsArg(c), distType, param, reps, aeps }));
}
/** 形状匹配（Hu 矩）：0 = 完全一样；method = i1|i2|i3 */
export function matchShapes(a: ContourLike, b: ContourLike, method: number | string = 'i1', parameter = 0): number {
  return call('matchShapes', { a: ptsArg(a), b: ptsArg(b), method, parameter }).value as number;
}
/** 凸性缺陷（N×1 CV_32SC4：起点/终点/最远点下标 + 深度×256）—— 数凸起、边缘凹口。
 *  给了 hull（CV_32SC1 的凸包**下标**）就用它，否则后端自己算。 */
export function convexityDefects(c: ContourLike, hull?: Mat | null): Mat {
  const params: Record<string, unknown> = { pts: ptsArg(c) };
  if (hull) params.hull = hull.id;
  return matOf(call('convexityDefects', params));
}

// —— (6) 直方图 ——
/** 直方图比对：correl（1 最像）/ chisqr / intersect（越大越像）/ bhattacharyya·hellinger（越小越像）/ kl */
export function compareHist(a: Mat, b: Mat, method: number | string = 'correl'): number {
  return call('compareHist', { a: a.id, b: b.id, method }).value as number;
}
/** 直方图反投影（传统跟踪里给候选区域打分） */
export function calcBackProject(src: Mat, hist: Mat, channel = 0, lo = 0, hi = 256, scale = 1): Mat {
  return matOf(call('calcBackProject', { src: src.id, hist: hist.id, channel, lo, hi, scale }));
}

// —— (7) 几何 / 光流 ——
/** 单应矩阵（≥4 对点）→ {mat, mask(内点掩膜, 仅 ransac/lmeds/rho 有)} */
export function findHomography(a: ContourLike | Point2[], b: ContourLike | Point2[],
                               method: number | string = 'ransac', ransacReprojThreshold = 3,
                               maxIters = 2000, confidence = 0.995): FindHomographyResult {
  const r = call('findHomography',
    { a: ptsArg(a), b: ptsArg(b), method, ransacReprojThreshold, maxIters, confidence });
  return { mat: matOf(r), mask: r.mask ? new Mat(r.mask as number) : null };
}
/** 去畸变（相机内参 K + 畸变系数 D） */
export function undistort(src: Mat, cameraMatrix: Mat, distCoeffs?: Mat, newCameraMatrix?: Mat): Mat {
  const opt: Record<string, unknown> = { src: src.id, cameraMatrix: cameraMatrix.id };
  if (distCoeffs) opt.distCoeffs = distCoeffs.id;
  if (newCameraMatrix) opt.newCameraMatrix = newCameraMatrix.id;
  return matOf(call('undistort', opt));
}
/** 重映射（map1/map2 是 CV_32FC1/32FC2 的坐标图）—— 自定义形变、坐标查表 */
export function remap(src: Mat, map1: Mat, map2?: Mat, interpolation: number | string = 'linear',
                      borderMode: number | string = 'constant', borderValue: ColorArg = [0, 0, 0]): Mat {
  const opt: Record<string, unknown> = { src: src.id, map1: map1.id, interpolation, borderMode, borderValue };
  if (map2) opt.map2 = map2.id;
  return matOf(call('remap', opt));
}
/** 稠密光流（Farneback）：回与输入同尺寸的 CV_32FC2 (dx, dy) —— 两帧运动场 */
export function calcOpticalFlowFarneback(prev: Mat, next: Mat, pyrScale = 0.5, levels = 3, winsize = 15,
                                         iterations = 3, polyN = 5, polySigma = 1.2, flags = 0): Mat {
  return matOf(call('calcOpticalFlowFarneback',
    { prev: prev.id, next: next.id, pyrScale, levels, winsize, iterations, polyN, polySigma, flags }));
}

// —— (8) 本机构建的 OpenCV **没编 photo 模块**（BUILD_opencv_photo=OFF）——
//   这两条不是"没实现", 是库没有: 调用会抛带原因的错。去噪用 bilateralFilter/medianBlur, 小面积修复
//   用 inRange + morphologyEx 凑。
/** ⚠ photo 模块未编入：调用会抛错（见上面说明） */
export function fastNlMeansDenoising(src: Mat, h = 5, templateWindowSize = 7, searchWindowSize = 21): Mat {
  return matOf(call('fastNlMeansDenoising', { src: src.id, h, templateWindowSize, searchWindowSize }));
}
/** ⚠ photo 模块未编入：调用会抛错（见上面说明） */
export function inpaint(src: Mat, mask: Mat, inpaintRadius = 3, flags: number | string = 'telea'): Mat {
  return matOf(call('inpaint', { src: src.id, mask: mask.id, inpaintRadius, flags }));
}

// ————————————————————————————————————————————————————————————
// default 导出：给 `import cv from 'cv'` 用（具名导入是主用法）
// ————————————————————————————————————————————————————————————
const cv = {
  CvError, Mat, MatVector, CLAHE, createCLAHE,
  Size, Point, Rect, Scalar, Circle, RotatedRect,
  available, count, releaseAll, ops, op,
  zeros, ones, full, fromData, imread, imdecode, imencode, imwrite,
  matFromArray, matFromImageData, CV_MAKETYPE,
  CV_8UC, CV_8SC, CV_16UC, CV_16SC, CV_32SC, CV_32FC, CV_64FC,
  cvtColor, inRange, convertTo, split, merge, lut, applyColorMap, equalizeHist, clahe,
  threshold, adaptiveThreshold,
  blur, boxFilter, gaussianBlur, medianBlur, bilateralFilter, filter2D, sobel, scharr, laplacian, canny,
  getStructuringElement, erode, dilate, morphologyEx, connectedComponents,
  resize, crop, flip, rotate, transpose, warpAffine, warpPerspective,
  getRotationMatrix2D, getPerspectiveTransform, copyMakeBorder, hconcat, vconcat,
  add, subtract, multiply, divide, absdiff,
  bitwiseAnd, bitwiseOr, bitwiseXor, bitwiseNot, addWeighted, compare, min, max, normalize,
  meanStdDev, minMaxLoc, countNonZero, sum, norm,
  findContours, findContoursTo, contourPoints, contourArea, arcLength, boundingRect, minAreaRect,
  convexHullOf, approxPolyDPOf, isContourConvex, contourPointCount, contourCircularity,
  moments, matchTemplate, calcHist,
  line, rectangle, circle, ellipse, polylines, putText,
  TermCriteria,
  sqrt, log, exp, pow, magnitude, phase, cartToPolar, polarToCart,
  pyrDown, pyrUp, reduce, findNonZero, integral, getAffineTransform,
  houghCircles, houghLinesP, distanceTransform, floodFill, watershed, kmeans,
  cornerHarris, goodFeaturesToTrack,
  drawContours, minEnclosingCircle, fitEllipse, fitLine, matchShapes, convexityDefects,
  compareHist, calcBackProject, findHomography, undistort, remap, calcOpticalFlowFarneback,
  fastNlMeansDenoising, inpaint,
};
export default cv;
