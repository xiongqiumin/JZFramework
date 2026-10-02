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
} from './types';

declare const __jz: any;
// 引擎注入的全局 console（本包不依赖 DOM/Node 类型库）
declare var console: { log(...a: any[]): void; error(...a: any[]): void; warn(...a: any[]): void };

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

/** 发一次通用运算；失败抛 CvError（不返回"看着成功的空图"）。 */
function call(name: string, params: Record<string, unknown>): any {
  if (!available())
    throw new CvError('notBuilt', `[cv] 本构建未编入 OpenCV，${name} 不可用`);
  const r = cvb().op(name, JSON.stringify(params));
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
  /** 句柄 id（C++ 侧那张表的键）。由本包的函数创建，不要自己 new。 */
  readonly id: number;
  private freed = false;

  /** @internal */
  constructor(id: number) { this.id = id; }

  /** 图像信息（宽/高/通道/类型…） */
  get info(): MatInfo {
    if (this.freed) throw new CvError('released', '[cv] 这个图像已经被 free() 了');
    const i: any = cvb().info(this.id);
    if (i && i.error) throw new CvError('released', `[cv] ${i.error}`);
    return i as MatInfo;
  }
  get cols(): number { return this.info.cols; }
  get rows(): number { return this.info.rows; }
  get channels(): number { return this.info.channels; }
  get typeName(): string { return this.info.typeName; }
  get empty(): boolean { return this.info.empty; }
  /** [宽, 高] */
  size(): [number, number] { const i = this.info; return [i.cols, i.rows]; }

  /** 深拷贝一份（之后可以各自 free） */
  clone(): Mat { return new Mat(cvb().clone(this.id)); }

  /** 释放这个句柄（C++ 侧那份像素也就没了）。之后再用会抛 released。 */
  free(): void {
    if (this.freed) return;
    this.freed = true;
    cvb().release(this.id);
  }

  /** 取一个像素：[b,g,r] / [gray] … （类型不同长度不同）；越界返回 null */
  pixel(x: number, y: number): number[] | null {
    return cvb().pixel(this.id, x, y);
  }
  /** 写一个像素（改的是这张图本身；少量采样/打点用，批量请用掩膜运算） */
  setPixel(x: number, y: number, values: number[] | number): boolean {
    const v = typeof values === 'number' ? [values] : values;
    return cvb().setPixel(this.id, x, y, JSON.stringify(v));
  }

  /** 存到磁盘（按扩展名定格式：.png/.jpg/.bmp/...） */
  save(path: string): boolean { return cvb().imwrite(this.id, path); }
  /** 编码成 dataURL（默认为 PNG）—— 可直接交给需要图片的地方 */
  encode(ext = 'png'): string { return cvb().imencode(this.id, ext); }
  /** 原始像素字节（8U）的 base64 —— 只在确实要"裸字节"时用 */
  data(): string { return cvb().data(this.id); }

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

/** 从原始像素字节恢复一张图（b64 来自 mat.data()） */
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
/** 翻转：'horizontal' | 'vertical' | 'both'（也可直接给 OpenCV 的 code） */
export function flip(src: Mat, how: FlipHow | number = 'horizontal'): Mat {
  return matOf(call('flip', typeof how === 'number' ? { src: src.id, code: how } : { src: src.id, how }));
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
export function meanStdDev(src: Mat): MeanStdDevResult {
  const r = call('meanStdDev', { src: src.id });
  return { mean: r.mean as number[], stddev: r.stddev as number[] };
}
/** 最小/最大值及位置 —— "最亮的点在哪儿"这类判断 */
export function minMaxLoc(src: Mat): MinMaxLocResult {
  const r = call('minMaxLoc', { src: src.id });
  return { minVal: r.minVal, maxVal: r.maxVal, minLoc: r.minLoc, maxLoc: r.maxLoc };
}
/** 非零像素数（掩膜"有多大"）。
 *  ⚠ 只吃**单通道**（OpenCV 的约束）：彩色图先 `cvtColor(img, 'BGR2GRAY')`，否则后端会抛错。 */
export function countNonZero(src: Mat): number { return call('countNonZero', { src: src.id }).value as number; }
/** 逐通道求和 */
export function sum(src: Mat): number[] { return call('sum', { src: src.id }).value as number[]; }
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
 *  每项已经算好 area / length / circularity / boundingRect / center / hull / approx —— 直接筛、直接点。 */
export function findContours(src: Mat, opts?: { mode?: RetrMode; method?: ChainMethod; approxEpsilon?: number }): Contour[] {
  return (call('findContours', { src: src.id, ...(opts || {}) }).value || []) as Contour[];
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
// default 导出：给 `import cv from 'cv'` 用（具名导入是主用法）
// ————————————————————————————————————————————————————————————
const cv = {
  CvError, Mat, available, count, releaseAll, ops, op,
  zeros, ones, full, fromData, imread, imdecode, imencode,
  cvtColor, inRange, convertTo, split, merge, lut, applyColorMap, equalizeHist, clahe,
  threshold, adaptiveThreshold,
  blur, boxFilter, gaussianBlur, medianBlur, bilateralFilter, filter2D, sobel, scharr, laplacian, canny,
  getStructuringElement, erode, dilate, morphologyEx, connectedComponents,
  resize, crop, flip, rotate, transpose, warpAffine, warpPerspective,
  getRotationMatrix2D, getPerspectiveTransform, copyMakeBorder, hconcat, vconcat,
  add, subtract, multiply, divide, absdiff,
  bitwiseAnd, bitwiseOr, bitwiseXor, bitwiseNot, addWeighted, compare, min, max, normalize,
  meanStdDev, minMaxLoc, countNonZero, sum, norm,
  findContours, moments, matchTemplate, calcHist,
  line, rectangle, circle, ellipse, polylines, putText,
};
export default cv;
