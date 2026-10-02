// cv/index.ts —— **本包的入口 = OpenCV.js 形状的公开层**（`import cv from 'cv'` 拿到的就是这个）。
//
// 分两层: core.ts 是"顺手"的实现（参数简写、返回值就是结果）; 这里是把 OpenCV.js 的形状照搬过来的
// 一层。为什么要单独一层 —— OpenCV.js 的代码
// 是另一套调用形状 ——
//     cv.COLOR_BGR2GRAY / cv.THRESH_BINARY + cv.THRESH_OTSU ...   常量（在 consts.ts）
//     new cv.Mat(rows, cols, cv.CV_8UC3) / cv.matFromArray(...)
//     cv.cvtColor(src, dst, code)          ← dst 是**出参**
//     cv.Canny(img, edges, 50, 150)        ← 函数名按 C++（大写/下划线）
//     cv.findContours(img, contours, hierarchy, cv.RETR_EXTERNAL, cv.CHAIN_APPROX_SIMPLE)
//     cv.MatVector + cv.contourArea(contours.get(i)) + cv.boundingRect(cnt)
// 本文件把这些形状**照搬**过来（内部一律转调 core.ts 的实现），于是 OpenCV.js 的示例、
// 以及大模型照着 OpenCV.js 生成的代码基本可以原样跑。要用我们自己的顺手形状, 从 'cv/core.js' 引。
//
// 口径（与 index.ts 的关系）:
//   · **dst 是可选的**: `cvtColor(src, dst, code)` 写进 dst; `cvtColor(src, code)` 仍返回新图。
//     后端每个 op 都返回新句柄, 所以"写进 dst" = 把 dst 重新绑到新图上（dst 这个 JS 对象的身份不变,
//     它原先绑的句柄被释放）。`new cv.Mat()` 建的空图就是给这种用法准备的。
//   · **出参形式一律无返回**（2026-10-03 按 opencv-js 实测对齐）: 给了 dst/出参的函数返回 undefined,
//     只有三个例外——threshold 回实际阈值、connectedComponents 回连通域个数、minMaxLoc 回对象。
//   · **绘图类是就地画**: `cv.circle(img, pt, r, color)` 直接改 img, 无返回（同 OpenCV.js）。
//   · **Mat 的成员按 OpenCV.js 分方法/属性**: `type()/depth()/empty()/elemSize()/elemSize1()/step1()/
//     channels()/total()/isContinuous()/mul()/dot()/inv()/diag()/t()` 是**方法**;
//     `cols/rows/data/data8U…/step/matSize` 是**属性**。
//   · 以 jz 为前缀/不在 OpenCV.js 里的能力（available/ops/op/zeros/full/crop/contourMetrics ...
//     以及 Contour 上现成的 area/boundingRect/hull/approx）仍然从 index.ts 用。
//   · **对不齐的**: VideoCapture / DNN / features2d / objdetect 那几大块没有（后端就没有）；
//     imshow 没有（本引擎没有 canvas，请用 mat.save() 或 computerUse 那套显示/比对）；
//     Mat.roi/row/col 是**拷贝**语义（OpenCV.js 是视图）; setTo 不支持 mask;
//     mat.data 是**快照**（改了不写回, 与 OpenCV.js 的 WASM 视图不同）。

import * as K from './consts';
import {
  Mat, CvError, MatVector, CLAHE, createCLAHE,
  Size, Point, Rect, Scalar, Circle, RotatedRect, TermCriteria,
  available, count, releaseAll, ops, op, zeros, ones, full, fromData, imwrite,
  imread as coreImread, imdecode, imencode as coreImencode,
  matFromArray, matFromImageData, CV_MAKETYPE,
  CV_8UC, CV_8SC, CV_16UC, CV_16SC, CV_32SC, CV_32FC, CV_64FC,
  cvtColor as coreCvtColor, inRange as coreInRange, convertTo, split as coreSplit,
  merge as coreMerge, lut as coreLut, applyColorMap as coreApplyColorMap,
  equalizeHist as coreEqualizeHist, clahe,
  threshold as coreThreshold, adaptiveThreshold as coreAdaptiveThreshold,
  blur as coreBlur, boxFilter as coreBoxFilter, gaussianBlur as coreGaussianBlur,
  medianBlur as coreMedianBlur, bilateralFilter as coreBilateralFilter,
  filter2D as coreFilter2D, sobel as coreSobel, scharr as coreScharr,
  laplacian as coreLaplacian, canny as coreCanny,
  getStructuringElement as coreGetStructuringElement,
  erode as coreErode, dilate as coreDilate, morphologyEx as coreMorphologyEx,
  connectedComponents as coreConnectedComponents,
  resize as coreResize, crop, flip as coreFlip, rotate, transpose, warpAffine as coreWarpAffine,
  warpPerspective as coreWarpPerspective, getRotationMatrix2D, getPerspectiveTransform,
  copyMakeBorder as coreCopyMakeBorder, hconcat, vconcat,
  add as coreAdd, subtract as coreSubtract, multiply as coreMultiply, divide as coreDivide,
  absdiff as coreAbsdiff, bitwiseAnd as coreAnd, bitwiseOr as coreOr, bitwiseXor as coreXor,
  bitwiseNot as coreNot, addWeighted as coreAddWeighted, compare as coreCompare,
  min as coreMin, max as coreMax, normalize as coreNormalize,
  meanStdDev as coreMeanStdDev, mean, minMaxLoc as coreMinMaxLoc, countNonZero, sum, norm,
  findContours as coreFindContours, findContoursTo as coreFindContoursTo,
  contourPoints, contourArea, arcLength, boundingRect, minAreaRect,
  contourPointCount, contourCircularity, convexHullOf, approxPolyDPOf, isContourConvex,
  moments, matchTemplate as coreMatchTemplate, calcHist,
  line as coreLine, rectangle as coreRectangle, circle as coreCircle, ellipse as coreEllipse,
  polylines as corePolylines, putText as corePutText,
  // 传统算法(2026-10-03): 这里都带 Core 别名 —— 同名对外那层是 OpenCV.js 的出参形状(见文件下半部)
  sqrt as coreSqrt, log as coreLog, exp as coreExp, pow as corePow,
  magnitude as coreMagnitude, phase as corePhase,
  cartToPolar as coreCartToPolar, polarToCart as corePolarToCart,
  pyrDown as corePyrDown, pyrUp as corePyrUp,
  reduce as coreReduce, findNonZero as coreFindNonZero, integral as coreIntegral,
  getAffineTransform, houghCircles as coreHoughCircles, houghLinesP as coreHoughLinesP,
  distanceTransform as coreDistanceTransform, floodFill as coreFloodFill, watershed as coreWatershed,
  kmeans as coreKmeans, cornerHarris as coreCornerHarris, goodFeaturesToTrack as coreGoodFeaturesToTrack,
  drawContours as coreDrawContours, minEnclosingCircle as coreMinEnclosingCircle,
  fitEllipse as coreFitEllipse, fitLine as coreFitLine, matchShapes, convexityDefects as coreConvexityDefects,
  compareHist, calcBackProject as coreCalcBackProject, findHomography as coreFindHomography,
  undistort as coreUndistort, remap as coreRemap, calcOpticalFlowFarneback as coreCalcOpticalFlowFarneback,
  fastNlMeansDenoising, inpaint,
} from './core';
import type {
  ColorArg, ColorCode, Size2, Point2, ComponentsResult, Contour, RetrMode, ChainMethod,
  CmpOp, TmMethod, ThreshType, ThresholdResult, MeanStdDevResult,
  CartToPolarResult, PolarToCartResult, IntegralResult, DistanceTransformResult, KmeansResult,
} from './types';
// ContourLike 要用 **core 那份**（它里面的 Mat 是真类）—— 从 './types' 拿会和实现侧的 Mat 撞成两个类型
import type { ContourLike } from './core';

export * from './consts';
// core 的公开面全部转出去(我们自己的"顺手"形状: available/ops/zeros/crop/contourArea/MatVector...)。
// 下面**同名的**那些(本文件自己声明的)按 ESM 语义**盖住** star 导出的版本 —— 于是
// `import { cvtColor } from 'cv'` 拿到的是 OpenCV.js 形状那个, 而 available/ops/... 照旧。
export * from './core';

/** 写进 dst（= 换绑句柄）。 */
function bindDst(dst: Mat, produced: any): void {
  dst.__bind(produced);
}
/** 运行时判"这个参数是不是 dst" */
function isDst(v: any): boolean { return v instanceof Mat; }
/** 点取值：cv.Point / [x,y] / {x,y} 都收（画图那套要自己算几何时用） */
function ptXY(p: any): [number, number] {
  if (p instanceof Point) return [p.x, p.y];
  if (Array.isArray(p)) return [p[0], p[1]];
  if (p && typeof p === 'object') return [p.x, p.y];
  return [0, 0];
}

// ————————————————————————————————————————————————————————————
// 颜色 / 通道
// ————————————————————————————————————————————————————————————

/** `cvtColor(src, code)` 返回新图；`cvtColor(src, dst, code)` 写进 dst（OpenCV.js 形状）。 */
export function cvtColor(src: Mat, code: ColorCode | number): Mat;
export function cvtColor(src: Mat, dst: Mat, code: ColorCode | number): void;
export function cvtColor(src: Mat, a: any, b?: any): any {
  return isDst(a) ? bindDst(a, coreCvtColor(src, b)) : coreCvtColor(src, a);
}

/** OpenCV.js 的 inRange(src, lowerb, upperb[, dst]) —— 注意 dst 在**第四个**位置。
 *  给了 dst 就写进去并**无返回**（与 OpenCV.js 一致）, 否则返回新图。 */
export function inRange(src: Mat, lower: ColorArg, upper: ColorArg, dst: Mat): void;
export function inRange(src: Mat, lower: ColorArg, upper: ColorArg): Mat;
export function inRange(src: Mat, lower: ColorArg, upper: ColorArg, dst?: Mat): any {
  const r = coreInRange(src, lower, upper);
  if (dst instanceof Mat) { bindDst(dst, r); return undefined as any; }
  return r;
}

/** OpenCV.js 的 cv.equalizeHist(src[, dst]) */
export function equalizeHist(src: Mat, dst: Mat, ...rest: any[]): void;
export function equalizeHist(src: Mat, ...rest: any[]): Mat;
export function equalizeHist(src: Mat, dst?: Mat): any {
  const r = coreEqualizeHist(src);
  return (dst instanceof Mat) ? bindDst(dst, r) : r;
}

/** OpenCV.js 的 cv.LUT(src, lut[, dst]) */
export function LUT(src: Mat, dst: Mat, ...rest: any[]): void;
export function LUT(src: Mat, table: number[], dst?: Mat): Mat;
export function LUT(src: Mat, table?: any, dst?: Mat): any {
  const r = coreLut(src, table);
  return (dst instanceof Mat) ? bindDst(dst, r) : r;
}

/** OpenCV.js 的 cv.applyColorMap(src, colormap[, dst])；也收 (src, dst, colormap) 的写法。 */
export function applyColorMap(src: Mat, dst: Mat, ...rest: any[]): void;
export function applyColorMap(src: Mat, ...rest: any[]): Mat;
export function applyColorMap(src: Mat, a: any, b?: any): any {
  if (isDst(a)) return bindDst(a, coreApplyColorMap(src, b));
  const r = coreApplyColorMap(src, a);
  return (b instanceof Mat) ? bindDst(b, r) : r;
}

// ————————————————————————————————————————————————————————————
// 阈值 / 滤波
// ————————————————————————————————————————————————————————————

/** OpenCV.js 形状：`threshold(src, dst, thresh, maxval, type)` 回**实际阈值**(number)；
 *  不给 dst 时按本包的形状回 `ThresholdResult`。 */
export function threshold(src: Mat, dst: Mat, thresh: number, maxval: number, type?: ThreshType | number): number;
export function threshold(src: Mat, thresh: number, maxval: number, type?: ThreshType | number): ThresholdResult;
export function threshold(src: Mat, a: any, b?: any, c?: any, d?: any): any {
  if (isDst(a)) {
    const r = coreThreshold(src, b, c, d);
    bindDst(a, r.mat);
    return r.thresh;
  }
  return coreThreshold(src, a, b, c);
}

/** OpenCV.js 的 adaptiveThreshold(src, dst, maxValue, adaptiveMethod, thresholdType, blockSize, C)。
 *  adaptiveMethod 收 0/1（cv.ADAPTIVE_THRESH_MEAN_C / GAUSSIAN_C）或 'mean'/'gaussian'。 */
export function adaptiveThreshold(src: Mat, dst: Mat, ...rest: any[]): void;
export function adaptiveThreshold(src: Mat, ...rest: any[]): Mat;
export function adaptiveThreshold(src: Mat, a: any, b?: any, c?: any, d?: any, e?: any, f?: any): any {
  if (isDst(a)) {
    const method = (b === 1 || b === 'gaussian' || b === 'GAUSSIAN') ? 'gaussian' : 'mean';
    return bindDst(a, coreAdaptiveThreshold(src, c, method, d, e, f));
  }
  return coreAdaptiveThreshold(src, a, b, c, d, e);
}

/** OpenCV.js 的 cv.blur(src, dst, ksize, borderType) */
export function blur(src: Mat, dst: Mat, ...rest: any[]): void;
export function blur(src: Mat, ...rest: any[]): Mat;
export function blur(src: Mat, a?: any, b?: any, c?: any): any {
  if (isDst(a)) return bindDst(a, coreBlur(src, b, c === undefined ? undefined : { borderType: c }));
  return coreBlur(src, a, b);
}

/** OpenCV.js 的 cv.boxFilter(src, dst, ddepth, ksize, ...)（ddepth/anchor/borderType 目前忽略） */
export function boxFilter(src: Mat, dst: Mat, ...rest: any[]): void;
export function boxFilter(src: Mat, ...rest: any[]): Mat;
export function boxFilter(src: Mat, a?: any, b?: any, c?: any, d?: any, e?: any): any {
  if (isDst(a)) return bindDst(a, coreBoxFilter(src, c, (e === undefined) ? true : e));
  return coreBoxFilter(src, a, b, c);
}

/** OpenCV.js 的 cv.GaussianBlur(src, dst, ksize, sigmaX, sigmaY, borderType) */
export function GaussianBlur(src: Mat, dst: Mat, ...rest: any[]): void;
export function GaussianBlur(src: Mat, ...rest: any[]): Mat;
export function GaussianBlur(src: Mat, a?: any, b?: any, c?: any, d?: any, e?: any): any {
  if (isDst(a)) return bindDst(a, coreGaussianBlur(src, b, c, { sigmaY: d, borderType: e }));
  return coreGaussianBlur(src, a, b, c);
}

/** OpenCV.js 的 cv.medianBlur(src, dst, ksize) */
export function medianBlur(src: Mat, dst: Mat, ...rest: any[]): void;
export function medianBlur(src: Mat, ...rest: any[]): Mat;
export function medianBlur(src: Mat, a?: any, b?: any): any {
  if (isDst(a)) return bindDst(a, coreMedianBlur(src, b));
  return coreMedianBlur(src, a);
}

/** OpenCV.js 的 cv.bilateralFilter(src, dst, d, sigmaColor, sigmaSpace) */
export function bilateralFilter(src: Mat, dst: Mat, ...rest: any[]): void;
export function bilateralFilter(src: Mat, ...rest: any[]): Mat;
export function bilateralFilter(src: Mat, a?: any, b?: any, c?: any, d?: any): any {
  if (isDst(a)) return bindDst(a, coreBilateralFilter(src, b, c, d));
  return coreBilateralFilter(src, a, b, c);
}

/** OpenCV.js 的 cv.filter2D(src, dst, ddepth, kernel, anchor, delta, borderType) */
export function filter2D(src: Mat, dst: Mat, ...rest: any[]): void;
export function filter2D(src: Mat, ...rest: any[]): Mat;
export function filter2D(src: Mat, a?: any, b?: any, c?: any, d?: any, e?: any): any {
  if (isDst(a)) return bindDst(a, coreFilter2D(src, c, d === undefined ? undefined : { delta: d }));
  return coreFilter2D(src, a, b);
}

/** OpenCV.js 的 cv.Sobel(src, dst, ddepth, dx, dy, ksize, scale, delta, borderType) */
export function Sobel(src: Mat, dst: Mat, ...rest: any[]): void;
export function Sobel(src: Mat, ...rest: any[]): Mat;
export function Sobel(src: Mat, a?: any, b?: any, c?: any, d?: any, e?: any, f?: any, g?: any): any {
  if (isDst(a)) return bindDst(a, coreSobel(src, c, d, e, { ddepth: b, scale: f, delta: g }));
  return coreSobel(src, a, b, c, d);
}

/** OpenCV.js 的 cv.Scharr(src, dst, ddepth, dx, dy, scale, delta, borderType) */
export function Scharr(src: Mat, dst: Mat, ...rest: any[]): void;
export function Scharr(src: Mat, ...rest: any[]): Mat;
export function Scharr(src: Mat, a?: any, b?: any, c?: any, d?: any, e?: any, f?: any): any {
  if (isDst(a)) return bindDst(a, coreScharr(src, c, d, { ddepth: b, scale: e }));
  return coreScharr(src, a, b, c);
}

/** OpenCV.js 的 cv.Laplacian(src, dst, ddepth, ksize, scale, delta, borderType) */
export function Laplacian(src: Mat, dst: Mat, ...rest: any[]): void;
export function Laplacian(src: Mat, ...rest: any[]): Mat;
export function Laplacian(src: Mat, a?: any, b?: any, c?: any, d?: any, e?: any): any {
  if (isDst(a)) return bindDst(a, coreLaplacian(src, c, { ddepth: b, scale: d }));
  return coreLaplacian(src, a, b);
}

/** OpenCV.js 的 cv.Canny(image, edges, threshold1, threshold2, apertureSize, L2gradient) */
export function Canny(src: Mat, dst: Mat, ...rest: any[]): void;
export function Canny(src: Mat, ...rest: any[]): Mat;
export function Canny(src: Mat, a?: any, b?: any, c?: any, d?: any, e?: any): any {
  if (isDst(a)) return bindDst(a, coreCanny(src, b, c, { apertureSize: d, l2gradient: e }));
  return coreCanny(src, a, b, c);
}

/** OpenCV.js 的 cv.getStructuringElement(shape, ksize)；shape 也收 cv.MORPH_RECT/ELLIPSE/CROSS 那几个数。 */
export function getStructuringElement(shape: any = 'rect', ksize?: Size2): Mat {
  let s: any = shape;
  if (typeof s === 'number') s = (s === K.MORPH_CROSS) ? 'cross' : (s === K.MORPH_ELLIPSE) ? 'ellipse' : 'rect';
  else if (typeof s === 'string') {
    const t = s.toLowerCase();
    s = t.indexOf('cross') >= 0 ? 'cross' : (t.indexOf('ellipse') >= 0 || t.indexOf('ellip') >= 0) ? 'ellipse' : 'rect';
  }
  return coreGetStructuringElement(s, ksize === undefined ? 3 : ksize);
}

/** OpenCV.js 的 cv.erode(src, dst, kernel, anchor, iterations, borderType) */
export function erode(src: Mat, dst: Mat, ...rest: any[]): void;
export function erode(src: Mat, ...rest: any[]): Mat;
export function erode(src: Mat, a?: any, b?: any, c?: any, d?: any, e?: any): any {
  if (isDst(a)) return bindDst(a, coreErode(src, b, d === undefined ? 1 : d));
  return coreErode(src, a, b, c);
}

/** OpenCV.js 的 cv.dilate(src, dst, kernel, anchor, iterations, borderType) */
export function dilate(src: Mat, dst: Mat, ...rest: any[]): void;
export function dilate(src: Mat, ...rest: any[]): Mat;
export function dilate(src: Mat, a?: any, b?: any, c?: any, d?: any, e?: any): any {
  if (isDst(a)) return bindDst(a, coreDilate(src, b, d === undefined ? 1 : d));
  return coreDilate(src, a, b, c);
}

/** OpenCV.js 的 cv.morphologyEx(src, dst, op, kernel, anchor, iterations, borderType) */
export function morphologyEx(src: Mat, dst: Mat, ...rest: any[]): void;
export function morphologyEx(src: Mat, ...rest: any[]): Mat;
export function morphologyEx(src: Mat, a?: any, b?: any, c?: any, d?: any, e?: any, f?: any): any {
  if (isDst(a)) return bindDst(a, coreMorphologyEx(src, b, c, e === undefined ? 1 : e));
  return coreMorphologyEx(src, a, b, c, d);
}

/** OpenCV.js 的 `connectedComponents(image, labels, connectivity, ltype)` —— 给了 labels 就写进去
 *  并回**连通域个数**（与 OpenCV.js 一致）；不给则按本包的形状回 `{labels,count,stats,centroids}`。
 *  ⚠ `ltype` 只支持 CV_32S(4)（后端固定用 32S 标签）, 给别的值会报错而不是悄悄忽略。 */
export function connectedComponents(image: Mat, labels: Mat, connectivity?: 4 | 8, ltype?: number): number;
export function connectedComponents(image: Mat, connectivity?: 4 | 8): ComponentsResult;
export function connectedComponents(src: Mat, labels?: any, connectivity?: 4 | 8, ltype?: number): any {
  if (ltype !== undefined && ltype !== CV_MAKETYPE(4, 1))
    throw new CvError('invalid', `[cv] connectedComponents 的 ltype 只支持 CV_32S(${CV_MAKETYPE(4, 1)}), 收到 ${ltype}`);
  const r: ComponentsResult = coreConnectedComponents(src, connectivity);
  if (labels instanceof Mat) {
    bindDst(labels, r.labels);
    return r.count;
  }
  return r;
}

/** [[...],[...]] -> N×M 的 Mat（按给定 CV 类型）—— 给 connectedComponentsWithStats 的出参用 */
function rowsToMat(rows: number[][], cols: number, type: number): Mat {
  const flat: number[] = [];
  for (const row of rows)
    for (let i = 0; i < cols; i++)
      flat.push(row[i] === undefined ? 0 : row[i]);
  return matFromArray(rows.length, cols, type, flat);
}

/** OpenCV.js 的 `connectedComponentsWithStats(image, labels, stats, centroids, connectivity, ltype)`：
 *  labels（Nx1 CV_32S）/ stats（N×5 CV_32S）/ centroids（N×2 CV_64F）都是**出参**，回连通域个数。
 *  不给这三个出参时（本包扩展）回 `{labels, count, stats, centroids}`。 */
export function connectedComponentsWithStats(image: Mat, labels: Mat, stats: Mat, centroids: Mat,
                                             connectivity?: 4 | 8, ltype?: number): number;
export function connectedComponentsWithStats(image: Mat, connectivity?: 4 | 8): ComponentsResult;
export function connectedComponentsWithStats(image: Mat, a?: any, b?: any, c?: any, d?: any, e?: any): any {
  const out = (a instanceof Mat && b instanceof Mat && c instanceof Mat);
  const conn: 4 | 8 = out ? ((d === 4) ? 4 : 8) : ((a === 4 || a === 8) ? a : 8);
  const r: ComponentsResult = coreConnectedComponents(image, conn);
  if (!out) return r;
  bindDst(a, r.labels);
  b.__bind(rowsToMat(r.stats, 5, CV_MAKETYPE(4, 1)));        // stats: N×5 CV_32S
  c.__bind(rowsToMat(r.centroids, 2, CV_MAKETYPE(6, 1)));    // centroids: N×2 CV_64F
  return r.count;
}

// ————————————————————————————————————————————————————————————
// 几何
// ————————————————————————————————————————————————————————————

/** OpenCV.js 的 cv.resize(src, dst, dsize, fx, fy, interpolation)；dsize 给 cv.Size 或 [w,h] */
export function resize(src: Mat, dst: Mat, ...rest: any[]): void;
export function resize(src: Mat, ...rest: any[]): Mat;
export function resize(src: Mat, dst: Mat, dsize?: any, fx?: number, fy?: number, interpolation?: any): Mat;
export function resize(src: Mat, sizeOrOpts: any, interpolation?: any): Mat;
export function resize(src: Mat, a: any, b?: any, c?: any, d?: any, e?: any): any {
  if (isDst(a)) {
    const zero = !b || (typeof b === 'number' && b === 0);
    return bindDst(a, zero
      ? coreResize(src, { fx: c, fy: d, interpolation: e })
      : coreResize(src, b, e));
  }
  return coreResize(src, a, b);
}

/** OpenCV.js 的 cv.flip(src, dst, flipCode)：0=上下翻, 正数=左右翻, 负数=既上下又左右 */
export function flip(src: Mat, dst: Mat, ...rest: any[]): void;
export function flip(src: Mat, ...rest: any[]): Mat;
export function flip(src: Mat, a?: any, b?: any): any {
  if (isDst(a)) {
    const code = Number(b);
    return bindDst(a, coreFlip(src, code === 0 ? 'vertical' : (code < 0 ? 'both' : 'horizontal')));
  }
  return coreFlip(src, a);
}

/** OpenCV.js 的 cv.warpAffine(src, dst, M, dsize) */
export function warpAffine(src: Mat, dst: Mat, ...rest: any[]): void;
export function warpAffine(src: Mat, ...rest: any[]): Mat;
export function warpAffine(src: Mat, a: any, b?: any, c?: any): any {
  if (isDst(a)) return bindDst(a, coreWarpAffine(src, b, c));
  return coreWarpAffine(src, a, b, c);
}

/** OpenCV.js 的 cv.warpPerspective(src, dst, M, dsize) */
export function warpPerspective(src: Mat, dst: Mat, ...rest: any[]): void;
export function warpPerspective(src: Mat, ...rest: any[]): Mat;
export function warpPerspective(src: Mat, a: any, b?: any, c?: any): any {
  if (isDst(a)) return bindDst(a, coreWarpPerspective(src, b, c));
  return coreWarpPerspective(src, a, b, c);
}

/** OpenCV.js 的 cv.copyMakeBorder(src, dst, top, bottom, left, right, borderType, value) */
export function copyMakeBorder(src: Mat, dst: Mat, ...rest: any[]): void;
export function copyMakeBorder(src: Mat, ...rest: any[]): Mat;
export function copyMakeBorder(src: Mat, a?: any, b?: any, c?: any, d?: any, e?: any, f?: any, g?: any): any {
  if (isDst(a)) {
    const opts: any = {};
    if (f !== undefined && typeof f === 'string') opts.borderType = f;
    if (g !== undefined) opts.value = g;
    return bindDst(a, coreCopyMakeBorder(src, b, c, d, e, opts));
  }
  return coreCopyMakeBorder(src, a, b, c, d, e);
}

// ————————————————————————————————————————————————————————————
// 通道拼接 / 逐元素运算
// ————————————————————————————————————————————————————————————

/** OpenCV.js 的 cv.split(src, mv)：各通道推进 MatVector, **无返回**。
 *  不给 mv 时（本包扩展）返回 `Mat[]`。 */
export function split(src: Mat, mv: MatVector): void;
export function split(src: Mat): Mat[];
export function split(src: Mat, mv?: MatVector | number[]): any {
  const chans = coreSplit(src, Array.isArray(mv) ? mv : undefined);
  if (mv instanceof MatVector) {
    for (const c of chans) mv.push_back(c);
    return;
  }
  return chans;
}
/** OpenCV.js 的 cv.merge(mv, dst)：写进 dst, **无返回**（也给本包的 `merge(mats)` 回一张新图）。 */
export function merge(mv: any, dst: Mat): void;
export function merge(mv: any): Mat;
export function merge(mv: any, dst?: Mat): any {
  const list: any[] = [];
  if (mv instanceof MatVector) {
    for (let i = 0; i < mv.size(); i++) list.push(mv.get(i));
  } else if (Array.isArray(mv)) {
    for (const m of mv) list.push(m);
  } else {
    throw new CvError('invalid', '[cv] merge 要 MatVector 或 Mat[]');
  }
  const r = coreMerge(list);
  if (dst instanceof Mat) { bindDst(dst, r); return; }
  return r;
}

/** OpenCV.js 的 cv.addWeighted(src1, alpha, src2, beta, gamma, dst)：给了 dst 就无返回 */
export function addWeighted(a: Mat, alpha: number, b: Mat, beta: number, gamma: number, dst: Mat): void;
export function addWeighted(a: Mat, alpha: number, b: Mat, beta: number, gamma?: number): Mat;
export function addWeighted(a: Mat, alpha: number, b: Mat, beta: number, gamma = 0, dst?: Mat): any {
  const r = coreAddWeighted(a, alpha, b, beta, gamma);
  if (dst instanceof Mat) { bindDst(dst, r); return undefined as any; }
  return r;
}

function binaryOp(core: (a: Mat, b: Mat) => Mat, a: Mat, b: any, c?: any): Mat {
  if (c instanceof Mat) { bindDst(c, core(a, b)); return undefined as any; }
  return core(a, b);
}
export function add(a: Mat, b: Mat, dst: Mat): void;
export function add(a: Mat, b: Mat): Mat;
export function add(a: Mat, b: Mat, dst?: Mat): Mat { return binaryOp(coreAdd, a, b, dst); }
export function subtract(a: Mat, b: Mat, dst: Mat): void;
export function subtract(a: Mat, b: Mat): Mat;
export function subtract(a: Mat, b: Mat, dst?: Mat): Mat { return binaryOp(coreSubtract, a, b, dst); }
/** OpenCV.js 的 cv.multiply(src1, src2, dst[, scale])（第 3 个参数是 Mat 就是 dst） */
export function multiply(a: Mat, b: Mat, dst: Mat, scale?: number): void;
export function multiply(a: Mat, b: Mat, scale?: number): Mat;
export function multiply(a: Mat, b: Mat, dstOrScale?: any, dst?: any): any {
  if (isDst(dstOrScale)) { bindDst(dstOrScale, coreMultiply(a, b)); return undefined as any; }
  return coreMultiply(a, b, dstOrScale);
}
export function divide(a: Mat, b: Mat, dst: Mat, scale?: number): void;
export function divide(a: Mat, b: Mat, scale?: number): Mat;
export function divide(a: Mat, b: Mat, dstOrScale?: any, dst?: any): any {
  if (isDst(dstOrScale)) { bindDst(dstOrScale, coreDivide(a, b)); return undefined as any; }
  return coreDivide(a, b, dstOrScale);
}
export function absdiff(a: Mat, b: Mat, dst: Mat): void;
export function absdiff(a: Mat, b: Mat): Mat;
export function absdiff(a: Mat, b: Mat, dst?: Mat): Mat { return binaryOp(coreAbsdiff, a, b, dst); }
export function bitwise_and(a: Mat, b: Mat, dst: Mat): void;
export function bitwise_and(a: Mat, b: Mat): Mat;
export function bitwise_and(a: Mat, b: Mat, dst?: Mat): Mat { return binaryOp(coreAnd, a, b, dst); }
export function bitwise_or(a: Mat, b: Mat, dst: Mat): void;
export function bitwise_or(a: Mat, b: Mat): Mat;
export function bitwise_or(a: Mat, b: Mat, dst?: Mat): Mat { return binaryOp(coreOr, a, b, dst); }
export function bitwise_xor(a: Mat, b: Mat, dst: Mat): void;
export function bitwise_xor(a: Mat, b: Mat): Mat;
export function bitwise_xor(a: Mat, b: Mat, dst?: Mat): Mat { return binaryOp(coreXor, a, b, dst); }
/** OpenCV.js 的 cv.bitwise_not(src, dst) —— 取反（掩膜取反最常用） */
export function bitwise_not(src: Mat, dst: Mat): void;
export function bitwise_not(src: Mat): Mat;
export function bitwise_not(src: Mat, dst?: Mat): any {
  const r = coreNot(src);
  if (dst instanceof Mat) { bindDst(dst, r); return undefined as any; }
  return r;
}
/** OpenCV.js 的 cv.min(src1, src2, dst) / cv.max(...) */
export function min(a: Mat, b: Mat, dst: Mat): void;
export function min(a: Mat, b: Mat): Mat;
export function min(a: Mat, b: Mat, dst?: Mat): Mat { return binaryOp(coreMin, a, b, dst); }
export function max(a: Mat, b: Mat, dst: Mat): void;
export function max(a: Mat, b: Mat): Mat;
export function max(a: Mat, b: Mat, dst?: Mat): Mat { return binaryOp(coreMax, a, b, dst); }
/** OpenCV.js 的 cv.normalize(src, dst, alpha, beta, norm_type) */
export function normalize(src: Mat, dst: Mat, ...rest: any[]): void;
export function normalize(src: Mat, ...rest: any[]): Mat;
export function normalize(src: Mat, a?: any, b?: any, c?: any, d?: any): any {
  if (isDst(a)) return bindDst(a, coreNormalize(src, b, c, d));
  return coreNormalize(src, a, b, c);
}
/** OpenCV.js 的 cv.compare(src1, src2, dst, cmpop)：给了 dst 就无返回 */
export function compare(a: Mat, b: any, dst: Mat, cmpop?: CmpOp | number): void;
export function compare(a: Mat, b: any, cmpop?: CmpOp | number): Mat;
export function compare(a: Mat, b: any, c?: any, d?: any): any {
  if (isDst(c)) { bindDst(c, coreCompare(a, b, d)); return undefined as any; }
  return coreCompare(a, b, c);
}

// ————————————————————————————————————————————————————————————
// 轮廓 / 统计
// ————————————————————————————————————————————————————————————

/** OpenCV.js 的 `findContours(image, contours, hierarchy, mode, method)`：结果填进出参
 *  （`contours` 里每项是**轮廓 Mat**, `hierarchy` 是 Mat）—— **返回 undefined**, 与 OpenCV.js 一致。
 *  派生量与它一样按需再问: contourArea / boundingRect / convexHull / approxPolyDP ...
 *  另: 只给 opts 的写法（`findContours(image, { mode, method })`）是本包的扩展, 直接回 `Contour[]`
 *  （每条轮廓的派生量都已算在里面, 代价是一次较大的 JSON, 见文件头）。 */
export function findContours(src: Mat, contours: MatVector, hierarchy?: Mat, mode?: RetrMode, method?: ChainMethod): void;
export function findContours(src: Mat, opts?: { mode?: RetrMode; method?: ChainMethod; approxEpsilon?: number }): Contour[];
export function findContours(src: Mat, mvOrOpts?: any, hierarchy?: any, mode?: any, method?: any): any {
  if (mvOrOpts instanceof MatVector) {
    coreFindContoursTo(src, mvOrOpts, hierarchy, mode, method);
    return;
  }
  return coreFindContours(src, mvOrOpts);
}

/** OpenCV.js 的 `convexHull(cnt, hull[, clockwise, returnPoints])`：写进 `hull`, **无返回**。
 *  不给 hull 时（本包的扩展）返回那张 Mat —— 内容是 Nx1 的 32SC2 轮廓点（returnPoints=false 时是 32SC1 下标）。 */
export function convexHull(c: ContourLike, hull?: Mat, clockwise = false, returnPoints = true): any {
  const r = convexHullOf(c, clockwise, returnPoints);
  if (hull instanceof Mat) {
    hull.__bind(r);                 // 出参语义: 绑过去, 不返回（与 OpenCV.js 一致）
    return;
  }
  return r;
}

/** OpenCV.js 的 `approxPolyDP(cnt, approx, eps, closed)`：写进 `approx`, **无返回**。
 *  不给 out 时（本包的扩展）返回那张 Mat。⚠ `eps` 是**绝对**阈值（与 OpenCV.js 一致,
 *  不再是"占周长的比例"）。 */
export function approxPolyDP(c: ContourLike, approxCurve?: Mat | number | null, eps = 2, closed = true): any {
  const e = (typeof approxCurve === 'number') ? approxCurve : eps;
  const r = approxPolyDPOf(c, e, closed);
  if (approxCurve instanceof Mat) {
    approxCurve.__bind(r);
    return;
  }
  return r;
}

/** 位置点统一成"`.x/.y` 与 `[0]/[1]` 都读得到"的 Point —— OpenCV.js 的 minMaxLoc 回 {x,y},
 *  我们 core 内部回 [x,y], 这里抹平, 免得两边代码写法不同。 */
function asPt(v: any): any {
  if (!v) return v;
  if (Array.isArray(v)) return new Point(v[0], v[1]);
  if (typeof v === 'object' && typeof v.x === 'number') return new Point(v.x, v.y);
  return v;
}

/** OpenCV.js 的 `minMaxLoc(src[, mask])` -> `{minVal,maxVal,minLoc,maxLoc}`（定位点是带 .x/.y 的
 *  cv.Point）。mask 也接了（只统计掩膜里的像素, 与 OpenCV.js 一致）。 */
export function minMaxLoc(src: Mat, mask?: Mat): any {
  const r: any = coreMinMaxLoc(src, mask);
  return { minVal: r.minVal, maxVal: r.maxVal, minLoc: asPt(r.minLoc), maxLoc: asPt(r.maxLoc) };
}

/** OpenCV.js 的 `meanStdDev(src, mean, stddev[, mask])`：出参式（mean/stddev 是 Mat, 写进去;
 *  与 OpenCV.js 一样是 1×N 的 CV_64F）—— **无返回**。不给出参时（本包扩展）回 `{mean, stddev}`。 */
export function meanStdDev(src: Mat, mean: Mat, stddev: Mat, mask?: Mat): void;
export function meanStdDev(src: Mat, mask?: Mat): MeanStdDevResult;
export function meanStdDev(src: Mat, a?: Mat, b?: Mat, mask?: Mat): any {
  const r: any = coreMeanStdDev(src, mask);
  const meanArr: number[] = r.mean || [];
  const sdArr: number[] = r.stddev || [];
  if (a instanceof Mat && b instanceof Mat) {
    a.__bind(matFromArray(1, meanArr.length, CV_MAKETYPE(6, 1), meanArr));
    b.__bind(matFromArray(1, sdArr.length, CV_MAKETYPE(6, 1), sdArr));
    return;
  }
  return { mean: meanArr, stddev: sdArr };
}

/** OpenCV.js 的 `cv.HuMoments(moments, dst)`：把 7 个 Hu 不变矩写成 dst（1×7 的 CV_64F）, **无返回**。
 *  ⚠ opencv-js 的 `cv.moments()` 结果里**没有** hu（要自己调 HuMoments 算）; 我们的 moments() 直接带了
 *  `hu`, 所以这条只是把已有数字落成一张 Mat, 不回后端。 */
export function HuMoments(m: any, dst: Mat): void {
  const hu: number[] = Array.isArray(m) ? m : (m && Array.isArray(m.hu) ? m.hu : null as any);
  if (!hu || hu.length < 7)
    throw new CvError('invalid', '[cv] HuMoments 要 moments() 的结果（或 7 个数的数组）');
  dst.__bind(matFromArray(1, 7, CV_MAKETYPE(6, 1), hu));
}

/** OpenCV.js 的 `matchTemplate(image, templ, result, method)` —— 写进 result, **无返回**。
 *  （也收我们原来的顺序 `(src, templ, method, result)`：第 3 个参数是不是 Mat 说了算。）
 *  不给 result 时（本包扩展）返回分数图。 */
export function matchTemplate(src: Mat, templ: Mat, result: Mat, method?: TmMethod | number): void;
export function matchTemplate(src: Mat, templ: Mat, method?: TmMethod | number): Mat;
export function matchTemplate(src: Mat, templ: Mat, a?: any, b?: any): any {
  const aIsMat = a instanceof Mat;
  const method = aIsMat ? b : a;
  const result: any = aIsMat ? a : b;
  const r: any = coreMatchTemplate(src, templ, method);
  if (result instanceof Mat) {
    bindDst(result, r.mat);
    return;
  }
  return r.mat;
}

// ————————————————————————————————————————————————————————————
// 绘图（OpenCV.js 是**就地画**：直接改第一张图）
// ————————————————————————————————————————————————————————————

function drawInPlace(src: Mat, produced: Mat): void { bindDst(src, produced); }

/** OpenCV.js 的 cv.line(img, pt1, pt2, color, thickness, lineType, shift) */
export function line(img: Mat, pt1: Point2, pt2: Point2, color?: ColorArg, thickness?: any, lineType?: any): void {
  const opts: any = {};
  if (typeof thickness === 'number') opts.thickness = thickness;
  if (lineType !== undefined) opts.lineType = lineType;
  drawInPlace(img, coreLine(img, pt1, pt2, color === undefined ? [0, 0, 255] : color, opts));
}
/** OpenCV.js 的 cv.rectangle(img, pt1, pt2, color, thickness, lineType, shift)（也收 Rect） */
export function rectangle(img: Mat, a: any, b?: any, c?: any, d?: any, e?: any): void {
  // 矩形形状: cv.Rect / {x,y,w,h} / [x,y,w,h]（4 个数的数组）
  const isRect = (a instanceof Rect)
              || (!!a && typeof a === 'object' && typeof a.width === 'number')
              || (Array.isArray(a) && a.length >= 4);
  if (isRect) {
    const opts: any = {};
    if (typeof c === 'number') opts.thickness = c;
    if (d !== undefined) opts.lineType = d;
    drawInPlace(img, coreRectangle(img, a as any, b === undefined ? [0, 0, 255] : b, opts));
  }
  // 对角两点（cv.Point / [x,y] / {x,y}）
  const p1 = ptXY(a), p2 = ptXY(b);
  const rect: any = [p1[0], p1[1], p2[0] - p1[0], p2[1] - p1[1]];
  const opts: any = {};
  if (typeof d === 'number') opts.thickness = d;
  drawInPlace(img, coreRectangle(img, rect, c === undefined ? [0, 0, 255] : c, opts));
}
/** OpenCV.js 的 cv.circle(img, center, radius, color, thickness, lineType, shift) */
export function circle(img: Mat, center: any, radius?: number, color?: ColorArg, thickness?: any, lineType?: any): void {
  const c = (center instanceof Point) ? center : center;
  const opts: any = {};
  if (typeof thickness === 'number') opts.thickness = thickness;
  if (lineType !== undefined) opts.lineType = lineType;
  drawInPlace(img, coreCircle(img, c, radius === undefined ? 5 : radius,
                                   color === undefined ? [0, 0, 255] : color, opts));
}
/** OpenCV.js 的 cv.ellipse(img, center, axes, angle, startAngle, endAngle, color, thickness, ...) */
export function ellipse(img: Mat, center: any, axes?: any, angle?: any, startAngle?: any, endAngle?: any, color?: ColorArg, thickness?: any): void {
  const opts: any = {};
  if (typeof angle === 'number') opts.angle = angle;
  if (typeof startAngle === 'number') opts.startAngle = startAngle;
  if (typeof endAngle === 'number') opts.endAngle = endAngle;
  if (typeof thickness === 'number') opts.thickness = thickness;
  opts.color = (color === undefined) ? [0, 0, 255] : color;
  drawInPlace(img, coreEllipse(img, center, axes === undefined ? [10, 10] : axes, opts));
}
/** OpenCV.js 的 cv.polylines(img, pts, isClosed, color, thickness) —— pts 收 MatVector 或点数组 */
export function polylines(img: Mat, pts: any, isClosed?: any, color?: ColorArg, thickness?: any): void {
  let list: any[] = [];
  if (pts instanceof MatVector) {
    for (let i = 0; i < pts.size(); i++) list.push(pts.get(i));
  } else {
    list = pts as any[];
  }
  const opts: any = {};
  if (typeof isClosed === 'boolean') opts.closed = isClosed;
  if (typeof thickness === 'number') opts.thickness = thickness;
  drawInPlace(img, corePolylines(img, list, (color instanceof Array || typeof color === 'string' || typeof color === 'number') ? color : [0, 0, 255], opts));
}
/** OpenCV.js 的 cv.putText(img, text, org, fontFace, fontScale, color, thickness, lineType) */
export function putText(img: Mat, text: string, org: Point2, fontFace?: any, fontScale?: any, color?: ColorArg, thickness?: any, lineType?: any): void {
  const opts: any = {};
  if (fontFace !== undefined) opts.fontFace = fontFace;
  if (typeof fontScale === 'number') opts.fontScale = fontScale;
  if (color !== undefined) opts.color = color;
  if (typeof thickness === 'number') opts.thickness = thickness;
  if (lineType !== undefined) opts.lineType = lineType;
  drawInPlace(img, corePutText(img, text, org, opts));
}

// ————————————————————————————————————————————————————————————
// 存取（OpenCV.js 的 imread/imencode 形状）
// ————————————————————————————————————————————————————————————

/** OpenCV.js 的 `imread(filename, flags)`：flags 认 IMREAD_GRAYSCALE / IMREAD_COLOR（其余按原样读）。 */
export function imread(path: string, flags?: number): Mat {
  const m = coreImread(path);
  if (flags === K.IMREAD_GRAYSCALE && m.channels() !== 1)
    return coreCvtColor(m, m.channels() === 4 ? 'BGRA2GRAY' : 'BGR2GRAY');
  if (flags === K.IMREAD_COLOR && m.channels() === 1)
    return coreCvtColor(m, 'GRAY2BGR');
  return m;
}
/** OpenCV.js 的 `imencode(ext, img)` 回一张**装着编码字节的图**(8UC1)；本包的老形状
 *  `imencode(src, ext)` 回 dataURL 字符串 —— 按第一个参数是字符串还是 Mat 区分。 */
export function imencode(a: any, b?: any): any {
  if (typeof a === 'string') {
    const url = coreImencode(b, a);
    const raw = atob(url.slice(url.indexOf(',') + 1));
    const bytes: number[] = [];
    for (let i = 0; i < raw.length; i++) bytes.push(raw.charCodeAt(i) & 0xFF);
    return matFromArray(bytes.length, 1, CV_MAKETYPE(0, 1), bytes);
  }
  return coreImencode(a, b === undefined ? 'png' : b);
}

// ————————————————————————————————————————————————————————————
// 传统算法(2026-10-03) 的 OpenCV.js 出参式外观
//   · 形状照 @techstark/opencv-js 的 .d.ts：出参给了就写进去并**无返回**；不给则回新图（本包扩展）。
//   · minEnclosingCircle 回 Circle、fitEllipse 回 RotatedRect、kmeans 回紧凑度、findHomography 回 Mat。
//   · 就地语义(floodFill/watershed/drawContours)与 OpenCV.js 一致：改第一张图, 无返回。
// ————————————————————————————————————————————————————————————

/** 一元出参式：`cv.foo(src, dst[, rest...])` */
function unary(core: (src: Mat, ...rest: any[]) => Mat, src: Mat, dst?: Mat, ...rest: any[]): any {
  const r = core(src, ...rest);
  if (dst instanceof Mat) { bindDst(dst, r); return; }
  return r;
}
/** 二元出参式：`cv.foo(a, b, dst[, rest...])` */
function binaryOut(core: (a: Mat, b: Mat, ...rest: any[]) => Mat, a: Mat, b: Mat, dst?: Mat, ...rest: any[]): any {
  const r = core(a, b, ...rest);
  if (dst instanceof Mat) { bindDst(dst, r); return; }
  return r;
}

export function sqrt(src: Mat, dst: Mat): void;
export function sqrt(src: Mat): Mat;
export function sqrt(src: Mat, dst?: Mat): any { return unary(coreSqrt, src, dst); }

export function log(src: Mat, dst: Mat): void;
export function log(src: Mat): Mat;
export function log(src: Mat, dst?: Mat): any { return unary(coreLog, src, dst); }

export function exp(src: Mat, dst: Mat): void;
export function exp(src: Mat): Mat;
export function exp(src: Mat, dst?: Mat): any { return unary(coreExp, src, dst); }

export function pow(src: Mat, power: number, dst: Mat): void;
export function pow(src: Mat, power?: number): Mat;
export function pow(src: Mat, power = 2, dst?: Mat): any { return unary(corePow, src, dst, power); }

export function magnitude(x: Mat, y: Mat, dst: Mat): void;
export function magnitude(x: Mat, y: Mat): Mat;
export function magnitude(x: Mat, y: Mat, dst?: Mat): any { return binaryOut(coreMagnitude, x, y, dst); }

export function phase(x: Mat, y: Mat, angle: Mat, angleInDegrees?: boolean): void;
export function phase(x: Mat, y: Mat, angleInDegrees?: boolean): Mat;
export function phase(x: Mat, y: Mat, a?: any, b?: any): any {
  const hasDst = a instanceof Mat;
  return binaryOut(corePhase, x, y, hasDst ? a : undefined, hasDst ? !!b : !!a);
}

export function cartToPolar(x: Mat, y: Mat, magnitude: Mat, angle: Mat, angleInDegrees?: boolean): void;
export function cartToPolar(x: Mat, y: Mat, angleInDegrees?: boolean): CartToPolarResult;
export function cartToPolar(x: Mat, y: Mat, a?: any, b?: any, c?: any): any {
  const hasOut = a instanceof Mat && b instanceof Mat;
  const r = coreCartToPolar(x, y, hasOut ? !!c : !!a);
  if (hasOut) { bindDst(a, r.magnitude); bindDst(b, r.angle); return; }
  return r;
}

export function polarToCart(magnitude: Mat, angle: Mat, x: Mat, y: Mat, angleInDegrees?: boolean): void;
export function polarToCart(magnitude: Mat, angle: Mat, angleInDegrees?: boolean): PolarToCartResult;
export function polarToCart(mag: Mat, ang: Mat, a?: any, b?: any, c?: any): any {
  const hasOut = a instanceof Mat && b instanceof Mat;
  const r = corePolarToCart(mag, ang, hasOut ? !!c : !!a);
  if (hasOut) { bindDst(a, r.x); bindDst(b, r.y); return; }
  return r;
}

export function pyrDown(src: Mat, dst: Mat, dstsize?: Size2, borderType?: number): void;
export function pyrDown(src: Mat, dstsize?: Size2): Mat;
export function pyrDown(src: Mat, a?: any, b?: any): any {
  if (a instanceof Mat) { bindDst(a, corePyrDown(src, b as Size2)); return; }
  return corePyrDown(src, a as Size2);
}

export function pyrUp(src: Mat, dst: Mat, dstsize?: Size2, borderType?: number): void;
export function pyrUp(src: Mat, dstsize?: Size2): Mat;
export function pyrUp(src: Mat, a?: any, b?: any): any {
  if (a instanceof Mat) { bindDst(a, corePyrUp(src, b as Size2)); return; }
  return corePyrUp(src, a as Size2);
}

export function reduce(src: Mat, dst: Mat, dim: number, rtype: number | string, dtype?: number): void;
export function reduce(src: Mat, dim?: number, rtype?: number | string, dtype?: number): Mat;
export function reduce(src: Mat, a?: any, b?: any, c?: any, d?: any): any {
  if (a instanceof Mat) { bindDst(a, coreReduce(src, b, c, d)); return; }
  return coreReduce(src, a, b, c);
}

export function findNonZero(src: Mat, idx: Mat): void;
export function findNonZero(src: Mat): Mat;
export function findNonZero(src: Mat, dst?: Mat): any {
  const r = coreFindNonZero(src);
  if (dst instanceof Mat) { bindDst(dst, r); return; }
  return r;
}

export function integral(src: Mat, sum: Mat, sdepth?: number): void;
export function integral(src: Mat, sum: Mat, sqsum: Mat, sdepth?: number, sqdepth?: number): void;
export function integral(src: Mat, sum: Mat, sqsum: Mat, tilted: Mat, sdepth?: number, sqdepth?: number): void;
export function integral(src: Mat, opts?: { sdepth?: number; sqsum?: boolean; tilted?: boolean }): IntegralResult;
export function integral(src: Mat, a?: any, b?: any, c?: any, d?: any): any {
  if (a instanceof Mat) {
    const hasSq = b instanceof Mat, hasTilt = c instanceof Mat;
    const r = coreIntegral(src, { sdepth: hasSq ? (hasTilt ? d : c) : b, sqsum: hasSq, tilted: hasTilt });
    bindDst(a, r.sum);
    if (hasSq) bindDst(b, r.sqsum as Mat);
    if (hasTilt) bindDst(c, r.tilted as Mat);
    return;
  }
  return coreIntegral(src, a);
}

export function cornerHarris(src: Mat, dst: Mat, blockSize: number, ksize: number, k: number, borderType?: number): void;
export function cornerHarris(src: Mat, blockSize?: number, ksize?: number, k?: number, borderType?: number): Mat;
export function cornerHarris(src: Mat, a?: any, b?: any, c?: any, d?: any, e?: any): any {
  if (a instanceof Mat) { bindDst(a, coreCornerHarris(src, b, c, d, e)); return; }
  return coreCornerHarris(src, a, b, c, d);
}

export function goodFeaturesToTrack(image: Mat, corners: Mat, maxCorners: number, qualityLevel: number, minDistance: number,
                                    mask?: Mat, blockSize?: number, useHarrisDetector?: boolean, k?: number): void;
export function goodFeaturesToTrack(image: Mat, maxCorners?: number, qualityLevel?: number, minDistance?: number,
                                    blockSize?: number, useHarrisDetector?: boolean, k?: number): Mat;
export function goodFeaturesToTrack(image: Mat, a?: any, b?: any, c?: any, d?: any, e?: any, f?: any, g?: any, h?: any): any {
  // 出参式: (image, corners, maxCorners, qualityLevel, minDistance, mask?, blockSize?, useHarrisDetector?, k?)
  if (a instanceof Mat) {
    // ⚠ 空 Mat(`new cv.Mat()`) 读 .rows 会抛: 先看它有没有绑图像
    if (e instanceof Mat && !e.isDeleted() && e.rows > 0)
      throw new CvError('invalid', '[cv] goodFeaturesToTrack 的 mask 暂未接后端（传空 Mat 或省略参数）');
    bindDst(a, coreGoodFeaturesToTrack(image, b, c, d, f, g === undefined ? false : g, h));
    return;
  }
  return coreGoodFeaturesToTrack(image, a, b, c, d, e, f);
}

export function HoughCircles(image: Mat, circles: Mat, method: number | string, dp: number, minDist: number,
                             param1?: number, param2?: number, minRadius?: number, maxRadius?: number): void;
export function HoughCircles(image: Mat, method?: number | string, dp?: number, minDist?: number,
                             param1?: number, param2?: number, minRadius?: number, maxRadius?: number): Mat;
export function HoughCircles(image: Mat, a?: any, b?: any, c?: any, d?: any, e?: any, f?: any, g?: any): any {
  if (a instanceof Mat) { bindDst(a, coreHoughCircles(image, b, c, d, e, f, g)); return; }
  return coreHoughCircles(image, a, b, c, d, e, f);
}

export function HoughLinesP(image: Mat, lines: Mat, rho: number, theta: number, threshold: number,
                            minLineLength?: number, maxLineGap?: number): void;
export function HoughLinesP(image: Mat, rho?: number, theta?: number, threshold?: number,
                            minLineLength?: number, maxLineGap?: number): Mat;
export function HoughLinesP(image: Mat, a?: any, b?: any, c?: any, d?: any, e?: any): any {
  if (a instanceof Mat) { bindDst(a, coreHoughLinesP(image, b, c, d, e)); return; }
  return coreHoughLinesP(image, a, b, c, d);
}

export function distanceTransform(src: Mat, dst: Mat, distanceType: number | string, maskSize: number | string, dstType?: number): void;
export function distanceTransform(src: Mat, dst: Mat, labels: Mat, distanceType: number | string, maskSize: number | string, labelType?: number): void;
export function distanceTransform(src: Mat, distanceType?: any, maskSize?: any, labels?: boolean, labelType?: any): DistanceTransformResult;
export function distanceTransform(src: Mat, a?: any, b?: any, c?: any, d?: any, e?: any): any {
  if (a instanceof Mat && b instanceof Mat) {           // (src, dst, labels, distanceType, maskSize, labelType)
    const r = coreDistanceTransform(src, c, d, true, e);
    bindDst(a, r.dist); bindDst(b, r.labels as Mat);
    return;
  }
  if (a instanceof Mat) {                               // (src, dst, distanceType, maskSize, dstType)
    bindDst(a, coreDistanceTransform(src, b, c, false).dist);
    return;
  }
  return coreDistanceTransform(src, a, b, c, d);        // 扩展: (src, distanceType, maskSize, labels, labelType)
}

export function floodFill(image: Mat, seedPoint: Point2, newVal: ColorArg, loDiff?: ColorArg, upDiff?: ColorArg, flags?: number): number;
export function floodFill(image: Mat, mask: Mat, seedPoint: Point2, newVal: ColorArg, loDiff?: ColorArg, upDiff?: ColorArg, flags?: number): number;
export function floodFill(image: Mat, a?: any, b?: any, c?: any, d?: any, e?: any, f?: any, g?: any): any {
  // OpenCV.js 是**就地**填充（改第一张图）并回填充像素数; 给了 mask 就把它也写出去
  const withMask = a instanceof Mat;
  const seed = withMask ? b : a, newVal = withMask ? c : b;
  const r = coreFloodFill(image, seed, newVal, {
    loDiff: withMask ? d : c, upDiff: withMask ? e : d, flags: withMask ? f : e,
    mask: withMask ? a : undefined,
  });
  image.__bind(r.mat as Mat);
  if (withMask && r.mask) a.__bind(r.mask as Mat);
  return r.count;
}

/** OpenCV.js 的 `cv.watershed(image, markers)`：**就地**改 markers, 无返回 */
export function watershed(image: Mat, markers: Mat): void { markers.__bind(coreWatershed(image, markers)); }

/** OpenCV.js 的 `cv.kmeans(data, K, bestLabels, criteria, attempts, flags[, centers])` -> 紧凑度 */
export function kmeans(data: Mat, K: number, bestLabels: Mat, criteria: any, attempts: number, flags: number,
                       centers?: Mat): number;
export function kmeans(data: Mat, K?: number, criteria?: any, attempts?: number, flags?: number): KmeansResult;
export function kmeans(data: Mat, a?: any, b?: any, c?: any, d?: any, e?: any, f?: any): any {
  // 出参式: (data, K, bestLabels, criteria, attempts, flags[, centers]) —— 注意 bestLabels 是**第 3 个**参数
  const hasOut = b instanceof Mat;
  const r = hasOut ? coreKmeans(data, a, c, d, e) : coreKmeans(data, a, b, c, d);
  if (!hasOut) return r;
  bindDst(b, r.labels);
  if (f instanceof Mat) bindDst(f, r.centers);
  return r.compactness;
}

/** OpenCV.js 的 `cv.drawContours(image, contours, contourIdx, color, ...)`：**就地**画, 无返回 */
export function drawContours(image: Mat, contours: any[], contourIdx: number, color: ColorArg, thickness?: number,
                             lineType?: number, hierarchy?: Mat, maxLevel?: number, offset?: Point2): void {
  image.__bind(coreDrawContours(image, contours, contourIdx, color, thickness, lineType));
}

/** OpenCV.js 的 `cv.minEnclosingCircle(points)` -> Circle */
export function minEnclosingCircle(points: any): Circle {
  const r = coreMinEnclosingCircle(points);
  return new Circle(new Point(r.center.x, r.center.y), r.radius);
}

/** OpenCV.js 的 `cv.fitEllipse(points)` -> RotatedRect */
export function fitEllipse(points: any): RotatedRect {
  const r = coreFitEllipse(points);
  return new RotatedRect(new Point(r.center.x, r.center.y), new Size(r.size.width, r.size.height), r.angle);
}

export function fitLine(points: any, line: Mat, distType: number | string, param: number, reps: number, aeps: number): void;
export function fitLine(points: any, distType?: number | string, param?: number, reps?: number, aeps?: number): Mat;
export function fitLine(points: any, a?: any, b?: any, c?: any, d?: any, e?: any): any {
  if (a instanceof Mat) { bindDst(a, coreFitLine(points, b, c, d, e)); return; }
  return coreFitLine(points, a, b, c, d);
}

/** OpenCV.js 的 `cv.convexityDefects(contour, convexhull, defects)`：写进出参, 无返回 */
export function convexityDefects(contour: any, convexhull: Mat | null, defects: Mat): void;
export function convexityDefects(contour: any): Mat;
export function convexityDefects(contour: any, a?: any, b?: any): any {
  if (b instanceof Mat) { bindDst(b, coreConvexityDefects(contour, a)); return; }   // a = hull 下标(可 null)
  if (a instanceof Mat) { bindDst(a, coreConvexityDefects(contour)); return; }
  return coreConvexityDefects(contour);
}

/** OpenCV.js 的 `cv.calcBackProject([src], [channel], hist, dst, [lo, hi], scale)` */
export function calcBackProject(images: any, channels: any, hist: Mat, dst: Mat, ranges: any, scale?: number): void;
export function calcBackProject(src: Mat, hist: Mat, channel?: number, lo?: number, hi?: number, scale?: number): Mat;
export function calcBackProject(a: any, b: any, c?: any, d?: any, e?: any, f?: any): any {
  if (Array.isArray(a)) {
    const src: Mat = a[0], hist: Mat = c;
    const ch = Array.isArray(b) ? b[0] : b;
    const lo = e ? e[0] : 0, hi = e ? e[1] : 256;
    bindDst(d, coreCalcBackProject(src, hist, ch, lo, hi, f === undefined ? 1 : f));
    return;
  }
  return coreCalcBackProject(a, b, c, d, e, f);
}

/** OpenCV.js 的 `cv.findHomography(srcPoints, dstPoints[, method, ransacReprojThreshold, mask])` -> H */
export function findHomography(srcPoints: any, dstPoints: any, mask: Mat, method?: number | string, ransacReprojThreshold?: number): Mat;
export function findHomography(srcPoints: any, dstPoints: any, method?: number | string, ransacReprojThreshold?: number,
                               mask?: Mat, maxIters?: number, confidence?: number): Mat;
export function findHomography(a: any, b: any, c?: any, d?: any, e?: any, f?: any, g?: any): Mat {
  const maskFirst = c instanceof Mat;                 // .d.ts 里还有 (src, dst, mask, method?) 这种顺序
  const r = coreFindHomography(a, b, maskFirst ? d : c, maskFirst ? e : d, f, g);
  const mask = maskFirst ? c : e;
  if (mask instanceof Mat && r.mask) bindDst(mask, r.mask as Mat);
  return r.mat as Mat;
}

export function undistort(src: Mat, dst: Mat, cameraMatrix: Mat, distCoeffs: Mat, newCameraMatrix?: Mat): void;
export function undistort(src: Mat, cameraMatrix: Mat, distCoeffs?: Mat, newCameraMatrix?: Mat): Mat;
export function undistort(src: Mat, a?: any, b?: any, c?: any, d?: any): any {
  if (a instanceof Mat) {
    // (src, dst, K, D[, newK]) 出参式 vs (src, K, D[, newK]) 扩展式: 第 3 个是不是 Mat 说了算
    if (b instanceof Mat && c instanceof Mat) { bindDst(a, coreUndistort(src, b, c, d)); return; }
    return coreUndistort(src, a, b, c);
  }
  return coreUndistort(src, a, b, c);
}

export function remap(src: Mat, dst: Mat, map1: Mat, map2: Mat, interpolation: number | string,
                      borderMode?: number | string, borderValue?: ColorArg): void;
export function remap(src: Mat, map1: Mat, map2?: Mat, interpolation?: number | string,
                      borderMode?: number | string, borderValue?: ColorArg): Mat;
export function remap(src: Mat, a?: any, b?: any, c?: any, d?: any, e?: any, f?: any): any {
  if (a instanceof Mat && c instanceof Mat) { bindDst(a, coreRemap(src, b, c, d, e, f)); return; }
  return coreRemap(src, a, b, c, d, e);
}

export function calcOpticalFlowFarneback(prev: Mat, next: Mat, flow: Mat, pyrScale: number, levels: number, winsize: number,
                                         iterations: number, polyN: number, polySigma: number, flags: number): void;
export function calcOpticalFlowFarneback(prev: Mat, next: Mat, pyrScale?: number, levels?: number, winsize?: number,
                                         iterations?: number, polyN?: number, polySigma?: number, flags?: number): Mat;
export function calcOpticalFlowFarneback(prev: Mat, next: Mat, a?: any, b?: any, c?: any, d?: any, e?: any, f?: any, g?: any, h?: any): any {
  if (a instanceof Mat) { bindDst(a, coreCalcOpticalFlowFarneback(prev, next, b, c, d, e, f, g, h)); return; }
  return coreCalcOpticalFlowFarneback(prev, next, a, b, c, d, e, f, g);
}

// ————————————————————————————————————————————————————————————
// default 导出：`import cv from 'cv'` 拿到的 OpenCV.js 形状命名空间
//   （常量 + 函数 + Mat/MatVector/值对象 都在上面了）
// ————————————————————————————————————————————————————————————
const cv = {
  ...K,
  // 类与常量之外的东西
  CvError, Mat, MatVector, CLAHE, createCLAHE,
  Size, Point, Rect, Scalar, Circle, RotatedRect,
  available, count, releaseAll, ops, op,
  zeros, ones, full, fromData, imread, imdecode, imencode, imwrite,
  matFromArray, matFromImageData, CV_MAKETYPE,
  CV_8UC, CV_8SC, CV_16UC, CV_16SC, CV_32SC, CV_32FC, CV_64FC,
  cvtColor, inRange, convertTo, split, merge, LUT, applyColorMap, equalizeHist, clahe,
  threshold, adaptiveThreshold,
  blur, boxFilter, GaussianBlur, medianBlur, bilateralFilter, filter2D,
  Sobel, Scharr, Laplacian, Canny,
  getStructuringElement, erode, dilate, morphologyEx, connectedComponents,
  resize, crop, flip, rotate, transpose, warpAffine, warpPerspective,
  getRotationMatrix2D, getPerspectiveTransform, copyMakeBorder, hconcat, vconcat,
  add, subtract, multiply, divide, absdiff,
  bitwiseAnd: bitwise_and, bitwiseOr: bitwise_or, bitwiseXor: bitwise_xor, bitwiseNot: bitwise_not,
  bitwise_and, bitwise_or, bitwise_xor, bitwise_not,
  addWeighted, compare, min, max, normalize,
  meanStdDev, mean, minMaxLoc, countNonZero, sum, norm,
  findContours, contourPoints, contourArea, arcLength, boundingRect, minAreaRect, convexHull,
  approxPolyDP, isContourConvex, contourPointCount, contourCircularity,
  moments, HuMoments, matchTemplate, calcHist,
  line, rectangle, circle, ellipse, polylines, putText, TermCriteria,
  sqrt, log, exp, pow, magnitude, phase, cartToPolar, polarToCart,
  pyrDown, pyrUp, reduce, findNonZero, integral, getAffineTransform,
  HoughCircles, HoughLinesP, distanceTransform,
  floodFill, watershed, kmeans, cornerHarris, goodFeaturesToTrack,
  drawContours, minEnclosingCircle, fitEllipse, fitLine, matchShapes, convexityDefects,
  compareHist, calcBackProject, findHomography, undistort, remap, calcOpticalFlowFarneback,
  connectedComponentsWithStats,
  fastNlMeansDenoising, inpaint,
};
export default cv;
